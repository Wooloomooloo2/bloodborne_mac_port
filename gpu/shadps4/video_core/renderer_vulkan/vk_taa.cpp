// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_taa.h"

#include "common/assert.h"
#include "video_core/host_shaders/taa_comp.h"
#include "video_core/host_shaders/taa_sharpen_comp.h"
#include "video_core/host_shaders/taa_sharpen_ldr_comp.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_shader.h"
#include "video_core/renderer_metal/mtl_texture.h"
#include "video_core/renderer_metal/vk_to_mtl.h"

namespace Vulkan::Taa {

namespace {

void Dispatch2D(Metal::Encoder& e, const Metal::HostKernel& kernel, u32 width, u32 height) {
    e.Dispatch({(width + kernel.local_size[0] - 1) / kernel.local_size[0],
                (height + kernel.local_size[1] - 1) / kernel.local_size[1], 1},
               {kernel.local_size[0], kernel.local_size[1], kernel.local_size[2]});
}

} // namespace

Kernels Build(Metal::Device& device, std::span<const u32> taa_spirv) {
    Kernels kernels;
    kernels.taa = Metal::BuildHostKernel(device, taa_spirv.empty() ? std::span<const u32>{TAA_COMP}
                                                                   : taa_spirv,
                                         {}, "taa");
    kernels.sharpen = Metal::BuildHostKernel(device, TAA_SHARPEN_COMP, {}, "taa_sharpen");
    kernels.sharpen_ldr =
        Metal::BuildHostKernel(device, TAA_SHARPEN_LDR_COMP, {}, "taa_sharpen_ldr");
    using namespace Metal::VkToMtl;
    kernels.sampler = Metal::CreateSampler(
        device, Metal::SamplerDesc{.min_filter = SamplerFilter(vk::Filter::eNearest),
                                   .mag_filter = SamplerFilter(vk::Filter::eNearest),
                                   .mip_filter = SamplerMipNotMipmapped,
                                   .address_u = AddressMode(vk::SamplerAddressMode::eClampToEdge),
                                   .address_v = AddressMode(vk::SamplerAddressMode::eClampToEdge),
                                   .address_w = AddressMode(vk::SamplerAddressMode::eClampToEdge)});
    ASSERT_MSG(kernels.sampler, "TAA: no sampler");
    return kernels;
}

void Record(Metal::Encoder& encoder, const Kernels& kernels, const Textures& textures,
            const Params& params, u32 width, u32 height) {
    const Metal::HostKernel& kernel = kernels.taa;
    encoder.SetComputePipeline(kernel.pipeline.Get(), Metal::Access::None); // textures only
    // Bindings 0-3: combined image samplers; 4-6: storage images.
    const std::array<void*, 7> bound{textures.color,   textures.depth,        textures.motion,
                                     textures.history, textures.result,       textures.next_history,
                                     textures.diagnostic};
    for (u32 b = 0; b < bound.size(); ++b) {
        encoder.SetTexture(Metal::Stage::Compute, kernel.texture_slot[b], bound[b]);
        if (b < 4) {
            encoder.SetSampler(Metal::Stage::Compute, kernel.sampler_slot[b],
                               kernels.sampler.Get());
        }
    }
    encoder.SetBytes(Metal::Stage::Compute, Metal::kPushConstantBuffer, &params, sizeof(params));
    Dispatch2D(encoder, kernel, width, height);
}

void RecordSharpen(Metal::Encoder& encoder, const Metal::HostKernel& kernel, void* resolved,
                   void* output, float strength, u32 width, u32 height) {
    encoder.SetComputePipeline(kernel.pipeline.Get(), Metal::Access::None); // textures only
    encoder.SetTexture(Metal::Stage::Compute, kernel.texture_slot[0], resolved);
    encoder.SetTexture(Metal::Stage::Compute, kernel.texture_slot[1], output);
    encoder.SetBytes(Metal::Stage::Compute, Metal::kPushConstantBuffer, &strength,
                     sizeof(strength));
    Dispatch2D(encoder, kernel, width, height);
}

} // namespace Vulkan::Taa
