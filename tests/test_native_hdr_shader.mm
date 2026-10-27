// SPDX-License-Identifier: GPL-2.0-or-later
// Admission and export preservation through the production IR adapter and SPIR-V backend.
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/native_hdr.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_pipeline_build.h"
#include "video_core/renderer_metal/mtl_render_pipeline.h"
#include "video_core/renderer_metal/mtl_shader.h"
#include "video_core/renderer_metal/mtl_spirv_inputs.h"
#import <Metal/Metal.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace Shader;
void UiCase(Metal::Device& device) {
    Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Fragment, SwStage::Fragment);
    runtime.hw.fs.color_buffers[0].data_format = AmdGpu::DataFormat::Format16_16_16_16;
    runtime.hw.fs.color_buffers[0].num_format = AmdGpu::NumberFormat::Float;
    runtime.hw.fs.color_buffers[0].swizzle = AmdGpu::IdentityMapping;
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    for (u32 c = 0; c < 4; ++c)
        ir.SetAttribute(IR::Attribute::RenderTarget0, ir.Imm32(0.125f * (c + 1)), c);
    ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    program.syntax_list.push_back(
        {.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    Optimization::CollectShaderInfoPass(program, profile);
    Backend::Bindings bindings{};
    auto spv = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
    const auto original = spv;
    Metal::ShaderModule module(spv, 0x5549);
    const auto* ui = module.NativeHdrUi();
    assert(ui && ui->OutputMask() == 7 && module.OutputMask() == 1);
    assert(ui == module.NativeHdrUi());
    assert(spv == original &&
           std::equal(module.Spirv().begin(), module.Spirv().end(), original.begin()));
    assert(!Metal::DuplicateFragmentOutput(ui->Spirv()).ok); // never overwrite other MRTs
    assert(!Metal::DuplicateFragmentOutput({}).ok);
    char cache[] = "/tmp/native-hdr-ui-XXXXXX";
    assert(mkdtemp(cache));
    Metal::ShaderCompiler compiler(device, cache);
    const auto result = compiler.Compile(ui->Spirv(), ui->Hash(), "native_hdr_ui_mrt");
    assert(result.status == Metal::CompileStatus::Ok && result.shader);
    // Read the production rewritten shader's three MRT exports. Different clear
    // colours and an additive blend prove each attachment accumulates independently.
    auto library = Metal::CompileLibrary(device, R"msl(
        #include <metal_stdlib>
        using namespace metal;
        vertex float4 triangle(uint i [[vertex_id]]) {
            float2 p = float2(float((i & 1u) << 2u), float((i & 2u) << 1u));
            return float4(p - 1.0f, 0.0f, 1.0f);
        }
    )msl",
                                         "HDR UI MRT fixture");
    id<MTLDevice> native = (__bridge id<MTLDevice>)device.NativeDevice();
    auto desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = [(__bridge id<MTLLibrary>)library.Get() newFunctionWithName:@"triangle"];
    desc.fragmentFunction = (__bridge id<MTLFunction>)result.shader->function;
    auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
    id<MTLTexture> images[3];
    for (u32 i = 0; i < 3; ++i) {
        auto texture =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                               width:4
                                                              height:4
                                                           mipmapped:NO];
        texture.storageMode = MTLStorageModeShared;
        texture.usage = MTLTextureUsageRenderTarget;
        images[i] = [native newTextureWithDescriptor:texture];
        auto attachment = pass.colorAttachments[i];
        attachment.texture = images[i];
        attachment.loadAction = MTLLoadActionClear;
        attachment.storeAction = MTLStoreActionStore;
        attachment.clearColor = MTLClearColorMake(i * 0.25, i * 0.25, i * 0.25, 0);
        auto blend = desc.colorAttachments[i];
        blend.pixelFormat = MTLPixelFormatRGBA16Float;
        blend.blendingEnabled = YES;
        blend.sourceRGBBlendFactor = MTLBlendFactorOne;
        blend.destinationRGBBlendFactor = MTLBlendFactorOne;
        blend.sourceAlphaBlendFactor = MTLBlendFactorOne;
        blend.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    }
    NSError* error = nil;
    auto pso = [native newRenderPipelineStateWithDescriptor:desc error:&error];
    assert(pso);
    auto queue = [native newCommandQueue];
    auto command = [queue commandBuffer];
    auto encoder = [command renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pso];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    assert(command.status == MTLCommandBufferStatusCompleted);
    for (u32 i = 0; i < 3; ++i) {
        __fp16 pixel[4]{};
        [images[i] getBytes:pixel
                bytesPerRow:8
                 fromRegion:MTLRegionMake2D(2, 2, 1, 1)
                mipmapLevel:0];
        for (u32 c = 0; c < 3; ++c)
            assert(std::abs(float(pixel[c]) - (i * 0.25f + 0.125f * (c + 1))) < 0.001f);
        assert(float(pixel[3]) == 0.5f);
    }
    std::puts("PASS: one-pass HDR UI exports compile and preserve the original SDR module");
}
void Case(u64 hash, bool hdr, bool missing_bloom, bool extra_export, Metal::Device& device,
          bool linear_prologue = false) {
    Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;
    info.pgm_hash = hash;
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Fragment, SwStage::Fragment);
    for (u32 i = 0; i < (hdr ? 3u : 1u); ++i) {
        runtime.hw.fs.color_buffers[i].data_format = AmdGpu::DataFormat::Format16_16_16_16;
        runtime.hw.fs.color_buffers[i].num_format = AmdGpu::NumberFormat::Float;
        runtime.hw.fs.color_buffers[i].swizzle = AmdGpu::IdentityMapping;
    }
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    std::vector<IR::F32> colors;
    for (u32 i = 0; i < 3; ++i) {
        auto x =
            IR::F32{ir.FPMul(ir.GetAttribute(IR::Attribute::FragCoord, 0), ir.Imm32(float(i + 1)))};
        auto compressed = ir.FPExp2(IR::F32{ir.FPMul(IR::F32{ir.FPNeg(x)}, ir.Imm32(1.44269502f))});
        auto raw_bloom = IR::F32{
            ir.FPMul(ir.GetAttribute(IR::Attribute::FragCoord, 1), ir.Imm32(0.1f * (i + 1)))};
        auto bloom = missing_bloom && i == 2 ? raw_bloom : IR::F32{ir.FPSaturate(raw_bloom)};
        auto tone = IR::F32{ir.FPAdd(compressed, bloom)};
        auto gamma = ir.FPExp2(IR::F32{ir.FPMul(ir.FPLog2(tone), ir.Imm32(0.45454545f))});
        colors.push_back(IR::F32{ir.FPAdd(ir.Imm32(0.01f), gamma)});
    }
    if (extra_export)
        ir.SetAttribute(IR::Attribute::RenderTarget1, ir.Imm32(0.0f), 0);
    for (u32 i = 0; i < 3; ++i)
        ir.SetAttribute(IR::Attribute::RenderTarget0, colors[i], i);
    ir.SetAttribute(IR::Attribute::RenderTarget0, ir.Imm32(1.0f), 3);
    ir.Epilogue();
    IR::Program program(info);
    IR::Block prologue(pool);
    if (linear_prologue) {
        program.blocks.push_back(&prologue);
        program.syntax_list.push_back(
            {.data = {.block = &prologue}, .type = IR::AbstractSyntaxNode::Type::Block});
    }
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    program.syntax_list.push_back(
        {.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    const size_t before = block.Instructions().size();
    const bool expected = NativeHdrToneShader(hash) && hdr && !missing_bloom && !extra_export;
    assert(ExportNativeHdr(program, runtime) == expected);
    u32 original = 0, additional = 0;
    for (const auto& inst : block) {
        if (inst.GetOpcode() != IR::Opcode::SetAttribute)
            continue;
        if (inst.Arg(0).Attribute() == IR::Attribute::RenderTarget0) {
            const auto component = inst.Arg(2).U32();
            if (component < 3)
                assert(inst.Arg(1) == colors[component]);
            ++original;
        } else
            ++additional;
    }
    assert(original == 4);
    if (!expected) {
        assert(block.Instructions().size() == before);
        return;
    }
    assert(additional == 8);
    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = true;
    Optimization::CollectShaderInfoPass(program, profile);
    assert(info.mrt_mask == 7);
    Backend::Bindings bindings{};
    auto spv = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
    // Module construction is also the warm-cache path: exports must be recovered
    // from the selected binary, independently of shared guest shader metadata.
    Metal::ShaderModule module(spv, hash);
    assert(module.OutputMask() == 7);
    char cache[] = "/tmp/native-hdr-module-XXXXXX";
    assert(mkdtemp(cache));
    Metal::ShaderCompiler compiler(device, cache);
    const auto result = compiler.Compile(spv, hash ^ 0x4844520000000000ull, "native_hdr_exports");
    assert(result.status == Metal::CompileStatus::Ok && result.shader);
}
int main() {
    Metal::Device device;
    UiCase(device);
    for (auto hash : {u64(0x5c02eaf8), u64(0x5e67d866)}) {
        Case(hash, true, false, false, device);
        Case(hash, true, false, false, device, true);
        Case(hash, false, false, false, device);
        Case(hash, true, true, false, device);
        Case(hash, true, false, true, device);
    }
    Case(0x12345678, true, false, false, device);
    std::puts(
        "PASS: native HDR exports preserve SDR, reject changed shaders, and compile to Metal");
}
