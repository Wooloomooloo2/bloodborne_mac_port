// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/amdgpu/regs_depth.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_metal/mtl_object.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/types.h"

namespace AmdGpu {
struct ColorBuffer;
}

namespace Shader {
struct ImageResource;
}

namespace VideoCore {

struct ImageViewInfo {
    ImageViewInfo() = default;
    ImageViewInfo(const AmdGpu::Image& image, const Shader::ImageResource& desc) noexcept;
    ImageViewInfo(const AmdGpu::ColorBuffer& col_buffer) noexcept;
    ImageViewInfo(const AmdGpu::DepthBuffer& depth_buffer, AmdGpu::DepthView view,
                  AmdGpu::DepthControl ctl);

    AmdGpu::ImageType type = AmdGpu::ImageType::Color2D;
    vk::Format format = vk::Format::eR8G8B8A8Unorm;
    SubresourceRange range;
    vk::ComponentMapping mapping{};
    u32 min_lod = 0;
    bool is_storage = false;

    auto operator<=>(const ImageViewInfo&) const = default;
};

struct Image;

struct ImageView {
    /// A view of the image's current backing texture.
    ImageView(const ImageViewInfo& info, const Image& image);
    ~ImageView();

    ImageView(const ImageView&) = delete;
    ImageView& operator=(const ImageView&) = delete;

    ImageView(ImageView&&) = default;
    ImageView& operator=(ImageView&&) = default;

    /// The id<MTLTexture> to bind (unretained): a texture view, or the backing texture itself
    /// when the view would be identical to it (same format and type, full range, identity
    /// swizzle), which render targets can then use.
    void* Texture() const noexcept {
        return view.Get();
    }

    ImageViewInfo info;
    Metal::Object view; ///< id<MTLTexture>
};

} // namespace VideoCore
