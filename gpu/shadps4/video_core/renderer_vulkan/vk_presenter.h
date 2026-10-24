// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/libraries/videoout/buffer.h"
#include "bbport_framegen.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_object.h"
#include "video_core/renderer_metal/mtl_present.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Frontend {
class WindowSDL;
}

namespace AmdGpu {
struct Liverpool;
}

namespace Vulkan {

/// bbport (Metal-native M4): a game frame after the post-process pass, at the size it is shown
/// (the game image fitted into the window), waiting to be presented to the CAMetalLayer.
struct Frame {
    u32 width{};
    u32 height{};
    Metal::Object texture; ///< id<MTLTexture>, BGRA8Unorm render target (empty: never drawn)
    Metal::Object generated; ///< optional post-processed intermediate image, same dimensions
    BbFrameGen::FramePtr framegen; ///< pins inputs until both presents complete
    u64 ready_tick{};      ///< draw-scheduler tick that wrote it; 0: blank (Present clears)
    bool is_hdr{false};
    u32 id{};
};

enum SchedulerType {
    Draw,
    Present,
    CpuFlip,
};

class Rasterizer;

class Presenter {
public:
    Presenter(Frontend::WindowSDL& window, AmdGpu::Liverpool* liverpool);
    ~Presenter();

    /// bbport: writes the Metal binary archive now if pipelines are pending (waits for a write
    /// in flight); for exits through _exit, which skip ~Presenter. False when the write failed.
    bool FlushPipelineArchive();

    HostPasses::PostProcessingPass::Settings& GetPPSettingsRef() {
        return pp_settings;
    }

    Frontend::WindowSDL& GetWindow() const {
        return window;
    }

    Rasterizer& GetRasterizer() const {
        return *rasterizer.get();
    }

    /// HDR output returns with the Metal presenter's EDR support (backlog); SDR only for now.
    bool IsHDRSupported() const {
        return false;
    }

    void SetHDR(bool) {}

    VideoCore::Image& RegisterVideoOutSurface(
        const Libraries::VideoOut::BufferAttributeGroup& attribute, VAddr cpu_address) {
        vo_buffers_addr.emplace_back(cpu_address);
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        const auto image_id = texture_cache.FindImage(desc);
        auto& image = texture_cache.GetImage(image_id);
        image.usage.vo_surface = 1u;
        return image;
    }

    bool IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const;

    /// On the thread recording into the draw scheduler: the post-process pass of the guest's
    /// display buffer into a free frame, submitted. Never null.
    Frame* PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                        VAddr cpu_address);

    /// A frame that presents as black. Never null.
    Frame* PrepareBlankFrame(bool present_thread);

    /// Presents `frame` on the present queue once its draw-scheduler tick is done, and returns
    /// it to the pool when the present completed. Never touches the draw scheduler.
    /// present_id (bbport, frame spike log): unused until presented-time stats (M5).
    void Present(Frame* frame, bool is_reusing_frame = false, bool is_game_frame = true,
                 u32 present_id = 0);

    /// bbport: frame spike log: steady-clock ns at the stages of the last Present().
    struct PresentTiming {
        u64 start, acquired, submitted, presented;
    };
    const PresentTiming& LastPresentTiming() const {
        return last_present;
    }
    bool IsDisplayTimingSupported() const {
        return false;
    }
    std::vector<vk::PastPresentationTimingGOOGLE> TakePastTimings() {
        return {};
    }
    /// The last presented frame again when it is back in the pool, else null (it is still being
    /// presented, so the screen shows it anyway).
    Frame* PrepareLastFrame();

    /// The Metal device (status lines: bbgpu_dump_breadcrumbs).
    [[nodiscard]] const Metal::Device& GetDevice() const noexcept {
        return device;
    }

private:
    /// The frames and their free list. Shared with the present completion handlers, which may
    /// outlive the presenter when a present is stuck at shutdown.
    struct FramePool {
        static constexpr u32 Count = 2;
        std::array<Frame, Count> frames;
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<Frame*> free; ///< under `mutex`
        Frame* last_presented{}; ///< under `mutex`

        /// Blocks until a frame is free and takes it.
        Frame* Take();
        /// Returns a frame to the free list.
        void Free(Frame* frame);
    };

    /// A free frame whose texture has the given size (recreated when it differs).
    Frame* GetRenderFrame(u32 width, u32 height);

    Frontend::WindowSDL& window;
    AmdGpu::Liverpool* liverpool;
    Metal::Device device;
    Scheduler draw_scheduler;
    std::deque<u64> recent_frame_ticks; ///< bbport: BB_FRAMES_AHEAD bound (PrepareFrame)
    Runtime runtime;
    std::unique_ptr<Rasterizer> rasterizer;
    VideoCore::TextureCache& texture_cache;
    void* layer; ///< CAMetalLayer* (the window's, SDL_Metal_GetLayer)
    /// Drawables in a window and in full screen (LayerConfigFromEnv, FullscreenDrawables), and
    /// the count the layer has now (bb:Present only).
    u32 windowed_drawables{2}, fullscreen_drawables{3}, layer_drawables{2};
    /// bbport: BB_SNAPSHOT_TRIGGER (empty: off). Present writes the drawable as a PNG when this
    /// file exists (its first line, else "<trigger>.png"), then removes it. Create the trigger
    /// atomically (temp file + mv) or the path line may not be there yet. The PNG is written by
    /// a detached thread: one still writing at exit leaves a truncated file (debug tool).
    std::string snapshot_trigger;
    /// The PNG path when the trigger file exists (and removes it); empty otherwise.
    bool SnapshotRequested() const;
    std::string TakeSnapshotRequest();
    HostPasses::PostProcessingPass::Settings pp_settings{};
    /// FSR 1 (BB_FSR1): created when enabled at start, as on the Vulkan renderer.
    std::unique_ptr<HostPasses::FsrPass> fsr_pass;
    HostPasses::FsrPass::Settings fsr_settings{};
    HostPasses::PostProcessingPass pp_pass;
    std::unique_ptr<Metal::PresentQueue> present_queue;
    std::shared_ptr<FramePool> pool;
    PresentTiming last_present{};
    std::vector<VAddr> vo_buffers_addr;
    // bbport: frame timeline (BB_FRAME_STATS): flips waiting for GPU completion. Last, so the
    // thread stops before anything it uses is destroyed.
    void WatchFrameCompletion(u64 tick, u64 submitted);
    std::mutex watch_mutex;
    std::condition_variable_any watch_cv;
    std::deque<std::pair<u64, u64>> watch_queue;
    std::jthread watch_thread;
};

} // namespace Vulkan
