// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: from Huax-1839/bloodborne-macos (ee3120a, GPL-2.0-or-later), renderer_metal/.

// Word-level SPIR-V rewrite: a geometry shader (one invocation per input primitive) as a compute
// kernel with one thread per primitive (see mtl_spirv_geometry.h and
// docs/metal/geometry_shader_design.md). The module's own code is kept; what changes:
//   - the entry point's model and execution modes (LocalSize 64 1 1 instead of the input and
//     output primitive, OutputVertices and Invocations), the Geometry capability;
//   - every Input and Output variable (and the pointer types to them) becomes Private, their
//     Location / BuiltIn decorations go (SPIRV-Cross would read them as stage interfaces);
//   - a new entry block: thread bounds check, then the Private inputs are filled from the
//     export shader's output buffer, the outputs zeroed, the emit counters reset;
//   - OpEmitVertex / OpEndPrimitive become stores into the record and index buffers, every
//     OpReturn of the entry function a loop that fills the rest of the index slots.

#include "video_core/renderer_metal/mtl_spirv_geometry.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include <spirv_cross/spirv.hpp>

namespace Metal {

namespace {

using Words = std::vector<u32>;

constexpr u32 kSpirv13 = 0x00010300;
constexpr u32 kRestart = 0xFFFFFFFFu;

struct Inst {
    size_t pos;
    u32 op;
    u32 len;
};

u32 Header(u32 op, size_t len) {
    return static_cast<u32>(len << spv::WordCountShift) | op;
}

void Append(Words& out, u32 op, std::initializer_list<u32> operands) {
    out.push_back(Header(op, operands.size() + 1));
    out.insert(out.end(), operands.begin(), operands.end());
}

/// Index of the result id word of a type, constant, variable or undef instruction (0: none).
u32 DeclResultIndex(u32 op) {
    if (op >= spv::OpTypeVoid && op < spv::OpTypeForwardPointer) {
        return 1;
    }
    if (op >= spv::OpConstantTrue && op <= spv::OpSpecConstantOp) {
        return 2;
    }
    if (op == spv::OpVariable || op == spv::OpUndef) {
        return 2;
    }
    return 0;
}

bool StartsDeclarations(u32 op) {
    return (op >= spv::OpTypeVoid && op <= spv::OpTypeForwardPointer) ||
           (op >= spv::OpConstantTrue && op <= spv::OpSpecConstantOp) || op == spv::OpVariable ||
           op == spv::OpUndef || op == spv::OpFunction;
}

struct Decoration {
    u32 kind;
    std::vector<u32> operands;
};

struct OutVar {
    u32 id{};
    u32 type{}; ///< pointee
    bool position{};
    bool stored{}; ///< position or a Location output (the others are only zeroed)
    u32 location{};
    ScalarType base{ScalarType::Float};
    u32 components{};
    u32 element{}; ///< scalar element type id
    u32 slot{};    ///< record slot
    u32 position_member{~0u}; ///< Position inside a gl_PerVertex block (~0u: the variable itself)
    u32 value_type{};         ///< type of the stored value (the vec4 of a block's member)
};

struct InVar {
    enum class Kind { PrimitiveId, InvocationId, GlIn, Attribute } kind{Kind::Attribute};
    u32 id{};
    u32 type{}; ///< pointee (the array for GlIn and Attribute)
    u32 location{};
    u32 element{};   ///< Attribute: T (scalar or vector); GlIn: the struct
    ScalarType base{ScalarType::Float};
    u32 components{};
    u32 scalar{};         ///< Attribute: scalar element type id
    u32 position_member{~0u}; ///< GlIn: member index of Position
    u32 position_type{};      ///< GlIn: its type (vec4)
};

class Rewriter {
public:
    Rewriter(std::span<const u32> code_, const GeometryEsLayout* es_)
        : mod{code_.begin(), code_.end()}, es{es_} {
        if (mod.size() >= 4) {
            bound = mod[3];
        }
    }

    GeometryRewrite Run();

private:
    [[noreturn]] void Fail(const std::string& message) const {
        throw std::runtime_error(message);
    }

    const u32* W(const Inst& inst) const {
        return &mod[inst.pos];
    }

    u32 NewId() {
        return bound++;
    }

    // ---- analysis ----
    void Parse();
    void Collect();
    void Analyze();
    bool ScalarInfo(u32 type, ScalarType& base, u32& components, u32& element) const;
    bool IsDecl(u32 id, u32 op) const {
        const auto it = defs.find(id);
        return it != defs.end() && (it->second[0] & spv::OpCodeMask) == op;
    }
    bool ConstantU32(u32 id, u32& value) const;
    const Decoration* FindDecoration(u32 id, u32 kind) const;
    const Decoration* FindMemberDecoration(u32 type, u32 member, u32 kind) const;

    // ---- declarations created on demand ----
    u32 FindDecl(u32 op, std::span<const u32> operands, u32 skip) const {
        for (const auto& [id, w] : defs) {
            if ((w[0] & spv::OpCodeMask) == op && w.size() == skip + operands.size() &&
                std::equal(operands.begin(), operands.end(), w.begin() + skip)) {
                return id;
            }
        }
        return 0;
    }
    u32 Type(u32 op, std::initializer_list<u32> operands) {
        if (const u32 id = FindDecl(op, operands, 2)) {
            return id;
        }
        return NewType(op, operands);
    }
    u32 NewType(u32 op, std::initializer_list<u32> operands) {
        const u32 id = NewId();
        Words w{Header(op, operands.size() + 2), id};
        w.insert(w.end(), operands.begin(), operands.end());
        added.insert(added.end(), w.begin(), w.end());
        defs[id] = std::move(w);
        return id;
    }
    u32 PointerType(u32 storage, u32 pointee) {
        return Type(spv::OpTypePointer, {storage, pointee});
    }
    u32 U(u32 value) {
        if (const auto it = u32_constants.find(value); it != u32_constants.end()) {
            return it->second;
        }
        u32 id = 0;
        for (const auto& [cid, w] : defs) {
            if ((w[0] & spv::OpCodeMask) == spv::OpConstant && w.size() == 4 && w[1] == u32_type &&
                w[3] == value) {
                id = cid;
                break;
            }
        }
        if (!id) {
            id = NewId();
            Words w{Header(spv::OpConstant, 4), u32_type, id, value};
            added.insert(added.end(), w.begin(), w.end());
            defs[id] = std::move(w);
        }
        u32_constants[value] = id;
        return id;
    }
    u32 Null(u32 type) {
        if (const auto it = null_constants.find(type); it != null_constants.end()) {
            return it->second;
        }
        const u32 id = NewId();
        Words w{Header(spv::OpConstantNull, 3), type, id};
        added.insert(added.end(), w.begin(), w.end());
        defs[id] = std::move(w);
        null_constants[type] = id;
        return id;
    }
    u32 Variable(u32 ptr_type, u32 storage) {
        const u32 id = NewId();
        Words w{Header(spv::OpVariable, 4), ptr_type, id, storage};
        added.insert(added.end(), w.begin(), w.end());
        defs[id] = std::move(w);
        return id;
    }
    u32 Emit(Words& out, u32 op, u32 type, std::initializer_list<u32> operands) {
        const u32 id = NewId();
        out.push_back(Header(op, operands.size() + 3));
        out.push_back(type);
        out.push_back(id);
        out.insert(out.end(), operands.begin(), operands.end());
        return id;
    }

    // ---- building ----
    void CreateDeclarations();
    void BuildPrologue();
    void EmitVertexCode(Words& out);
    void EndPrimitiveCode(Words& out);
    void ReturnCode(Words& out);
    void AppendIndex(Words& out, u32 value);
    void StoreRecordWords(Words& out, u32 var, const OutVar& v, u32 record_words_base);
    u32 BitcastToU32(Words& out, u32 value, u32 element);
    u32 BitcastFromU32(Words& out, u32 word, u32 element);
    u32 LoadEsWord(Words& out, u32 element_words_base, u32 word_offset);
    void Assemble();

    Words mod;
    const GeometryEsLayout* es;
    u32 bound{};
    std::vector<Inst> insts;
    size_t first_function{};
    u32 version{};

    std::unordered_map<u32, Words> defs;
    std::unordered_map<u32, std::vector<Decoration>> decorations;
    std::map<std::pair<u32, u32>, std::vector<Decoration>> member_decorations;
    Words added;       ///< created declarations, before the first OpFunction
    Words annotations; ///< created decorations
    std::map<u32, u32> u32_constants, null_constants;

    // Module facts.
    u32 entry_function{};
    size_t entry_point_index{};
    u32 entry_points{};
    bool has_streams{};
    std::set<u32> capabilities;
    std::vector<u32> interface_vars;
    GeometryInfo info;
    std::vector<InVar> in_vars;
    std::vector<OutVar> out_vars;
    std::set<u32> converted;      ///< ids of every Input / Output variable
    std::set<u32> output_ids;     ///< the Output ones
    bool has_output_mode{};
    std::set<u32> block_structs;  ///< struct types of the gl_in array
    u32 num_functions{};

    // Created ids.
    u32 u32_type{}, bool_type{}, ptr_sb_u32{}, ptr_unif_u32{};
    u32 gid_var{}, es_buf{}, rec_buf{}, idx_buf{}, params{};
    u32 thread_var{}, vtx_count{}, idx_pos{};
    Words prologue;       ///< entry block code (before the branch on the bounds check)
    Words init_code;      ///< the block that fills inputs (after the bounds check)
    u32 label_entry{}, label_ret{}, label_init{};
    Words result;
};

bool Rewriter::ConstantU32(u32 id, u32& value) const {
    const auto it = defs.find(id);
    if (it == defs.end() || (it->second[0] & spv::OpCodeMask) != spv::OpConstant ||
        it->second.size() != 4) {
        return false;
    }
    value = it->second[3];
    return true;
}

const Decoration* Rewriter::FindDecoration(u32 id, u32 kind) const {
    const auto it = decorations.find(id);
    if (it == decorations.end()) {
        return nullptr;
    }
    for (const Decoration& d : it->second) {
        if (d.kind == kind) {
            return &d;
        }
    }
    return nullptr;
}

const Decoration* Rewriter::FindMemberDecoration(u32 type, u32 member, u32 kind) const {
    const auto it = member_decorations.find({type, member});
    if (it == member_decorations.end()) {
        return nullptr;
    }
    for (const Decoration& d : it->second) {
        if (d.kind == kind) {
            return &d;
        }
    }
    return nullptr;
}

/// A 32-bit float / int / uint scalar or a vector of one.
bool Rewriter::ScalarInfo(u32 type, ScalarType& base, u32& components, u32& element) const {
    const auto scalar = [&](u32 t, ScalarType& b) {
        if (IsDecl(t, spv::OpTypeFloat) && defs.at(t)[2] == 32) {
            b = ScalarType::Float;
            return true;
        }
        if (IsDecl(t, spv::OpTypeInt) && defs.at(t)[2] == 32) {
            b = defs.at(t)[3] ? ScalarType::Int : ScalarType::Uint;
            return true;
        }
        return false;
    };
    if (scalar(type, base)) {
        components = 1;
        element = type;
        return true;
    }
    if (IsDecl(type, spv::OpTypeVector)) {
        const Words& w = defs.at(type);
        if (w[3] >= 1 && w[3] <= 4 && scalar(w[2], base)) {
            components = w[3];
            element = w[2];
            return true;
        }
    }
    return false;
}

void Rewriter::Parse() {
    if (mod.size() < 5 || mod[0] != spv::MagicNumber) {
        Fail("not a SPIR-V module");
    }
    version = mod[1];
    if (version < kSpirv13) {
        Fail("SPIR-V below 1.3");
    }
    first_function = mod.size();
    for (size_t i = 5; i < mod.size();) {
        const u32 len = mod[i] >> spv::WordCountShift;
        const u32 op = mod[i] & spv::OpCodeMask;
        if (len == 0 || i + len > mod.size()) {
            Fail("truncated instruction");
        }
        if (op == spv::OpFunction && first_function == mod.size()) {
            first_function = insts.size();
        }
        if (op == spv::OpFunction) {
            ++num_functions;
        }
        insts.push_back({i, op, len});
        i += len;
    }
}

void Rewriter::Collect() {
    // Execution modes of the entry point and the other facts; Input / Output pointer types and
    // variables become Private (in `mod`) before `defs` is built.
    for (size_t index = 0; index < insts.size(); ++index) {
        const Inst& inst = insts[index];
        u32* w = &mod[inst.pos];
        if (index < first_function) {
            if (inst.op == spv::OpTypePointer && inst.len >= 4 &&
                (w[2] == spv::StorageClassInput || w[2] == spv::StorageClassOutput)) {
                w[2] = spv::StorageClassPrivate;
            }
            if (inst.op == spv::OpVariable && inst.len >= 4 &&
                (w[3] == spv::StorageClassInput || w[3] == spv::StorageClassOutput)) {
                converted.insert(w[2]);
                if (w[3] == spv::StorageClassOutput) {
                    output_ids.insert(w[2]);
                }
                w[3] = spv::StorageClassPrivate;
            }
        }
        switch (inst.op) {
        case spv::OpCapability:
            capabilities.insert(w[1]);
            break;
        case spv::OpEntryPoint: {
            ++entry_points;
            if (w[1] != spv::ExecutionModelGeometry) {
                Fail("not a geometry shader");
            }
            entry_function = w[2];
            entry_point_index = index;
            size_t name_end = 3;
            while (name_end < inst.len) {
                const u32 word = w[name_end++];
                if ((word & 0xff) == 0 || (word & 0xff00) == 0 || (word & 0xff0000) == 0 ||
                    (word & 0xff000000) == 0) {
                    break;
                }
            }
            for (size_t i = name_end; i < inst.len; ++i) {
                interface_vars.push_back(w[i]);
            }
            break;
        }
        case spv::OpExecutionMode:
            if (inst.len >= 3 && w[1] == entry_function) {
                switch (w[2]) {
                case spv::ExecutionModeInputPoints:
                    info.input = GsInputPrimitive::Points;
                    info.input_vertices = 1;
                    break;
                case spv::ExecutionModeInputLines:
                    info.input = GsInputPrimitive::Lines;
                    info.input_vertices = 2;
                    break;
                case spv::ExecutionModeTriangles:
                    info.input = GsInputPrimitive::Triangles;
                    info.input_vertices = 3;
                    break;
                case spv::ExecutionModeInputLinesAdjacency:
                    Fail("input primitive lines with adjacency");
                case spv::ExecutionModeInputTrianglesAdjacency:
                    Fail("input primitive triangles with adjacency");
                case spv::ExecutionModeOutputTriangleStrip:
                    info.output = GsOutputPrimitive::TriangleStrip;
                    has_output_mode = true;
                    break;
                case spv::ExecutionModeOutputLineStrip:
                    info.output = GsOutputPrimitive::LineStrip;
                    has_output_mode = true;
                    break;
                case spv::ExecutionModeOutputPoints:
                    Fail("output primitive points");
                case spv::ExecutionModeOutputVertices:
                    if (inst.len >= 4) {
                        info.max_vertices = w[3];
                    }
                    break;
                case spv::ExecutionModeInvocations:
                    if (inst.len >= 4 && w[3] > 1) {
                        Fail("Invocations > 1");
                    }
                    break;
                default:
                    break;
                }
            }
            break;
        case spv::OpDecorate:
            if (inst.len >= 3) {
                decorations[w[1]].push_back({w[2], std::vector<u32>(w + 3, w + inst.len)});
            }
            break;
        case spv::OpMemberDecorate:
            if (inst.len >= 4) {
                member_decorations[{w[1], w[2]}].push_back(
                    {w[3], std::vector<u32>(w + 4, w + inst.len)});
            }
            break;
        case 220: // OpEmitStreamVertex
        case 221: // OpEndStreamPrimitive
            has_streams = true;
            break;
        default:
            break;
        }
        if (index < first_function) {
            if (const u32 r = DeclResultIndex(inst.op); r != 0 && inst.len > r) {
                defs[w[r]] = Words(w, w + inst.len);
            }
        }
    }
    if (entry_points != 1) {
        Fail("expected exactly one entry point");
    }
}

void Rewriter::Analyze() {
    if (has_streams) {
        Fail("OpEmitStreamVertex (vertex streams)");
    }
    if (!has_output_mode) {
        Fail("no output primitive");
    }
    if (info.max_vertices == 0) {
        Fail("no OutputVertices");
    }
    if (capabilities.count(spv::CapabilityGeometryStreams)) {
        Fail("GeometryStreams capability");
    }
    u32_type = Type(spv::OpTypeInt, {32, 0});
    bool_type = Type(spv::OpTypeBool, {});

    bool have_position = false;
    for (const u32 id : converted) {
        const Words w = defs.at(id);
        const bool is_output = output_ids.count(id) != 0;
        const u32 pointer = w[1];
        if (!IsDecl(pointer, spv::OpTypePointer)) {
            Fail("variable of a non-pointer type");
        }
        const u32 pointee = defs.at(pointer)[3];
        const Decoration* builtin = FindDecoration(id, spv::DecorationBuiltIn);
        const Decoration* location = FindDecoration(id, spv::DecorationLocation);
        if (FindDecoration(id, spv::DecorationComponent)) {
            Fail("Component decoration");
        }
        if (is_output) {
            OutVar v;
            v.id = id;
            v.type = pointee;
            if (builtin) {
                switch (builtin->operands.at(0)) {
                case spv::BuiltInPosition: {
                    ScalarType base;
                    u32 comps, element;
                    if (!ScalarInfo(pointee, base, comps, element) || base != ScalarType::Float ||
                        comps != 4) {
                        Fail("Position output is not a vec4");
                    }
                    v.position = v.stored = true;
                    v.base = base;
                    v.components = 4;
                    v.element = element;
                    v.value_type = pointee;
                    have_position = true;
                    break;
                }
                case spv::BuiltInPointSize:
                case spv::BuiltInClipDistance:
                case spv::BuiltInCullDistance:
                    break;
                default:
                    Fail("output builtin " + std::to_string(builtin->operands.at(0)));
                }
            } else if (location) {
                if (!ScalarInfo(pointee, v.base, v.components, v.element)) {
                    Fail("output at location " + std::to_string(location->operands.at(0)) +
                         " is not a 32-bit scalar or vector");
                }
                v.location = location->operands.at(0);
                v.stored = true;
                v.value_type = pointee;
            } else if (IsDecl(pointee, spv::OpTypeStruct)) {
                // The gl_PerVertex block: its Position member is the position.
                const Words& s = defs.at(pointee);
                for (u32 m = 0; m + 2 < s.size(); ++m) {
                    const Decoration* mb = FindMemberDecoration(pointee, m, spv::DecorationBuiltIn);
                    if (mb && mb->operands.at(0) == spv::BuiltInPosition) {
                        v.position_member = m;
                        v.value_type = s[2 + m];
                    }
                }
                ScalarType base;
                u32 comps, element;
                if (v.position_member == ~0u || !ScalarInfo(v.value_type, base, comps, element) ||
                    base != ScalarType::Float || comps != 4) {
                    Fail("output block without a vec4 Position");
                }
                v.position = v.stored = true;
                v.base = base;
                v.components = 4;
                v.element = element;
                have_position = true;
                block_structs.insert(pointee);
            } else {
                Fail("output without Location or BuiltIn");
            }
            out_vars.push_back(v);
        } else {
            InVar v;
            v.id = id;
            v.type = pointee;
            if (builtin) {
                switch (builtin->operands.at(0)) {
                case spv::BuiltInPrimitiveId:
                    v.kind = InVar::Kind::PrimitiveId;
                    break;
                case spv::BuiltInInvocationId:
                    v.kind = InVar::Kind::InvocationId;
                    break;
                default:
                    Fail("input builtin " + std::to_string(builtin->operands.at(0)));
                }
            } else {
                // An array with one element per input vertex: gl_in (structs) or an attribute.
                if (!IsDecl(pointee, spv::OpTypeArray)) {
                    Fail("input that is not an array");
                }
                const Words& array = defs.at(pointee);
                u32 length = 0;
                if (!ConstantU32(array[3], length) || length != info.input_vertices) {
                    Fail("input array length differs from the input primitive");
                }
                v.element = array[2];
                if (IsDecl(v.element, spv::OpTypeStruct)) {
                    v.kind = InVar::Kind::GlIn;
                    const Words& s = defs.at(v.element);
                    for (u32 m = 0; m + 2 < s.size(); ++m) {
                        const Decoration* mb =
                            FindMemberDecoration(v.element, m, spv::DecorationBuiltIn);
                        if (mb && mb->operands.at(0) == spv::BuiltInPosition) {
                            v.position_member = m;
                            v.position_type = s[2 + m];
                        }
                    }
                    ScalarType base;
                    u32 comps, element;
                    if (v.position_member != ~0u &&
                        (!ScalarInfo(v.position_type, base, comps, element) ||
                         base != ScalarType::Float || comps != 4)) {
                        Fail("gl_in Position is not a vec4");
                    }
                    block_structs.insert(v.element);
                } else {
                    if (!location) {
                        Fail("input without Location or BuiltIn");
                    }
                    if (!ScalarInfo(v.element, v.base, v.components, v.scalar)) {
                        Fail("input at location " + std::to_string(location->operands.at(0)) +
                             " is not a 32-bit scalar or vector");
                    }
                    v.location = location->operands.at(0);
                }
            }
            in_vars.push_back(v);
        }
    }
    if (!have_position) {
        Fail("no Position output");
    }
    // Record slots: the varyings sorted by location.
    std::vector<OutVar*> locations;
    for (OutVar& v : out_vars) {
        if (v.stored && !v.position) {
            locations.push_back(&v);
        }
    }
    std::sort(locations.begin(), locations.end(),
              [](const OutVar* a, const OutVar* b) { return a->location < b->location; });
    for (size_t i = 0; i < locations.size(); ++i) {
        if (i && locations[i]->location == locations[i - 1]->location) {
            Fail("two outputs at location " + std::to_string(locations[i]->location));
        }
        locations[i]->slot = static_cast<u32>(i) + 1;
        info.varyings.push_back(
            {locations[i]->location, locations[i]->components, locations[i]->base});
    }
}

// ---- building -----------------------------------------------------------------------------------

void Rewriter::CreateDeclarations() {
    ptr_sb_u32 = PointerType(spv::StorageClassStorageBuffer, u32_type);
    ptr_unif_u32 = PointerType(spv::StorageClassUniform, u32_type);

    // The buffers: struct { uint data[]; } (Block), one variable each.
    const u32 runtime = NewType(spv::OpTypeRuntimeArray, {u32_type});
    Append(annotations, spv::OpDecorate, {runtime, spv::DecorationArrayStride, 4});
    const u32 buffer_struct = NewType(spv::OpTypeStruct, {runtime});
    Append(annotations, spv::OpDecorate, {buffer_struct, spv::DecorationBlock});
    Append(annotations, spv::OpMemberDecorate, {buffer_struct, 0, spv::DecorationOffset, 0});
    const u32 buffer_ptr = PointerType(spv::StorageClassStorageBuffer, buffer_struct);
    const auto buffer_var = [&](u32 binding, bool read_only) {
        const u32 var = Variable(buffer_ptr, spv::StorageClassStorageBuffer);
        Append(annotations, spv::OpDecorate, {var, spv::DecorationDescriptorSet, kGeometryKernelSet});
        Append(annotations, spv::OpDecorate, {var, spv::DecorationBinding, binding});
        if (read_only) {
            Append(annotations, spv::OpDecorate, {var, spv::DecorationNonWritable});
        }
        return var;
    };
    es_buf = buffer_var(kGeometryBindingEs, true);
    rec_buf = buffer_var(kGeometryBindingRecords, false);
    idx_buf = buffer_var(kGeometryBindingIndices, false);

    const u32 params_struct = NewType(spv::OpTypeStruct, {u32_type, u32_type});
    Append(annotations, spv::OpDecorate, {params_struct, spv::DecorationBlock});
    Append(annotations, spv::OpMemberDecorate, {params_struct, 0, spv::DecorationOffset, 0});
    Append(annotations, spv::OpMemberDecorate, {params_struct, 1, spv::DecorationOffset, 4});
    params = Variable(PointerType(spv::StorageClassUniform, params_struct), spv::StorageClassUniform);
    Append(annotations, spv::OpDecorate, {params, spv::DecorationDescriptorSet, kGeometryKernelSet});
    Append(annotations, spv::OpDecorate, {params, spv::DecorationBinding, kGeometryBindingParams});

    const u32 uvec3 = Type(spv::OpTypeVector, {u32_type, 3});
    gid_var = Variable(PointerType(spv::StorageClassInput, uvec3), spv::StorageClassInput);
    Append(annotations, spv::OpDecorate, {gid_var, spv::DecorationBuiltIn, spv::BuiltInGlobalInvocationId});

    const u32 private_u32 = PointerType(spv::StorageClassPrivate, u32_type);
    thread_var = Variable(private_u32, spv::StorageClassPrivate);
    vtx_count = Variable(private_u32, spv::StorageClassPrivate);
    idx_pos = Variable(private_u32, spv::StorageClassPrivate);
}

u32 Rewriter::BitcastToU32(Words& out, u32 value, u32 element) {
    return element == u32_type ? value : Emit(out, spv::OpBitcast, u32_type, {value});
}

u32 Rewriter::BitcastFromU32(Words& out, u32 word, u32 element) {
    return element == u32_type ? word : Emit(out, spv::OpBitcast, element, {word});
}

u32 Rewriter::LoadEsWord(Words& out, u32 element_words_base, u32 word_offset) {
    const u32 index = Emit(out, spv::OpIAdd, u32_type, {element_words_base, U(word_offset)});
    const u32 ptr = Emit(out, spv::OpAccessChain, ptr_sb_u32, {es_buf, U(0), index});
    return Emit(out, spv::OpLoad, u32_type, {ptr});
}

void Rewriter::BuildPrologue() {
    Words& out = prologue;
    label_entry = NewId();
    label_ret = NewId();
    label_init = NewId();

    const u32 uvec3 = Type(spv::OpTypeVector, {u32_type, 3});
    const u32 gid = Emit(out, spv::OpLoad, uvec3, {gid_var});
    const u32 t = Emit(out, spv::OpCompositeExtract, u32_type, {gid, 0});
    const u32 count_ptr = Emit(out, spv::OpAccessChain, ptr_unif_u32, {params, U(0)});
    const u32 count = Emit(out, spv::OpLoad, u32_type, {count_ptr});
    const u32 out_of_range = Emit(out, spv::OpUGreaterThanEqual, bool_type, {t, count});
    Append(out, spv::OpSelectionMerge, {label_init, 0});
    Append(out, spv::OpBranchConditional, {out_of_range, label_ret, label_init});
    Append(out, spv::OpLabel, {label_ret});
    Append(out, spv::OpReturn, {});
    Append(out, spv::OpLabel, {label_init});

    // The init block: thread, counters, primitive id, zeroed outputs, inputs.
    Words& in = init_code;
    Append(in, spv::OpStore, {thread_var, t});
    Append(in, spv::OpStore, {vtx_count, U(0)});
    Append(in, spv::OpStore, {idx_pos, U(0)});
    const u32 first_ptr = Emit(in, spv::OpAccessChain, ptr_unif_u32, {params, U(1)});
    const u32 first_prim = Emit(in, spv::OpLoad, u32_type, {first_ptr});
    const u32 primitive_id = Emit(in, spv::OpIAdd, u32_type, {first_prim, t});
    for (const OutVar& v : out_vars) {
        Append(in, spv::OpStore, {v.id, Null(v.type)});
    }
    const u32 stride_words = es ? es->stride / 4 : 0;
    const u32 base = Emit(in, spv::OpIMul, u32_type, {t, U(info.input_vertices)});
    for (const InVar& v : in_vars) {
        switch (v.kind) {
        case InVar::Kind::PrimitiveId:
            // (glslang declares gl_PrimitiveIDIn as an int, the recompiler's as a uint)
            Append(in, spv::OpStore, {v.id, BitcastFromU32(in, primitive_id, v.type)});
            continue;
        case InVar::Kind::InvocationId:
            Append(in, spv::OpStore, {v.id, BitcastFromU32(in, U(0), v.type)});
            continue;
        default:
            break;
        }
        Append(in, spv::OpStore, {v.id, Null(v.type)});
        const InterfaceMember* member = nullptr;
        for (const InterfaceMember& m : es->members) {
            if (v.kind == InVar::Kind::GlIn ? m.position
                                            : (!m.position && m.location == v.location)) {
                member = &m;
                break;
            }
        }
        if (!member) {
            continue; // the export shader does not write it: zero
        }
        for (u32 vertex = 0; vertex < info.input_vertices; ++vertex) {
            const u32 element = vertex ? Emit(in, spv::OpIAdd, u32_type, {base, U(vertex)}) : base;
            const u32 words = Emit(in, spv::OpIMul, u32_type, {element, U(stride_words)});
            if (v.kind == InVar::Kind::GlIn) {
                if (v.position_member == ~0u) {
                    continue;
                }
                const u32 f32 = defs.at(v.position_type)[2];
                std::array<u32, 4> parts{};
                for (u32 j = 0; j < 4; ++j) {
                    const u32 word = j < member->components
                                         ? LoadEsWord(in, words, member->offset / 4 + j)
                                         : U(0);
                    parts[j] = Emit(in, spv::OpBitcast, f32, {word});
                }
                const u32 value = Emit(in, spv::OpCompositeConstruct, v.position_type,
                                       {parts[0], parts[1], parts[2], parts[3]});
                const u32 ptr = Emit(in, spv::OpAccessChain,
                                     PointerType(spv::StorageClassPrivate, v.position_type),
                                     {v.id, U(vertex), U(v.position_member)});
                Append(in, spv::OpStore, {ptr, value});
            } else {
                std::array<u32, 4> parts{};
                for (u32 j = 0; j < v.components; ++j) {
                    const u32 word = j < member->components
                                         ? LoadEsWord(in, words, member->offset / 4 + j)
                                         : U(0);
                    parts[j] = BitcastFromU32(in, word, v.scalar);
                }
                u32 value = parts[0];
                if (v.components == 2) {
                    value = Emit(in, spv::OpCompositeConstruct, v.element, {parts[0], parts[1]});
                } else if (v.components == 3) {
                    value = Emit(in, spv::OpCompositeConstruct, v.element,
                                 {parts[0], parts[1], parts[2]});
                } else if (v.components == 4) {
                    value = Emit(in, spv::OpCompositeConstruct, v.element,
                                 {parts[0], parts[1], parts[2], parts[3]});
                }
                const u32 ptr = Emit(in, spv::OpAccessChain,
                                     PointerType(spv::StorageClassPrivate, v.element),
                                     {v.id, U(vertex)});
                Append(in, spv::OpStore, {ptr, value});
            }
        }
    }
}

/// Appends `value` (a record index or the restart marker) to the thread's index slots.
void Rewriter::AppendIndex(Words& out, u32 value) {
    const u32 slots = info.IndexSlots();
    const u32 p = Emit(out, spv::OpLoad, u32_type, {idx_pos});
    const u32 in_range = Emit(out, spv::OpULessThan, bool_type, {p, U(slots)});
    const u32 clamped = Emit(out, spv::OpSelect, u32_type, {in_range, p, U(slots - 1)});
    const u32 t = Emit(out, spv::OpLoad, u32_type, {thread_var});
    const u32 base = Emit(out, spv::OpIMul, u32_type, {t, U(slots)});
    const u32 index = Emit(out, spv::OpIAdd, u32_type, {base, clamped});
    const u32 ptr = Emit(out, spv::OpAccessChain, ptr_sb_u32, {idx_buf, U(0), index});
    Append(out, spv::OpStore, {ptr, value});
    const u32 next = Emit(out, spv::OpIAdd, u32_type, {p, U(1)});
    Append(out, spv::OpStore, {idx_pos, next});
}

void Rewriter::StoreRecordWords(Words& out, u32 var, const OutVar& v, u32 record_words_base) {
    u32 value;
    if (v.position_member != ~0u) {
        const u32 ptr = Emit(out, spv::OpAccessChain,
                             PointerType(spv::StorageClassPrivate, v.value_type),
                             {var, U(v.position_member)});
        value = Emit(out, spv::OpLoad, v.value_type, {ptr});
    } else {
        value = Emit(out, spv::OpLoad, v.type, {var});
    }
    for (u32 j = 0; j < v.components; ++j) {
        const u32 component =
            v.components == 1 ? value : Emit(out, spv::OpCompositeExtract, v.element, {value, j});
        const u32 word = BitcastToU32(out, component, v.element);
        const u32 index = Emit(out, spv::OpIAdd, u32_type, {record_words_base, U(v.slot * 4 + j)});
        const u32 ptr = Emit(out, spv::OpAccessChain, ptr_sb_u32, {rec_buf, U(0), index});
        Append(out, spv::OpStore, {ptr, word});
    }
}

void Rewriter::EmitVertexCode(Words& out) {
    const u32 max = info.max_vertices;
    const u32 n = Emit(out, spv::OpLoad, u32_type, {vtx_count});
    const u32 in_range = Emit(out, spv::OpULessThan, bool_type, {n, U(max)});
    const u32 clamped = Emit(out, spv::OpSelect, u32_type, {in_range, n, U(max - 1)});
    const u32 t = Emit(out, spv::OpLoad, u32_type, {thread_var});
    const u32 base = Emit(out, spv::OpIMul, u32_type, {t, U(max)});
    const u32 record = Emit(out, spv::OpIAdd, u32_type, {base, clamped});
    const u32 words = Emit(out, spv::OpIMul, u32_type, {record, U(info.RecordStride() / 4)});
    for (const OutVar& v : out_vars) {
        if (v.stored) {
            StoreRecordWords(out, v.id, v, words);
        }
    }
    AppendIndex(out, record);
    const u32 next = Emit(out, spv::OpIAdd, u32_type, {n, U(1)});
    Append(out, spv::OpStore, {vtx_count, next});
}

void Rewriter::EndPrimitiveCode(Words& out) {
    AppendIndex(out, U(kRestart));
}

/// Replaces `OpReturn`: fills index slots idx_pos .. 2 * max_vertices - 1 with the restart
/// marker, then returns. (A loop of its own blocks, valid inside any enclosing construct.)
void Rewriter::ReturnCode(Words& out) {
    const u32 slots = info.IndexSlots();
    const u32 header = NewId(), body = NewId(), cont = NewId(), merge = NewId();
    Append(out, spv::OpBranch, {header});
    Append(out, spv::OpLabel, {header});
    const u32 p = Emit(out, spv::OpLoad, u32_type, {idx_pos});
    const u32 go = Emit(out, spv::OpULessThan, bool_type, {p, U(slots)});
    Append(out, spv::OpLoopMerge, {merge, cont, 0});
    Append(out, spv::OpBranchConditional, {go, body, merge});
    Append(out, spv::OpLabel, {body});
    const u32 t = Emit(out, spv::OpLoad, u32_type, {thread_var});
    const u32 base = Emit(out, spv::OpIMul, u32_type, {t, U(slots)});
    const u32 index = Emit(out, spv::OpIAdd, u32_type, {base, p});
    const u32 ptr = Emit(out, spv::OpAccessChain, ptr_sb_u32, {idx_buf, U(0), index});
    Append(out, spv::OpStore, {ptr, U(kRestart)});
    Append(out, spv::OpBranch, {cont});
    Append(out, spv::OpLabel, {cont});
    const u32 next = Emit(out, spv::OpIAdd, u32_type, {p, U(1)});
    Append(out, spv::OpStore, {idx_pos, next});
    Append(out, spv::OpBranch, {header});
    Append(out, spv::OpLabel, {merge});
    Append(out, spv::OpReturn, {});
}

void Rewriter::Assemble() {
    Words out;
    out.reserve(mod.size() + added.size() + prologue.size() + init_code.size() + 4096);
    out.insert(out.end(), mod.begin(), mod.begin() + 5);

    bool annotations_done = false, declarations_done = false;
    size_t added_pos = 0;
    bool in_entry = false, entry_label_done = false;
    std::set<size_t> skip;

    // Return code and emit code are generated at their place (new ids), so `bound` is written
    // last.
    for (size_t index = 0; index < insts.size(); ++index) {
        if (skip.count(index)) {
            continue;
        }
        const Inst& inst = insts[index];
        const u32* w = W(inst);
        const u32 op = inst.op;
        if (!annotations_done && StartsDeclarations(op)) {
            annotations_done = true;
            out.insert(out.end(), annotations.begin(), annotations.end());
        }
        if (op == spv::OpFunction && !declarations_done) {
            declarations_done = true;
            added_pos = out.size(); // the created declarations go here (constants are still
                                    // being created while the function bodies are written)
        }

        if (op == spv::OpCapability && w[1] == spv::CapabilityGeometry) {
            if (!capabilities.count(spv::CapabilityShader)) {
                Append(out, spv::OpCapability, {spv::CapabilityShader}); // Geometry implied it
            }
            continue;
        }
        if (op == spv::OpEntryPoint && index == entry_point_index) {
            // [hdr, model, function, name..., interface...]
            size_t name_end = 3;
            while (name_end < inst.len) {
                const u32 word = w[name_end++];
                if ((word & 0xff) == 0 || (word & 0xff00) == 0 || (word & 0xff0000) == 0 ||
                    (word & 0xff000000) == 0) {
                    break;
                }
            }
            Words entry(w, w + inst.len);
            entry[1] = spv::ExecutionModelGLCompute;
            for (u32 id : {gid_var, es_buf, rec_buf, idx_buf, params, thread_var, vtx_count,
                           idx_pos}) {
                entry.push_back(id);
            }
            entry[0] = Header(spv::OpEntryPoint, entry.size());
            out.insert(out.end(), entry.begin(), entry.end());
            Append(out, spv::OpExecutionMode,
                   {entry_function, spv::ExecutionModeLocalSize, kGeometryThreads, 1, 1});
            continue;
        }
        if (op == spv::OpExecutionMode && w[1] == entry_function) {
            switch (w[2]) {
            case spv::ExecutionModeInputPoints:
            case spv::ExecutionModeInputLines:
            case spv::ExecutionModeTriangles:
            case spv::ExecutionModeInputLinesAdjacency:
            case spv::ExecutionModeInputTrianglesAdjacency:
            case spv::ExecutionModeOutputTriangleStrip:
            case spv::ExecutionModeOutputLineStrip:
            case spv::ExecutionModeOutputPoints:
            case spv::ExecutionModeOutputVertices:
            case spv::ExecutionModeInvocations:
                continue;
            default:
                break;
            }
        }
        if (op == spv::OpDecorate && inst.len >= 3) {
            if (converted.count(w[1])) {
                continue; // Location, BuiltIn, Flat, ...: not a stage interface any more
            }
            if (w[2] == spv::DecorationBlock && block_structs.count(w[1])) {
                continue;
            }
        }
        if (op == spv::OpMemberDecorate && block_structs.count(w[1])) {
            continue;
        }

        if (op == spv::OpFunction) {
            in_entry = w[2] == entry_function;
        }
        if (op == spv::OpLabel && in_entry && !entry_label_done) {
            entry_label_done = true;
            // New entry block: the leading Function variables, the bounds check, the init block,
            // then the original first block (unchanged, so phi predecessors stay valid).
            Append(out, spv::OpLabel, {label_entry});
            for (size_t next = index + 1; next < insts.size(); ++next) {
                const Inst& v = insts[next];
                if (v.op != spv::OpVariable) {
                    break;
                }
                out.insert(out.end(), W(v), W(v) + v.len);
                skip.insert(next);
            }
            out.insert(out.end(), prologue.begin(), prologue.end());
            out.insert(out.end(), init_code.begin(), init_code.end());
            Append(out, spv::OpBranch, {w[1]});
            out.insert(out.end(), w, w + inst.len);
            continue;
        }
        if (op == spv::OpEmitVertex) {
            EmitVertexCode(out);
            continue;
        }
        if (op == spv::OpEndPrimitive) {
            EndPrimitiveCode(out);
            continue;
        }
        if (op == spv::OpReturn && in_entry) {
            ReturnCode(out);
            continue;
        }
        out.insert(out.end(), w, w + inst.len);
    }
    if (!entry_label_done) {
        Fail("entry function not found");
    }
    out.insert(out.begin() + added_pos, added.begin(), added.end());
    out[3] = bound;
    result = std::move(out);
}

GeometryRewrite Rewriter::Run() {
    Parse();
    Collect();
    Analyze();
    GeometryRewrite rewrite;
    rewrite.info = info;
    rewrite.ok = true;
    if (!es) {
        return rewrite;
    }
    if (num_functions == 0) {
        Fail("no function");
    }
    CreateDeclarations();
    BuildPrologue();
    Assemble();
    rewrite.spirv = std::move(result);
    return rewrite;
}

} // namespace

GeometryRewrite AnalyzeGeometryShader(std::span<const u32> spirv) {
    GeometryRewrite failure;
    try {
        return Rewriter{spirv, nullptr}.Run();
    } catch (const std::exception& e) {
        failure.error = e.what();
    }
    return failure;
}

GeometryRewrite RewriteGeometryShader(std::span<const u32> spirv, const GeometryEsLayout& es) {
    GeometryRewrite failure;
    try {
        if (es.stride == 0 || es.stride % 4 != 0) {
            throw std::runtime_error("the export shader's output stride is not a multiple of 4");
        }
        for (const InterfaceMember& m : es.members) {
            if (m.offset % 4 != 0) {
                throw std::runtime_error("an export output is not 4-byte aligned");
            }
        }
        return Rewriter{spirv, &es}.Run();
    } catch (const std::exception& e) {
        failure.error = e.what();
    }
    return failure;
}

} // namespace Metal
