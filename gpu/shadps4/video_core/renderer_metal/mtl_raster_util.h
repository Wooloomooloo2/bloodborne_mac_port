// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: from Huax-1839/bloodborne-macos (ee3120a, GPL-2.0-or-later), renderer_metal/.

#pragma once

#include <algorithm>

#include "common/types.h"
#include "video_core/renderer_metal/vk_to_mtl.h"

/// Plain C++ helpers of the Metal rasterizer's recording (mtl_rasterizer.mm), apart so the unit
/// test (tests/test_mtl_raster.mm) renders with the very functions the rasterizer calls.
namespace Metal::RasterUtil {

/// A Metal viewport (the fields of MTLViewport).
struct MetalViewport {
    double origin_x, origin_y, width, height, znear, zfar;
};

/// Coordinates (decided once, proved by mtl-raster-test): the recompiler's vertex shaders write
/// Vulkan clip space (+y down), Metal's NDC has +y up, and both framebuffers have their origin at
/// the top left. Metal's viewport transform accepts a negative height, which turns the y axis
/// over, so a Vulkan viewport {x, y, w, h} becomes {x, y + h, w, -h}: for every clip space
/// position the window position is the one Vulkan computes (y_window = y + (y_ndc + 1) / 2 * h),
/// and the shaders need no flip. This holds for either sign of the Vulkan height (the game's
/// viewports are often flipped, negative; the clip-disabled screen-space path positive): the
/// Metal height has the opposite sign.
inline MetalViewport ToMetalViewport(float x, float y, float width, float height, float min_depth,
                                     float max_depth) {
    // Metal wants the depth range inside [0, 1].
    return {x,
            double(y) + double(height),
            width,
            -double(height),
            std::clamp(min_depth, 0.0f, 1.0f),
            std::clamp(max_depth, 0.0f, 1.0f)};
}

/// MTLWinding for the Vulkan front face, in the same vocabulary of both APIs: the facing of a
/// triangle is decided by the orientation of its window space positions as displayed (y down),
/// which the viewport conversion above keeps identical, whichever sign the heights have. The
/// answer is the plain mapping (counter-clockwise stays counter-clockwise); no flip per draw.
inline u32 FrontWinding(vk::FrontFace face) {
    return VkToMtl::Winding(face);
}

/// A scissor rectangle in framebuffer pixels (the fields of MTLScissorRect).
struct ScissorRect {
    u32 x, y, width, height;
};

/// The Vulkan scissor (x, y, width, height; x and y may be negative) clamped to the render
/// target [0, target_width] x [0, target_height], which Metal requires. Returns false when
/// nothing is left (`out` is then {0, 0, 0, 0}).
inline bool ClampScissor(s32 x, s32 y, u32 width, u32 height, u32 target_width, u32 target_height,
                         ScissorRect& out) {
    const s64 x0 = std::clamp<s64>(x, 0, target_width);
    const s64 y0 = std::clamp<s64>(y, 0, target_height);
    const s64 x1 = std::clamp<s64>(s64(x) + s64(width), 0, target_width);
    const s64 y1 = std::clamp<s64>(s64(y) + s64(height), 0, target_height);
    if (x1 <= x0 || y1 <= y0) {
        out = {0, 0, 0, 0};
        return false;
    }
    out = {u32(x0), u32(y0), u32(x1 - x0), u32(y1 - y0)};
    return true;
}

} // namespace Metal::RasterUtil
