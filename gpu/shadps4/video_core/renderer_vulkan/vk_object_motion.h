// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: object motion vectors for temporal upscaling (docs/ROADMAP.md, step 1).
//
// G-buffer shaders preserve clip positions for the exact indexed range of each draw.
// Matching includes vertex streams, index contents and base offsets. A missing match falls
// back to camera motion. See motion_history.h for CPU bookkeeping and its regression test.

#pragma once

#include <array>
#include "video_core/renderer_vulkan/motion_history.h"

#include "common/types.h"
#include "video_core/renderer_metal/mtl_object.h"

namespace Metal {
class Device;
}

namespace Vulkan {

class Scheduler;
struct RenderState;

class ObjectMotion {
public:
    ObjectMotion(Metal::Device& device, Scheduler& scheduler);
    ~ObjectMotion();

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

    using DrawInfo = Motion::Draw;
    u32 PrepareDraw(const DrawInfo& draw);
    /// Small skeletons: whether the bone palette changed (Motion::History::Moving).
    bool Moving(const Motion::History::GateKey& key, u64 palette) {
        return history.Moving(key, palette);
    }
    /// Index range and topology hash of an indexed draw, reused across frames.
    template <class Scan>
    Motion::IndexRangeCache::Result IndexRange(const Motion::IndexRangeCache::Key& key,
                                               Scan&& scan) {
        return index_ranges.Get(key, frame, scan);
    }

    /// Rendering of a motion pipeline: the motion attachment (render-target size) as colour 7,
    /// cleared by the first pass of the frame, and the position history as the pass's
    /// vertex_rw_buffer.
    void Attach(RenderState& state, u32 width, u32 height);

    /// Start of a frame (display pass).
    void OnFrameStart();
    void InvalidateHistory() { history.NextFrame(); history.NextFrame(); }

    /// The motion texture (id<MTLTexture>, RGBA32Float) for the compose pass and whether it
    /// holds this frame's vectors at `width` x `height`.
    void* PrepareRead(u32 width, u32 height, bool& valid);
    /// The motion texture (id<MTLTexture>), or null before the first Attach/PrepareRead.
    [[nodiscard]] void* View() const noexcept {
        return image.Get();
    }
    /// The motion texture if it holds this frame's vectors at `width` x `height`, else null.
    [[nodiscard]] void* Image(u32 width, u32 height) const noexcept {
        return written && width == image_width && height == image_height ? image.Get() : nullptr;
    }

private:
    void EnsureImage(u32 width, u32 height);

    Metal::Device& device;
    Scheduler& scheduler;
    bool enabled = false;

    // Parameter ring (host visible): FrameSlots frames of ParamsPerFrame entries (two u32x4),
    // element 0 all zero (motion off).
    static constexpr u32 FrameSlots = 4;
    static constexpr u32 ParamsPerFrame = 8192;
    // Positions (device local): two halves (current/previous frame) of vec4; element 0 is reserved.
    static constexpr u32 PositionsPerFrame = 4u << 20;
    Motion::History history{PositionsPerFrame};
    Motion::IndexRangeCache index_ranges;
    std::array<u64, FrameSlots> params_ticks{};
    // Reached by shaders only through their GPU addresses (Shader::MotionVectors): resident
    // and declared on the encoders that use them (see the constructor for the ordering).
    Metal::Object params_buffer;
    u32* params_mapped{};
    Metal::Object positions_buffer;

    u64 frame = 0;
    u32 params_used = 0;
    Metal::Object image; ///< id<MTLTexture>
    u32 image_width = 0, image_height = 0;
    bool written = false; ///< this frame's vectors are in the image
};

} // namespace Vulkan
