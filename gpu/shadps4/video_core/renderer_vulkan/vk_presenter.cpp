// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

// bbport (Metal-native M4): the presenter on the window's CAMetalLayer. The post-process pass
// draws the guest's display buffer into a frame (the game image fitted into the window, so the
// present is a 1:1 copy) on the draw scheduler; Present draws the frame letterboxed into a
// drawable on a second command queue that waits for the frame's device tick. The shape follows
// Huax-1839/bloodborne-macos's mtl_presenter.mm (GPL-2.0-or-later), with opt-in interpolation.

#include "bbport_cpu.h"
#include "bbport_gpu_capture.h"
#include "bbport_overlay.h"
#include "bbport_settings.h"
#include "bbport_toggles.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "video_core/renderer_metal/mtl_pso_cache.h"
#include "video_core/renderer_metal/mtl_texture.h"
#include "video_core/renderer_metal/vk_to_mtl.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <optional>
#include <unistd.h>

namespace Vulkan {

namespace {

/// MTLPixelFormatBGRA8Unorm: frames and drawables (the post-process pass writes gamma-encoded
/// values, as into MoltenVK's swapchain before).
constexpr u32 FrameFormat = 80;

u64 NowNs() {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}

Metal::LayerConfig LayerConfigFromEnv() {
    Metal::LayerConfig config{};
    config.pixel_format = FrameFormat;
    // MoltenVK has no Mailbox and 0.4.0 fell back to Fifo: only Immediate turns display sync off.
    config.display_sync = EmulatorSettings.GetPresentMode() != "Immediate";
    // Two drawables up to a 60 FPS limit, as the Vulkan swapchain had (measured 2026-10-14:
    // 48.5 -> 31.7 ms present latency at the same frame rate as three). Above it, three: with two
    // on a 120 Hz display every frame longer than one refresh waits a whole extra refresh for a
    // drawable, which locked 90 and 120 FPS limits at 60 (2026-10-17). BB_SWAPCHAIN_IMAGES
    // overrides, CAMetalLayer takes 2..3.
    const u32 limit = EmulatorSettings.GetFrameLimit();
    config.max_drawables = limit == 0 || limit > 60 ? 3 : 2;
    if (const char* env = std::getenv("BB_SWAPCHAIN_IMAGES")) {
        config.max_drawables = u32(std::clamp(std::atoi(env), 2, 3));
    }
    return config;
}

// Full screen: three. macOS shows a full-screen layer's frames directly, without the window
// compositor that otherwise absorbs a late frame, so with two a frame that misses a refresh
// also holds up the next nextDrawable: 10-18 late frames per run against 0-2 with three at the
// 60 cap on an M4 Max, and 30-40 FPS on a MacBook Air M5 (#25, 2026-10-21). BB_SWAPCHAIN_IMAGES
// overrides here too.
u32 FullscreenDrawables(const Metal::LayerConfig& config) {
    return std::getenv("BB_SWAPCHAIN_IMAGES") ? config.max_drawables : 3;
}

vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

} // namespace

Frame* Presenter::FramePool::Take() {
    std::unique_lock lock{mutex};
    cv.wait(lock, [this] { return !free.empty(); });
    Frame* const frame = free.front();
    free.pop_front();
    return frame;
}

void Presenter::FramePool::Free(Frame* frame) {
    // Called after all submitted images release their ownership, or immediately
    // for a skipped frame. Render work pins its own snapshots. Reusing
    // frame->texture needs no interpolation inputs.
    frame->framegen.reset();
    {
        std::scoped_lock lock{mutex};
        free.push_back(frame);
    }
    cv.notify_one();
}

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_}, draw_scheduler{device, true},
      runtime{device, draw_scheduler},
      rasterizer{std::make_unique<Rasterizer>(device, draw_scheduler, runtime, liverpool)},
      texture_cache{rasterizer->GetTextureCache()},
      layer{window.GetWindowInfo().render_surface}, pp_pass{device, FrameFormat, FrameFormat},
      present_queue{std::make_unique<Metal::PresentQueue>(device)},
      pool{std::make_shared<FramePool>()} {
    ASSERT_MSG(layer != nullptr, "Presenter: the window has no CAMetalLayer");
    if (const char* env = std::getenv("BB_SNAPSHOT_TRIGGER"); env && *env) {
        snapshot_trigger = env;
    }
    Metal::LayerConfig config = LayerConfigFromEnv();
    if (rasterizer->GetUpscaler().FrameGenerator()) config.max_drawables = 3;
    // Snapshots read the drawable back.
    config.framebuffer_only = snapshot_trigger.empty();
    Metal::ConfigureLayer(device, layer, config);
    windowed_drawables = layer_drawables = config.max_drawables;
    fullscreen_drawables = FullscreenDrawables(config);
    // The settings menu and FPS counter, drawn into the drawable (Present).
    BbOverlay::Init(device, FrameFormat);
    for (u32 i = 0; i < FramePool::Count; ++i) {
        pool->frames[i].id = i;
        pool->free.push_back(&pool->frames[i]);
    }
    fsr_settings.enable = EmulatorSettings.IsFsrEnabled();
    fsr_settings.use_rcas = EmulatorSettings.IsRcasEnabled();
    fsr_settings.rcas_attenuation = static_cast<float>(EmulatorSettings.GetRcasAttenuation() / 1000.f);
    if (fsr_settings.enable) {
        fsr_pass = std::make_unique<HostPasses::FsrPass>(device);
    }
    std::printf("Presenter: CAMetalLayer, %u drawables (%u in full screen), display sync %s, "
                "frame limit %u\n",
                config.max_drawables, fullscreen_drawables, config.display_sync ? "on" : "off",
                EmulatorSettings.GetFrameLimit());
}

Presenter::~Presenter() {
    // Bounded: a present stuck behind a device tick is logged and left behind (its completion
    // handler only touches the frame pool, which it keeps alive).
    present_queue.reset();
    draw_scheduler.Finish();
    if (rasterizer) {
        rasterizer->GetPipelineCache().Sync();
    }
}

bool Presenter::FlushPipelineArchive() {
    return rasterizer->GetPipelineCache().Psos().Flush();
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

Frame* Presenter::GetRenderFrame(u32 width, u32 height) {
    Frame* const frame = pool->Take();
    frame->framegen.reset();
    if (!frame->texture || frame->width != width || frame->height != height) {
        // Command buffers that still use the old texture hold references of their own
        // (Encoder::KeepAlive in the post-process pass; the present command buffer).
        Metal::TextureDesc desc{};
        desc.pixel_format = FrameFormat;
        desc.width = width;
        desc.height = height;
        desc.render_target = true;
        frame->texture = Metal::CreateTexture(device, desc, "Frame", nullptr);
        frame->generated.Reset();
        ASSERT_MSG(frame->texture, "Presenter: no {}x{} frame texture", width, height);
        frame->width = width;
        frame->height = height;
    }
    frame->is_hdr = false;
    return frame;
}

Frame* Presenter::PrepareLastFrame() {
    std::scoped_lock lock{pool->mutex};
    Frame* const frame = pool->last_presented;
    const auto it = std::ranges::find(pool->free, frame);
    if (frame == nullptr || it == pool->free.end()) {
        return nullptr;
    }
    pool->free.erase(it);
    return frame;
}

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    // bbport: scaled upscaler presets: the output-size display buffer drawn by the port.
    TemporalUpscaler::Display display{};
    const bool upscaled = rasterizer->GetUpscaler().DisplayOverride(cpu_address, display);
    const vk::Format view_format = GetFrameViewFormat(attribute.attrib.pixel_format);

    // The source, viewed with its sRGB format and alpha One (the frame must not blend with UI).
    Metal::Object input;
    u32 image_width = 0;
    u32 image_height = 0;
    if (upscaled) {
        // The display buffer has the guest render target's format; the video-out format views
        // it as Vulkan's mutable-format view did (BGRA view of an RGBA buffer: the texture has
        // PixelFormatView usage).
        const u32 format =
            Metal::SafeViewPixelFormat(Metal::TexturePixelFormat(display.texture),
                                       Metal::VkToMtl::PixelFormat(view_format), true);
        input = Metal::CreateTextureView(
            display.texture, format, Metal::TextureType::Tex2D, 0, 1, 0, 1,
            {Metal::VkToMtl::SwizzleRed, Metal::VkToMtl::SwizzleGreen,
             Metal::VkToMtl::SwizzleBlue, Metal::VkToMtl::SwizzleOne});
        ASSERT_MSG(input, "Presenter: no view of the upscaled display buffer");
        image_width = display.width;
        image_height = display.height;
    } else {
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        const VideoCore::ImageId image_id = texture_cache.FindImage(desc);
        texture_cache.UpdateImage(image_id);
        auto& image = texture_cache.GetImage(image_id);
        VideoCore::ImageViewInfo view_info{};
        view_info.format = view_format;
        view_info.mapping.a = vk::ComponentSwizzle::eOne;
        // The view belongs to the image's view cache: one more reference for the command buffer.
        input = Metal::Object::Retain(image.FindView(view_info).Texture());
        image_width = image.info.size.width;
        image_height = image.info.size.height;
        runtime.AccessImage(&image, false);
    }

    // The frame is the image fitted into the window (pixels), so Present copies it 1:1.
    const Metal::FitRect fit = Metal::FitImage(image_width, image_height,
                                               u32(std::max(window.GetWidth(), 0)),
                                               u32(std::max(window.GetHeight(), 0)));
    const u32 frame_width = fit.width ? fit.width : std::max(image_width, 1u);
    const u32 frame_height = fit.height ? fit.height : std::max(image_height, 1u);
    Frame* const frame = GetRenderFrame(frame_width, frame_height);

    // Vulkan had no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches the
    // post-process pass still sRGB encoded and is decoded there.
    pp_settings.srgb_input =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb;
    if (auto* generator = rasterizer->GetUpscaler().FrameGenerator()) {
        if (BbFrameGen::Active() && display.framegen && !pp_settings.srgb_input && !fsr_pass) {
            // Read through the actual video-out view after the retail gamma LUT.
            frame->framegen = std::move(display.framegen);
            if (!frame->generated) {
                Metal::TextureDesc desc{};
                desc.pixel_format = FrameFormat;
                desc.width = frame->width; desc.height = frame->height;
                desc.render_target = true;
                frame->generated = Metal::CreateTexture(device, desc, "Generated frame", nullptr);
            }
            if (frame->generated) {
                draw_scheduler.EndRendering();
                draw_scheduler.Record([generator, packet = frame->framegen,
                    displayed = Metal::Object::Retain(input.Get())](Metal::Encoder& e) {
                    generator->Interpolate(e, packet, displayed.Get());
                });
                pp_pass.Render(draw_scheduler,
                    Metal::Object::Retain(frame->framegen->generated.Get()),
                    Metal::Object::Retain(frame->generated.Get()), frame->width, frame->height,
                    pp_settings);
            } else {
                frame->framegen.reset();
                draw_scheduler.Record([generator](Metal::Encoder&) { generator->Invalidate(); });
            }
        } else {
            draw_scheduler.Record([generator](Metal::Encoder&) { generator->Invalidate(); });
        }
    }
    if (fsr_pass) {
        input = fsr_pass->Render(draw_scheduler, std::move(input), {image_width, image_height},
                                 {frame->width, frame->height}, fsr_settings, frame->is_hdr);
    }
    pp_pass.Render(draw_scheduler, std::move(input), Metal::Object::Retain(frame->texture.Get()),
                   frame->width, frame->height, pp_settings);

    // Flush frame creation commands.
    frame->ready_tick = draw_scheduler.CurrentTick();
    const u64 flush_start = bbport::CpuCycles();
    draw_scheduler.Flush();
    const u64 flush_end = bbport::CpuCycles();
    BbStats::flip_submit_ns.fetch_add(bbport::CyclesToNs(flush_end - flush_start),
                                      std::memory_order_relaxed);
    if (BbStats::enabled) {
        WatchFrameCompletion(frame->ready_tick, flush_end);
    }

    // bbport: the GPU command thread runs at most BB_FRAMES_AHEAD (default 1) guest frames
    // ahead of the GPU: it waits here for the frame that many flips back. When the GPU is the
    // bottleneck it finishes frames at an even rate; without this bound the command thread ran
    // ahead and then blocked wherever a resource ran out, so flips (and the guest's frame
    // timing) came in bursts: 12.5/25 ms alternation at 80 FPS. 0 turns it off.
    static const u32 frames_ahead = [] {
        const char* env = std::getenv("BB_FRAMES_AHEAD");
        return env ? u32(std::max(0, std::atoi(env))) : 1u;
    }();
    if (frames_ahead) {
        recent_frame_ticks.push_back(frame->ready_tick);
        while (recent_frame_ticks.size() > frames_ahead) {
            const u64 tick = recent_frame_ticks.front();
            recent_frame_ticks.pop_front();
            if (recent_frame_ticks.size() == frames_ahead) {
                const u64 wait_start = bbport::CpuCycles();
                draw_scheduler.Wait(tick);
                BbStats::frames_ahead_ns.fetch_add(
                    bbport::CyclesToNs(bbport::CpuCycles() - wait_start), std::memory_order_relaxed);
            }
        }
    }
    return frame;
}

// bbport: frame timeline (BB_FRAME_STATS): a helper thread waits for each flip's submission
// and adds the time from the commit's return to the GPU finishing it.
void Presenter::WatchFrameCompletion(u64 tick, u64 submitted) {
    std::scoped_lock lk{watch_mutex};
    if (!watch_thread.joinable()) {
        watch_thread = std::jthread([this](std::stop_token stop) {
            Common::SetCurrentThreadName("bb:FrameWatch");
            u64 last_done = 0;
            while (!stop.stop_requested()) {
                std::pair<u64, u64> item;
                {
                    std::unique_lock wl{watch_mutex};
                    watch_cv.wait(wl, stop, [this] { return !watch_queue.empty(); });
                    if (stop.stop_requested()) {
                        return;
                    }
                    item = watch_queue.front();
                    watch_queue.pop_front();
                }
                draw_scheduler.GetTimeline().Wait(item.first);
                const u64 done = bbport::CpuCycles();
                BbStats::gpu_latency_ns.fetch_add(bbport::CyclesToNs(done - item.second),
                                                  std::memory_order_relaxed);
                if (last_done) {
                    BbStats::gpu_done_gap_ns.fetch_add(bbport::CyclesToNs(done - last_done),
                                                       std::memory_order_relaxed);
                    BbStats::gpu_done_frames.fetch_add(1, std::memory_order_relaxed);
                }
                last_done = done;
            }
        });
    }
    if (watch_queue.size() < 16) {
        watch_queue.emplace_back(tick, submitted);
        watch_cv.notify_one();
    }
}

Frame* Presenter::PrepareBlankFrame(bool) {
    // No content: Present clears the drawable black. No scheduler work, so either thread may ask.
    Frame* const frame = pool->Take();
    frame->framegen.reset();
    frame->ready_tick = 0;
    return frame;
}

bool Presenter::SnapshotRequested() const {
    return !snapshot_trigger.empty() && access(snapshot_trigger.c_str(), F_OK) == 0;
}

std::string Presenter::TakeSnapshotRequest() {
    if (!SnapshotRequested()) {
        return {};
    }
    std::string path;
    if (FILE* file = std::fopen(snapshot_trigger.c_str(), "r")) {
        char line[1024]{};
        if (std::fgets(line, sizeof(line), file)) {
            path = line;
            while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) {
                path.pop_back();
            }
        }
        std::fclose(file);
    }
    std::remove(snapshot_trigger.c_str());
    return path.empty() ? snapshot_trigger + ".png" : path;
}

namespace {
/// bbport: frame pacing (bbport.ini frame_pacing, default off): the minimum time each frame stays
/// on screen, the frame cap's period. On variable-refresh displays (ProMotion) frames at a steady
/// cap otherwise alternate between neighbouring refresh slots (12.5 / 20.8 ms at 60 FPS on a
/// MacBook Pro); with it they are shown for one period each. It holds frames until their slot:
/// about one more frame of input latency (measured +23 ms at 60 FPS). Slightly under the period
/// (97%), so a frame on time is never held back to the next refresh.
double PacingSeconds() {
    if (!BbSettings::Get().frame_pacing.load(std::memory_order_relaxed)) {
        return 0;
    }
    const u32 fps = EmulatorSettings.GetFrameLimit();
    return fps ? 0.97 / fps : 0;
}
} // namespace

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame, u32) {
    last_present = {NowNs(), 0, 0, 0};
    const double now = BbFrameRates::Now();
    present_average.Frame(now);
    const bool framegen_active = BbFrameGen::Active();
    if (framegen_active) frame_rates->RealFrame(now, is_reusing_frame, is_game_frame);
    const auto readings = [&] {
        return framegen_active ? frame_rates->Read(BbFrameRates::Now()) : present_average.Read();
    };
    std::function<void(double)> on_displayed;
    if (framegen_active && (BbSettings::Get().show_fps || BbOverlay::CapturesInput())) {
        // Drawable callbacks can outlive the presenter. They own only the small
        // tracker, and never touch ImGui, the device, or a frame-pool slot.
        on_displayed = [rates = frame_rates](double timestamp) {
            rates->Displayed(timestamp, BbFrameRates::Now());
        };
    }
    // nextDrawable and the present passes create autoreleased objects, and this thread
    // (bb:Present) has no pool of its own: without one, drawables never return to the layer.
    Metal::WithAutoreleasePool([&] {
        // Minimised, hidden or occluded: nextDrawable would block up to 1 s. The frame returns
        // to the pool at once, so PrepareFrame's Take never waits on a skipped present.
        if (!Metal::ShouldAcquireDrawable(window.IsVisible(), window.GetWidth(),
                                          window.GetHeight(), SnapshotRequested())) {
            BbOverlay::Render(nullptr, nullptr, 0, 0, readings());
            pool->Free(frame);
            return;
        }
        if (const u32 wanted = window.IsFullscreen() ? fullscreen_drawables : windowed_drawables;
            wanted != layer_drawables) {
            Metal::SetMaximumDrawables(layer, wanted);
            layer_drawables = wanted;
            std::printf("Presenter: %u drawables (%s)\n", wanted,
                        window.IsFullscreen() ? "full screen" : "window");
        }
        Metal::Drawable drawable;
        std::shared_ptr<Frame> pair_owner;
        bool generated_presented = false;
        double duration = 0;
        // Submit the generated image before asking for the real image's drawable.
        // Direct scanout keeps drawables in use longer than the compositor. Holding
        // an unsubmitted real drawable while waiting for a second one exhausts the
        // three-buffer layer and inserts extra refresh intervals between pairs.
        Metal::WithAutoreleasePool([&] {
            drawable = Metal::NextDrawable(layer);
            if (!drawable.drawable) return;
            last_present.acquired = NowNs();
            const bool pair = framegen_active && !is_reusing_frame && frame->framegen &&
                frame->generated && frame->framegen->interpolated.load(std::memory_order_acquire);
            if (!pair) return;
            // Both callbacks pin the pool slot. If the real acquisition fails,
            // the generated callback still keeps its source from being overwritten.
            pair_owner = std::shared_ptr<Frame>(frame, [pool = pool](Frame* owned) {
                pool->Free(owned);
            });
            duration = BbFrameGen::PresentDuration(frame->framegen->description.interval,
                                                   EmulatorSettings.GetFrameLimit());
            void* const cb = present_queue->Begin();
            const auto tick = draw_scheduler.GetTimeline().DeviceTickOf(frame->ready_tick);
            ASSERT_MSG(tick.has_value(), "Generated frame tick was not committed");
            present_queue->WaitForDeviceTick(cb, *tick);
            pp_pass.Passes().Present(cb, frame->generated.Get(), frame->width, frame->height,
                drawable.texture, drawable.width, drawable.height);
            BbOverlay::Render(cb, drawable.texture, drawable.width, drawable.height, readings());
            present_queue->Commit(cb, drawable.drawable.Get(), [pair_owner] {}, duration,
                                  on_displayed);
            generated_presented = true;
            // Drain autoreleased Metal references too, before the next acquisition.
            drawable = {};
        });
        if (generated_presented) drawable = Metal::NextDrawable(layer);
        if (!drawable.drawable) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) {
                LOG_WARNING(Render_Vulkan, "No drawable from the layer: skipping frame(s)");
            }
            if (!generated_presented) {
                BbOverlay::Render(nullptr, nullptr, 0, 0, readings());
                pool->Free(frame);
            }
            return;
        }

        void* const command_buffer = present_queue->Begin();
        void* source = nullptr;
        if (frame->ready_tick != 0) {
            // PrepareFrame flushed the tick, so it has a device tick (0: already complete).
            const std::optional<u64> device_tick =
                draw_scheduler.GetTimeline().DeviceTickOf(frame->ready_tick);
            ASSERT_MSG(device_tick.has_value(), "Present: frame tick {} was not committed",
                       frame->ready_tick);
            present_queue->WaitForDeviceTick(command_buffer, *device_tick);
            source = frame->texture.Get();
        }
        // A blank frame clears the drawable. Every acquired drawable is presented: one that is
        // not returns to the layer only after about a display refresh.
        pp_pass.Passes().Present(command_buffer, source, frame->width, frame->height,
                                 drawable.texture, drawable.width, drawable.height);
        // The settings menu / FPS counter over the presented image, before the snapshot (which
        // shows it) and the commit. UI time advances once per real/generated pair;
        // the FPS readings come from independent render/presentation measurements.
        if (generated_presented) {
            BbOverlay::Replay(command_buffer, drawable.texture);
        } else {
            BbOverlay::Render(command_buffer, drawable.texture, drawable.width, drawable.height,
                              readings());
        }
        std::string snapshot_path = TakeSnapshotRequest();
        std::shared_ptr<Metal::Object> snapshot;
        if (!snapshot_path.empty()) {
            snapshot = std::make_shared<Metal::Object>(Metal::EncodeSnapshot(
                device, command_buffer, drawable.texture, drawable.width, drawable.height));
        }
        present_queue->Commit(
            command_buffer, drawable.drawable.Get(),
            [pool = pool, frame, pair_owner, snapshot, path = std::move(snapshot_path),
             width = drawable.width, height = drawable.height] {
                if (!pair_owner) pool->Free(frame);
                if (snapshot && *snapshot) {
                    // Off the completion thread: it is serial, and the PNG takes ~150 ms that
                    // later presents would wait behind.
                    std::thread([snapshot, path, width, height] {
                        const bool ok = Metal::WriteSnapshotPng(path.c_str(), snapshot->Get(),
                                                                width, height);
                        std::printf("Snapshot: %s %ux%u %s\n", path.c_str(), width, height,
                                    ok ? "written" : "FAILED");
                        std::fflush(stdout);
                    }).detach();
                }
            }, generated_presented ? duration : PacingSeconds(), on_displayed);
        last_present.submitted = NowNs();
        last_present.presented = last_present.submitted;
    });
    BbGpuCapture::OnPresent(device.NativeDevice()); // bbport: BB_GPU_CAPTURE one-frame capture

    if (!is_reusing_frame) {
        std::scoped_lock lock{pool->mutex};
        pool->last_presented = frame;
    }
    if (!is_reusing_frame && is_game_frame) {
        DebugState.IncFlipFrameNum();
    }
}

} // namespace Vulkan
