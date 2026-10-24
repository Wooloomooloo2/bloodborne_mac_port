// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: camera motion vectors for temporal upscaling (docs/upscaler.md). The scene constants
// give the camera of each frame; the previous one is kept here. BB_DEBUG_MOTION=1 blends the
// motion vectors as colors into the frame before it is copied to the display.

#pragma once

#include <array>
#include <memory>

#include "common/types.h"
#include "video_core/renderer_metal/mtl_host_kernel.h"
#include "video_core/texture_cache/types.h"

namespace Metal {
class Device;
class Encoder;
} // namespace Metal

namespace VideoCore {
class TextureCache;
struct Buffer;
} // namespace VideoCore

namespace Vulkan {

class Scheduler;
class Runtime;
class ObjectMotion;

class CameraMotion {
public:
    /// camera_motion.comp's push constants (std430; padded to the MSL struct's 16-byte size).
    struct alignas(16) Params {
        std::array<float, 12> reproject; ///< previous view * inverse current view, 3x4 rows
        std::array<float, 4> proj;
        std::array<float, 4> prev_proj;
        std::array<float, 2> size;
        std::array<float, 2> jitter;
        std::array<float, 2> previous_jitter;
        u32 mode;
    };

    /// Encodes camera_motion.comp (`kernel`, BuildHostKernel of CAMERA_MOTION_COMP) over
    /// `params.size`: `depth` (id<MTLTexture>, depth in red: D32/D32S8 or a float colour format)
    /// into `motion` (RG16Float, ShaderWrite), pixels, previous minus current. `object` is the
    /// object motion texture (read with mode 1; any readable 2D texture otherwise).
    static void Record(Metal::Encoder& encoder, const Metal::HostKernel& kernel, void* depth,
                       void* motion, void* object, const Params& params);

    CameraMotion(Metal::Device& device, Scheduler& scheduler,
                 VideoCore::TextureCache& texture_cache, Runtime& runtime);
    ~CameraMotion();
    void InvalidateHistory() { current.valid = previous.valid = false; }
    void SetObjectMotion(ObjectMotion* motion) noexcept {
        object_motion = motion;
    }
    /// The object motion texture (id<MTLTexture>, after RecordMotion), for the debug view; or
    /// null.
    void* ObjectMotionView() const noexcept;
    /// The object motion texture if it holds this frame's vectors at `width` x `height`, or null.
    void* ObjectMotionImage(u32 width, u32 height) const noexcept;

    void SetJitter(std::array<float, 2> value) noexcept {
        previous_jitter = jitter;
        jitter = value;
    }

    [[nodiscard]] bool Enabled() const noexcept {
        return debug_overlay || for_upscaler;
    }

    /// Both cameras and the scene depth of the current frame are known.
    [[nodiscard]] bool Ready() const noexcept {
        return current.valid && previous.valid && depth_id;
    }
    [[nodiscard]] VideoCore::ImageId Depth() const noexcept {
        return depth_id;
    }
    /// The render size in the scene constants (the game's viewport; its targets may be
    /// allocated larger, e.g. 1916x1080 for a 1916x1078 scene), or zero before the first camera.
    [[nodiscard]] std::array<u32, 2> RenderSize() const noexcept {
        return render_size;
    }
    /// Vertical field of view and near/far planes of the current camera.
    [[nodiscard]] float VerticalFov() const noexcept;
    [[nodiscard]] float Near() const noexcept;
    /// Conservative frame-generation cut detection (translation, rotation, projection).
    [[nodiscard]] bool Continuous() const noexcept;
    // Current projection, previous projection, and row Z of previous-view * inverse-view.
    [[nodiscard]] std::array<std::array<float, 4>, 3> TaaDepthParameters() const noexcept;

    /// Records the motion vector pass (Record) in order on the scheduler: `depth_texture` into
    /// `motion_texture` (id<MTLTexture>, RG16Float, ShaderWrite), pixels, previous minus current.
    void RecordMotion(void* depth_texture, void* motion_texture, u32 width, u32 height);

    /// A bound constant buffer of 864 bytes: checks the scene constant signature.
    void OnConstants(const float* data);

    /// The G-buffer pass (5+ color targets): its depth is the scene depth.
    void OnGBufferPass(VideoCore::ImageId depth);

    /// The pass copying the finished frame (`frame`, the last target drawn) to the display: the
    /// frame's depth and camera are complete. Records the debug overlay, starts a new frame.
    void OnDisplayPass(VideoCore::ImageId frame);

private:
    struct Camera {
        std::array<float, 12> view{};     ///< world to view, 3x4 rows
        std::array<float, 12> inv_view{}; ///< view to world, 3x4 rows
        std::array<float, 4> proj{};      ///< x scale, y scale, z scale, z offset
        bool valid = false;
    };

    void Overlay(VideoCore::ImageId frame);
    [[nodiscard]] Params MakeParams(u32 width, u32 height, u32 mode) const noexcept;

    Metal::Device& device;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    bool debug_overlay = false;
    bool for_upscaler = false;
    Metal::HostKernel motion_kernel; ///< built at construction when for_upscaler

    ObjectMotion* object_motion = nullptr;
    Camera current, previous;
    bool frame_has_camera = false;
    std::array<float, 2> jitter{}, previous_jitter{};
    std::array<u32, 2> render_size{};
    VideoCore::ImageId depth_id{};

    Metal::HostKernel overlay_kernel; ///< built at construction when debug_overlay
    /// Debug: the last two finished frames (RGBA8 packed), for the reprojection check.
    std::unique_ptr<VideoCore::Buffer> frames[2];
    u32 frame_index = 0;
};

} // namespace Vulkan
