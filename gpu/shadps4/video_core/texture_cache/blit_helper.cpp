// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <string>
#include <fmt/format.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_metal/mtl_blit_shaders.h"
#include "video_core/renderer_metal/mtl_depth_stencil.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_render_pipeline.h"
#include "video_core/renderer_metal/mtl_texture.h"
#include "video_core/renderer_metal/vk_to_mtl.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"

#include <vulkan/vulkan_format_traits.hpp>

namespace VideoCore {

namespace {

constexpr std::array<u8, 4> IdentitySwizzle{
    Metal::VkToMtl::SwizzleRed, Metal::VkToMtl::SwizzleGreen, Metal::VkToMtl::SwizzleBlue,
    Metal::VkToMtl::SwizzleAlpha};

/// Which flavour of ms_image_blit reads and writes the format: 0 float, 1 uint, 2 int.
u32 NumericFlavour(vk::Format format) {
    const char* numeric = vk::componentNumericFormat(format, 0);
    if (std::strcmp(numeric, "UINT") == 0) {
        return 1;
    }
    if (std::strcmp(numeric, "SINT") == 0) {
        return 2;
    }
    return 0;
}

/// Records one fullscreen-triangle draw as its own render pass, with `pipeline` reading `source`
/// at fragment texture 0. Render state the rasterizer may have left (culling, depth bias) is
/// reset, and the scheduler's dynamic state is invalidated so the next draw sets its own again.
void DrawFullscreen(Vulkan::Scheduler& scheduler, const Metal::RenderPassDesc& pass,
                    void* pipeline, void* depth_stencil, Metal::Object source) {
    scheduler.EndRendering();
    scheduler.Record([pass, pipeline, depth_stencil,
                      source = std::move(source)](Metal::Encoder& e) mutable {
        void* const texture = source.Get();
        e.KeepAlive(std::move(source));
        e.BeginRenderPass(pass);
        e.SetRenderPipeline(pipeline, Metal::Access::None); // textures only
        e.SetDepthStencil(depth_stencil);
        e.SetCullMode(Metal::CullMode::None);
        e.SetDepthBias(0.0f, 0.0f, 0.0f);
        e.SetViewport({0.0f, 0.0f, float(pass.width), float(pass.height), 0.0f, 1.0f});
        e.SetScissor({0, 0, pass.width, pass.height});
        e.SetTexture(Metal::Stage::Fragment, 0, texture);
        e.Draw(Metal::Primitive::Triangle, 3);
        e.EndRenderPass();
    });
    scheduler.GetDynamicState().Invalidate();
}

} // namespace

BlitHelper::BlitHelper(Metal::Device& device_, Vulkan::Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_},
      library{Metal::CompileLibrary(device_, Metal::BlitShaders::Source, "Blit helper")} {
    Metal::DepthStencilKey key{};
    key.depth_test = true;
    key.depth_write = true;
    key.depth_compare = 7; // VK_COMPARE_OP_ALWAYS
    depth_write_state = scheduler.GetDepthStencilCache().Get(key);
    no_depth_state = scheduler.GetDepthStencilCache().Get(Metal::DepthStencilKey{});
}

BlitHelper::~BlitHelper() = default;

void* BlitHelper::GetPipeline(const PipelineKey& key) {
    std::scoped_lock lock{pipeline_mutex};
    const auto it =
        std::ranges::find(pipelines, key, &std::pair<PipelineKey, Metal::Object>::first);
    if (it != pipelines.end()) {
        return it->second.Get();
    }
    const Metal::RenderPipelineDesc desc{
        .library = library.Get(),
        .vertex_function = Metal::BlitShaders::FullscreenTriangle,
        .fragment_function = key.fragment,
        .color_format = key.color_format,
        .depth_format = key.depth_format,
        .stencil_format = key.stencil_format,
        .samples = key.samples,
    };
    const std::string label = fmt::format("{} {}x", key.fragment, key.samples);
    pipelines.emplace_back(key, Metal::CreateRenderPipeline(device, desc, label));
    return pipelines.back().second.Get();
}

void BlitHelper::ReinterpretColorAsMsDepth(u32 width, u32 height, u32 num_samples,
                                           vk::Format src_pixel_format, vk::Format dst_pixel_format,
                                           void* source, void* dest) {
    // The textures' own formats and sample count: the guest's may have been mapped or clamped.
    const u32 src_format = Metal::TexturePixelFormat(source);
    const u32 dst_format = Metal::TexturePixelFormat(dest);
    const u32 samples = Metal::TextureSampleCount(dest);
    if (samples != num_samples || src_format != Metal::VkToMtl::PixelFormat(src_pixel_format) ||
        dst_format != Metal::VkToMtl::PixelFormat(dst_pixel_format)) {
        LOG_DEBUG(Render, "Colour to MS depth: textures {} -> {} x{} (guest {} -> {} x{})",
                  src_format, dst_format, samples, vk::to_string(src_pixel_format),
                  vk::to_string(dst_pixel_format), num_samples);
    }
    const bool has_stencil = Metal::VkToMtl::HasStencilPixelFormat(dst_format);

    // A 2D view: the source may be an array texture or have mips.
    Metal::Object src_view = Metal::CreateTextureView(
        source, src_format, Metal::TextureType::Tex2D, 0, 1, 0, 1, IdentitySwizzle);
    ASSERT_MSG(src_view, "Colour to MS depth: no 2D view of the colour texture");

    void* const pipeline = GetPipeline({
        .fragment = Metal::BlitShaders::ColorToMsDepth,
        .color_format = 0,
        .depth_format = dst_format,
        .stencil_format = has_stencil ? dst_format : 0,
        .samples = samples,
    });

    Metal::RenderPassDesc pass{};
    pass.depth.texture = dest;
    pass.depth.has_stencil = has_stencil; // the stencil is loaded and stored (kept)
    pass.depth.clear_depth = true;
    pass.width = width;
    pass.height = height;
    DrawFullscreen(scheduler, pass, pipeline, depth_write_state, std::move(src_view));
}

void BlitHelper::CopyBetweenMsImages(u32 width, u32 height, u32 num_samples,
                                     vk::Format pixel_format, bool src_msaa, void* source,
                                     void* dest) {
    // The textures' own sample counts: the guest's may have been clamped.
    const u32 samples = Metal::TextureSampleCount(dest);
    const u32 src_samples = Metal::TextureSampleCount(source);
    if (samples != num_samples || (src_samples > 1) != src_msaa) {
        LOG_DEBUG(Render, "MS image copy: textures have {} -> {} samples (guest {}, MSAA source {})",
                  src_samples, samples, num_samples, src_msaa);
    }
    const bool msaa_source = src_samples > 1;

    Metal::Object src_view = Metal::CreateTextureView(
        source, Metal::TexturePixelFormat(source),
        msaa_source ? Metal::TextureType::Tex2DMultisample : Metal::TextureType::Tex2D, 0, 1, 0, 1,
        IdentitySwizzle);
    ASSERT_MSG(src_view, "MS image copy: no 2D view of the source texture");

    const u32 flavour = NumericFlavour(pixel_format);
    void* const pipeline = GetPipeline({
        .fragment = msaa_source ? Metal::BlitShaders::MsImageBlitMsaa[flavour]
                                : Metal::BlitShaders::MsImageBlit[flavour],
        .color_format = Metal::TexturePixelFormat(dest),
        .depth_format = 0,
        .stencil_format = 0,
        .samples = samples,
    });

    Metal::RenderPassDesc pass{};
    pass.colors[0].texture = dest;
    pass.colors[0].clear = true;
    pass.width = width;
    pass.height = height;
    DrawFullscreen(scheduler, pass, pipeline, no_depth_state, std::move(src_view));
}

} // namespace VideoCore
