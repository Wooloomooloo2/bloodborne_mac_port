// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <atomic>
#include "common/types.h"
#include "video_core/renderer_metal/mtl_screen_lighting.h"
#include "video_core/texture_cache/types.h"

namespace Metal {
class Device;
}
namespace VideoCore {
class TextureCache;
}
namespace Vulkan {
class Scheduler;
class Runtime;
class CameraMotion;
class SceneTargets;
struct RenderState;

/// An optional stage immediately after the admitted opaque scene draw, ahead of
/// transparencies, fog, temporal reconstruction and post-processing. No game assets change.
class ScreenLighting {
public:
    ScreenLighting(Metal::Device&, Scheduler&, VideoCore::TextureCache&, Runtime&, CameraMotion&,
                   SceneTargets&);
    ~ScreenLighting();
    bool Enabled() const;
    bool TrackFrames() const { return Enabled() || control_file; }
    void OnFrameStart();
    void OnGBuffer(VideoCore::ImageId depth, VideoCore::ImageId albedo);
    void AfterOpaqueDraw(VideoCore::ImageId color, VideoCore::ImageId depth,
                         const RenderState& state, std::shared_ptr<std::atomic<bool>> encoded);

private:
    Metal::Device& device;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    CameraMotion& camera;
    SceneTargets& scene_targets;
    std::unique_ptr<Metal::ScreenLightingPasses> passes;
    VideoCore::ImageId frame_depth{}, frame_albedo{};
    u64 depth_uid{}, albedo_uid{}, frame{};
    bool done{}, control_file{};
    std::atomic<bool> logged{false};
    u32 view{};
};
} // namespace Vulkan
