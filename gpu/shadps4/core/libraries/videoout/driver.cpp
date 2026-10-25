// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <cmath>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <SDL3/SDL_events.h>
#include <time.h>
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include "common/assert.h"
#include "bbport_toggles.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "common/debug.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/videoout/driver.h"
#include "core/libraries/videoout/videoout_error.h"
#include "imgui/renderer/imgui_core.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "bbport_split.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Vulkan {
extern std::atomic<u64> g_bb_compile_ns;
extern std::atomic<u32> g_bb_compiles;
} // namespace Vulkan

namespace Libraries::VideoOut {

constexpr static bool Is32BppPixelFormat(PixelFormat format) {
    switch (format) {
    case PixelFormat::A8R8G8B8Srgb:
    case PixelFormat::A8B8G8R8Srgb:
    case PixelFormat::A2R10G10B10:
    case PixelFormat::A2R10G10B10Srgb:
    case PixelFormat::A2R10G10B10Bt2020Pq:
        return true;
    default:
        return false;
    }
}

constexpr u32 PixelFormatBpp(PixelFormat pixel_format) {
    switch (pixel_format) {
    case PixelFormat::A16R16G16B16Float:
        return 8;
    default:
        return 4;
    }
}

VideoOutDriver::VideoOutDriver(u32 width, u32 height) {
    main_port.resolution.full_width = width;
    main_port.resolution.full_height = height;
    main_port.resolution.pane_width = width;
    main_port.resolution.pane_height = height;
    if (BbSplit::Remote()) {
        return; // bbport: presentation and vblanks happen in the GPU host (ApplyRemoteEvent)
    }
    const char* separate = std::getenv("BB_PRESENT_THREAD");
    separate_swap = !(separate && separate[0] == '0');
    if (separate_swap) {
        swap_thread = std::jthread([&](std::stop_token token) { SwapThread(token); });
    }
    present_thread = std::jthread([&](std::stop_token token) { PresentThread(token); });
}

void VideoOutDriver::RunPresenter(std::function<void()> work, bool if_idle) {
    if (!separate_swap) {
        work();
        return;
    }
    {
        std::scoped_lock lock{swap_mutex};
        if (if_idle && (swap_busy || !swap_queue.empty())) {
            return; // a redraw of the last frame is pointless while frames are queued
        }
        swap_queue.push_back(std::move(work));
    }
    swap_cv.notify_one();
}

void VideoOutDriver::SwapThread(std::stop_token token) {
    Common::SetCurrentThreadName("bb:Present");
    while (true) {
        std::function<void()> work;
        {
            std::unique_lock lock{swap_mutex};
            swap_busy = false;
            if (!swap_cv.wait(lock, token, [this] { return !swap_queue.empty(); })) {
                return;
            }
            work = std::move(swap_queue.front());
            swap_queue.pop_front();
            swap_busy = true;
        }
        work();
    }
}

VideoOutDriver::~VideoOutDriver() = default;

int VideoOutDriver::Open(const ServiceThreadParams* params) {
    if (main_port.is_open) {
        return ORBIS_VIDEO_OUT_ERROR_RESOURCE_BUSY;
    }
    main_port.is_open = true;
    if (liverpool) {
        liverpool->SetVoPort(&main_port);
    }
    return 1;
}

void VideoOutDriver::Close(s32 handle) {
    std::scoped_lock lock{mutex};

    // Mark as closed
    main_port.is_open = false;
    main_port.flip_rate = 0;
    main_port.prev_index = -1;

    // Clear port information
    std::memset(main_port.buffer_labels.data(), 0, sizeof(main_port.buffer_labels));
    std::memset(main_port.groups.data(), 0, sizeof(main_port.groups));
    std::memset(&main_port.vblank_status, 0, sizeof(main_port.vblank_status));
    main_port.flip_status = FlipStatus{};

    // Re-initialize buffers
    std::memset(main_port.buffer_slots.data(), 0, sizeof(main_port.buffer_slots));
    for (auto& buffer : main_port.buffer_slots) {
        buffer.group_index = -1;
    }

    // Clear events
    for (auto event : main_port.flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                                Kernel::OrbisKernelEvent::Filter::VideoOut);
        }
    }
    main_port.flip_events.clear();
    for (auto event : main_port.vblank_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->RemoveEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                                Kernel::OrbisKernelEvent::Filter::VideoOut);
        }
    }
    main_port.vblank_events.clear();
}

VideoOutPort* VideoOutDriver::GetPort(int handle) {
    if (handle != 1) [[unlikely]] {
        return nullptr;
    }
    return &main_port;
}

int VideoOutDriver::RegisterBuffers(VideoOutPort* port, s32 startIndex, void* const* addresses,
                                    s32 bufferNum, const BufferAttribute* attribute) {
    const s32 group_index = port->FindFreeGroup();
    if (group_index >= MaxDisplayBufferGroups) {
        return ORBIS_VIDEO_OUT_ERROR_NO_EMPTY_SLOT;
    }

    if (startIndex + bufferNum > MaxDisplayBuffers || startIndex > MaxDisplayBuffers ||
        bufferNum > MaxDisplayBuffers) {
        LOG_ERROR(Lib_VideoOut,
                  "Attempted to register too many buffers startIndex = {}, bufferNum = {}",
                  startIndex, bufferNum);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    const s32 end_index = startIndex + bufferNum;
    if (bufferNum > 0 &&
        std::any_of(port->buffer_slots.begin() + startIndex, port->buffer_slots.begin() + end_index,
                    [](auto& buffer) { return buffer.group_index != -1; })) {
        return ORBIS_VIDEO_OUT_ERROR_SLOT_OCCUPIED;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "startIndex = {}, bufferNum = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             startIndex, bufferNum, GetPixelFormatString(attribute->pixel_format),
             attribute->aspect_ratio, static_cast<u32>(attribute->tiling_mode), attribute->width,
             attribute->height, attribute->pitch_in_pixel, attribute->option);

    auto& group = port->groups[group_index];
    std::memcpy(&group.attrib, attribute, sizeof(BufferAttribute));
    group.is_occupied = true;

    for (u32 i = 0; i < bufferNum; i++) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(addresses[i]);
        port->buffer_slots[startIndex + i] = VideoOutBuffer{
            .group_index = group_index,
            .address_left = address,
            .address_right = 0,
        };
        // Reset flip label also when registering buffer
        port->buffer_labels[startIndex + i] = 0;
        port->SignalVoLabel();

        if (!BbSplit::Remote()) {
            Vulkan::FrameCapture::AddDisplayBuffer(address);
            presenter->RegisterVideoOutSurface(group, address);
        }
        LOG_INFO(Lib_VideoOut, "buffers[{}] = {:#x}", i + startIndex, address);
    }

    return group_index;
}

int VideoOutDriver::UnregisterBuffers(VideoOutPort* port, s32 attributeIndex) {
    if (attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    auto& group = port->groups[attributeIndex];
    group.is_occupied = false;

    for (auto& buffer : port->buffer_slots) {
        if (buffer.group_index != attributeIndex) {
            continue;
        }
        buffer.group_index = -1;
    }

    return ORBIS_OK;
}

int VideoOutDriver::ChangeBufferAttribute(VideoOutPort* port, s32 attributeIndex,
                                          const BufferAttribute* attribute) {
    if (attributeIndex >= MaxDisplayBufferGroups || !port->groups[attributeIndex].is_occupied) {
        LOG_ERROR(Lib_VideoOut, "Invalid attribute index {}", attributeIndex);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    if (attribute->reserved0 != 0 || attribute->reserved1 != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid reserved members");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    if (attribute->aspect_ratio != 0) {
        LOG_ERROR(Lib_VideoOut, "Invalid aspect ratio = {}", attribute->aspect_ratio);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ASPECT_RATIO;
    }
    if (attribute->width > attribute->pitch_in_pixel) {
        LOG_ERROR(Lib_VideoOut, "Buffer width {} is larger than pitch {}", attribute->width,
                  attribute->pitch_in_pixel);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_PITCH;
    }
    if (attribute->tiling_mode < TilingMode::Tile || attribute->tiling_mode > TilingMode::Linear) {
        LOG_ERROR(Lib_VideoOut, "Invalid tilingMode = {}",
                  static_cast<u32>(attribute->tiling_mode));
        return ORBIS_VIDEO_OUT_ERROR_INVALID_TILING_MODE;
    }

    LOG_INFO(Lib_VideoOut,
             "attributeIndex = {}, pixelFormat = {}, aspectRatio = {}, "
             "tilingMode = {}, width = {}, height = {}, pitchInPixel = {}, option = {:#x}",
             attributeIndex, GetPixelFormatString(attribute->pixel_format), attribute->aspect_ratio,
             static_cast<u32>(attribute->tiling_mode), attribute->width, attribute->height,
             attribute->pitch_in_pixel, attribute->option);

    std::unique_lock lock{port->port_mutex};
    std::memcpy(&port->groups[attributeIndex].attrib, attribute, sizeof(BufferAttribute));
    return 0;
}

// bbport: frame spike log (BB_FRAME_STATS=1). Each frame carries timestamps from the game's
// submit to the display (VK_GOOGLE_display_timing); a frame that comes later than 1.25 frame
// periods after the previous one at any stage (BB_SPIKE_MS overrides) prints a "Spike" line
// with every stage and what the GPU process did meanwhile, so judder can be traced to a stage.
namespace {

u64 NowNs() {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}

struct GameFlip {
    s64 flip_arg;
    u64 game_ns, arrival_ns;
};
std::mutex game_flips_mutex;
std::deque<GameFlip> game_flips;

/// Pops the game-side times of the flip with this argument (older unmatched ones are dropped).
void TakeGameFlip(s64 flip_arg, u64* game_ns, u64* arrival_ns) {
    std::scoped_lock lk{game_flips_mutex};
    while (!game_flips.empty()) {
        const GameFlip f = game_flips.front();
        game_flips.pop_front();
        if (f.flip_arg == flip_arg) {
            *game_ns = f.game_ns;
            *arrival_ns = f.arrival_ns;
            return;
        }
    }
}

struct FrameTrace {
    u32 id;
    u64 game_ns, arrival_ns, flip_packet_ns, queued_ns, flip_ns, display_ns;
    Vulkan::Presenter::PresentTiming present;
    // What the GPU process did between the previous flip and this one.
    double gpu_cpu_ms, rec_busy_ms, rec_idle_ms, ahead_ms, drain_ms, host_copies_ms,
        compile_wait_ms, game_fault_ms, game_main_cpu_ms, game_flip_cpu_ms, game_cpu_ms,
        game_busiest_ms;
    char game_busiest[32];
    u64 preempted, draws, compiles, gpu_done, game_faults;
    // Cumulative at the flip, for the benchmark's per-frame deltas.
    u64 gpu_busy_ns, upload_bytes;
};

/// Counter deltas between flips. Counters the 5 s summary resets are re-based by Rebase().
struct SpikeCounters {
    enum : u32 { GpuCpu, Preempted, Draws, RecBusy, RecIdle, Ahead, Drain, HostCopies,
                 CompileWait, Compiles, GpuDone, GameFaults, GameFaultNs, GameMainCpu,
                 GameFlipCpu, Count };
    u64 last[Count]{};
    static void Read(u64* v) {
        v[GpuCpu] = BbStats::gpu_user_us.load() + BbStats::gpu_sys_us.load();
        v[Preempted] = BbStats::gpu_invol_switches.load();
        v[Draws] = BbStats::draws.load();
        v[RecBusy] = BbStats::rec_busy_ns.load();
        v[RecIdle] = BbStats::rec_idle_ns.load();
        v[Ahead] = BbStats::frames_ahead_ns.load();
        v[Drain] = BbStats::drain_ns.load();
        v[HostCopies] = BbStats::host_copies_wait_ns.load();
        v[CompileWait] = BbStats::compile_wait_ns.load();
        v[Compiles] = Vulkan::g_bb_compiles.load();
        v[GpuDone] = BbStats::gpu_done_frames.load();
        BbGpuShared* shared = BbSplit::Host() ? BbSplit::Shared() : nullptr;
        v[GameFaults] = shared ? __atomic_load_n(&shared->game_faults, __ATOMIC_RELAXED) : 0;
        v[GameFaultNs] = shared ? __atomic_load_n(&shared->game_fault_ns, __ATOMIC_RELAXED) : 0;
        v[GameMainCpu] = shared ? __atomic_load_n(&shared->game_main_cpu_ns, __ATOMIC_RELAXED) : 0;
        v[GameFlipCpu] = shared ? __atomic_load_n(&shared->game_flip_cpu_ns, __ATOMIC_RELAXED) : 0;
    }
    void Fill(FrameTrace& t) {
        u64 now[Count];
        Read(now);
        u64 d[Count];
        for (u32 i = 0; i < Count; ++i) {
            d[i] = now[i] >= last[i] ? now[i] - last[i] : now[i];
            last[i] = now[i];
        }
        t.gpu_cpu_ms = d[GpuCpu] / 1e3;
        t.rec_busy_ms = d[RecBusy] / 1e6;
        t.rec_idle_ms = d[RecIdle] / 1e6;
        t.ahead_ms = d[Ahead] / 1e6;
        t.drain_ms = d[Drain] / 1e6;
        t.host_copies_ms = d[HostCopies] / 1e6;
        t.compile_wait_ms = d[CompileWait] / 1e6;
        t.preempted = d[Preempted];
        t.draws = d[Draws];
        t.compiles = d[Compiles];
        t.gpu_done = d[GpuDone];
        t.game_faults = d[GameFaults];
        t.game_fault_ms = d[GameFaultNs] / 1e6;
        t.game_main_cpu_ms = d[GameMainCpu] / 1e6;
        t.game_flip_cpu_ms = d[GameFlipCpu] / 1e6;
        // Per-frame values from the game's last flip submit (not counters).
        BbGpuShared* shared = BbSplit::Host() ? BbSplit::Shared() : nullptr;
        t.game_cpu_ms = shared ? __atomic_load_n(&shared->game_frame_cpu_ns, __ATOMIC_ACQUIRE) / 1e6 : 0;
        t.game_busiest_ms = shared ? __atomic_load_n(&shared->game_busiest_ns, __ATOMIC_RELAXED) / 1e6 : 0;
        if (shared) {
            std::memcpy(t.game_busiest, shared->game_busiest_name, sizeof(t.game_busiest));
            t.game_busiest[sizeof(t.game_busiest) - 1] = 0;
        } else {
            t.game_busiest[0] = 0;
        }
    }
    /// After the summary exchanged its counters to zero.
    void Rebase() {
        Read(last);
    }
};

std::atomic<u32> spikes_game, spikes_flip, spikes_present, spikes_display, spikes_printed,
    frames_not_displayed;
/// vkQueuePresent to the screen (display timing), for the 5 s summary.
std::atomic<u64> latency_sum_ns, latency_count, latency_max_ns;

/// MoltenVK reports Metal's presentedTime: the uptime clock, which stops while the Mac sleeps;
/// the steady clock does not. 0 (not displayed) stays 0.
u64 DisplayTimeToSteady(u64 display_time) {
    if (!display_time) {
        return 0;
    }
#ifdef __APPLE__
    return display_time + (NowNs() - clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
#else
    return display_time;
#endif
}

/// Benchmark (BB_BENCHMARK, docs/DESIGN_BENCHMARK.md): one CSV row per presented frame while
/// the game's phase is "measuring" (BB_BENCH_OUT); at "done" or "failed" the footer, then the
/// window closes and the game exits. Runs on the presenting thread.
class BenchRecorder {
public:
    void OnFrame(const FrameTrace& t, const FrameTrace& prev, double frame_ms, double game_ms) {
        static const char* path = std::getenv("BB_BENCH_OUT");
        BbGpuShared* shared = BbSplit::Host() ? BbSplit::Shared() : nullptr;
        if (!path || !*path || !shared || finished) {
            return;
        }
        const u32 phase = __atomic_load_n(&shared->bench_phase, __ATOMIC_ACQUIRE);
        if (phase == 0) {
            return;
        }
        if (!file) {
            file = std::fopen(path, "w");
            if (!file) {
                std::printf("Benchmark: cannot write %s\n", path);
                finished = true;
                return;
            }
            const char* pass = std::getenv("BB_BENCHMARK");
            std::fprintf(file, "# bloodborne-benchmark 1\n# pass=%s\n# started=%.3f\n",
                         pass ? pass : "", UnixSeconds());
            std::fprintf(file, "t_ms,frame_ms,game_ms,gpu_ms,draws,compiles,compile_wait_ms,"
                               "upload_mb,game_cpu_ms,gpu_thread_ms,ahead_ms,game_faults\n");
            start_ns = t.present.presented;
            std::printf("Benchmark: measuring (%s)\n", path);
        }
        if (phase == 1) {
            const auto delta = [](u64 a, u64 b) { return a >= b ? a - b : 0; };
            std::fprintf(file, "%.2f,%.2f,%.2f,%.2f,%llu,%llu,%.2f,%.2f,%.2f,%.2f,%.2f,%llu\n",
                         (double(t.present.presented) - double(start_ns)) / 1e6, frame_ms, game_ms,
                         delta(t.gpu_busy_ns, prev.gpu_busy_ns) / 1e6,
                         static_cast<unsigned long long>(t.draws),
                         static_cast<unsigned long long>(t.compiles), t.compile_wait_ms,
                         delta(t.upload_bytes, prev.upload_bytes) / 1e6, t.game_cpu_ms,
                         t.gpu_cpu_ms, t.ahead_ms, static_cast<unsigned long long>(t.game_faults));
            return;
        }
        char error[sizeof(shared->bench_error)];
        std::memcpy(error, shared->bench_error, sizeof(error));
        error[sizeof(error) - 1] = 0;
        std::fprintf(file, "# ended=%.3f\n", UnixSeconds());
        if (phase == 2) {
            std::fprintf(file, "# result=ok\n");
        } else {
            std::fprintf(file, "# result=error: %s\n", error[0] ? error : "unknown");
        }
        std::fclose(file);
        file = nullptr;
        finished = true;
        std::printf("Benchmark: %s; closing the window\n", phase == 2 ? "done" : error);
        std::fflush(stdout);
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
    }

private:
    static double UnixSeconds() {
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    std::FILE* file = nullptr;
    u64 start_ns = 0;
    bool finished = false;
};

BenchRecorder bench_recorder;

/// Runs on the presenting thread: matches display times to frames and reports late ones.
class SpikeLog {
public:
    void OnPresented(const std::shared_ptr<FrameTrace>& trace,
                     const std::vector<vk::PastPresentationTimingGOOGLE>& timings) {
        if (!display_timing) {
            Evaluate(*trace);
            return;
        }
        pending[trace->id % pending.size()] = trace;
        for (const auto& timing : timings) {
            auto& entry = pending[timing.presentID % pending.size()];
            if (!entry || entry->id != timing.presentID) {
                continue;
            }
            entry->display_ns = DisplayTimeToSteady(timing.actualPresentTime);
            if (timing.actualPresentTime == 0) {
                frames_not_displayed.fetch_add(1, std::memory_order_relaxed);
            }
            Evaluate(*entry);
            entry.reset();
        }
    }

    bool display_timing = false;

private:
    void Evaluate(const FrameTrace& t) {
        if (t.display_ns > t.present.presented && t.present.presented) {
            const u64 latency = t.display_ns - t.present.presented;
            latency_sum_ns.fetch_add(latency, std::memory_order_relaxed);
            latency_count.fetch_add(1, std::memory_order_relaxed);
            if (latency > latency_max_ns.load(std::memory_order_relaxed)) {
                latency_max_ns.store(latency, std::memory_order_relaxed);
            }
        }
        if (!have_prev) {
            prev = t;
            have_prev = true;
            return;
        }
        static const double limit_ms = [] {
            if (const char* env = std::getenv("BB_SPIKE_MS"); env && std::atof(env) > 0) {
                return std::atof(env);
            }
            const u32 limit = EmulatorSettings.GetFrameLimit();
            const u32 hz = limit ? limit : EmulatorSettings.GetVblankFrequency();
            return 1.25e3 / std::max(1u, hz);
        }();
        const auto gap = [](u64 a, u64 b) { return a && b ? (double(a) - double(b)) / 1e6 : 0.0; };
        const double game = gap(t.game_ns, prev.game_ns);
        const double flip = gap(t.flip_ns, prev.flip_ns);
        const double present = gap(t.present.presented, prev.present.presented);
        const double display = gap(t.display_ns, prev.display_ns);
        bench_recorder.OnFrame(t, prev, display > 0 ? display : present, game);
        const bool late_game = game > limit_ms, late_flip = flip > limit_ms,
                   late_present = present > limit_ms, late_display = display > limit_ms;
        spikes_game += late_game;
        spikes_flip += late_flip;
        spikes_present += late_present;
        spikes_display += late_display;
        if ((late_game || late_flip || late_present || late_display) &&
            spikes_printed.fetch_add(1, std::memory_order_relaxed) < 20) {
            const auto mark = [](bool late) { return late ? "!" : ""; };
            std::printf(
                "Spike #%u (%u frames after #%u), ms apart: game %.1f%s, flip %.1f%s, present "
                "%.1f%s, display %.1f%s | game->host %.2f, host->flip packet %.1f, frame prep "
                "%.1f, queued %.1f, flip->presented %.1f (acquire %.1f, submit %.1f, "
                "vkQueuePresent %.1f), presented->display %.1f | GPU thread on CPU %.1f ms, %llu "
                "preempted, %llu draws; recording busy %.1f idle %.1f, frames-ahead wait %.1f, "
                "recorder waits %.1f, host copies %.1f; %llu GPU frames done; %llu compiles "
                "(%.1f ms waited) | game: %llu GPU page faults, %.1f ms waited; on CPU: main "
                "thread %.1f ms, flip thread %.1f ms, all threads %.1f ms, busiest %s %.1f ms\n",
                t.id, t.id - prev.id, prev.id, game, mark(late_game), flip, mark(late_flip),
                present, mark(late_present), display, mark(late_display),
                gap(t.arrival_ns, t.game_ns), gap(t.flip_packet_ns, t.arrival_ns),
                gap(t.queued_ns, t.flip_packet_ns), gap(t.flip_ns, t.queued_ns),
                gap(t.present.presented, t.flip_ns), gap(t.present.acquired, t.present.start),
                gap(t.present.submitted, t.present.acquired),
                gap(t.present.presented, t.present.submitted),
                gap(t.display_ns, t.present.presented), t.gpu_cpu_ms,
                static_cast<unsigned long long>(t.preempted),
                static_cast<unsigned long long>(t.draws), t.rec_busy_ms, t.rec_idle_ms,
                t.ahead_ms, t.drain_ms, t.host_copies_ms,
                static_cast<unsigned long long>(t.gpu_done),
                static_cast<unsigned long long>(t.compiles), t.compile_wait_ms,
                static_cast<unsigned long long>(t.game_faults), t.game_fault_ms,
                t.game_main_cpu_ms, t.game_flip_cpu_ms, t.game_cpu_ms, t.game_busiest,
                t.game_busiest_ms);
        }
        prev = t;
    }

    std::array<std::shared_ptr<FrameTrace>, 128> pending;
    FrameTrace prev{};
    bool have_prev = false;
};

SpikeCounters spike_counters;
SpikeLog spike_log;
u32 next_trace_id = 1;

} // namespace

void NoteGameFlip(s64 flip_arg, u32 game_us) {
    if (!BbStats::enabled) {
        return;
    }
    const u64 now = NowNs();
    const u32 latency_us = static_cast<u32>(now / 1000) - game_us;
    std::scoped_lock lk{game_flips_mutex};
    game_flips.push_back({flip_arg, now - u64(latency_us) * 1000, now});
    if (game_flips.size() > 32) {
        game_flips.pop_front();
    }
}

constexpr size_t FT_HISTORY_CAP = 1024;
static std::atomic<size_t> s_ft_count{0};
static double s_ft_history[FT_HISTORY_CAP];
static std::chrono::steady_clock::time_point s_last_flip_pt;

void RecordFlipFrameTime() {
    auto now = std::chrono::steady_clock::now();
    if (s_last_flip_pt.time_since_epoch().count() != 0) {
        double ms = std::chrono::duration<double, std::milli>(now - s_last_flip_pt).count();
        size_t idx = s_ft_count.fetch_add(1, std::memory_order_relaxed);
        s_ft_history[idx % FT_HISTORY_CAP] = ms;
    }
    s_last_flip_pt = now;
}

void GetFrametimeStats(double* avg_fps, double* p95_ms, double* p99_ms) {
    size_t count = s_ft_count.load(std::memory_order_relaxed);
    if (count == 0) {
        if (p95_ms) *p95_ms = 0.0;
        if (p99_ms) *p99_ms = 0.0;
        return;
    }
    size_t n = std::min(count, FT_HISTORY_CAP);
    std::vector<double> samples;
    samples.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        samples.push_back(s_ft_history[i]);
    }
    std::sort(samples.begin(), samples.end());
    if (p95_ms) {
        size_t idx95 = (n * 95) / 100;
        if (idx95 >= n) idx95 = n - 1;
        *p95_ms = samples[idx95];
    }
    if (p99_ms) {
        size_t idx99 = (n * 99) / 100;
        if (idx99 >= n) idx99 = n - 1;
        *p99_ms = samples[idx99];
    }
}

void VideoOutDriver::Flip(const Request& req) {
    // bbport: frame spike log: this frame's stages so far, and what the GPU process did since
    // the previous flip; the presenting thread adds the present and display times.
    std::shared_ptr<FrameTrace> trace;
    if (BbStats::enabled) {
        trace = std::make_shared<FrameTrace>();
        trace->id = next_trace_id++;
        trace->game_ns = req.game_ns;
        trace->arrival_ns = req.arrival_ns;
        trace->flip_packet_ns = req.flip_packet_ns;
        trace->queued_ns = req.queued_ns;
        trace->flip_ns = NowNs();
        spike_counters.Fill(*trace);
        trace->gpu_busy_ns = presenter->GetDevice().GpuBusyNs();
        trace->upload_bytes = BbStats::image_upload_bytes.load(std::memory_order_relaxed) +
                              BbStats::buffer_upload_bytes.load(std::memory_order_relaxed);
        if (BbSplit::Host()) {
            // Benchmark: the game waits for gameplay (thousands of draws a frame).
            __atomic_store_n(&BbSplit::Shared()->bench_frame_draws, u32(trace->draws),
                             __ATOMIC_RELAXED);
        }
        spike_log.display_timing = presenter->IsDisplayTimingSupported();
    }
    // Update HDR status before presenting, then present the frame (bbport: on the swap thread).
    RunPresenter([this, frame = req.frame, hdr = req.port->is_hdr, trace] {
        presenter->SetHDR(hdr);
        presenter->Present(frame, false, true, trace ? trace->id : 0);
        if (trace) {
            trace->present = presenter->LastPresentTiming();
            spike_log.OnPresented(trace, presenter->TakePastTimings());
        }
    });

    Vulkan::FrameCapture::OnFlip(req.index >= 0 ? req.port->buffer_slots[req.index].address_left
                                                : 0);
    BbStats::flips.fetch_add(1, std::memory_order_relaxed);
    RecordFlipFrameTime();

    // bbport: BB_FRAME_STATS=1 prints flip rate and frame time spread every 5 seconds.
    static const bool frame_stats = EmulatorSettingsImpl::Flag("BB_FRAME_STATS", false);
    if (frame_stats) {
        using Clock = std::chrono::steady_clock;
        static Clock::time_point window_start = Clock::now(), last = window_start;
        static u32 frames;
        static double worst_ms;
        const auto now = Clock::now();
        const double frame_ms = std::chrono::duration<double, std::milli>(now - last).count();
        worst_ms = std::max(worst_ms, frame_ms);
        static std::vector<double> intervals;
        intervals.push_back(frame_ms);
        last = now;
        // Stall diagnostics: what happened during a long frame.
        static u64 last_gpu_ns, last_images, last_image_bytes, last_buffer_bytes;
        static u64 last_t[6], last_minflt, last_sigf, last_pc, last_pp, last_rc, last_rp;
        const u64 pc = BbStats::protect_calls.load(), pp = BbStats::protect_pages.load(),
                  rc = BbStats::protect_revoke_calls.load(), rp = BbStats::protect_revoke_pages.load();
        const u64 minflt = BbStats::gpu_minor_faults.load(), sigf = BbStats::gpu_signal_faults.load();
        static u64 last_copy_cpu, last_copy_sys, last_copy_flt;
        const u64 copy_cpu = BbStats::t_copy_cpu.load(), copy_sys = BbStats::copy_sys_us.load(),
                  copy_flt = BbStats::copy_minflt.load();
        static u64 last_proc_flt, last_copy_ns, last_copy_bytes, last_rf, last_trf, last_twf;
        const u64 rf = BbStats::read_faults.load(), trf = BbStats::t_read_faults.load(),
                  twf = BbStats::t_write_faults.load();
        if (frame_ms > 40.0 && last_gpu_ns != 0) {
            std::printf("       fault handlers: %llu read faults %.1f ms, write faults %.1f ms\n",
                        static_cast<unsigned long long>(rf - last_rf), (trf - last_trf) / 1e6,
                        (twf - last_twf) / 1e6);
        }
        last_rf = rf;
        last_trf = trf;
        last_twf = twf;
        const u64 copy_ns = BbStats::t_copy.load(), copy_bytes = BbStats::copy_bytes.load();
        u64 proc_flt = 0;
#ifndef _WIN32
        if (rusage usage{}; getrusage(RUSAGE_SELF, &usage) == 0) {
            proc_flt = usage.ru_minflt;
        }
#endif
        const u64 t_now[6] = {BbStats::t_resident.load(), BbStats::t_protect.load(),
                              BbStats::t_image_create.load(), BbStats::t_refresh.load(),
                              BbStats::t_staging.load(), BbStats::t_host_wait.load()};
        static u64 last_draws, last_dispatches, last_subs, last_sys, last_user, last_invol, last_vol;
        const u64 draws = BbStats::draws.load(), dispatches = BbStats::dispatches.load(),
                  subs = BbStats::submissions.load(), sys_us = BbStats::gpu_sys_us.load(),
                  user_us = BbStats::gpu_user_us.load(), invol = BbStats::gpu_invol_switches.load(),
                  vol = BbStats::gpu_vol_switches.load();
        u64 gpu_ns = 0;
#ifndef _WIN32
        if (const int clock = BbStats::gpu_thread_clock.load(); clock != -1) {
            timespec ts{};
            clock_gettime(static_cast<clockid_t>(clock), &ts);
            gpu_ns = u64(ts.tv_sec) * 1000000000ull + u64(ts.tv_nsec);
        }
#endif
#ifdef __APPLE__
        gpu_ns = (sys_us + user_us) * 1000;
#endif
        const u64 images = BbStats::images_registered.load();
        const u64 image_bytes = BbStats::image_upload_bytes.load();
        const u64 buffer_bytes = BbStats::buffer_upload_bytes.load();
        if (frame_ms > 40.0 && last_gpu_ns != 0) {
            std::printf("Stall: %.1f ms frame; GPU thread on CPU %.1f ms; %llu images registered, "
                        "%.1f MB image uploads, %.1f MB buffer uploads\n",
                        frame_ms, (gpu_ns - last_gpu_ns) / 1e6,
                        static_cast<unsigned long long>(images - last_images),
                        (image_bytes - last_image_bytes) / 1e6,
                        (buffer_bytes - last_buffer_bytes) / 1e6);
            std::printf("       %llu draws, %llu dispatches, %llu submissions; GPU thread user %.1f ms, "
                        "kernel %.1f ms, %llu preempted, %llu waits\n",
                        static_cast<unsigned long long>(draws - last_draws),
                        static_cast<unsigned long long>(dispatches - last_dispatches),
                        static_cast<unsigned long long>(subs - last_subs), (user_us - last_user) / 1e3,
                        (sys_us - last_sys) / 1e3, static_cast<unsigned long long>(invol - last_invol),
                        static_cast<unsigned long long>(vol - last_vol));
        }
        if (frame_ms > 40.0 && last_gpu_ns != 0) {
            std::printf("       ms in: resident %.1f, protect %.1f, image create %.1f, "
                        "image refresh %.1f, staging %.1f, waiting for host copies %.1f\n",
                        (t_now[0] - last_t[0]) / 1e6, (t_now[1] - last_t[1]) / 1e6,
                        (t_now[2] - last_t[2]) / 1e6, (t_now[3] - last_t[3]) / 1e6,
                        (t_now[4] - last_t[4]) / 1e6, (t_now[5] - last_t[5]) / 1e6);
            std::printf("       GPU thread page faults %llu (process %llu), protection faults %llu; "
                        "protect calls %llu (%llu pages), of which write-revoking %llu (%llu pages)\n",
                        static_cast<unsigned long long>(minflt - last_minflt),
                        static_cast<unsigned long long>(proc_flt - last_proc_flt),
                        static_cast<unsigned long long>(sigf - last_sigf),
                        static_cast<unsigned long long>(pc - last_pc),
                        static_cast<unsigned long long>(pp - last_pp),
                        static_cast<unsigned long long>(rc - last_rc),
                        static_cast<unsigned long long>(rp - last_rp));
        }
        last_pc = pc;
        last_pp = pp;
        last_rc = rc;
        last_rp = rp;
        last_minflt = minflt;
        if (frame_ms > 40.0 && last_gpu_ns != 0 && copy_ns > last_copy_ns) {
            std::printf("       guest copies %.1f MB in %.1f thread-ms, %.1f ms on CPU (kernel %.1f ms, "
                        "%llu page faults in large copies) (%.2f GB/s per thread)\n",
                        (copy_bytes - last_copy_bytes) / 1e6, (copy_ns - last_copy_ns) / 1e6,
                        (copy_cpu - last_copy_cpu) / 1e6, (copy_sys - last_copy_sys) / 1e3,
                        static_cast<unsigned long long>(copy_flt - last_copy_flt),
                        double(copy_bytes - last_copy_bytes) / double(copy_ns - last_copy_ns));
        }
        last_copy_ns = copy_ns;
        last_copy_cpu = copy_cpu;
        last_copy_sys = copy_sys;
        last_copy_flt = copy_flt;
        last_copy_bytes = copy_bytes;
        last_proc_flt = proc_flt;
        last_sigf = sigf;
        std::copy(std::begin(t_now), std::end(t_now), std::begin(last_t));
        last_draws = draws;
        last_dispatches = dispatches;
        last_subs = subs;
        last_sys = sys_us;
        last_user = user_us;
        last_invol = invol;
        last_vol = vol;
        last_gpu_ns = gpu_ns;
        last_images = images;
        last_image_bytes = image_bytes;
        last_buffer_bytes = buffer_bytes;
        ++frames;
        const double window = std::chrono::duration<double>(now - window_start).count();
        if (window >= 5.0) {
            const u32 compiles = Vulkan::g_bb_compiles.exchange(0);
            const u64 compile_ns = Vulkan::g_bb_compile_ns.exchange(0);
            const u64 direct = Vulkan::Scheduler::direct_recordings.exchange(0);
            const u64 faults = BbStats::tracker_faults.exchange(0);
            // GPU command thread CPU time per draw: comparable between builds even when the scene
            // (and so the frame rate) differs a little.
            static u64 window_draws = 0, window_gpu_us = 0;
            const u64 all_draws = BbStats::draws.load();
            const u64 gpu_us = BbStats::gpu_user_us.load() + BbStats::gpu_sys_us.load();
            const double us_per_draw = all_draws > window_draws
                                           ? double(gpu_us - window_gpu_us) / (all_draws - window_draws)
                                           : 0.0;
            const double draws_per_frame = frames ? double(all_draws - window_draws) / frames : 0.0;
            window_draws = all_draws;
            window_gpu_us = gpu_us;
            // GPU busy time (all command buffers of the Metal device, both queues) per frame.
            static u64 window_busy_ns = 0;
            const u64 busy_ns = presenter ? presenter->GetDevice().GpuBusyNs() : 0;
            const double gpu_ms = frames ? (busy_ns - window_busy_ns) / (frames * 1e6) : 0.0;
            const double gpu_share = (busy_ns - window_busy_ns) / (window * 1e7);
            window_busy_ns = busy_ns;
            std::printf("Frame stats: %.1f FPS, worst frame %.1f ms (vblank %u Hz); "
                        "%u shader/pipeline compiles, %.1f ms; %llu recorder syncs; "
                        "%.0f write faults/s, %lld hot pages; GPU thread %.2f us/draw, "
                        "%.0f draws/frame, idle %.1f%%; blocked: recorder %.1f%%, host copies "
                        "%.1f%% (%.0f/frame), copy threads %.1f%%, GPU ticks %.1f%%; "
                        "reduced-size draws %.0f/frame of %.0f in the scene; GPU busy %.2f "
                        "ms/frame (%.1f%%)\n",
                        frames / window, worst_ms, EmulatorSettings.GetVblankFrequency(), compiles,
                        compile_ns / 1e6, static_cast<unsigned long long>(direct),
                        faults / window, static_cast<long long>(BbStats::hot_pages.load()),
                        us_per_draw, draws_per_frame,
                        BbStats::gpu_idle_ns.exchange(0) / (window * 1e7),
                        BbStats::sync_recording_ns.exchange(0) / (window * 1e7),
                        BbStats::host_copies_wait_ns.exchange(0) / (window * 1e7),
                        frames ? double(BbStats::host_copy_waits.exchange(0)) / frames : 0.0,
                        BbStats::copy_threads_wait_ns.exchange(0) / (window * 1e7),
                        BbStats::tick_wait_ns.exchange(0) / (window * 1e7),
                        frames ? double(BbStats::reduced_draws.exchange(0)) / frames : 0.0,
                        frames ? double(BbStats::scene_draws.exchange(0)) / frames : 0.0, gpu_ms,
                        gpu_share);
            // Texture-cache collector input: Metal's allocation minus the resident guest
            // buffers (the arena) against the trigger.
            // And where this session's pipelines came from (binary archive or compiler).
            u64 archive_hits = 0, compiled = 0;
            if (presenter) {
                const auto& psos = presenter->GetRasterizer().GetPipelineCache().Psos().GetCounters();
                archive_hits = psos.archive_hits.load();
                compiled = psos.compiled.load();
            }
            std::printf("GPU memory: %.0f MiB allocated, %.0f MiB resident buffers; collector "
                        "usage %.0f MiB, trigger %.0f MiB, %llu images freed; pipelines: %llu "
                        "from the binary archive, %llu compiled\n",
                        BbStats::gc_allocated_bytes.load() / 1048576.0,
                        BbStats::gc_resident_bytes.load() / 1048576.0,
                        BbStats::gc_used_bytes.load() / 1048576.0,
                        BbStats::gc_trigger_bytes.load() / 1048576.0,
                        static_cast<unsigned long long>(BbStats::gc_freed_images.load()),
                        static_cast<unsigned long long>(archive_hits),
                        static_cast<unsigned long long>(compiled));
            // bbport: lazy texture usages (VideoCore::Image::allowed_shader_write).
            if (const u64 upgrades = BbStats::texture_upgrades.exchange(0)) {
                std::printf("Texture usage upgrades: %llu (%.1f MB copied)\n",
                            static_cast<unsigned long long>(upgrades),
                            BbStats::texture_upgrade_bytes.exchange(0) / 1e6);
            }
            // Frame timeline: where each frame's time goes on the GPU side (ms per frame).
            if (frames) {
                const auto per_frame = [&](std::atomic<u64>& total) {
                    return total.exchange(0) / (frames * 1e6);
                };
                const double rec_busy = per_frame(BbStats::rec_busy_ns);
                const double rec_idle = per_frame(BbStats::rec_idle_ns);
                const double flip_submit = per_frame(BbStats::flip_submit_ns);
                const double queue_submit = per_frame(BbStats::queue_submit_ns);
                const double submits = double(BbStats::queue_submits.exchange(0)) / frames;
                const double ahead = per_frame(BbStats::frames_ahead_ns);
                const double drain = per_frame(BbStats::drain_ns);
                const u64 gpu_frames = BbStats::gpu_done_frames.exchange(0);
                const double gpu_gap =
                    gpu_frames ? BbStats::gpu_done_gap_ns.exchange(0) / (gpu_frames * 1e6) : 0.0;
                const double gpu_latency =
                    gpu_frames ? BbStats::gpu_latency_ns.exchange(0) / (gpu_frames * 1e6) : 0.0;
                std::printf("Frame timeline (ms/frame): recording thread busy %.2f (flip submit "
                            "%.2f, frames-ahead wait %.2f), waiting for packets %.2f; "
                            "vkQueueSubmit %.2f in %.1f calls; command thread waiting for "
                            "recording %.2f; flip submit -> GPU done %.2f, GPU done every %.2f; "
                            "per frame %.1f tessellated, %.1f rect/quad-list draws; hoisted "
                            "%.1f uploads, copy shader %.1f hoisted / %.1f inline / %.1f deferred (%.1f "
                            "conflicts); arena ordering "
                            "requests %.1f naming render passes, %.1f others; %.1f not hoisted (in "
                            "flight); busy-slot reuses %.1f guest memory, %.1f motion; ordering "
                            "fence waits %.1f per frame on %.1f encoders, at most %llu on one\n",
                            rec_busy, flip_submit, ahead, rec_idle, queue_submit, submits, drain,
                            gpu_latency, gpu_gap, double(BbStats::tess_draws.exchange(0)) / frames,
                            double(BbStats::rect_draws.exchange(0)) / frames,
                            double(BbStats::hoisted_copies.exchange(0)) / frames,
                            double(BbStats::hoisted_shader_copies.exchange(0)) / frames,
                            double(BbStats::inline_shader_copies.exchange(0)) / frames,
                            double(BbStats::deferred_shader_copies.exchange(0)) / frames,
                            double(BbStats::deferred_copy_conflicts.exchange(0)) / frames,
                            double(BbStats::arena_barriers.exchange(0)) / frames,
                            double(BbStats::arena_other_barriers.exchange(0)) / frames,
                            double(BbStats::hoists_in_flight.exchange(0)) / frames,
                            double(BbStats::busy_slot_reuses.exchange(0)) / frames,
                            double(BbStats::busy_motion_slot_reuses.exchange(0)) / frames,
                            double(BbStats::order_waits.exchange(0)) / frames,
                            double(BbStats::order_encoders.exchange(0)) / frames,
                            static_cast<unsigned long long>(BbStats::order_max_waits.exchange(0)));
            }
            if (const u64 built = BbStats::background_builds.exchange(0)) {
                std::printf("Pipelines: %llu built in the background (%.1f ms on the creation "
                            "threads); render path waited %.1f ms; %llu draws skipped while "
                            "compiling\n",
                            static_cast<unsigned long long>(built),
                            BbStats::background_build_ns.exchange(0) / 1e6,
                            BbStats::compile_wait_ns.exchange(0) / 1e6,
                            static_cast<unsigned long long>(
                                BbStats::compile_skipped_draws.exchange(0)));
            }
            // Frame pacing: spread of the guest flip intervals (judder that the mean hides).
            if (intervals.size() > 2) {
                std::vector<double> sorted = intervals;
                std::sort(sorted.begin(), sorted.end());
                const double median = sorted[sorted.size() / 2];
                double sum = 0, sq = 0;
                u32 spikes = 0;
                for (const double ms : intervals) {
                    sum += ms;
                    sq += ms * ms;
                    spikes += ms > 1.5 * median;
                }
                const double mean = sum / intervals.size();
                std::printf("Frame pacing: median %.2f ms, stddev %.2f ms, p99 %.2f ms, "
                            "%u frames over 1.5x median\n",
                            median, std::sqrt(std::max(0.0, sq / intervals.size() - mean * mean)),
                            sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)], spikes);
            }
            if (BbStats::enabled) {
                const u32 shown = spikes_printed.exchange(0);
                const u64 latency_frames = std::max<u64>(1, latency_count.exchange(0));
                std::printf("Spikes (frames later than 1.25 periods): game %u, flip %u, present "
                            "%u, display %u%s; %u frames never displayed; present->display "
                            "mean %.1f ms, max %.1f ms\n",
                            spikes_game.exchange(0), spikes_flip.exchange(0),
                            spikes_present.exchange(0), spikes_display.exchange(0),
                            shown > 20 ? " (first 20 printed)" : "",
                            frames_not_displayed.exchange(0),
                            latency_sum_ns.exchange(0) / 1e6 / double(latency_frames),
                            latency_max_ns.exchange(0) / 1e6);
                spike_counters.Rebase();
            }
            intervals.clear();
            window_start = now;
            frames = 0;
            worst_ms = 0;
        }
    }

    // Update flip status.
    auto* port = req.port;
    {
        std::unique_lock lock{port->port_mutex};
        auto& flip_status = port->flip_status;
        flip_status.count++;
        flip_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
        flip_status.tsc = Libraries::Kernel::sceKernelReadTsc();
        flip_status.flip_arg = req.flip_arg;
        flip_status.current_buffer = req.index;
        if (req.eop) {
            --flip_status.gc_queue_num;
        }
        --flip_status.flip_pending_num;
        if (BbSplit::Host()) {
            const BbGpuFlipEvent event{{BBGPU_EV_FLIP, sizeof(BbGpuFlipEvent)},
                                       1,
                                       req.index,
                                       req.eop,
                                       0,
                                       req.flip_arg,
                                       flip_status.process_time,
                                       flip_status.tsc};
            BbSplit::Post(&event);
            __atomic_fetch_add(&BbSplit::Shared()->host_flips, 1, __ATOMIC_RELAXED);
        }
    }

    // Trigger flip events for the port.
    for (auto event : port->flip_events) {
        auto equeue = Kernel::GetEqueue(event);
        if (equeue != nullptr) {
            equeue->TriggerEvent(
                static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                Kernel::OrbisKernelEvent::Filter::VideoOut,
                reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Flip) |
                                        (req.flip_arg << 16)));
        }
    }

    // Reset prev flip label
    if (port->prev_index != -1) {
        port->buffer_labels[port->prev_index] = 0;
        port->SignalVoLabel();
    }
    // save to prev buf index
    port->prev_index = req.index;
}

void VideoOutDriver::DrawBlankFrame() {
    RunPresenter([this] {
        const auto empty_frame = presenter->PrepareBlankFrame(true);
        presenter->Present(empty_frame, false, false);
    }, true);
}

void VideoOutDriver::DrawLastFrame() {
    RunPresenter([this] {
        const auto frame = presenter->PrepareLastFrame();
        if (frame != nullptr) {
            presenter->Present(frame, true);
        }
    }, true);
}

bool VideoOutDriver::SubmitFlip(VideoOutPort* port, s32 index, s64 flip_arg,
                                bool is_eop /*= false*/) {
    {
        std::unique_lock lock{port->port_mutex};
        if (index != -1 && port->flip_status.flip_pending_num > 16) {
            LOG_ERROR(Lib_VideoOut, "Flip queue is full");
            return false;
        }

        if (is_eop) {
            ++port->flip_status.gc_queue_num;
        }
        ++port->flip_status.flip_pending_num; // integral GPU and CPU pending flips counter
        port->flip_status.submit_tsc = Libraries::Kernel::sceKernelReadTsc();
        if (BbSplit::Host() && is_eop) {
            const BbGpuFlipEvent event{{BBGPU_EV_EOP_FLIP_QUEUED, sizeof(BbGpuFlipEvent)},
                                       1,
                                       index,
                                       1,
                                       0,
                                       flip_arg,
                                       0,
                                       port->flip_status.submit_tsc};
            BbSplit::Post(&event);
        }
    }
    if (BbSplit::Remote()) {
        return true; // sceVideoOutSubmitFlip sends the flip to the host
    }

    if (!is_eop) {
        // Non EOP flips can arrive from any thread so ask GPU thread to perform them
        liverpool->SendCommand([=, this]() { SubmitFlipInternal(port, index, flip_arg, is_eop); });
    } else {
        SubmitFlipInternal(port, index, flip_arg, is_eop);
    }

    return true;
}

void VideoOutDriver::SubmitFlipInternal(VideoOutPort* port, s32 index, s64 flip_arg, bool is_eop) {
    const u64 flip_packet_ns = BbStats::enabled ? NowNs() : 0;
    Vulkan::Frame* frame;
    if (index == -1) {
        frame = presenter->PrepareBlankFrame(false);
    } else {
        const auto& buffer = port->buffer_slots[index];
        ASSERT_MSG(buffer.group_index >= 0, "Trying to flip an unregistered buffer!");
        const auto& group = port->groups[buffer.group_index];
        frame = presenter->PrepareFrame(group, buffer.address_left);
    }

    {
        std::scoped_lock lock{mutex};
        Request request{
            .frame = frame,
            .port = port,
            .flip_arg = flip_arg,
            .index = index,
            .eop = is_eop,
        };
        if (BbStats::enabled) {
            TakeGameFlip(flip_arg, &request.game_ns, &request.arrival_ns);
            request.flip_packet_ns = flip_packet_ns;
            request.queued_ns = NowNs();
        }
        requests.push(request);
    }
    request_cv.notify_one();
}

void VideoOutDriver::PresentThread(std::stop_token token) {
    const std::chrono::nanoseconds vblank_period(1000000000 /
                                                 EmulatorSettings.GetVblankFrequency());

    Common::SetCurrentThreadName("shadPS4:PresentThread");
    Common::SetCurrentThreadRealtime(vblank_period);

    Common::AccurateTimer timer{vblank_period};

    // bbport: frame limit (see EmulatorSettings::GetFrameLimit). A request waits in the queue
    // until its slot; slots advance by one period (no drift) but never lag behind by more.
    const u32 frame_limit = EmulatorSettings.GetFrameLimit();
    const auto frame_period = frame_limit ? std::chrono::nanoseconds(1000000000 / frame_limit)
                                          : std::chrono::nanoseconds(0);
    auto next_flip = std::chrono::steady_clock::now();
    std::printf("VideoOut: vblank %u Hz, frame limit %u FPS\n",
                EmulatorSettings.GetVblankFrequency(), frame_limit);

    const auto receive_request = [this] -> Request {
        std::scoped_lock lk{mutex};
        if (!requests.empty()) {
            const auto request = requests.front();
            requests.pop();
            return request;
        }
        return {};
    };

    // bbport: with a frame limit (uncapped presets) a queued flip is presented as soon as it
    // arrives and its slot allows, between vblanks, instead of on the next vblank tick.
    const bool immediate_flips = frame_limit != 0;

    while (!token.stop_requested()) {
        timer.Start();
        const auto tick_deadline = std::chrono::steady_clock::now() + vblank_period;

        if (DebugState.IsGuestThreadsPaused()) {
            DrawLastFrame();
            timer.End();
            continue;
        }

        // Check if it's time to take a request.
        auto& vblank_status = main_port.vblank_status;
        const auto now = std::chrono::steady_clock::now();
        const bool flip_slot = !frame_limit || now >= next_flip;
        if (flip_slot && vblank_status.count % (main_port.flip_rate + 1) == 0) {
            const auto request = receive_request();
            if (request && frame_limit) {
                next_flip = std::max(next_flip + frame_period, now - frame_period);
            }
            if (!request) {
                if (timer.GetTotalWait().count() < 0) { // Dont draw too fast
                    if (!main_port.is_open) {
                        DrawBlankFrame();
                    } else if (ImGui::Core::MustKeepDrawing()) {
                        DrawLastFrame();
                    }
                }
            } else {
                Flip(request);
                FRAME_END;
            }
        }

        {
            // Needs lock here as can be concurrently read by `sceVideoOutGetVblankStatus`
            std::scoped_lock lock{main_port.vo_mutex};

            // Trigger flip events for the port
            for (auto event : main_port.vblank_events) {
                auto equeue = Kernel::GetEqueue(event);
                if (equeue != nullptr) {
                    equeue->TriggerEvent(
                        static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                        Kernel::OrbisKernelEvent::Filter::VideoOut,
                        reinterpret_cast<void*>(
                            static_cast<u64>(OrbisVideoOutInternalEventId::Vblank) |
                            (vblank_status.count << 16)));
                }
            }

            // Update vblank status
            vblank_status.count++;
            vblank_status.process_time = Libraries::Kernel::sceKernelGetProcessTime();
            vblank_status.tsc = Libraries::Kernel::sceKernelReadTsc();
            main_port.vblank_cv.notify_all();
        }
        if (BbSplit::Host()) {
            const BbGpuVblank event{{BBGPU_EV_VBLANK, sizeof(BbGpuVblank)}, 1, 0,
                                    vblank_status.count - 1, vblank_status.process_time,
                                    vblank_status.tsc};
            BbSplit::Post(&event);
        }

        if (!immediate_flips || main_port.flip_rate != 0) {
            timer.End();
            continue;
        }
        while (!token.stop_requested()) {
            {
                std::unique_lock lk{mutex};
                if (!request_cv.wait_until(lk, tick_deadline,
                                           [&] { return !requests.empty(); })) {
                    break; // next vblank
                }
            }
            if (std::chrono::steady_clock::now() < next_flip) {
                if (next_flip >= tick_deadline) {
                    break;
                }
                std::this_thread::sleep_until(next_flip);
            }
            const auto now = std::chrono::steady_clock::now();
            const auto request = receive_request();
            if (request) {
                next_flip = std::max(next_flip + frame_period, now - frame_period);
                Flip(request);
                FRAME_END;
            }
        }
        std::this_thread::sleep_until(tick_deadline);
    }
}

void VideoOutDriver::ApplyRemoteEvent(const void* record) {
    const auto* r = static_cast<const BbGpuRecord*>(record);
    auto* port = &main_port;
    switch (r->type) {
    case BBGPU_EV_FLIP: {
        const auto* e = static_cast<const BbGpuFlipEvent*>(record);
        BbStats::flips.fetch_add(1, std::memory_order_relaxed);
        RecordFlipFrameTime();
        {
            std::unique_lock lock{port->port_mutex};
            auto& flip_status = port->flip_status;
            flip_status.count++;
            flip_status.process_time = e->process_time;
            flip_status.tsc = e->tsc;
            flip_status.flip_arg = e->flip_arg;
            flip_status.current_buffer = e->index;
            if (e->eop) {
                --flip_status.gc_queue_num;
            }
            --flip_status.flip_pending_num;
        }
        for (auto event : port->flip_events) {
            auto equeue = Kernel::GetEqueue(event);
            if (equeue != nullptr) {
                equeue->TriggerEvent(
                    static_cast<u64>(OrbisVideoOutInternalEventId::Flip),
                    Kernel::OrbisKernelEvent::Filter::VideoOut,
                    reinterpret_cast<void*>(static_cast<u64>(OrbisVideoOutInternalEventId::Flip) |
                                            (e->flip_arg << 16)));
            }
        }
        port->prev_index = e->index;
        break;
    }
    case BBGPU_EV_EOP_FLIP_QUEUED: {
        const auto* e = static_cast<const BbGpuFlipEvent*>(record);
        std::unique_lock lock{port->port_mutex};
        ++port->flip_status.gc_queue_num;
        ++port->flip_status.flip_pending_num;
        port->flip_status.submit_tsc = e->tsc;
        break;
    }
    case BBGPU_EV_VBLANK: {
        const auto* e = static_cast<const BbGpuVblank*>(record);
        std::scoped_lock lock{port->vo_mutex};
        for (auto event : port->vblank_events) {
            auto equeue = Kernel::GetEqueue(event);
            if (equeue != nullptr) {
                equeue->TriggerEvent(static_cast<u64>(OrbisVideoOutInternalEventId::Vblank),
                                     Kernel::OrbisKernelEvent::Filter::VideoOut,
                                     reinterpret_cast<void*>(
                                         static_cast<u64>(OrbisVideoOutInternalEventId::Vblank) |
                                         (e->count << 16)));
            }
        }
        port->vblank_status.count = e->count + 1;
        port->vblank_status.process_time = e->process_time;
        port->vblank_status.tsc = e->tsc;
        port->vblank_cv.notify_all();
        break;
    }
    default:
        break;
    }
}

} // namespace Libraries::VideoOut
