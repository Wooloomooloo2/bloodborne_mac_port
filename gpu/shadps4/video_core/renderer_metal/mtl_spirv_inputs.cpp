// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: fragment inputs the vertex stage does not write (mtl_spirv_inputs.h).

#include "video_core/renderer_metal/mtl_spirv_inputs.h"

#include <fmt/format.h>
#include <unordered_map>
#include <unordered_set>

namespace Metal {

namespace {

constexpr u32 kMagic = 0x07230203;
constexpr u32 kHeaderWords = 5;

// Opcodes.
constexpr u32 OpEntryPoint = 15;
constexpr u32 OpTypeStruct = 30;
constexpr u32 OpTypeArray = 28;
constexpr u32 OpTypePointer = 32;
constexpr u32 OpConstant = 43;
constexpr u32 OpConstantNull = 46;
constexpr u32 OpFunctionCall = 57;
constexpr u32 OpVariable = 59;
constexpr u32 OpLoad = 61;
constexpr u32 OpStore = 62;
constexpr u32 OpCopyMemory = 63;
constexpr u32 OpCopyMemorySized = 64;
constexpr u32 OpAccessChain = 65;
constexpr u32 OpInBoundsAccessChain = 66;
constexpr u32 OpPtrAccessChain = 67;
constexpr u32 OpDecorate = 71;
constexpr u32 OpMemberDecorate = 72;
constexpr u32 OpCopyObject = 83;

// Storage classes and decorations.
constexpr u32 StorageInput = 1;
constexpr u32 StorageOutput = 3;
constexpr u32 StoragePrivate = 6;
constexpr u32 DecorationBlock = 2;
constexpr u32 DecorationBuiltIn = 11;
constexpr u32 DecorationLocation = 30;

struct Instruction {
    u32 offset;
    u32 words;
    u32 op;
};

struct Pointer {
    u32 storage;
    u32 pointee;
};

struct Variable {
    u32 type;
    u32 storage;
};

/// What both functions need to know about the module's globals.
struct Module {
    std::vector<Instruction> instructions;
    std::unordered_map<u32, u32> location;   ///< id -> Location
    std::unordered_set<u32> builtin;         ///< ids decorated BuiltIn (variables)
    std::unordered_set<u32> builtin_structs; ///< struct types with a BuiltIn member
    std::unordered_set<u32> blocks;          ///< struct types decorated Block
    std::unordered_set<u32> structs;
    std::unordered_map<u32, Pointer> pointers;
    std::unordered_map<u32, std::pair<u32, u32>> arrays; ///< id -> (element, length id)
    std::unordered_map<u32, u32> constants;              ///< OpConstant id -> first word
    std::vector<std::pair<u32, Variable>> variables;     ///< global OpVariables, in order
    std::string error;
};

bool Parse(std::span<const u32> spirv, Module& m) {
    if (spirv.size() < kHeaderWords || spirv[0] != kMagic) {
        m.error = "not a SPIR-V module";
        return false;
    }
    bool in_function = false;
    for (u32 offset = kHeaderWords; offset < spirv.size();) {
        const u32 words = spirv[offset] >> 16, op = spirv[offset] & 0xFFFF;
        if (words == 0 || offset + words > spirv.size()) {
            m.error = "truncated instruction";
            return false;
        }
        const u32* w = spirv.data() + offset;
        m.instructions.push_back({offset, words, op});
        switch (op) {
        case 54: // OpFunction
            in_function = true;
            break;
        case OpDecorate:
            if (words >= 3 && w[2] == DecorationBuiltIn) {
                m.builtin.insert(w[1]);
            } else if (words >= 3 && w[2] == DecorationBlock) {
                m.blocks.insert(w[1]);
            } else if (words >= 4 && w[2] == DecorationLocation) {
                m.location[w[1]] = w[3];
            }
            break;
        case OpMemberDecorate:
            if (words >= 4 && w[3] == DecorationBuiltIn) {
                m.builtin_structs.insert(w[1]);
            }
            break;
        case OpTypeStruct:
            m.structs.insert(w[1]);
            break;
        case OpTypeArray:
            if (words >= 4) {
                m.arrays[w[1]] = {w[2], w[3]};
            }
            break;
        case OpTypePointer:
            if (words >= 4) {
                m.pointers[w[1]] = {w[2], w[3]};
            }
            break;
        case OpConstant:
            if (words >= 4) {
                m.constants[w[2]] = w[3];
            }
            break;
        case OpVariable:
            if (!in_function && words >= 4) {
                m.variables.push_back({w[2], {w[1], w[3]}});
            }
            break;
        default:
            break;
        }
        offset += words;
    }
    return true;
}

/// Locations a variable of pointee type `type` covers: (first, count); count 0 when unknown.
std::pair<u32, u32> Covered(const Module& m, u32 id, u32 type) {
    const auto loc = m.location.find(id);
    if (loc == m.location.end()) {
        return {0, 0};
    }
    u32 count = 1;
    if (const auto array = m.arrays.find(type); array != m.arrays.end()) {
        const auto length = m.constants.find(array->second.second);
        if (length == m.constants.end()) {
            return {0, 0};
        }
        count = length->second;
    }
    return {loc->second, count};
}

} // namespace

std::optional<u64> OutputLocations(std::span<const u32> spirv) {
    Module m;
    if (!Parse(spirv, m)) {
        return std::nullopt;
    }
    u64 mask = 0;
    for (const auto& [id, var] : m.variables) {
        if (var.storage != StorageOutput || m.builtin.contains(id)) {
            continue;
        }
        const auto ptr = m.pointers.find(var.type);
        if (ptr == m.pointers.end()) {
            return std::nullopt;
        }
        u32 pointee = ptr->second.pointee;
        if (const auto array = m.arrays.find(pointee); array != m.arrays.end()) {
            pointee = array->second.first;
        }
        if (m.builtin_structs.contains(pointee)) {
            continue; // gl_PerVertex
        }
        if (m.structs.contains(pointee) || m.blocks.contains(pointee)) {
            return std::nullopt;
        }
        const auto [first, count] = Covered(m, id, ptr->second.pointee);
        if (count == 0 || first + count > 64) {
            return std::nullopt;
        }
        for (u32 i = 0; i < count; ++i) {
            mask |= u64(1) << (first + i);
        }
    }
    return mask;
}

ZeroInputsRewrite ZeroUnwrittenInputs(std::span<const u32> spirv, u64 locations) {
    ZeroInputsRewrite result;
    Module m;
    if (!Parse(spirv, m)) {
        result.error = m.error;
        return result;
    }
    // The Input variables to zero and the pointee type of each.
    std::unordered_map<u32, u32> targets; ///< variable id -> pointee type
    for (const auto& [id, var] : m.variables) {
        if (var.storage != StorageInput || m.builtin.contains(id)) {
            continue;
        }
        const auto ptr = m.pointers.find(var.type);
        if (ptr == m.pointers.end()) {
            continue;
        }
        const auto [first, count] = Covered(m, id, ptr->second.pointee);
        for (u32 i = 0; i < count && first + i < 64; ++i) {
            if (locations & (u64(1) << (first + i))) {
                targets[id] = ptr->second.pointee;
                break;
            }
        }
    }
    if (targets.empty()) {
        result.error = "no input at those locations";
        return result;
    }
    // Pointers derived from them (access chains), and every pointee needing a Private pointer.
    std::unordered_set<u32> tracked;
    for (const auto& [id, type] : targets) {
        tracked.insert(id);
    }
    std::unordered_map<u32, u32> chain_pointee; ///< access chain id -> pointee of its type
    for (const Instruction& in : m.instructions) {
        const u32* w = spirv.data() + in.offset;
        switch (in.op) {
        case OpAccessChain:
        case OpInBoundsAccessChain:
            if (in.words >= 4 && tracked.contains(w[3])) {
                const auto ptr = m.pointers.find(w[1]);
                if (ptr == m.pointers.end()) {
                    result.error = "an access chain with an unknown pointer type";
                    return result;
                }
                tracked.insert(w[2]);
                chain_pointee[w[2]] = ptr->second.pointee;
            }
            break;
        case OpLoad:
        case OpDecorate:
        case OpEntryPoint:
            break;
        case OpStore:
        case OpCopyMemory:
        case OpCopyMemorySized:
        case OpPtrAccessChain:
        case OpCopyObject:
        case OpFunctionCall:
            for (u32 i = 1; i < in.words; ++i) {
                if (tracked.contains(w[i])) {
                    result.error = fmt::format("an input to zero is used by opcode {}", in.op);
                    return result;
                }
            }
            break;
        default:
            break;
        }
    }
    u32 bound = spirv[3];
    std::unordered_map<u32, u32> private_pointer; ///< pointee -> new Private pointer type
    std::unordered_map<u32, u32> null_constant;   ///< pointee -> new OpConstantNull
    for (const auto& [id, type] : targets) {
        private_pointer.try_emplace(type, 0);
        null_constant.try_emplace(type, 0);
    }
    for (const auto& [id, type] : chain_pointee) {
        private_pointer.try_emplace(type, 0);
    }
    for (auto& [type, ptr] : private_pointer) {
        ptr = bound++;
    }
    for (auto& [type, constant] : null_constant) {
        constant = bound++;
    }

    // Emit. New pointer types (and null constants) follow the Input pointer type of the same
    // pointee, which precedes every use of either.
    const bool interface_lists_all = spirv[1] >= 0x00010400;
    std::unordered_set<u32> emitted;
    std::vector<u32>& out = result.spirv;
    out.reserve(spirv.size() + 16 * private_pointer.size());
    out.insert(out.end(), spirv.begin(), spirv.begin() + kHeaderWords);
    for (const Instruction& in : m.instructions) {
        const u32* w = spirv.data() + in.offset;
        switch (in.op) {
        case OpEntryPoint: {
            // [op, model, id, name..., interface ids...]
            u32 name_end = 3;
            while (name_end < in.words) {
                const u32 word = w[name_end++];
                if ((word & 0xFF000000u) == 0 || (word & 0x00FF0000u) == 0 ||
                    (word & 0x0000FF00u) == 0 || (word & 0x000000FFu) == 0) {
                    break;
                }
            }
            std::vector<u32> words(w, w + name_end);
            for (u32 i = name_end; i < in.words; ++i) {
                if (interface_lists_all || !targets.contains(w[i])) {
                    words.push_back(w[i]);
                }
            }
            words[0] = (u32(words.size()) << 16) | OpEntryPoint;
            out.insert(out.end(), words.begin(), words.end());
            continue;
        }
        case OpDecorate:
            if (targets.contains(w[1])) {
                continue; // Location, Flat and the like: Private variables have none
            }
            break;
        case OpTypePointer: {
            out.insert(out.end(), w, w + in.words);
            const u32 pointee = w[3];
            if (w[2] == StorageInput && private_pointer.contains(pointee) &&
                emitted.insert(pointee).second) {
                out.insert(out.end(),
                           {(4u << 16) | OpTypePointer, private_pointer[pointee], StoragePrivate,
                            pointee});
                if (const auto null = null_constant.find(pointee); null != null_constant.end()) {
                    out.insert(out.end(), {(3u << 16) | OpConstantNull, pointee, null->second});
                }
            }
            continue;
        }
        case OpVariable:
            if (const auto target = targets.find(w[2]); target != targets.end()) {
                out.insert(out.end(), {(5u << 16) | OpVariable, private_pointer[target->second],
                                       w[2], StoragePrivate, null_constant[target->second]});
                ++result.zeroed;
                continue;
            }
            break;
        case OpAccessChain:
        case OpInBoundsAccessChain:
            if (const auto chain = chain_pointee.find(w[2]); chain != chain_pointee.end()) {
                const size_t at = out.size();
                out.insert(out.end(), w, w + in.words);
                out[at + 1] = private_pointer[chain->second];
                continue;
            }
            break;
        default:
            break;
        }
        out.insert(out.end(), w, w + in.words);
    }
    if (emitted.size() != private_pointer.size()) {
        result.spirv.clear();
        result.error = "a pointee type has no Input pointer to place its Private pointer after";
        return result;
    }
    out[3] = bound;
    result.ok = true;
    return result;
}

ZeroInputsRewrite DuplicateFragmentOutput(std::span<const u32> spirv) {
    ZeroInputsRewrite result;
    Module m;
    if (!Parse(spirv, m)) { result.error = m.error; return result; }
    const auto fail = [&](const char* reason) {
        result.error = reason;
        return result;
    };
    if (OutputLocations(spirv) != std::optional<u64>{1})
        return fail("HDR UI requires exactly one colour output at location zero");
    u32 output = 0, pointer = 0, vector = 0, scalar = 0, entry = 0, entries = 0;
    for (const auto& [id, var] : m.variables) {
        if (var.storage == StorageOutput && m.location.contains(id) && m.location.at(id) == 0) {
            if (output || !m.pointers.contains(var.type)) return fail("ambiguous UI output");
            output = id; pointer = var.type; vector = m.pointers.at(pointer).pointee;
        }
    }
    bool float32 = false;
    for (const auto& in : m.instructions) {
        const auto* w = spirv.data() + in.offset;
        if (in.op == OpEntryPoint) {
            ++entries;
            if (in.words < 4 || w[1] != 4) return fail("not a fragment entry point");
            entry = w[2];
        }
        if (in.op == 23 && in.words == 4 && w[1] == vector && w[3] == 4) scalar = w[2];
        if (in.op == OpDecorate && in.words >= 4 && w[1] == output &&
            w[2] == 32 && w[3] != 0) return fail("dual-source UI output");
    }
    for (const auto& in : m.instructions) {
        const auto* w = spirv.data() + in.offset;
        if (in.op == 22 && in.words == 3 && w[1] == scalar && w[2] == 32) float32 = true;
    }
    if (!output || !float32 || entries != 1) return fail("UI output is not a sole float4");
    u32 bound = spirv[3];
    const u32 black = bound++, white = bound++;
    auto& out = result.spirv;
    out.assign(spirv.begin(), spirv.begin() + kHeaderWords);
    bool decorated = false, globals = false, in_entry = false;
    u32 returns = 0;
    for (const auto& in : m.instructions) {
        const auto* w = spirv.data() + in.offset;
        if (!decorated && in.op >= 19 && in.op <= 39) {
            out.insert(out.end(), {(4u << 16) | OpDecorate, black, DecorationLocation, 1,
                                   (4u << 16) | OpDecorate, white, DecorationLocation, 2});
            decorated = true;
        }
        if (in.op == 54) { // OpFunction: globals must precede every function.
            if (!globals) {
                out.insert(out.end(), {(4u << 16) | OpVariable, pointer, black, StorageOutput,
                                       (4u << 16) | OpVariable, pointer, white, StorageOutput});
                globals = true;
            }
            in_entry = in.words >= 3 && w[2] == entry;
        }
        if (in.op == OpEntryPoint) {
            const auto at = out.size();
            out.insert(out.end(), w, w + in.words);
            out[at] = ((in.words + 2) << 16) | OpEntryPoint;
            out.push_back(black); out.push_back(white);
            continue;
        }
        if (in_entry && in.op == 253) { // OpReturn; discarded fragments never reach this.
            const u32 value = bound++;
            out.insert(out.end(), {(4u << 16) | OpLoad, vector, value, output,
                                   (3u << 16) | OpStore, black, value,
                                   (3u << 16) | OpStore, white, value});
            ++returns;
        }
        out.insert(out.end(), w, w + in.words);
        if (in.op == 56) in_entry = false;
    }
    if (!decorated || !globals || !returns) {
        result.spirv.clear(); result.error = "incomplete UI entry point"; return result;
    }
    out[3] = bound;
    result.ok = true;
    return result;
}

} // namespace Metal
