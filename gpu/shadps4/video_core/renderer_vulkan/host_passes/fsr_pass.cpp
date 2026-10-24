//  SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
//  SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "video_core/host_shaders/fsr_easu_comp.h"
#include "video_core/host_shaders/fsr_rcas_comp.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_texture.h"
#include "video_core/renderer_metal/vk_to_mtl.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#define A_CPU
#include "core/debug_state.h"
#include "video_core/host_shaders/fsr/ffx_a.h"
#include "video_core/host_shaders/fsr/ffx_fsr1.h"

typedef u32 uvec4[4];

struct FSRConstants {
    uvec4 Const0;
    uvec4 Const1;
    uvec4 Const2;
    uvec4 Const3;
    uvec4 Sample;
};

namespace Vulkan::HostPasses {

FsrPass::FsrPass(Metal::Device& device_, u32 num_images)
    : device{device_}, easu{[&] {
          ASSERT(num_images > 0); // before anything is built or allocated
          return Metal::BuildHostKernel(device_, FSR_EASU_COMP, {}, "fsr.comp [EASU]");
      }()},
      rcas{Metal::BuildHostKernel(device_, FSR_RCAS_COMP, {}, "fsr.comp [RCAS]")},
      images(num_images) {
    // Linear, clamp to edge, no mips.
    sampler = Metal::CreateSampler(
        device, Metal::SamplerDesc{
                    .min_filter = Metal::VkToMtl::SamplerFilter(vk::Filter::eLinear),
                    .mag_filter = Metal::VkToMtl::SamplerFilter(vk::Filter::eLinear),
                    .mip_filter = Metal::VkToMtl::SamplerMipNotMipmapped,
                    .address_u = Metal::VkToMtl::AddressMode(vk::SamplerAddressMode::eClampToEdge),
                    .address_v = Metal::VkToMtl::AddressMode(vk::SamplerAddressMode::eClampToEdge),
                    .address_w = Metal::VkToMtl::AddressMode(vk::SamplerAddressMode::eClampToEdge),
                    .max_lod = 1000.0f,
                });
    ASSERT_MSG(sampler, "FSR: no sampler");
}

FsrPass::~FsrPass() = default;

bool FsrPass::Applies(const Settings& settings, Size2 input, Size2 output) {
    return settings.enable && (input.width < output.width || input.height < output.height);
}

FsrPass::Targets FsrPass::Acquire(Size2 output) {
    if (output != cur_size) {
        // The old textures are released here; a recording that still uses them holds its own
        // reference until its command buffer completes.
        cur_size = output;
        for (Img& img : images) {
            img = {};
        }
    }
    Img& img = images[cur_image];
    if (++cur_image >= images.size()) {
        cur_image = 0;
    }
    if (!img.output) {
        const Metal::TextureDesc desc{
            .pixel_format = Metal::VkToMtl::PixelFormat(vk::Format::eR16G16B16A16Sfloat),
            .width = output.width,
            .height = output.height,
            .shader_write = true,
        };
        img.intermediary = Metal::CreateTexture(device, desc, "FSR Intermediary Image", nullptr);
        img.output = Metal::CreateTexture(device, desc, "FSR Output Image", nullptr);
        ASSERT_MSG(img.intermediary && img.output, "FSR: no {}x{} textures", output.width,
                   output.height);
    }
    return {Metal::Object::Retain(img.intermediary.Get()), Metal::Object::Retain(img.output.Get())};
}

void FsrPass::Encode(Metal::Encoder& e, void* input, Size2 input_size, Size2 output_size,
                     const Settings& settings, bool hdr, void* intermediary, void* output) const {
    constexpr u32 thread_group_work_region_dim = 16;
    const Metal::Size3 groups{
        (output_size.width + (thread_group_work_region_dim - 1)) / thread_group_work_region_dim,
        (output_size.height + (thread_group_work_region_dim - 1)) / thread_group_work_region_dim,
        1};
    const auto run = [&](const Metal::HostKernel& kernel, void* in, void* out,
                         const FSRConstants& consts) {
        e.SetComputePipeline(kernel.pipeline.Get(), Metal::Access::None); // textures only
        e.SetTexture(Metal::Stage::Compute, kernel.texture_slot[0], in);
        e.SetTexture(Metal::Stage::Compute, kernel.texture_slot[1], out);
        e.SetSampler(Metal::Stage::Compute, kernel.sampler_slot[2], sampler.Get());
        e.SetBytes(Metal::Stage::Compute, Metal::kPushConstantBuffer, &consts, sizeof(consts));
        e.Dispatch(groups, {kernel.local_size[0], kernel.local_size[1], kernel.local_size[2]});
    };

    FSRConstants consts{};
    FsrEasuCon(reinterpret_cast<AU1*>(&consts.Const0), reinterpret_cast<AU1*>(&consts.Const1),
               reinterpret_cast<AU1*>(&consts.Const2), reinterpret_cast<AU1*>(&consts.Const3),
               static_cast<AF1>(input_size.width), static_cast<AF1>(input_size.height),
               static_cast<AF1>(input_size.width), static_cast<AF1>(input_size.height),
               static_cast<AF1>(output_size.width), static_cast<AF1>(output_size.height));
    consts.Sample[0] = hdr && !settings.use_rcas ? 1 : 0;

    if (settings.use_rcas) {
        run(easu, input, intermediary, consts);
        consts = {};
        FsrRcasCon(reinterpret_cast<AU1*>(&consts.Const0), settings.rcas_attenuation);
        consts.Sample[0] = hdr ? 1 : 0;
        run(rcas, intermediary, output, consts);
    } else {
        run(easu, input, output, consts);
    }
}

void* FsrPass::Render(Metal::Encoder& e, void* input, Size2 input_size, Size2 output_size,
                      const Settings& settings, bool hdr) {
    if (!Applies(settings, input_size, output_size)) {
        DebugState.is_using_fsr = false;
        return input;
    }
    DebugState.is_using_fsr = true;
    Targets targets = Acquire(output_size);
    void* const intermediary = targets.intermediary.Get();
    void* const output = targets.output.Get();
    Encode(e, input, input_size, output_size, settings, hdr, intermediary, output);
    e.KeepAlive(std::move(targets.intermediary));
    e.KeepAlive(std::move(targets.output));
    return output;
}

Metal::Object FsrPass::Render(Scheduler& scheduler, Metal::Object input, Size2 input_size,
                              Size2 output_size, const Settings& settings, bool hdr) {
    if (!Applies(settings, input_size, output_size)) {
        DebugState.is_using_fsr = false;
        return input;
    }
    DebugState.is_using_fsr = true;
    Targets targets = Acquire(output_size);
    Metal::Object result = Metal::Object::Retain(targets.output.Get());
    scheduler.EndRendering();
    scheduler.Record([this, input = std::move(input), targets = std::move(targets), input_size,
                      output_size, settings, hdr](Metal::Encoder& e) mutable {
        Encode(e, input.Get(), input_size, output_size, settings, hdr, targets.intermediary.Get(),
               targets.output.Get());
        e.KeepAlive(std::move(input));
        e.KeepAlive(std::move(targets.intermediary));
        e.KeepAlive(std::move(targets.output));
    });
    // The guest pipelines' dynamic state must be emitted again after these pipelines.
    scheduler.GetDynamicState().Invalidate();
    return result;
}

} // namespace Vulkan::HostPasses
