// SPDX-License-Identifier: GPL-2.0-or-later
#include "shader_recompiler/native_hdr.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace Shader {
namespace {
bool DependsOnExp(IR::Value value, unsigned depth = 0) {
    const auto* inst = value.TryInst();
    if (!inst || depth > 48)
        return depth > 48;
    if (inst->GetOpcode() == IR::Opcode::FPExp2)
        return true;
    for (size_t i = 0; i < inst->NumArgs(); ++i)
        if (DependsOnExp(inst->Arg(i), depth + 1))
            return true;
    return false;
}
bool Log2E(IR::Value v) {
    return v.IsImmediate() && v.Type() == IR::Type::F32 && std::abs(v.F32() - 1.44269502f) < 1e-6f;
}
bool NoiseTerms(IR::Value v, std::vector<IR::Value>& terms, u32& gamma_count, unsigned depth = 0) {
    if (depth > 8)
        return false;
    const auto* inst = v.TryInst();
    if (inst && inst->GetOpcode() == IR::Opcode::FPAdd32)
        return NoiseTerms(inst->Arg(0), terms, gamma_count, depth + 1) &&
               NoiseTerms(inst->Arg(1), terms, gamma_count, depth + 1);
    if (inst && inst->GetOpcode() == IR::Opcode::FPExp2)
        ++gamma_count;
    else
        terms.push_back(v);
    return true;
}
} // namespace
bool ExportNativeHdr(IR::Program& program, const RuntimeInfo& runtime) {
    if (program.info.sw_stage != SwStage::Fragment || !NativeHdrToneShader(program.info.pgm_hash) ||
        runtime.hw.fs.color_buffers[1].data_format != AmdGpu::DataFormat::Format16_16_16_16 ||
        runtime.hw.fs.color_buffers[2].data_format != AmdGpu::DataFormat::Format16_16_16_16)
        return false;
    // A linear body plus a prologue/return block is normal. Reject conditionals/loops,
    // and require all colour stores and their HDR expressions in one body block.
    for (const auto& node : program.syntax_list)
        if (node.type != IR::AbstractSyntaxNode::Type::Block &&
            node.type != IR::AbstractSyntaxNode::Type::Return)
            return false;
    IR::Block* body = nullptr;
    for (auto* candidate : program.blocks) {
        for (const auto& inst : *candidate) {
            if (inst.GetOpcode() != IR::Opcode::SetAttribute)
                continue;
            if (body && body != candidate)
                return false;
            body = candidate;
        }
    }
    if (!body)
        return false;
    auto& block = *body;
    std::vector<IR::Value> scene, bloom;
    IR::Inst* first_export = nullptr;
    unsigned mask = 0;
    for (auto& inst : block) {
        if (inst.GetOpcode() == IR::Opcode::SetAttribute) {
            if (inst.Arg(0).Attribute() != IR::Attribute::RenderTarget0)
                return false;
            if (!first_export)
                first_export = &inst;
            mask |= 1u << inst.Arg(2).U32();
        }
        if (first_export)
            continue;
        if (inst.GetOpcode() == IR::Opcode::FPExp2) {
            auto* multiply = inst.Arg(0).TryInst();
            if (multiply && multiply->GetOpcode() == IR::Opcode::FPMul32 &&
                (Log2E(multiply->Arg(0)) || Log2E(multiply->Arg(1))))
                scene.push_back(inst.Arg(0));
        }
        if (inst.GetOpcode() == IR::Opcode::FPSaturate32) {
            auto* input = inst.Arg(0).TryInst();
            if (input && input->GetOpcode() == IR::Opcode::FPMul32 && !DependsOnExp(inst.Arg(0)))
                bloom.push_back(inst.Arg(0));
        }
    }
    if (!first_export || mask != 15 || scene.size() != 3 || bloom.size() != 3) {
        std::printf("HDR adapter %08x: refused scene=%zu bloom=%zu mask=%x\n",
                    u32(program.info.pgm_hash), scene.size(), bloom.size(), mask);
        return false;
    }
    std::vector<IR::Value> noise_terms;
    u32 gamma_count = 0;
    if (!NoiseTerms(first_export->Arg(1), noise_terms, gamma_count) || gamma_count != 1 ||
        noise_terms.size() > 3) {
        std::printf("HDR adapter %08x: refused gamma/noise pattern (%u/%zu)\n",
                    u32(program.info.pgm_hash), gamma_count, noise_terms.size());
        return false;
    }
    IR::IREmitter ir{block, std::find_if(block.begin(), block.end(),
                                         [&](const auto& inst) { return &inst == first_export; })};
    for (u32 channel = 0; channel < 3; ++channel) {
        const u32 index = program.info.pgm_hash == 0x5c02eaf8 ? 2 - channel : channel;
        // exp2(-x * log2(e)) is the game's SDR shoulder. Export x BEFORE that
        // compression and export bloom BEFORE its [0,1] clamp; neither loses peaks.
        ir.SetAttribute(IR::Attribute::RenderTarget1,
                        IR::F32{ir.FPMul(IR::F32{scene[index]}, ir.Imm32(-0.69314718f))}, channel);
        ir.SetAttribute(IR::Attribute::RenderTarget2, IR::F32{bloom[index]}, channel);
    }
    auto noise = ir.Imm32(0.0f);
    for (const auto& term : noise_terms)
        noise = IR::F32{ir.FPAdd(noise, IR::F32{term})};
    ir.SetAttribute(IR::Attribute::RenderTarget1, noise, 3);
    ir.SetAttribute(IR::Attribute::RenderTarget2, ir.Imm32(1.0f), 3);
    program.info.mrt_mask = 7u;
    std::printf("HDR adapter %08x: exported exposed lighting and unclipped bloom\n",
                u32(program.info.pgm_hash));
    return true;
}
} // namespace Shader
