// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: from Huax-1839/bloodborne-macos (ee3120a, GPL-2.0-or-later), renderer_metal/.

// Word-level SPIR-V rewrite of a vertex shader for GCN RectList primitives (see the description at
// RewriteRectListVertex, mtl_spirv_rectlist.h). Same style as mtl_spirv_pervertex.cpp: the module is
// parsed into instructions, the new declarations are appended behind the module's own, the new
// entry function is appended after the last function.
//
// New entry function, in order:
//   vid = VertexIndex; base = (vid / 6) * 3; k = vid % 6
//   for j in 0..2:  index = base + j; <reset Private variables with initializers>; call original;
//                   load every Output leaf (access chains created once, before the first call)
//   corner / bary coefficients from the three positions (as EmitRectListTCS)
//   for every float leaf: c, a, b = values of the corner and the two vertices after it;
//                   v3 = b0 l0 + b1 l1 + b2 l2;
//                   store k == 0 ? c : (k == 1 || k == 3 ? a : (k == 2 || k == 5 ? b : v3))
//   integer and Flat leaves: c for all six vertices (one value across the whole rectangle)
// so that vertices 0..2 of a rectangle's six are the triangle (c, a, b), 3..5 are (a, v3, b).

#define SPV_ENABLE_UTILITY_CODE
#include "video_core/renderer_metal/mtl_spirv_rectlist.h"

#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include <spirv_cross/spirv.hpp>

namespace Metal {

namespace {

using Words = std::vector<u32>;

constexpr u32 kSpirv14 = 0x00010400;
constexpr size_t kMaxLeaves = 512;

struct Inst {
    size_t pos;
    u32 op;
    u32 len;
};

u32 Header(u32 op, size_t len) {
    return static_cast<u32>(len << spv::WordCountShift) | op;
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

/// Opcodes of the module section that starts the global declarations (after all annotations).
bool StartsDeclarations(u32 op) {
    return (op >= spv::OpTypeVoid && op <= spv::OpTypeForwardPointer) ||
           (op >= spv::OpConstantTrue && op <= spv::OpSpecConstantOp) || op == spv::OpVariable ||
           op == spv::OpUndef || op == spv::OpFunction;
}

bool IsAtomicWrite(u32 op) {
    return (op >= spv::OpAtomicStore && op <= spv::OpAtomicXor) ||
           op == spv::OpAtomicFlagTestAndSet || op == spv::OpAtomicFlagClear ||
           op == spv::OpAtomicFMinEXT || op == spv::OpAtomicFMaxEXT || op == spv::OpAtomicFAddEXT;
}

/// One scalar or vector Output the new entry reads three times and writes once.
struct Leaf {
    u32 var{};
    std::vector<u32> path;  ///< constant indices below the variable
    u32 type{};             ///< scalar or vector type of the leaf
    bool is_float{};        ///< 32-bit float scalar or vector
    bool is_vector{};
    bool is_position{};
    bool flat{};            ///< decorated Flat: the corner's value for all six vertices
    u32 pointer{};          ///< the access chain (or the variable)
    u32 value[3]{};         ///< loaded after call j
};

class Rewriter {
public:
    explicit Rewriter(std::span<const u32> code_) : code{code_}, bound{code_[3]} {}

    RectListRewrite Run();

private:
    [[noreturn]] void Fail(const std::string& message) const {
        throw std::runtime_error(message);
    }

    const u32* W(const Inst& inst) const {
        return &code[inst.pos];
    }

    u32 NewId() {
        return bound++;
    }

    // ---- declarations (types, constants) created on demand, appended behind the module's own ----

    u32 FindDecl(u32 op, std::span<const u32> operands, u32 skip_words) const {
        for (const auto& [id, w] : defs) {
            if ((w[0] & spv::OpCodeMask) == op && w.size() == skip_words + operands.size() &&
                std::equal(operands.begin(), operands.end(), w.begin() + skip_words)) {
                return id;
            }
        }
        return 0;
    }

    u32 Type(u32 op, std::initializer_list<u32> operands) {
        if (const u32 id = FindDecl(op, operands, 2)) {
            return id;
        }
        const u32 id = NewId();
        Words w{Header(op, operands.size() + 2), id};
        w.insert(w.end(), operands.begin(), operands.end());
        added.insert(added.end(), w.begin(), w.end());
        defs[id] = std::move(w);
        return id;
    }

    u32 Constant(u32 type, u32 value) {
        const auto key = std::make_pair(type, value);
        if (const auto it = constants.find(key); it != constants.end()) {
            return it->second;
        }
        for (const auto& [id, w] : defs) {
            if ((w[0] & spv::OpCodeMask) == spv::OpConstant && w.size() == 4 && w[1] == type &&
                w[3] == value) {
                return constants[key] = id;
            }
        }
        const u32 id = NewId();
        Words w{Header(spv::OpConstant, 4), type, id, value};
        added.insert(added.end(), w.begin(), w.end());
        defs[id] = std::move(w);
        return constants[key] = id;
    }

    u32 Variable(u32 ptr_type, u32 storage_class) {
        const u32 id = NewId();
        Words w{Header(spv::OpVariable, 4), ptr_type, id, storage_class};
        added.insert(added.end(), w.begin(), w.end());
        defs[id] = std::move(w);
        return id;
    }

    u32 PointerType(u32 storage_class, u32 pointee) {
        return Type(spv::OpTypePointer, {storage_class, pointee});
    }

    bool IsDecl(u32 id, u32 op) const {
        const auto it = defs.find(id);
        return it != defs.end() && (it->second[0] & spv::OpCodeMask) == op;
    }

    bool IsFloat32(u32 type) const {
        return IsDecl(type, spv::OpTypeFloat) && defs.at(type)[2] == 32;
    }

    // ---- passes ----

    void ParseInstructions();
    void CollectModule();
    void CheckBody();
    void FindIndexVariable();
    void Flatten(u32 var, u32 type, std::vector<u32>& path, bool position, bool flat);
    void CollectLeaves();
    void BuildFunction();
    void Assemble();

    std::span<const u32> code;
    u32 bound;
    std::vector<Inst> insts;
    size_t first_function{};
    u32 version{};

    std::unordered_map<u32, Words> defs;  ///< declarations (original + created), by result id
    std::map<std::pair<u32, u32>, u32> constants;
    Words added;                          ///< created declarations, emitted before the first OpFunction
    Words annotations;                    ///< created decorations
    Words function_code;                  ///< the new entry function

    // Module facts.
    u32 entry_function{};
    size_t entry_point_index{};
    u32 entry_points{};
    u32 entry_function_type{};
    std::unordered_map<u32, u32> builtin_of;  ///< id -> BuiltIn
    std::map<std::pair<u32, u32>, u32> member_builtin_of;  ///< (struct, member) -> BuiltIn
    std::set<u32> flat_ids;
    std::set<std::pair<u32, u32>> flat_members;
    std::unordered_map<u32, u32> pointer_class;  ///< id of a pointer -> storage class
    std::vector<u32> output_variables;
    std::vector<u32> reset_variables;  ///< Private variables with an initializer
    u32 index_var{};       ///< the original VertexIndex input (becomes Private), 0: none
    u32 index_type{};      ///< T of the index (32-bit integer)
    u32 input_var{};       ///< the new VertexIndex input
    u32 new_entry{};

    std::vector<Leaf> leaves;
    size_t position_leaf{};

    Words result;
};

void Rewriter::ParseInstructions() {
    if (code.size() < 5 || code[0] != spv::MagicNumber) {
        Fail("not a SPIR-V module");
    }
    version = code[1];
    if (version < kSpirv14) {
        Fail("SPIR-V below 1.4");
    }
    first_function = code.size();
    for (size_t i = 5; i < code.size();) {
        const u32 len = code[i] >> spv::WordCountShift;
        const u32 op = code[i] & spv::OpCodeMask;
        if (len == 0 || i + len > code.size()) {
            Fail("truncated instruction");
        }
        if (op == spv::OpFunction && first_function == code.size()) {
            first_function = insts.size();
        }
        insts.push_back({i, op, len});
        i += len;
    }
}

void Rewriter::CollectModule() {
    for (size_t index = 0; index < insts.size(); ++index) {
        const Inst& inst = insts[index];
        const u32* w = W(inst);
        if (index < first_function) {
            if (const u32 r = DeclResultIndex(inst.op); r != 0 && inst.len > r) {
                defs[w[r]] = Words(w, w + inst.len);
            }
        }
        switch (inst.op) {
        case spv::OpEntryPoint:
            ++entry_points;
            if (w[1] != spv::ExecutionModelVertex) {
                Fail("not a vertex shader");
            }
            entry_function = w[2];
            entry_point_index = index;
            break;
        case spv::OpDecorate:
            if (inst.len >= 4 && w[2] == spv::DecorationBuiltIn) {
                builtin_of[w[1]] = w[3];
            }
            if (inst.len >= 3 && w[2] == spv::DecorationFlat) {
                flat_ids.insert(w[1]);
            }
            break;
        case spv::OpMemberDecorate:
            if (inst.len >= 5 && w[3] == spv::DecorationBuiltIn) {
                member_builtin_of[{w[1], w[2]}] = w[4];
            }
            if (inst.len >= 4 && w[3] == spv::DecorationFlat) {
                flat_members.insert({w[1], w[2]});
            }
            break;
        default:
            break;
        }
        // Storage class of every pointer-valued id (variables, access chains, ...).
        bool has_result = false, has_type = false;
        spv::HasResultAndType(static_cast<spv::Op>(inst.op), &has_result, &has_type);
        if (has_result && has_type && inst.len >= 3) {
            const auto type = defs.find(w[1]);
            if (type != defs.end() && (type->second[0] & spv::OpCodeMask) == spv::OpTypePointer) {
                pointer_class[w[2]] = type->second[2];
            }
        }
    }
    if (entry_points != 1) {
        Fail("expected exactly one entry point");
    }
}

void Rewriter::CheckBody() {
    for (size_t index = first_function; index < insts.size(); ++index) {
        const Inst& inst = insts[index];
        const u32* w = W(inst);
        switch (inst.op) {
        case spv::OpKill:
        case spv::OpTerminateInvocation:
        case spv::OpDemoteToHelperInvocation:
            Fail("terminator " + std::to_string(inst.op) + " in the vertex shader");
        case spv::OpImageWrite:
            Fail("image write");
        case spv::OpImageTexelPointer:
            Fail("image texel pointer");
        case spv::OpStore:
        case spv::OpCopyMemory:
        case spv::OpCopyMemorySized: {
            const auto it = pointer_class.find(w[1]);
            if (it == pointer_class.end()) {
                Fail("store through a pointer of unknown storage class");
            }
            if (it->second != spv::StorageClassFunction && it->second != spv::StorageClassPrivate &&
                it->second != spv::StorageClassOutput) {
                Fail("store to storage class " + std::to_string(it->second));
            }
            break;
        }
        default:
            if (IsAtomicWrite(inst.op)) {
                Fail("atomic operation " + std::to_string(inst.op));
            }
            break;
        }
    }
}

void Rewriter::FindIndexVariable() {
    for (size_t index = 0; index < first_function; ++index) {
        const Inst& inst = insts[index];
        const u32* w = W(inst);
        if (inst.op != spv::OpVariable || inst.len < 4) {
            continue;
        }
        const u32 id = w[2];
        switch (w[3]) {
        case spv::StorageClassInput: {
            const auto builtin = builtin_of.find(id);
            if (builtin == builtin_of.end()) {
                Fail("vertex attribute input (a Location input)");
            }
            if (builtin->second == spv::BuiltInVertexIndex) {
                index_var = id;
            } else if (builtin->second != spv::BuiltInInstanceIndex) {
                Fail("input builtin " + std::to_string(builtin->second));
            }
            break;
        }
        case spv::StorageClassOutput:
            output_variables.push_back(id);
            break;
        case spv::StorageClassPrivate:
            if (inst.len >= 5) {
                reset_variables.push_back(id);
            }
            break;
        default:
            break;
        }
    }
    if (index_var != 0) {
        const u32 pointee = defs.at(defs.at(index_var)[1])[3];
        if (!IsDecl(pointee, spv::OpTypeInt) || defs.at(pointee)[2] != 32) {
            Fail("VertexIndex is not a 32-bit integer");
        }
        index_type = pointee;
    }
}

void Rewriter::Flatten(u32 var, u32 type, std::vector<u32>& path, bool position, bool flat) {
    const Words& def = defs.at(type);
    switch (def[0] & spv::OpCodeMask) {
    case spv::OpTypeFloat:
    case spv::OpTypeInt:
    case spv::OpTypeVector: {
        Leaf leaf;
        leaf.var = var;
        leaf.path = path;
        leaf.type = type;
        leaf.is_position = position;
        leaf.flat = flat;
        u32 scalar = type;
        if ((def[0] & spv::OpCodeMask) == spv::OpTypeVector) {
            leaf.is_vector = true;
            scalar = def[2];
        }
        if (IsFloat32(scalar)) {
            leaf.is_float = true;
        } else if (!IsDecl(scalar, spv::OpTypeInt)) {
            Fail("output of a type that is neither 32-bit float nor integer");
        }
        if (leaves.size() >= kMaxLeaves) {
            Fail("too many output components");
        }
        leaves.push_back(std::move(leaf));
        break;
    }
    case spv::OpTypeArray: {
        const u32 length_id = def[3];
        if (!IsDecl(length_id, spv::OpConstant) || defs.at(length_id).size() != 4) {
            Fail("output array of a non-constant length");
        }
        const u32 length = defs.at(length_id)[3];
        for (u32 i = 0; i < length; ++i) {
            path.push_back(i);
            Flatten(var, def[2], path, position, flat);
            path.pop_back();
        }
        break;
    }
    case spv::OpTypeStruct:
        for (u32 member = 0; member + 2 < def.size(); ++member) {
            const auto builtin = member_builtin_of.find({type, member});
            path.push_back(member);
            Flatten(var, def[2 + member], path,
                    builtin != member_builtin_of.end() && builtin->second == spv::BuiltInPosition,
                    flat || flat_members.count({type, member}) != 0);
            path.pop_back();
        }
        break;
    default:
        Fail("output of an unsupported type (opcode " + std::to_string(def[0] & spv::OpCodeMask) +
             ")");
    }
}

void Rewriter::CollectLeaves() {
    for (const u32 var : output_variables) {
        const u32 pointee = defs.at(defs.at(var)[1])[3];
        std::vector<u32> path;
        const auto builtin = builtin_of.find(var);
        Flatten(var, pointee, path,
                builtin != builtin_of.end() && builtin->second == spv::BuiltInPosition,
                flat_ids.count(var) != 0);
    }
    size_t positions = 0;
    for (size_t i = 0; i < leaves.size(); ++i) {
        if (leaves[i].is_position) {
            ++positions;
            position_leaf = i;
        }
    }
    if (positions != 1) {
        Fail("expected exactly one Position output");
    }
    const Leaf& position = leaves[position_leaf];
    if (!position.is_float || !position.is_vector || defs.at(position.type)[3] != 4) {
        Fail("Position is not a float4");
    }
}

void Rewriter::BuildFunction() {
    Words& out = function_code;
    const auto emit = [&](u32 op, u32 type, std::initializer_list<u32> operands) {
        const u32 id = NewId();
        out.push_back(Header(op, operands.size() + 3));
        out.push_back(type);
        out.push_back(id);
        out.insert(out.end(), operands.begin(), operands.end());
        return id;
    };
    const auto emit_void = [&](u32 op, std::initializer_list<u32> operands) {
        out.push_back(Header(op, operands.size() + 1));
        out.insert(out.end(), operands.begin(), operands.end());
    };

    const u32 void_type = Type(spv::OpTypeVoid, {});
    const u32 bool_type = Type(spv::OpTypeBool, {});
    const u32 f32 = defs.at(leaves[position_leaf].type)[2];
    const u32 u32_type = Type(spv::OpTypeInt, {32, 0});
    const u32 T = index_type ? index_type : u32_type;
    const bool is_signed = defs.at(T)[3] == 1;
    const u32 c3 = Constant(T, 3);
    const u32 c6 = Constant(T, 6);
    const u32 minus_one = Constant(f32, std::bit_cast<u32>(-1.0f));
    const u32 plus_one = Constant(f32, std::bit_cast<u32>(1.0f));

    // The input and the Private index.
    const u32 input_pointer = index_var != 0 ? defs.at(index_var)[1] : PointerType(spv::StorageClassInput, T);
    input_var = Variable(input_pointer, spv::StorageClassInput);
    annotations.push_back(Header(spv::OpDecorate, 4));
    annotations.push_back(input_var);
    annotations.push_back(spv::DecorationBuiltIn);
    annotations.push_back(spv::BuiltInVertexIndex);

    // The new entry function; the leaves' access chains are created once, in its first block.
    new_entry = NewId();
    const u32 label = NewId();
    out.insert(out.end(), {Header(spv::OpFunction, 5), void_type, new_entry, 0u, entry_function_type});
    out.insert(out.end(), {Header(spv::OpLabel, 2), label});

    const u32 vid = emit(spv::OpLoad, T, {input_var});
    const u32 quotient = emit(is_signed ? spv::OpSDiv : spv::OpUDiv, T, {vid, c6});
    const u32 base = emit(spv::OpIMul, T, {quotient, c3});
    const u32 k = emit(is_signed ? spv::OpSMod : spv::OpUMod, T, {vid, c6});

    for (Leaf& leaf : leaves) {
        if (leaf.path.empty()) {
            leaf.pointer = leaf.var;
            continue;
        }
        const u32 pointer_type = PointerType(spv::StorageClassOutput, leaf.type);
        const u32 id = NewId();
        out.push_back(Header(spv::OpAccessChain, 4 + leaf.path.size()));
        out.push_back(pointer_type);
        out.push_back(id);
        out.push_back(leaf.var);
        for (const u32 index : leaf.path) {
            out.push_back(Constant(u32_type, index));
        }
        leaf.pointer = id;
    }

    for (u32 j = 0; j < 3; ++j) {
        const u32 index = j == 0 ? base : emit(spv::OpIAdd, T, {base, Constant(T, j)});
        if (index_var != 0) {
            emit_void(spv::OpStore, {index_var, index});
        }
        for (const u32 var : reset_variables) {
            const Words& def = defs.at(var);
            const u32 initializer = def.size() >= 5 ? def[4] : 0;
            // (a Private variable's initializer is a constant instruction)
            if (initializer != 0) {
                emit_void(spv::OpStore, {var, initializer});
            }
        }
        const u32 call = NewId();
        out.insert(out.end(), {Header(spv::OpFunctionCall, 4), void_type, call, entry_function});
        for (Leaf& leaf : leaves) {
            leaf.value[j] = emit(spv::OpLoad, leaf.type, {leaf.pointer});
        }
    }

    // Corner and barycentric weights from the three positions (EmitRectListTCS).
    std::array<u32, 3> x{}, y{};
    for (u32 j = 0; j < 3; ++j) {
        x[j] = emit(spv::OpCompositeExtract, f32, {leaves[position_leaf].value[j], 0});
        y[j] = emit(spv::OpCompositeExtract, f32, {leaves[position_leaf].value[j], 1});
    }
    std::array<u32, 3> equal_x{}, equal_y{};
    for (u32 i = 0; i < 3; ++i) {
        equal_x[i] = emit(spv::OpFOrdEqual, bool_type, {x[i], x[(i + 1) % 3]});
        equal_y[i] = emit(spv::OpFOrdEqual, bool_type, {y[i], y[(i + 1) % 3]});
    }
    std::array<u32, 3> corner{}, weight{};
    for (u32 i = 0; i < 3; ++i) {
        const u32 xy = emit(spv::OpLogicalAnd, bool_type, {equal_x[i], equal_y[(i + 2) % 3]});
        const u32 yx = emit(spv::OpLogicalAnd, bool_type, {equal_y[i], equal_x[(i + 2) % 3]});
        corner[i] = emit(spv::OpLogicalOr, bool_type, {xy, yx});
        weight[i] = emit(spv::OpSelect, f32, {corner[i], minus_one, plus_one});
    }
    // Which vertex is the corner: 1 if corner[1], else 2 if corner[2], else 0 (the TCS's order).
    const u32 c0 = Constant(T, 0), c1 = Constant(T, 1), c2 = Constant(T, 2), c5 = Constant(T, 5);
    const u32 is_k0 = emit(spv::OpIEqual, bool_type, {k, c0});
    const u32 is_k1 = emit(spv::OpIEqual, bool_type, {k, c1});
    const u32 is_k2 = emit(spv::OpIEqual, bool_type, {k, c2});
    const u32 is_k3 = emit(spv::OpIEqual, bool_type, {k, c3});
    const u32 is_k5 = emit(spv::OpIEqual, bool_type, {k, c5});
    const u32 pick_a = emit(spv::OpLogicalOr, bool_type, {is_k1, is_k3});
    const u32 pick_b = emit(spv::OpLogicalOr, bool_type, {is_k2, is_k5});

    for (Leaf& leaf : leaves) {
        const u32 t = leaf.type;
        const auto& l = leaf.value;
        const auto select = [&](u32 cond, u32 yes, u32 no) {
            return emit(spv::OpSelect, t, {cond, yes, no});
        };
        // Vertex of the corner, the one after it, the one after that.
        const u32 vc = select(corner[1], l[1], select(corner[2], l[2], l[0]));
        if (!leaf.is_float || leaf.flat) {
            emit_void(spv::OpStore, {leaf.pointer, vc});
            continue;
        }
        const u32 va = select(corner[1], l[2], select(corner[2], l[0], l[1]));
        const u32 vb = select(corner[1], l[0], select(corner[2], l[1], l[2]));
        const auto scaled = [&](u32 value, u32 factor) {
            return emit(leaf.is_vector ? static_cast<u32>(spv::OpVectorTimesScalar)
                                       : static_cast<u32>(spv::OpFMul),
                        t, {value, factor});
        };
        const u32 s0 = scaled(l[0], weight[0]);
        const u32 s1 = scaled(l[1], weight[1]);
        const u32 s2 = scaled(l[2], weight[2]);
        const u32 v3 = emit(spv::OpFAdd, t, {s0, emit(spv::OpFAdd, t, {s1, s2})});
        const u32 value = select(is_k0, vc, select(pick_a, va, select(pick_b, vb, v3)));
        emit_void(spv::OpStore, {leaf.pointer, value});
    }
    emit_void(spv::OpReturn, {});
    emit_void(spv::OpFunctionEnd, {});
}

void Rewriter::Assemble() {
    Words out;
    out.reserve(code.size() + added.size() + function_code.size() + annotations.size() + 64);
    out.insert(out.end(), code.begin(), code.begin() + 5);

    bool annotations_done = false, declarations_done = false;
    for (size_t index = 0; index < insts.size(); ++index) {
        const Inst& inst = insts[index];
        const u32* w = W(inst);
        const u32 op = inst.op;
        if (!annotations_done && StartsDeclarations(op)) {
            annotations_done = true;
            out.insert(out.end(), annotations.begin(), annotations.end());
        }
        if (op == spv::OpFunction && !declarations_done) {
            declarations_done = true;
            out.insert(out.end(), added.begin(), added.end());
        }

        if (op == spv::OpDecorate && index_var != 0 && w[1] == index_var &&
            w[2] == spv::DecorationBuiltIn) {
            continue;  // moves to the new input
        }
        if (op == spv::OpVariable && index < first_function && index_var != 0 && w[2] == index_var) {
            continue;  // re-declared as Private among the created declarations
        }
        if ((op == spv::OpName || op == spv::OpExecutionMode || op == spv::OpExecutionModeId) &&
            inst.len >= 2 && w[1] == entry_function) {
            if (op == spv::OpName) {
                continue;  // the new entry point carries the name
            }
            Words mode(w, w + inst.len);
            mode[1] = new_entry;
            out.insert(out.end(), mode.begin(), mode.end());
            continue;
        }
        if (op == spv::OpEntryPoint && index == entry_point_index) {
            Words entry(w, w + inst.len);
            entry[2] = new_entry;
            entry.push_back(input_var);
            entry[0] = Header(spv::OpEntryPoint, entry.size());
            out.insert(out.end(), entry.begin(), entry.end());
            continue;
        }
        out.insert(out.end(), w, w + inst.len);
    }
    if (!declarations_done) {
        Fail("module without functions");
    }
    out.insert(out.end(), function_code.begin(), function_code.end());
    out[3] = bound;
    result = std::move(out);
}

RectListRewrite Rewriter::Run() {
    ParseInstructions();
    CollectModule();
    CheckBody();
    FindIndexVariable();
    CollectLeaves();

    // The original entry function: void(), found by its OpFunction.
    for (size_t index = first_function; index < insts.size(); ++index) {
        const Inst& inst = insts[index];
        if (inst.op == spv::OpFunction && W(inst)[2] == entry_function) {
            entry_function_type = W(inst)[4];
            break;
        }
    }
    if (!IsDecl(entry_function_type, spv::OpTypeFunction) || defs.at(entry_function_type).size() != 3) {
        Fail("the entry function is not void()");
    }

    // The old index variable becomes a Private variable of the same id (declared before the first
    // function with the created declarations).
    if (index_var != 0) {
        const u32 private_pointer = PointerType(spv::StorageClassPrivate, index_type);
        added.insert(added.end(), {Header(spv::OpVariable, 4), private_pointer, index_var,
                                   static_cast<u32>(spv::StorageClassPrivate)});
    }
    BuildFunction();
    Assemble();

    RectListRewrite rewrite;
    rewrite.ok = true;
    rewrite.spirv = std::move(result);
    rewrite.leaf_outputs = static_cast<u32>(leaves.size());
    return rewrite;
}

} // namespace

RectListRewrite RewriteRectListVertex(std::span<const u32> spirv) {
    RectListRewrite failure;
    try {
        if (spirv.size() < 5) {
            throw std::runtime_error("not a SPIR-V module");
        }
        return Rewriter{spirv}.Run();
    } catch (const std::exception& e) {
        failure.error = e.what();
    }
    return failure;
}

} // namespace Metal
