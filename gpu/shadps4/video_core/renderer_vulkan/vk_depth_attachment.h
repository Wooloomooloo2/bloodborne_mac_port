// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the depth buffer as a draw binds it.
//
// One rule for both sides of a draw: the render pass attaches the depth buffer
// (Rasterizer::PrepareRenderState, Rasterizer::BeginRenderingFull) and the pipeline key carries
// its depth/stencil formats (PipelineCache::RefreshGraphicsKey). Metal requires a render
// pipeline's depthAttachmentPixelFormat/stencilAttachmentPixelFormat to equal the pass's
// attachments exactly, so the two must never be computed apart.

#pragma once

#include "video_core/amdgpu/regs.h"

namespace Vulkan {

struct DepthAttachment {
    using ZFormat = AmdGpu::DepthBuffer::ZFormat;
    using StencilFormat = AmdGpu::DepthBuffer::StencilFormat;

    ZFormat z_format{ZFormat::Invalid};                  ///< Invalid: no depth aspect bound
    StencilFormat stencil_format{StencilFormat::Invalid}; ///< Invalid: no stencil aspect bound

    bool HasDepth() const {
        return z_format != ZFormat::Invalid;
    }
    bool HasStencil() const {
        return stencil_format != StencilFormat::Invalid;
    }
    /// The pass has a depth/stencil attachment.
    bool Bound() const {
        return HasDepth() || HasStencil();
    }
};

/// The depth buffer is attached while a depth or stencil test uses it, then with each valid
/// aspect (a depth/stencil format binds both, a stencil test without depth keeps the depth).
inline DepthAttachment BoundDepthAttachment(const AmdGpu::Regs& regs) {
    const auto& db = regs.depth_buffer;
    const bool bound = (regs.depth_control.depth_enable && db.DepthValid()) ||
                       (regs.depth_control.stencil_enable && db.StencilValid());
    DepthAttachment attachment{};
    if (!bound) {
        return attachment;
    }
    if (db.DepthValid()) {
        attachment.z_format = db.z_info.format;
    }
    if (db.StencilValid()) {
        attachment.stencil_format = db.stencil_info.format;
    }
    return attachment;
}

} // namespace Vulkan
