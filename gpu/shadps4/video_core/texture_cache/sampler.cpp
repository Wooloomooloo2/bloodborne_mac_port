// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_texture.h"
#include "video_core/renderer_metal/vk_to_mtl.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/texture_cache/sampler.h"

namespace VideoCore {

namespace {

void LogOnce(std::atomic<bool>& logged, const char* what) {
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        LOG_WARNING(Render_Vulkan, "{}", what);
    }
}

} // namespace

Sampler::Sampler(Metal::Device& device, const AmdGpu::Sampler& sampler,
                 const AmdGpu::BorderColorBuffer /*border_color_base*/, const bool is_depth,
                 float extra_lod_bias) {
    using namespace Vulkan;
    using namespace Metal::VkToMtl;
    const Metal::Limits& limits = device.GetLimits();
    const bool anisotropy_enable = AmdGpu::IsAnisoFilter(sampler.xy_mag_filter) ||
                                   AmdGpu::IsAnisoFilter(sampler.xy_min_filter);
    const float max_anisotropy =
        anisotropy_enable ? std::clamp(sampler.MaxAniso(), 1.0f, limits.max_anisotropy) : 1.0f;

    // Metal has no custom border colours. The LOD bias needs macOS 26 (logged by the core).
    const float lod_bias = std::clamp(sampler.LodBias() + extra_lod_bias, -limits.max_lod_bias,
                                      limits.max_lod_bias);
    BorderInfo border = BorderColor(LiverpoolToVK::BorderColor(sampler.border_color_type));
    if (!border.supported) {
        static std::atomic<bool> logged{false};
        LogOnce(logged, "Custom border color is not supported, falling back to black");
        border.color = BorderColor(vk::BorderColor::eFloatOpaqueBlack).color;
    }

    Metal::SamplerDesc desc{};
    desc.min_filter = SamplerFilter(LiverpoolToVK::Filter(sampler.xy_min_filter));
    desc.mag_filter = SamplerFilter(LiverpoolToVK::Filter(sampler.xy_mag_filter));
    desc.min_lod = sampler.MinLod();
    desc.max_lod = sampler.MaxLod();
    desc.lod_bias = lod_bias;
    desc.mip_filter = desc.max_lod <= desc.min_lod && desc.min_lod == 0.0f
                          ? SamplerMipNotMipmapped
                          : MipFilter(LiverpoolToVK::MipFilter(sampler.mip_filter));
    desc.address_u = AddressMode(LiverpoolToVK::ClampMode(sampler.clamp_x));
    desc.address_v = AddressMode(LiverpoolToVK::ClampMode(sampler.clamp_y));
    desc.address_w = AddressMode(LiverpoolToVK::ClampMode(sampler.clamp_z));
    desc.border = border.color;
    desc.max_anisotropy = static_cast<u32>(max_anisotropy);
    // GCN compares per instruction; a plain read of a compare sampler is undefined.
    desc.compare = is_depth;
    desc.compare_function = SamplerCompare(LiverpoolToVK::DepthCompare(sampler.depth_compare_func));
    desc.normalized = true; // Unnormalized coordinates are handled in the shader.
    handle = Metal::CreateSampler(device, desc);
    ASSERT_MSG(handle, "Failed to create sampler");
}

Sampler::~Sampler() = default;

} // namespace VideoCore
