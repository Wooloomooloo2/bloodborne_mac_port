// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_metal/mtl_texture.h"
#include "video_core/renderer_metal/vk_to_mtl.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"

#include <algorithm>
#include <atomic>
#include <magic_enum/magic_enum.hpp>

namespace VideoCore {

namespace {

bool IsArrayViewType(AmdGpu::ImageType type) {
    return type == AmdGpu::ImageType::Color1DArray || type == AmdGpu::ImageType::Color2DArray ||
           type == AmdGpu::ImageType::Color2DMsaaArray;
}

/// The Metal texture type of a view: 1D views are 2D (1D images are 2D textures one texel high),
/// multisampled when the backing texture is, and 3D for every view of a 3D texture (Metal has no
/// 2D views of 3D textures).
Metal::TextureType ViewTextureType(AmdGpu::ImageType type, Metal::TextureType texture_type) {
    if (texture_type == Metal::TextureType::Tex3D) {
        return Metal::TextureType::Tex3D;
    }
    const bool multisample = texture_type == Metal::TextureType::Tex2DMultisample ||
                             texture_type == Metal::TextureType::Tex2DMultisampleArray;
    if (IsArrayViewType(type)) {
        return multisample ? Metal::TextureType::Tex2DMultisampleArray
                           : Metal::TextureType::Tex2DArray;
    }
    return multisample ? Metal::TextureType::Tex2DMultisample : Metal::TextureType::Tex2D;
}

void LogOnce(std::atomic<bool>& logged, const char* what) {
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        LOG_WARNING(Render_Vulkan, "{}", what);
    }
}

} // namespace

bool IsViewTypeCompatible(AmdGpu::ImageType view_type, AmdGpu::ImageType image_type) {
    switch (view_type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
        return image_type == AmdGpu::ImageType::Color1D;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DMsaaArray:
        return image_type == AmdGpu::ImageType::Color2D || image_type == AmdGpu::ImageType::Color3D;
    case AmdGpu::ImageType::Color3D:
        return image_type == AmdGpu::ImageType::Color3D;
    default:
        UNREACHABLE();
    }
}

ImageViewInfo::ImageViewInfo(const AmdGpu::Image& image, const Shader::ImageResource& desc) noexcept
    : is_storage{desc.is_written} {
    const auto dfmt = image.GetDataFmt();
    auto nfmt = image.GetNumberFmt();
    if (is_storage && nfmt == AmdGpu::NumberFormat::Srgb) {
        nfmt = AmdGpu::NumberFormat::Unorm;
    }
    format = Vulkan::LiverpoolToVK::SurfaceFormat(dfmt, nfmt);
    if (desc.is_depth) {
        format = Vulkan::LiverpoolToVK::PromoteFormatToDepth(format);
    }

    range.base.level = image.base_level;
    range.base.layer = std::min<u32>(image.base_array, image.NumLayers() - 1);
    range.extent.levels = image.NumViewLevels(desc.is_array);
    range.extent.layers = image.NumViewLayers(desc.is_array);
    type = image.GetViewType(desc.is_array);
    min_lod = static_cast<u32>(image.min_lod);

    if (!is_storage) {
        mapping = Vulkan::LiverpoolToVK::ComponentMapping(image.DstSelect());
    }
}

ImageViewInfo::ImageViewInfo(const AmdGpu::ColorBuffer& col_buffer) noexcept {
    range.base.layer = col_buffer.BaseSlice();
    range.extent.layers = col_buffer.NumSlices() - range.base.layer;
    type = range.extent.layers > 1 ? AmdGpu::ImageType::Color2DArray : AmdGpu::ImageType::Color2D;
    format =
        Vulkan::LiverpoolToVK::SurfaceFormat(col_buffer.GetDataFmt(), col_buffer.GetNumberFmt());
}

ImageViewInfo::ImageViewInfo(const AmdGpu::DepthBuffer& depth_buffer, AmdGpu::DepthView view,
                             AmdGpu::DepthControl ctl) {
    format = Vulkan::LiverpoolToVK::DepthFormat(depth_buffer.z_info.format,
                                                depth_buffer.stencil_info.format);
    is_storage = ctl.depth_write_enable;
    range.base.layer = view.slice_start;
    range.extent.layers = view.NumSlices() - range.base.layer;
    type = range.extent.layers > 1 ? AmdGpu::ImageType::Color2DArray : AmdGpu::ImageType::Color2D;
}

ImageView::ImageView(const ImageViewInfo& info_, const Image& image) : info{info_} {
    if (info.min_lod != 0) {
        static std::atomic<bool> logged{false};
        LogOnce(logged, "view min LOD has no Metal equivalent (ignored)");
    }
    if (!IsViewTypeCompatible(info.type, image.info.type)) {
        LOG_ERROR(Render_Vulkan, "image view type {} is incompatible with image type {}",
                  magic_enum::enum_name(info.type), magic_enum::enum_name(image.info.type));
    }

    const Metal::TextureDesc& desc = image.backing->desc;
    // When sampling D32/D16 texture from shader, the T# specifies R32/R16 format: depth views
    // keep the image's format. Stencil views of depth-stencil images read the stencil plane.
    vk::Format format = info.format;
    u32 pixel_format = 0;
    if (image.has_depth && Vulkan::LiverpoolToVK::IsFormatDepthCompatible(format)) {
        format = image.info.pixel_format;
        pixel_format = desc.pixel_format;
    } else if (image.has_stencil && Vulkan::LiverpoolToVK::IsFormatStencilCompatible(format)) {
        format = image.info.pixel_format;
        pixel_format = Metal::StencilViewPixelFormat(desc.pixel_format);
    } else {
        pixel_format = Metal::VkToMtl::PixelFormat(format);
        if (pixel_format == 0) {
            static std::atomic<bool> logged{false};
            LogOnce(logged, "image view format has no Metal pixel format (using the image's)");
            pixel_format = desc.pixel_format;
        }
    }

    // Metal aborts on views it cannot make (an uncompressed view of a BC texture, a colour view
    // of a depth texture, ...): such views read the texture in its own format.
    const u32 safe_format =
        Metal::SafeViewPixelFormat(desc.pixel_format, pixel_format, desc.format_views);
    if (safe_format != pixel_format) {
        static std::atomic<bool> logged{false};
        LogOnce(logged, "image view format cannot view the texture in Metal (block or depth "
                        "reinterpretation): using the image's format");
        pixel_format = safe_format;
        format = image.info.pixel_format;
    }

    const Metal::TextureType type = ViewTextureType(info.type, desc.type);
    const bool is_3d = type == Metal::TextureType::Tex3D;
    if (is_3d && image.info.type == AmdGpu::ImageType::Color3D &&
        info.type != AmdGpu::ImageType::Color3D) {
        static std::atomic<bool> logged{false};
        LogOnce(logged, "2D views of 3D images have no Metal equivalent (the 3D texture is bound)");
    }
    const bool is_array = type == Metal::TextureType::Tex2DArray ||
                          type == Metal::TextureType::Tex2DMultisampleArray;
    const u32 base_level = std::min(info.range.base.level, desc.levels - 1);
    const u32 levels = std::clamp(info.range.extent.levels, 1u, desc.levels - base_level);
    const u32 base_layer = is_3d ? 0 : std::min(info.range.base.layer, desc.layers - 1);
    const u32 layers =
        is_3d || !is_array ? 1 : std::clamp(info.range.extent.layers, 1u, desc.layers - base_layer);
    const auto swizzle = Metal::VkToMtl::ComposeSwizzle(info.mapping, format);

    const bool identical = pixel_format == desc.pixel_format && type == desc.type &&
                           base_level == 0 && levels == desc.levels && base_layer == 0 &&
                           layers == desc.layers && swizzle == Metal::VkToMtl::IdentitySwizzle;
    if (identical) {
        view = Metal::Object::Retain(image.GetImage());
        return;
    }
    view = Metal::CreateTextureView(image.GetImage(), pixel_format, type, base_level, levels,
                                    base_layer, layers, {swizzle.r, swizzle.g, swizzle.b, swizzle.a});
    ASSERT_MSG(view, "Failed to create texture view of image {:#x} ({}x{}x{}) {}:{} {}:{}",
               image.info.guest_address, image.info.size.width, image.info.size.height,
               image.info.size.depth, base_level, base_level + levels - 1, base_layer,
               base_layer + layers - 1);
}

ImageView::~ImageView() = default;

} // namespace VideoCore
