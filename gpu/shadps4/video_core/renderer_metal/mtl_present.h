// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: presenting to a CAMetalLayer (Metal-native renderer, M4): aspect fit, the post-process
// pass (host_shaders/post_process.frag in MSL), the letterboxed present blit, layer setup,
// drawables and a second command queue for presents that waits on the device's shared event.
// The shape follows Huax-1839/bloodborne-macos's mtl_presenter.mm (GPL-2.0-or-later). Plain C++;
// Metal objects cross as void* (unretained) or Metal::Object (retained).

#pragma once

#include <functional>
#include <memory>
#include "common/types.h"
#include "video_core/renderer_metal/mtl_object.h"

namespace Metal {

class Device;
class Encoder;

struct FitRect {
    s32 x, y;
    u32 width, height;
};
/// Largest rect of the source's aspect inside dst, centred (letterbox / pillarbox); same rounding
/// as vk_presenter.cpp FitImage at 82d0c4a. A zero-sized source or destination gives {0,0,0,0}.
FitRect FitImage(u32 src_width, u32 src_height, u32 dst_width, u32 dst_height);

/// post_process.frag's push constants (12 bytes, shader layout).
/// Whether Present should ask the layer for a drawable. A minimised, hidden or fully occluded
/// macOS window keeps its pixel size, and nextDrawable on a layer the window server does not
/// composite can block up to its 1 s timeout per frame (the frame pool fills and the game stalls):
/// such frames are skipped. A snapshot request (BB_SNAPSHOT_TRIGGER) still acquires, since
/// snapshots are taken while the screen is locked. A zero-sized window never acquires.
bool ShouldAcquireDrawable(bool window_visible, s32 width, s32 height, bool snapshot_requested);

/// post_process.frag's push constants (12 bytes, shader layout).
struct PostProcessSettings {
    float gamma{1.0f};
    u32 hdr{};
    u32 srgb_input{};
};

/// Pipelines and sampler for the post-process pass and the present blit (compiled once).
class PresentPasses {
public:
    PresentPasses(Device& device, u32 frame_format /*MTLPixelFormat of frames*/,
                  u32 drawable_format /*MTLPixelFormat of the layer*/);
    ~PresentPasses();

    PresentPasses(const PresentPasses&) = delete;
    PresentPasses& operator=(const PresentPasses&) = delete;

    /// Encodes the post-process pass: samples `input` (id<MTLTexture>, any float-sampleable
    /// format; linear filter) over the whole `output` (render target of frame_format).
    void PostProcess(Encoder& encoder, void* input, void* output, u32 out_width, u32 out_height,
                     const PostProcessSettings& settings);
    /// Encodes into a raw id<MTLCommandBuffer> (present queue): clears `target` black, then if
    /// `frame` is non-null draws it into FitImage(frame size, target size), linear.
    void Present(void* command_buffer, void* frame, u32 frame_width, u32 frame_height,
                 void* target, u32 target_width, u32 target_height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

struct LayerConfig {
    u32 pixel_format{80}; // MTLPixelFormatBGRA8Unorm
    u32 max_drawables{2};
    bool display_sync{true};
    bool framebuffer_only{true};
};
/// Configures a CAMetalLayer (void* = CAMetalLayer*, e.g. SDL_Metal_GetLayer): device, pixel
/// format, sRGB colorspace, maximumDrawableCount, displaySyncEnabled, framebufferOnly.
void ConfigureLayer(Device& device, void* layer, const LayerConfig& config);
/// maximumDrawableCount (2 or 3) after ConfigureLayer, e.g. when the window enters full screen.
void SetMaximumDrawables(void* layer, u32 count);

struct Drawable {
    Object drawable;     ///< id<CAMetalDrawable> (retained)
    void* texture{};     ///< its id<MTLTexture>, unretained (lives as long as `drawable`)
    u32 width{}, height{};
};
/// [layer nextDrawable]; empty Drawable (drawable null) on timeout/nil. Caller holds an
/// autorelease pool.
Drawable NextDrawable(void* layer);

/// Debug snapshot of what is shown (BB_SNAPSHOT_TRIGGER; screenshots while the screen is locked
/// or asleep, when the window server captures nothing): encodes a copy of `texture` (BGRA8,
/// width x height; a drawable needs framebuffer_only off) into a new shared id<MTLBuffer> on
/// `command_buffer`. Null when the buffer cannot be made or `texture` is not BGRA8 (logged).
Object EncodeSnapshot(Device& device, void* command_buffer, void* texture, u32 width, u32 height);
/// After that command buffer completed: writes the copy (`buffer`, an id<MTLBuffer>) as an sRGB
/// PNG at `path`. False on failure.
bool WriteSnapshotPng(const char* path, void* buffer, u32 width, u32 height);

/// Runs `body` inside @autoreleasepool (for plain C++ callers on threads without a pool).
void WithAutoreleasePool(const std::function<void()>& body);

/// A second command queue for presents. Its command buffers can wait for a device tick of the
/// main queue (the shared event) and report completion through a callback. They are committed
/// raw (never through Device::Commit, whose ticks assume one queue); their GPU time counts in
/// Device::GpuBusyNs.
class PresentQueue {
public:
    /// `shutdown_timeout_ms`: how long the destructor waits for command buffers in flight.
    explicit PresentQueue(Device& device, u32 shutdown_timeout_ms = 3000);
    /// Waits up to the shutdown timeout for every command buffer in flight (and its callback).
    /// On timeout (a present stuck behind a device tick that never comes) it logs an error and
    /// leaks the queue's state, which the pending callbacks still use, instead of hanging.
    ~PresentQueue();

    PresentQueue(const PresentQueue&) = delete;
    PresentQueue& operator=(const PresentQueue&) = delete;

    /// New id<MTLCommandBuffer> (unretained by the caller, valid until Commit). Every command
    /// buffer begun must be committed.
    void* Begin();
    /// Encodes a wait for the device's shared event to reach `device_tick` (0: no wait).
    void WaitForDeviceTick(void* command_buffer, u64 device_tick);
    /// presentDrawable (if `drawable`, an id<CAMetalDrawable>, is non-null), then commit;
    /// `on_complete` (may be empty) runs on a Metal completion thread once the command buffer
    /// completed (also on error).
    /// `min_duration` > 0 (seconds): presentDrawable:afterMinimumDuration: (the previous drawable
    /// stays on screen at least that long), for even frame pacing on variable-refresh displays.
    /// `on_presented`: actual drawable presentation time, not GPU completion. May arrive after
    /// WaitIdle or queue destruction; capture independent owned state, never the queue/caller.
    void Commit(void* command_buffer, void* drawable, std::function<void()> on_complete,
                double min_duration = 0, std::function<void(double)> on_presented = {});
    /// Blocks until nothing is in flight.
    void WaitIdle();
    /// Blocks until nothing is in flight or `milliseconds` passed; true when idle.
    bool WaitIdleFor(u32 milliseconds);
    /// Command buffers committed whose completion callbacks have not finished.
    [[nodiscard]] u32 InFlight() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Metal
