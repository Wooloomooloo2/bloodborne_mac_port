// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bbport_toggles.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_metal/mtl_texture.h"
#include "video_core/renderer_metal/vk_to_mtl.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_bind_helper.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore {

using namespace Vulkan;

Common::IncrementalIdProvider<u64> Image::global_image_uid{};

bool LazyTextureUsage() {
    static const bool on = [] {
        const char* env = std::getenv("BB_LAZY_TEXTURE_USAGE");
        const bool lazy = !(env && env[0] == '0');
        // bbport: in the log for reports (launcher: Advanced → Texture compression).
        std::printf("Textures: lossless compression %s\n",
                    lazy ? "on" : "off (BB_LAZY_TEXTURE_USAGE=0)");
        return lazy;
    }();
    return on;
}

namespace {

u32 TexturePixelFormat(vk::Format format) {
    const u32 pixel_format = Metal::VkToMtl::PixelFormat(format);
    if (pixel_format != 0) {
        return pixel_format;
    }
    static std::mutex mutex;
    static std::vector<vk::Format> logged;
    std::scoped_lock lock{mutex};
    if (std::ranges::find(logged, format) == logged.end()) {
        logged.push_back(format);
        LOG_ERROR(Render_Vulkan, "image format {} has no Metal texture format, using RGBA8Unorm",
                  vk::to_string(format));
    }
    DEBUG_ASSERT_MSG(false, "image format without a Metal texture format");
    // The stand-in for formats Metal has no texture format for.
    return Metal::VkToMtl::PixelFormat(vk::Format::eR8G8B8A8Unorm);
}

Metal::TextureType TextureTypeOf(const ImageInfo& info, u32 samples) {
    if (info.type == AmdGpu::ImageType::Color3D) {
        return Metal::TextureType::Tex3D;
    }
    // 1D images are 2D textures one texel high (as the shader side samples them, see
    // mtl_shader.mm texture_1D_as_2D); cubes are already 2D arrays (amdgpu/resource.h).
    const bool array = info.type == AmdGpu::ImageType::Color1DArray ||
                       info.type == AmdGpu::ImageType::Color2DArray ||
                       info.type == AmdGpu::ImageType::Color2DMsaaArray ||
                       info.resources.layers > 1;
    if (samples > 1) {
        return array ? Metal::TextureType::Tex2DMultisampleArray
                     : Metal::TextureType::Tex2DMultisample;
    }
    return array ? Metal::TextureType::Tex2DArray : Metal::TextureType::Tex2D;
}

/// The highest supported count at or below `num_samples` (halving, at least 1).
u32 ClampSamples(u32 num_samples, u32 supported_mask) {
    u32 samples = std::max(num_samples, 1u);
    while (samples > 1 && !(supported_mask & samples)) {
        samples /= 2;
    }
    return samples;
}

} // namespace

Image::Image(Metal::Device& device_, Vulkan::Runtime& runtime_,
             Common::SlotVector<ImageView>& slot_image_views_, const ImageInfo& info_)
    : guest_begin{info_.guest_address}, guest_end{info_.guest_address + info_.guest_size},
      info{info_}, runtime{&runtime_}, slot_image_views{&slot_image_views_}, device{&device_} {
    BbStats::Timer timer{BbStats::t_image_create};
    if (info.pixel_format == vk::Format::eUndefined) {
        return;
    }

    image_uid = global_image_uid.Next();
    mip_hashes.resize(info.resources.levels);
    has_depth = info.props.is_depth;
    has_stencil = info.props.is_depth && info.props.has_stencil;

    supported_samples_mask = 0;
    for (const u32 samples : {1u, 2u, 4u, 8u}) {
        if (samples == 1 || Metal::SupportsSampleCount(device_, samples)) {
            supported_samples_mask |= samples;
        }
    }

    // Usage: block (BC) images are only sampled and filled by buffer copies. Metal cannot write
    // compressed textures, not even through uncompressed views (the 2026-10-11 BC rule). Every
    // other image can be rendered to, written by compute (colour, single-sample: clears and
    // copies by compute need no re-creation) and viewed with another pixel format (Vulkan's
    // eMutableFormat).
    texture_desc.pixel_format = TexturePixelFormat(info.pixel_format);
    texture_desc.width = info.size.width;
    texture_desc.height = info.type == AmdGpu::ImageType::Color1D ||
                                  info.type == AmdGpu::ImageType::Color1DArray
                              ? 1
                              : info.size.height;
    texture_desc.depth = info.type == AmdGpu::ImageType::Color3D ? info.size.depth : 1;
    texture_desc.levels = info.resources.levels;
    texture_desc.layers = info.type == AmdGpu::ImageType::Color3D ? 1 : info.resources.layers;
    texture_desc.render_target = !info.props.is_block;
    texture_desc.shader_write = !info.props.is_block && !info.props.is_depth;
    texture_desc.format_views = !info.props.is_block;
    // bbport: both usages only when a binding needs them (allowed_shader_write).
    allowed_shader_write = texture_desc.shader_write;
    allowed_format_views = texture_desc.format_views;
    if (LazyTextureUsage()) {
        texture_desc.shader_write = false;
        texture_desc.format_views = false;
    }

    backing = &CreateBacking(info.num_samples);
}

Image::~Image() = default;

Image::BackingImage& Image::CreateBacking(u32 num_samples) {
    const u32 samples = ClampSamples(num_samples, supported_samples_mask);
    Metal::TextureDesc desc = texture_desc;
    desc.samples = samples;
    desc.type = TextureTypeOf(info, samples);
    // Multisample textures can be neither written by shaders nor mipmapped.
    desc.shader_write = texture_desc.shader_write && samples == 1;
    if (samples > 1) {
        desc.levels = 1;
        // No usage upgrades of multisample backings (no copies between them): both from the start.
        desc.format_views = allowed_format_views;
    }

    auto& new_backing = backing_images.emplace_back();
    new_backing.num_samples = num_samples;
    new_backing.desc = desc;
    const std::string label = fmt::format(
        "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{}", info.size.width, info.size.height,
        info.size.depth, AmdGpu::NameOf(info.tile_mode), vk::to_string(info.pixel_format),
        info.guest_address, info.guest_size, info.resources.layers, info.resources.levels,
        num_samples);
    new_backing.texture =
        Metal::CreateTexture(*device, desc, label, &new_backing.allocated_size);
    ASSERT_MSG(new_backing.texture, "Failed creating texture for {}", label);
    return new_backing;
}

u32 Image::CurrentUsage() const {
    return (backing && backing->desc.shader_write ? UsageShaderWrite : 0) |
           (backing && backing->desc.format_views ? UsageFormatViews : 0);
}

void Image::ApplyNewUsage(u32 needed) {
    needed &= (allowed_shader_write ? UsageShaderWrite : 0) |
              (allowed_format_views ? UsageFormatViews : 0);
    needed &= ~CurrentUsage();
    if (!needed || !backing || !backing->image_view_ids.empty() || backing_images.size() != 1) {
        return;
    }
    // Both usages (as Runtime::UpgradeUsage): no replacement of the texture later.
    texture_desc.shader_write = allowed_shader_write;
    texture_desc.format_views = allowed_format_views;
    const u32 samples = backing->num_samples;
    backing_images.clear();
    backing = &CreateBacking(samples);
}

u32 Image::NeededUsage(const ImageViewInfo& view_info) const {
    const Metal::TextureDesc& desc = backing->desc;
    u32 needed = 0;
    if (view_info.is_storage && allowed_shader_write && !desc.shader_write && desc.samples <= 1) {
        needed |= UsageShaderWrite;
    }
    if (!allowed_format_views || desc.format_views) {
        return needed;
    }
    // The view ImageView makes (depth and stencil views first). sRGB twins, swizzles and level
    // or layer ranges need no PixelFormatView usage (checked with the Metal API validation);
    // other pixel formats and stencil views of depth textures do.
    if (has_depth && Vulkan::LiverpoolToVK::IsFormatDepthCompatible(view_info.format)) {
        return needed;
    }
    if (has_stencil && Vulkan::LiverpoolToVK::IsFormatStencilCompatible(view_info.format)) {
        const u32 stencil = Metal::StencilViewPixelFormat(desc.pixel_format);
        return needed | (stencil != 0 && stencil != desc.pixel_format ? UsageFormatViews : 0);
    }
    const u32 requested = Metal::VkToMtl::PixelFormat(view_info.format);
    if (requested != 0 && requested != desc.pixel_format &&
        Metal::SafeViewPixelFormat(desc.pixel_format, requested, false) != requested &&
        Metal::SafeViewPixelFormat(desc.pixel_format, requested, true) == requested) {
        needed |= UsageFormatViews;
    }
    return needed;
}

ImageView& Image::FindView(const ImageViewInfo& view_info, bool ensure_guest_samples) {
    if (ensure_guest_samples && backing->num_samples > 1 != info.num_samples > 1) {
        runtime->SetBackingSamples(this, info.num_samples);
    }
    const auto& view_infos = backing->image_view_infos;
    if (backing->last_view < view_infos.size() && view_infos[backing->last_view] == view_info) {
        return (*slot_image_views)[backing->image_view_ids[backing->last_view]];
    }
    const auto it = std::ranges::find(view_infos, view_info);
    if (it != view_infos.end()) {
        backing->last_view = static_cast<u32>(std::distance(view_infos.begin(), it));
        const auto view_id = backing->image_view_ids[backing->last_view];
        return (*slot_image_views)[view_id];
    }
    if (const u32 needed = NeededUsage(view_info)) {
        if (Vulkan::BindHelper::OnHelper()) {
            // Rasterizer::TexturesBindableOnHelper keeps such bindings on the GPU thread.
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) {
                LOG_ERROR(Render_Vulkan, "texture usage upgrade needed on the binding helper");
            }
        } else {
            runtime->UpgradeUsage(this, needed);
            return FindView(view_info, false);
        }
    }
    const auto view_id = slot_image_views->insert(view_info, *this);
    backing->image_view_infos.emplace_back(view_info);
    backing->image_view_ids.emplace_back(view_id);
    return (*slot_image_views)[view_id];
}

} // namespace VideoCore
