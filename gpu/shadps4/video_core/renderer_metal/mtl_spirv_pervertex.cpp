// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: from Huax-1839/bloodborne-macos (ee3120a, GPL-2.0-or-later), renderer_metal/.

// Word-level SPIR-V rewrite of PerVertexKHR fragment inputs (pull-model interpolation of GCN
// v_interp_p0/p1/p2; the recompiler declares them as `in T fs_inN_p[3]` + PerVertexKHR). MSL has
// no per-vertex fragment inputs, so the module is changed before SPIRV-Cross sees it. The design
// and its limits are described at RewritePerVertexInputs (mtl_spirv_pervertex.h).
//
// Reconstruction (general case), all in the entry point's first block, before any branch (the
// derivatives need a uniform quad):
//   A      = value of the ordinary smooth input at the pixel = b0 v0 + b1 v1 + b2 v2
//   dA/dx  = db0/dx v0 + db1/dx v1 + db2/dx v2        (A is linear in v, so the derivative of A
//   dA/dy  = db0/dy v0 + db1/dy v1 + db2/dy v2         equals the combination of b's derivatives)
// i.e. M v = [A, dA/dx, dA/dy] with the rows of M = (b, db/dx, db/dy), b = BaryCoordKHR
// (perspective-correct). With r0 = b, r1 = db/dx, r2 = db/dy the inverse has the columns
// (r1 x r2, r2 x r0, r0 x r1) / det, det = r0 . (r1 x r2); since r1 and r2 sum to zero their cross
// product is n * (1,1,1) and det = n. The system is singular exactly when r1 and r2 are collinear
// (3 n^2 / (|r1|^2 |r2|^2) is the squared sine between them): below 1e-6 the vertices fall back
// to v_i = A. Precision: the derivatives are float differences of A, so the absolute error of
// v_i is about 6e-8 |A| times the triangle's size in pixels (the 1/|db/dx| amplification): 1e-5
// relative for a 100 px triangle, 1e-3 for 10000 px.

#include "video_core/renderer_metal/mtl_spirv_pervertex.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

#include <spirv_cross/spirv.hpp>

namespace Metal {

namespace {

using Words = std::vector<u32>;

constexpr u32 kSpirv14 = 0x00010400;
constexpr const char* kBarycentricExtension = "SPV_KHR_fragment_shader_barycentric";

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

/// Opcodes of the module section that starts the global declarations (after all annotations).
bool StartsDeclarations(u32 op) {
    return (op >= spv::OpTypeVoid && op <= spv::OpTypeForwardPointer) ||
           (op >= spv::OpConstantTrue && op <= spv::OpSpecConstantOp) || op == spv::OpVariable ||
           op == spv::OpUndef || op == spv::OpFunction;
}

struct PerVertexVar {
    u32 id{};
    u32 element{};      ///< T (scalar float or vector of float)
    u32 array_type{};   ///< T[3]
    u32 flat_ptr{};     ///< Input pointer to T
    u32 location{~0u};
    u32 component{~0u};
    bool only_vertex0{true};
    bool any_use{};
    u32 input{};        ///< the new ordinary input
};

class Rewriter {
public:
    explicit Rewriter(std::span<const u32> code_) : code{code_}, bound{code_[3]} {}

    PerVertexRewrite Run();

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
        for (const auto& [id, w] : defs) {
            if ((w[0] & spv::OpCodeMask) == spv::OpConstant && w.size() == 4 && w[1] == type &&
                w[3] == value) {
                return id;
            }
        }
        const u32 id = NewId();
        Words w{Header(spv::OpConstant, 4), type, id, value};
        added.insert(added.end(), w.begin(), w.end());
        defs[id] = std::move(w);
        return id;
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

    // ---- passes ----

    void ParseInstructions();
    void CollectModule();
    void ValidateVars();
    void AnalyseUses();
    void CreateDeclarations();
    void BuildEntryCode();
    void Assemble();

    std::span<const u32> code;
    u32 bound;
    std::vector<Inst> insts;
    size_t first_function{};
    u32 version{};

    std::unordered_map<u32, Words> defs;  ///< declarations (original + created), by result id
    Words added;                          ///< created declarations, emitted before the first OpFunction
    Words annotations;                    ///< created decorations
    Words entry_code;                     ///< emitted at the start of the entry function

    // Module facts.
    u32 entry_function{};
    size_t entry_point_index{};  ///< insts index of the fragment OpEntryPoint
    u32 entry_points{};
    std::set<u32> capabilities;
    bool has_extension{};
    u32 bary_var{};              ///< existing or created BaryCoordKHR input
    bool bary_created{};
    std::map<u32, PerVertexVar> vars;  ///< by variable id
    std::unordered_map<u32, u32> decorated_builtin;  ///< id -> BuiltIn

    // Uses: pointer ids derived from the variables (access chains), mapped to their variable, and
    // the Private pointer types that replace their (Input) result types.
    std::unordered_map<u32, u32> derived;
    std::unordered_map<u32, u32> retype;  ///< result id of a derived access chain -> new type id
    std::unordered_map<u32, u32> private_pointers;  ///< Input pointer type -> Private pointer type

    Words result;
    bool need_general{};
    u32 flat_count{};
    u32 general_count{};
};

void Rewriter::ParseInstructions() {
    if (code.size() < 5 || code[0] != spv::MagicNumber) {
        Fail("not a SPIR-V module");
    }
    version = code[1];
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
    std::map<u32, std::vector<std::pair<u32, std::vector<u32>>>> decorations;
    for (size_t index = 0; index < first_function; ++index) {
        const Inst& inst = insts[index];
        const u32* w = W(inst);
        if (const u32 r = DeclResultIndex(inst.op); r != 0 && inst.len > r) {
            defs[w[r]] = Words(w, w + inst.len);
        }
        switch (inst.op) {
        case spv::OpCapability:
            capabilities.insert(w[1]);
            break;
        case spv::OpExtension:
            has_extension |= std::strncmp(reinterpret_cast<const char*>(w + 1),
                                          kBarycentricExtension, (inst.len - 1) * 4) == 0;
            break;
        case spv::OpEntryPoint:
            ++entry_points;
            if (w[1] == spv::ExecutionModelFragment) {
                entry_function = w[2];
                entry_point_index = index;
            }
            break;
        case spv::OpDecorate:
            if (inst.len >= 3) {
                decorations[w[1]].push_back({w[2], std::vector<u32>(w + 3, w + inst.len)});
                if (w[2] == spv::DecorationBuiltIn && inst.len >= 4) {
                    decorated_builtin[w[1]] = w[3];
                }
            }
            break;
        default:
            break;
        }
    }
    if (entry_points != 1 || entry_function == 0) {
        Fail("expected exactly one fragment entry point");
    }
    for (const auto& [id, list] : decorations) {
        const bool per_vertex = std::any_of(list.begin(), list.end(), [](const auto& d) {
            return d.first == spv::DecorationPerVertexKHR;
        });
        if (!per_vertex) {
            continue;
        }
        PerVertexVar var;
        var.id = id;
        for (const auto& [decoration, operands] : list) {
            switch (decoration) {
            case spv::DecorationPerVertexKHR:
                break;
            case spv::DecorationLocation:
                var.location = operands.at(0);
                break;
            case spv::DecorationComponent:
                var.component = operands.at(0);
                break;
            default:
                Fail("PerVertexKHR input with an unsupported decoration " + std::to_string(decoration));
            }
        }
        vars[id] = var;
    }
    for (const auto& [id, builtin] : decorated_builtin) {
        if (builtin == spv::BuiltInBaryCoordKHR && IsDecl(id, spv::OpVariable)) {
            bary_var = id;
        }
    }
}

void Rewriter::ValidateVars() {
    for (auto& [id, var] : vars) {
        if (!IsDecl(id, spv::OpVariable) || defs[id][3] != spv::StorageClassInput) {
            Fail("PerVertexKHR decoration on something other than an Input variable");
        }
        const u32 pointer = defs[id][1];
        if (!IsDecl(pointer, spv::OpTypePointer) || !IsDecl(defs[pointer][3], spv::OpTypeArray)) {
            Fail("PerVertexKHR input is not an array");
        }
        var.array_type = defs[pointer][3];
        const Words& array = defs[var.array_type];
        const u32 length = array[3];
        if (!IsDecl(length, spv::OpConstant) || defs[length][3] != 3) {
            Fail("PerVertexKHR input array is not 3 long");
        }
        var.element = array[2];
        const auto is_float32 = [&](u32 type) {
            return IsDecl(type, spv::OpTypeFloat) && defs[type][2] == 32;
        };
        const bool scalar = is_float32(var.element);
        const bool vector = IsDecl(var.element, spv::OpTypeVector) && is_float32(defs[var.element][2]);
        if (!scalar && !vector) {
            Fail("PerVertexKHR input is not an array of 32-bit float scalars or vectors");
        }
        if (var.location == ~0u) {
            Fail("PerVertexKHR input without Location");
        }
    }
}

void Rewriter::AnalyseUses() {
    for (const auto& [id, var] : vars) {
        derived[id] = id;
    }
    // Pointer chains rooted at a variable (also chains of chains), until nothing changes.
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t index = first_function; index < insts.size(); ++index) {
            const Inst& inst = insts[index];
            if ((inst.op == spv::OpAccessChain || inst.op == spv::OpInBoundsAccessChain) &&
                inst.len >= 4) {
                const u32* w = W(inst);
                const auto base = derived.find(w[3]);
                if (base != derived.end() && !derived.count(w[2])) {
                    derived[w[2]] = base->second;
                    changed = true;
                }
            }
        }
    }
    for (size_t index = first_function; index < insts.size(); ++index) {
        const Inst& inst = insts[index];
        const u32* w = W(inst);
        switch (inst.op) {
        case spv::OpAccessChain:
        case spv::OpInBoundsAccessChain: {
            if (inst.len < 4 || !derived.count(w[3])) {
                break;
            }
            const u32 result_type = w[1];
            if (!IsDecl(result_type, spv::OpTypePointer) ||
                defs[result_type][2] != spv::StorageClassInput) {
                Fail("access chain into a PerVertexKHR input with an unexpected result type");
            }
            auto cached = private_pointers.find(result_type);
            if (cached == private_pointers.end()) {
                cached = private_pointers
                             .emplace(result_type,
                                      PointerType(spv::StorageClassPrivate, defs[result_type][3]))
                             .first;
            }
            retype[w[2]] = cached->second;
            if (const auto var = vars.find(w[3]); var != vars.end()) {
                var->second.any_use = true;
                // Vertex index: the first index, a constant 0 keeps the input flat.
                bool zero = false;
                if (inst.len >= 5 && IsDecl(w[4], spv::OpConstant) && defs[w[4]].size() == 4) {
                    zero = IsDecl(defs[w[4]][1], spv::OpTypeInt) && defs[w[4]][3] == 0;
                }
                var->second.only_vertex0 &= zero;
            }
            break;
        }
        case spv::OpLoad:
            if (inst.len >= 4) {
                if (const auto var = vars.find(w[3]); var != vars.end()) {
                    var->second.any_use = true;
                    var->second.only_vertex0 = false;  // the whole array
                }
            }
            break;
        case spv::OpLabel:
        case spv::OpFunction:
        case spv::OpFunctionEnd:
            break;
        default: {
            // Any other instruction mentioning one of the pointers (a call, a store, a copy):
            // beyond what this rewrite handles. (False positives only cost the fallback.)
            for (u32 i = 1; i < inst.len; ++i) {
                if (derived.count(w[i])) {
                    Fail("PerVertexKHR input pointer used by " + std::to_string(inst.op));
                }
            }
            break;
        }
        }
    }
    for (auto& [id, var] : vars) {
        if (!var.any_use) {
            var.only_vertex0 = true;  // unused: the cheap flat form
        }
        (var.only_vertex0 ? flat_count : general_count)++;
    }
    need_general = general_count != 0;
}

void Rewriter::CreateDeclarations() {
    const u32 f32 = Type(spv::OpTypeFloat, {32});
    const u32 vec3 = Type(spv::OpTypeVector, {f32, 3});
    // The new ordinary inputs, the Private arrays (the old variables), the barycentric input.
    for (auto& [id, var] : vars) {
        const u32 input_ptr = PointerType(spv::StorageClassInput, var.element);
        var.input = Variable(input_ptr, spv::StorageClassInput);
        Append(annotations, spv::OpDecorate, {var.input, spv::DecorationLocation, var.location});
        if (var.component != ~0u) {
            Append(annotations, spv::OpDecorate, {var.input, spv::DecorationComponent, var.component});
        }
        if (var.only_vertex0) {
            Append(annotations, spv::OpDecorate, {var.input, spv::DecorationFlat});
        }
    }
    if (need_general && bary_var == 0) {
        bary_var = Variable(PointerType(spv::StorageClassInput, vec3), spv::StorageClassInput);
        bary_created = true;
        Append(annotations, spv::OpDecorate,
               {bary_var, spv::DecorationBuiltIn, spv::BuiltInBaryCoordKHR});
    } else if (need_general) {
        const u32 pointer = defs[bary_var][1];
        const u32 pointee = defs[pointer][3];
        if (!IsDecl(pointee, spv::OpTypeVector) || defs[pointee][2] != f32 ||
            defs[pointee][3] != 3) {
            Fail("existing BaryCoordKHR input is not a vec3");
        }
    }
    // The old variables become Private arrays; their definitions move behind the new types.
    for (auto& [id, var] : vars) {
        const u32 private_ptr = PointerType(spv::StorageClassPrivate, var.array_type);
        const u32 var_id = id;
        added.insert(added.end(), {Header(spv::OpVariable, 4), private_ptr, var_id,
                                   static_cast<u32>(spv::StorageClassPrivate)});
    }
}

void Rewriter::BuildEntryCode() {
    Words& out = entry_code;
    const auto emit = [&](u32 op, u32 type, std::initializer_list<u32> operands) {
        const u32 id = NewId();
        out.push_back(Header(op, operands.size() + 3));
        out.push_back(type);
        out.push_back(id);
        out.insert(out.end(), operands.begin(), operands.end());
        return id;
    };

    const u32 f32 = Type(spv::OpTypeFloat, {32});
    const u32 vec3 = Type(spv::OpTypeVector, {f32, 3});
    const u32 bool_type = Type(spv::OpTypeBool, {});
    std::array<std::array<u32, 3>, 3> coef{};  // coef[k][i]: weight of y_k in vertex i

    if (need_general) {
        const u32 b = emit(spv::OpLoad, vec3, {bary_var});
        const u32 db_x = emit(spv::OpDPdxFine, vec3, {b});
        const u32 db_y = emit(spv::OpDPdyFine, vec3, {b});
        const auto extract = [&](u32 v) {
            return std::array<u32, 3>{emit(spv::OpCompositeExtract, f32, {v, 0}),
                                      emit(spv::OpCompositeExtract, f32, {v, 1}),
                                      emit(spv::OpCompositeExtract, f32, {v, 2})};
        };
        const auto r0 = extract(b);
        const auto r1 = extract(db_x);
        const auto r2 = extract(db_y);
        const auto cross = [&](const std::array<u32, 3>& a, const std::array<u32, 3>& c) {
            const auto component = [&](int i, int j) {  // a_i c_j - a_j c_i
                const u32 p = emit(spv::OpFMul, f32, {a[i], c[j]});
                const u32 q = emit(spv::OpFMul, f32, {a[j], c[i]});
                return emit(spv::OpFSub, f32, {p, q});
            };
            return std::array<u32, 3>{component(1, 2), component(2, 0), component(0, 1)};
        };
        const auto c0 = cross(r1, r2);
        const auto c1 = cross(r2, r0);
        const auto c2 = cross(r0, r1);
        const std::array<std::array<u32, 3>, 3> cols{c0, c1, c2};
        // det = r0 . c0
        const u32 d0 = emit(spv::OpFMul, f32, {r0[0], c0[0]});
        const u32 d1 = emit(spv::OpFMul, f32, {r0[1], c0[1]});
        const u32 d2 = emit(spv::OpFMul, f32, {r0[2], c0[2]});
        const u32 det = emit(spv::OpFAdd, f32, {emit(spv::OpFAdd, f32, {d0, d1}), d2});
        // Well-conditioned when 3 det^2 / (|r1|^2 |r2|^2) (the squared sine of the angle between
        // the gradients) is above 1e-6; NaN and zero compare false.
        const u32 dot1 = emit(spv::OpDot, f32, {db_x, db_x});
        const u32 dot2 = emit(spv::OpDot, f32, {db_y, db_y});
        const u32 zero = Constant(f32, std::bit_cast<u32>(0.0f));
        const u32 one = Constant(f32, std::bit_cast<u32>(1.0f));
        const u32 three = Constant(f32, std::bit_cast<u32>(3.0f));
        const u32 threshold = Constant(f32, std::bit_cast<u32>(1e-6f));
        const u32 lhs = emit(spv::OpFMul, f32, {emit(spv::OpFMul, f32, {det, det}), three});
        const u32 rhs =
            emit(spv::OpFMul, f32, {emit(spv::OpFMul, f32, {dot1, dot2}), threshold});
        const u32 good = emit(spv::OpFOrdGreaterThan, bool_type, {lhs, rhs});
        const u32 rdet = emit(spv::OpFDiv, f32, {one, det});
        for (int k = 0; k < 3; ++k) {
            for (int i = 0; i < 3; ++i) {
                const u32 scaled = emit(spv::OpFMul, f32, {cols[k][i], rdet});
                coef[k][i] = emit(spv::OpSelect, f32, {good, scaled, k == 0 ? one : zero});
            }
        }
    }

    for (const auto& [id, var] : vars) {
        const u32 a = emit(spv::OpLoad, var.element, {var.input});
        u32 v[3] = {a, a, a};
        if (!var.only_vertex0) {
            const u32 y1 = emit(spv::OpDPdxFine, var.element, {a});
            const u32 y2 = emit(spv::OpDPdyFine, var.element, {a});
            const bool vector = IsDecl(var.element, spv::OpTypeVector);
            const auto scale = [&](u32 value, u32 factor) {
                return emit(vector ? static_cast<u32>(spv::OpVectorTimesScalar)
                                   : static_cast<u32>(spv::OpFMul),
                            var.element, {value, factor});
            };
            for (int i = 0; i < 3; ++i) {
                const u32 s0 = scale(a, coef[0][i]);
                const u32 s1 = scale(y1, coef[1][i]);
                const u32 s2 = scale(y2, coef[2][i]);
                v[i] = emit(spv::OpFAdd, var.element, {emit(spv::OpFAdd, var.element, {s0, s1}), s2});
            }
        }
        const u32 array = emit(spv::OpCompositeConstruct, var.array_type, {v[0], v[1], v[2]});
        Append(out, spv::OpStore, {id, array});
    }
}

void Rewriter::Assemble() {
    // Final layout. `out` is the result; the sections are the original order with insertions.
    Words out;
    out.reserve(code.size() + added.size() + entry_code.size() + 64);
    out.insert(out.end(), code.begin(), code.begin() + 5);
    out[3] = bound;

    const bool general_capabilities = need_general;
    bool capabilities_done = false, extension_done = false, annotations_done = false;
    bool declarations_done = false;
    bool in_entry_function = false, entry_code_done = false;

    const auto want_capabilities = [&] {
        if (!capabilities_done) {
            capabilities_done = true;
            if (general_capabilities) {
                if (!capabilities.count(spv::CapabilityFragmentBarycentricKHR)) {
                    Append(out, spv::OpCapability, {spv::CapabilityFragmentBarycentricKHR});
                }
                if (!capabilities.count(spv::CapabilityDerivativeControl)) {
                    Append(out, spv::OpCapability, {spv::CapabilityDerivativeControl});
                }
            }
        }
    };
    const auto want_extension = [&] {
        if (!extension_done) {
            extension_done = true;
            if (general_capabilities && !has_extension) {
                const size_t chars = std::strlen(kBarycentricExtension) + 1;
                const size_t words = (chars + 3) / 4;
                out.push_back(Header(spv::OpExtension, words + 1));
                const size_t at = out.size();
                out.resize(at + words, 0);
                std::memcpy(&out[at], kBarycentricExtension, chars);
            }
        }
    };

    for (size_t index = 0; index < insts.size(); ++index) {
        const Inst& inst = insts[index];
        const u32* w = W(inst);
        const u32 op = inst.op;
        if (op != spv::OpCapability) {
            want_capabilities();
        }
        if (op == spv::OpExtInstImport || op == spv::OpMemoryModel) {
            want_extension();
        }
        if (!annotations_done && StartsDeclarations(op)) {
            annotations_done = true;
            out.insert(out.end(), annotations.begin(), annotations.end());
        }
        if (op == spv::OpFunction && !declarations_done) {
            declarations_done = true;
            out.insert(out.end(), added.begin(), added.end());
        }

        if (op == spv::OpDecorate && vars.count(w[1]) &&
            (w[2] == spv::DecorationPerVertexKHR || w[2] == spv::DecorationLocation ||
             w[2] == spv::DecorationComponent)) {
            continue;
        }
        if (op == spv::OpVariable && index < first_function && vars.count(w[2])) {
            continue;  // re-declared as Private among the created declarations
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
            Words entry(w, w + name_end);
            for (size_t i = name_end; i < inst.len; ++i) {
                if (version >= kSpirv14 || !vars.count(w[i])) {
                    entry.push_back(w[i]);  // 1.4+ lists Private globals too
                }
            }
            for (const auto& [id, var] : vars) {
                entry.push_back(var.input);
            }
            if (bary_created) {
                entry.push_back(bary_var);
            }
            entry[0] = Header(spv::OpEntryPoint, entry.size());
            out.insert(out.end(), entry.begin(), entry.end());
            continue;
        }

        if ((op == spv::OpAccessChain || op == spv::OpInBoundsAccessChain) && index > first_function) {
            if (const auto it = retype.find(w[2]); it != retype.end()) {
                out.insert(out.end(), w, w + inst.len);
                out[out.size() - inst.len + 1] = it->second;
                continue;
            }
        }

        if (op == spv::OpFunction) {
            in_entry_function = w[2] == entry_function;
        }
        const bool after_variables = in_entry_function && !entry_code_done &&
                                     op != spv::OpFunction && op != spv::OpFunctionParameter &&
                                     op != spv::OpLabel && op != spv::OpVariable;
        if (after_variables) {
            entry_code_done = true;
            out.insert(out.end(), entry_code.begin(), entry_code.end());
        }
        out.insert(out.end(), w, w + inst.len);
    }
    if (!entry_code_done) {
        Fail("entry function not found");
    }
    result = std::move(out);
}

PerVertexRewrite Rewriter::Run() {
    ParseInstructions();
    CollectModule();
    PerVertexRewrite rewrite;
    if (vars.empty()) {
        rewrite.ok = true;
        rewrite.spirv.assign(code.begin(), code.end());
        return rewrite;
    }
    ValidateVars();
    AnalyseUses();
    CreateDeclarations();
    BuildEntryCode();
    Assemble();
    rewrite.ok = true;
    rewrite.spirv = std::move(result);
    rewrite.flat_inputs = flat_count;
    rewrite.interpolated_inputs = general_count;
    return rewrite;
}

} // namespace

bool HasPerVertexInputs(std::span<const u32> spirv) {
    if (spirv.size() < 5 || spirv[0] != spv::MagicNumber) {
        return false;
    }
    for (size_t i = 5; i < spirv.size();) {
        const u32 len = spirv[i] >> spv::WordCountShift;
        if (len == 0 || i + len > spirv.size()) {
            return false;
        }
        if ((spirv[i] & spv::OpCodeMask) == spv::OpDecorate && len >= 3 &&
            spirv[i + 2] == spv::DecorationPerVertexKHR) {
            return true;
        }
        i += len;
    }
    return false;
}

PerVertexRewrite RewritePerVertexInputs(std::span<const u32> spirv) {
    PerVertexRewrite failure;
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
