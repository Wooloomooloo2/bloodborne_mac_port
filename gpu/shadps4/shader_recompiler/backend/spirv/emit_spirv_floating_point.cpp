// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

#include <cstdlib>
#include <cstring>

namespace Shader::Backend::SPIRV {

// bbport: NoContraction ("precise") only where results must be bit-identical across shaders:
// positions, so depth prepasses and later passes agree. SPIRV-Cross turns each precise add,
// multiply and FMA into a call to an [[clang::optnone]] helper, which the Metal compiler cannot
// optimize: on every fragment and compute shader that cost the city scene 19.6 -> 12.1 ms of
// GPU time per frame. Default (BB_SPIRV_PRECISE=invariant): positions are declared Invariant
// instead of NoContraction in the vertex stages, compiled with preserveInvariance
// (renderer_metal/mtl_shader.mm BuildLibrary), so Metal keeps positions matching across shaders
// without turning every vertex-stage add and multiply into a helper call: +12% FPS at native
// 1080p on an M3 Pro (FINDINGS 2026-10-19), +15% at 1440p on an M4 Max with matching screenshots
// (FINDINGS 2026-10-21). BB_SPIRV_PRECISE=vertex: NoContraction in every non-fragment,
// non-compute stage (the default until 0.5.3).
// Stages whose positions do not leave through a vertex function keep NoContraction:
// tessellation evaluation (MoltenVK dropped Invariant on post-tessellation vertex functions) and
// the geometry stage and its export stage, which the Metal renderer runs as compute kernels.
// Dropping NoContraction without Invariant broke geometry on MoltenVK (FINDINGS 2026-10-13).
// =all: everywhere; =none: nowhere (experiments). The mode is part of the shader cache version
// (vk_pipeline_serialization.cpp), so switching it rebuilds the cache.
int PreciseMode() {
    static const int mode = [] {
        const char* env = std::getenv("BB_SPIRV_PRECISE");
        return !env                           ? 3
               : !std::strcmp(env, "vertex")    ? 0
               : !std::strcmp(env, "invariant") ? 3
               : !std::strcmp(env, "all")       ? 1
               : !std::strcmp(env, "none")      ? 2
                                                : 3;
    }();
    return mode;
}

bool InvariantPosition() {
    return PreciseMode() == 3;
}

Id Decorate(EmitContext& ctx, IR::Inst* inst, Id op) {
    const bool keeps_precise = ctx.sw_stage == SwStage::TessellationEval ||
                               ctx.sw_stage == SwStage::Geometry || ctx.hw_stage == HwStage::Export;
    if (PreciseMode() == 2 || (PreciseMode() == 3 && !keeps_precise)) {
        return op;
    }
    if (PreciseMode() == 1 ||
        (ctx.sw_stage != SwStage::Fragment && ctx.sw_stage != SwStage::Compute)) {
        ctx.Decorate(op, spv::Decoration::NoContraction);
    }
    return op;
}

Id EmitFPAbs32(EmitContext& ctx, Id value) {
    return ctx.OpFAbs(ctx.F32[1], value);
}

Id EmitFPAbs64(EmitContext& ctx, Id value) {
    return ctx.OpFAbs(ctx.F64[1], value);
}

Id EmitFPAdd32(EmitContext& ctx, IR::Inst* inst, Id a, Id b) {
    return Decorate(ctx, inst, ctx.OpFAdd(ctx.F32[1], a, b));
}

Id EmitFPAdd64(EmitContext& ctx, IR::Inst* inst, Id a, Id b) {
    return Decorate(ctx, inst, ctx.OpFAdd(ctx.F64[1], a, b));
}

Id EmitFPSub32(EmitContext& ctx, IR::Inst* inst, Id a, Id b) {
    return Decorate(ctx, inst, ctx.OpFSub(ctx.F32[1], a, b));
}

Id EmitFPFma32(EmitContext& ctx, IR::Inst* inst, Id a, Id b, Id c) {
    return Decorate(ctx, inst, ctx.OpFma(ctx.F32[1], a, b, c));
}

Id EmitFPFma64(EmitContext& ctx, IR::Inst* inst, Id a, Id b, Id c) {
    return Decorate(ctx, inst, ctx.OpFma(ctx.F64[1], a, b, c));
}

Id EmitFPMax32(EmitContext& ctx, Id a, Id b) {
    return ctx.OpFMax(ctx.F32[1], a, b);
}

Id EmitFPMax64(EmitContext& ctx, Id a, Id b) {
    return ctx.OpFMax(ctx.F64[1], a, b);
}

Id EmitFPMin32(EmitContext& ctx, Id a, Id b) {
    return ctx.OpFMin(ctx.F32[1], a, b);
}

Id EmitFPMin64(EmitContext& ctx, Id a, Id b) {
    return ctx.OpFMin(ctx.F64[1], a, b);
}

Id EmitFPMinTri32(EmitContext& ctx, Id a, Id b, Id c) {
    if (ctx.profile.supports_trinary_minmax) {
        return ctx.OpFMin3AMD(ctx.F32[1], a, b, c);
    }
    return ctx.OpFMin(ctx.F32[1], a, ctx.OpFMin(ctx.F32[1], b, c));
}

Id EmitFPMaxTri32(EmitContext& ctx, Id a, Id b, Id c) {
    if (ctx.profile.supports_trinary_minmax) {
        return ctx.OpFMax3AMD(ctx.F32[1], a, b, c);
    }
    return ctx.OpFMax(ctx.F32[1], a, ctx.OpFMax(ctx.F32[1], b, c));
}

Id EmitFPMedTri32(EmitContext& ctx, Id a, Id b, Id c) {
    if (ctx.profile.supports_trinary_minmax) {
        return ctx.OpFMid3AMD(ctx.F32[1], a, b, c);
    }
    const Id mmx{ctx.OpFMin(ctx.F32[1], ctx.OpFMax(ctx.F32[1], a, b), c)};
    return ctx.OpFMax(ctx.F32[1], ctx.OpFMin(ctx.F32[1], a, b), mmx);
}

Id EmitFPMul32(EmitContext& ctx, IR::Inst* inst, Id a, Id b) {
    return Decorate(ctx, inst, ctx.OpFMul(ctx.F32[1], a, b));
}

Id EmitFPMul64(EmitContext& ctx, IR::Inst* inst, Id a, Id b) {
    return Decorate(ctx, inst, ctx.OpFMul(ctx.F64[1], a, b));
}

Id EmitFPDiv32(EmitContext& ctx, IR::Inst* inst, Id a, Id b) {
    return Decorate(ctx, inst, ctx.OpFDiv(ctx.F32[1], a, b));
}

Id EmitFPDiv64(EmitContext& ctx, IR::Inst* inst, Id a, Id b) {
    return Decorate(ctx, inst, ctx.OpFDiv(ctx.F64[1], a, b));
}

Id EmitFPNeg32(EmitContext& ctx, Id value) {
    return ctx.OpFNegate(ctx.F32[1], value);
}

Id EmitFPNeg64(EmitContext& ctx, Id value) {
    return ctx.OpFNegate(ctx.F64[1], value);
}

Id EmitFPSin(EmitContext& ctx, Id value) {
    return ctx.OpSin(ctx.F32[1], ctx.OpFMul(ctx.F32[1], ctx.pi_x2, value));
}

Id EmitFPCos(EmitContext& ctx, Id value) {
    return ctx.OpCos(ctx.F32[1], ctx.OpFMul(ctx.F32[1], ctx.pi_x2, value));
}

Id EmitFPExp2(EmitContext& ctx, Id value) {
    return ctx.OpExp2(ctx.F32[1], value);
}

Id EmitFPPow(EmitContext& ctx, Id x, Id y) {
    return ctx.OpPow(ctx.F32[1], x, y);
}

Id EmitFPLdexp(EmitContext& ctx, Id value, Id exp) {
    return ctx.OpLdexp(ctx.F32[1], value, exp);
}

Id EmitFPLog2(EmitContext& ctx, Id value) {
    return ctx.OpLog2(ctx.F32[1], value);
}

Id EmitFPRecip32(EmitContext& ctx, Id value) {
    return ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f), value);
}

Id EmitFPRecip64(EmitContext& ctx, Id value) {
    return ctx.OpFDiv(ctx.F64[1], ctx.Constant(ctx.F64[1], f64{1.0}), value);
}

Id EmitFPRecipSqrt32(EmitContext& ctx, Id value) {
    return ctx.OpInverseSqrt(ctx.F32[1], value);
}

Id EmitFPRecipSqrt64(EmitContext& ctx, Id value) {
    return ctx.OpInverseSqrt(ctx.F64[1], value);
}

Id EmitFPSqrt(EmitContext& ctx, Id value) {
    return ctx.OpSqrt(ctx.F32[1], value);
}

// bbport: NaN and Inf tested on the bit pattern. Metal's fast math (our default) assumes neither
// exists and folds isnan/isinf to false, which turned GCN's NaN-safe ops (clamp, legacy min/max,
// v_cmp_class) back into plain ones: a red bloom ball on the hunter's breath in Cainhurst.
static Id MagnitudeBits32(EmitContext& ctx, Id value) {
    return ctx.OpBitwiseAnd(ctx.U32[1], ctx.OpBitcast(ctx.U32[1], value),
                            ctx.ConstU32(0x7fffffffu));
}

Id EmitFPSaturate32(EmitContext& ctx, Id value) {
    const Id zero{ctx.ConstF32(f32{0.0})};
    const Id one{ctx.ConstF32(f32{1.0})};
    // bbport: GCN's clamp modifier turns NaN into 0 (FClamp leaves NaN undefined).
    return ctx.OpSelect(ctx.F32[1], EmitFPIsNan32(ctx, value), zero,
                        ctx.OpFClamp(ctx.F32[1], value, zero, one));
}

Id EmitFPSaturate64(EmitContext& ctx, Id value) {
    const Id zero{ctx.Constant(ctx.F64[1], f64{0.0})};
    const Id one{ctx.Constant(ctx.F64[1], f64{1.0})};
    return ctx.OpFClamp(ctx.F64[1], value, zero, one);
}

Id EmitFPClamp32(EmitContext& ctx, Id value, Id min_value, Id max_value) {
    return ctx.OpFClamp(ctx.F32[1], value, min_value, max_value);
}

Id EmitFPClamp64(EmitContext& ctx, Id value, Id min_value, Id max_value) {
    return ctx.OpFClamp(ctx.F64[1], value, min_value, max_value);
}

Id EmitFPRoundEven32(EmitContext& ctx, Id value) {
    return ctx.OpRoundEven(ctx.F32[1], value);
}

Id EmitFPRoundEven64(EmitContext& ctx, Id value) {
    return ctx.OpRoundEven(ctx.F64[1], value);
}

Id EmitFPFloor32(EmitContext& ctx, Id value) {
    return ctx.OpFloor(ctx.F32[1], value);
}

Id EmitFPFloor64(EmitContext& ctx, Id value) {
    return ctx.OpFloor(ctx.F64[1], value);
}

Id EmitFPCeil32(EmitContext& ctx, Id value) {
    return ctx.OpCeil(ctx.F32[1], value);
}

Id EmitFPCeil64(EmitContext& ctx, Id value) {
    return ctx.OpCeil(ctx.F64[1], value);
}

Id EmitFPTrunc32(EmitContext& ctx, Id value) {
    return ctx.OpTrunc(ctx.F32[1], value);
}

Id EmitFPTrunc64(EmitContext& ctx, Id value) {
    return ctx.OpTrunc(ctx.F64[1], value);
}

Id EmitFPFract32(EmitContext& ctx, Id value) {
    return ctx.OpFract(ctx.F32[1], value);
}

Id EmitFPFract64(EmitContext& ctx, Id value) {
    return ctx.OpFract(ctx.F64[1], value);
}

Id EmitFPFrexpSig32(EmitContext& ctx, Id value) {
    const auto frexp = ctx.OpFrexpStruct(ctx.frexp_result_f32, value);
    return ctx.OpCompositeExtract(ctx.F32[1], frexp, 0);
}

Id EmitFPFrexpSig64(EmitContext& ctx, Id value) {
    const auto frexp = ctx.OpFrexpStruct(ctx.frexp_result_f64, value);
    return ctx.OpCompositeExtract(ctx.F64[1], frexp, 0);
}

Id EmitFPFrexpExp32(EmitContext& ctx, Id value) {
    const auto frexp = ctx.OpFrexpStruct(ctx.frexp_result_f32, value);
    return ctx.OpBitcast(ctx.U32[1], ctx.OpCompositeExtract(ctx.S32[1], frexp, 1));
}

Id EmitFPFrexpExp64(EmitContext& ctx, Id value) {
    const auto frexp = ctx.OpFrexpStruct(ctx.frexp_result_f64, value);
    return ctx.OpBitcast(ctx.U32[1], ctx.OpCompositeExtract(ctx.S32[1], frexp, 1));
}

Id EmitFPOrdEqual16(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFOrdEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPOrdEqual32(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFOrdEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPOrdEqual64(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFOrdEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPUnordEqual16(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFUnordEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPUnordEqual32(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFUnordEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPUnordEqual64(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFUnordEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPOrdNotEqual16(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFOrdNotEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPOrdNotEqual32(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFOrdNotEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPOrdNotEqual64(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFOrdNotEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPUnordNotEqual16(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFUnordNotEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPUnordNotEqual32(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFUnordNotEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPUnordNotEqual64(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFUnordNotEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPOrdLessThan16(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFOrdLessThan(ctx.U1[1], lhs, rhs);
}

Id EmitFPOrdLessThan32(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFOrdLessThan(ctx.U1[1], lhs, rhs);
}

Id EmitFPOrdLessThan64(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFOrdLessThan(ctx.U1[1], lhs, rhs);
}

Id EmitFPUnordLessThan16(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFUnordLessThan(ctx.U1[1], lhs, rhs);
}

Id EmitFPUnordLessThan32(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFUnordLessThan(ctx.U1[1], lhs, rhs);
}

Id EmitFPUnordLessThan64(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFUnordLessThan(ctx.U1[1], lhs, rhs);
}

Id EmitFPOrdGreaterThan16(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFOrdGreaterThan(ctx.U1[1], lhs, rhs);
}

Id EmitFPOrdGreaterThan32(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFOrdGreaterThan(ctx.U1[1], lhs, rhs);
}

Id EmitFPOrdGreaterThan64(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFOrdGreaterThan(ctx.U1[1], lhs, rhs);
}

Id EmitFPUnordGreaterThan16(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFUnordGreaterThan(ctx.U1[1], lhs, rhs);
}

Id EmitFPUnordGreaterThan32(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFUnordGreaterThan(ctx.U1[1], lhs, rhs);
}

Id EmitFPUnordGreaterThan64(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFUnordGreaterThan(ctx.U1[1], lhs, rhs);
}

Id EmitFPOrdLessThanEqual16(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFOrdLessThanEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPOrdLessThanEqual32(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFOrdLessThanEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPOrdLessThanEqual64(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFOrdLessThanEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPUnordLessThanEqual16(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFUnordLessThanEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPUnordLessThanEqual32(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFUnordLessThanEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPUnordLessThanEqual64(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFUnordLessThanEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPOrdGreaterThanEqual16(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFOrdGreaterThanEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPOrdGreaterThanEqual32(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFOrdGreaterThanEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPOrdGreaterThanEqual64(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFOrdGreaterThanEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPUnordGreaterThanEqual16(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFUnordGreaterThanEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPUnordGreaterThanEqual32(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFUnordGreaterThanEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPUnordGreaterThanEqual64(EmitContext& ctx, Id lhs, Id rhs) {
    return ctx.OpFUnordGreaterThanEqual(ctx.U1[1], lhs, rhs);
}

Id EmitFPIsNan16(EmitContext& ctx, Id value) {
    return ctx.OpIsNan(ctx.U1[1], value);
}

Id EmitFPIsNan32(EmitContext& ctx, Id value) {
    return ctx.OpUGreaterThan(ctx.U1[1], MagnitudeBits32(ctx, value), ctx.ConstU32(0x7f800000u));
}

Id EmitFPIsNan64(EmitContext& ctx, Id value) {
    return ctx.OpIsNan(ctx.U1[1], value);
}

Id EmitFPIsInf32(EmitContext& ctx, Id value) {
    return ctx.OpIEqual(ctx.U1[1], MagnitudeBits32(ctx, value), ctx.ConstU32(0x7f800000u));
}

Id EmitFPIsInf64(EmitContext& ctx, Id value) {
    return ctx.OpIsInf(ctx.U1[1], value);
}

void EmitFPCmpClass32(EmitContext&) {
    UNREACHABLE();
}

} // namespace Shader::Backend::SPIRV
