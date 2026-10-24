// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: BB_GPU_PROFILE=1 — GPU time per render pass, dispatch and upscaler run. Each mark
// labels the Metal encoders opened after it (until the next mark); every such encoder samples
// the GPU timestamp at its start and end (Metal::TimestampSink: Apple GPUs sample only at
// encoder boundaries). A label is charged its segments, from the start of its first encoder to
// the start of the next label's (what the Vulkan profiler's timestamps measured on MoltenVK),
// and, apart, its encoders' start-to-end times (these overlap: Apple GPUs run adjacent encoders
// concurrently). Work encoded outside our encoders (MetalFX, the present queue, hoisted
// uploads) shows only in the command buffers' GPU busy time (Metal::Device::GpuBusyNs). Results
// are read four frames later and printed every 5 s: ms per frame by label.
//
// A mark outside a render pass ends the open compute or blit encoder (as the Vulkan profiler's
// timestamps did), and sampled encoders may overlap less on the GPU: compare totals with
// BB_FRAME_STATS's "GPU busy" from a run without the profiler.

#pragma once

#include <array>
#include <chrono>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_metal/mtl_counters.h"
#include "video_core/renderer_metal/mtl_encoder.h"

namespace Metal {
class Device;
}

namespace Vulkan {

class Scheduler;

class GpuProfiler final : public Metal::TimestampSink {
public:
    /// The profiler when BB_GPU_PROFILE=1 (and the GPU samples timestamps), else null.
    static GpuProfiler* Get() noexcept {
        return instance_ptr;
    }
    static void Init(Metal::Device& device, Scheduler& scheduler);

    /// Labels the encoders from here on `key`; `describe` names it the first time the key is
    /// seen. Command thread.
    template <typename Describe>
    void Mark(u64 key, Describe&& describe) {
        std::string name;
        if (described.insert(key).second) {
            name = describe();
        }
        Label(key, std::move(name));
    }

    /// The label of the open segment, for Resume after a nested segment (transfers).
    [[nodiscard]] u64 Current() const noexcept {
        return current;
    }
    /// Continues the segment of `key` (a label seen before) where a nested one ends.
    void Resume(u64 key) {
        Label(key, {});
    }

    /// Whether `other` is the scheduler it records into.
    [[nodiscard]] bool Records(const Scheduler* other) const noexcept {
        return other == &scheduler;
    }

    /// At the display pass: closes the frame, reads an old frame's results.
    void BeginFrame();

    /// Metal::TimestampSink (encoding thread).
    bool Reserve(void*& sample_buffer, u32& index, u32 count) override;

private:
    GpuProfiler(Metal::Device& device, Scheduler& scheduler);
    void Label(u64 key, std::string name);
    // Encoding thread (scheduler closures).
    void EndFrame(u64 tick);
    void Collect(u32 which);
    void Print();

    static constexpr u32 NumSlices = 4;
    static inline GpuProfiler* instance_ptr = nullptr;

    Metal::Device& device;
    Scheduler& scheduler;
    // Command thread.
    std::unordered_set<u64> described;
    u64 current = 0;
    // Encoding thread.
    Metal::TimestampBuffer samples;
    u32 slice_samples = 0; ///< samples per slice (a slice per frame in flight)
    struct Sampled {
        u64 key;
        u32 index; ///< first sample in the buffer
        u32 count; ///< 3: render (vertex start, fragment start, fragment end); 2: start, end
    };
    struct Slice {
        std::vector<Sampled> encoders;
        u32 used = 0;
        u64 tick = 0; ///< the frame's last submission: complete before the slice is read
        bool pending = false;
        u64 unsampled = 0; ///< encoders opened with the slice full
    };
    std::array<Slice, NumSlices> slices{};
    u32 slice = 0;
    u64 label = 0;
    std::unordered_map<u64, std::string> names;
    struct Total {
        double ms = 0;         ///< segments: first encoder start to the next label's start
        double encoder_ms = 0; ///< its encoders' start-to-end times (they overlap others)
        u64 encoders = 0;
    };
    std::unordered_map<u64, Total> totals;
    u64 frames = 0, dropped = 0, unsampled = 0;
    double span_ms = 0; ///< first encoder start to last encoder end, summed over frames
    u64 window_busy_ns = 0, window_frames = 0; ///< GpuBusyNs and frames ended at the last print
    u64 frames_ended = 0;
    u64 cpu_ns0 = 0, gpu_ticks0 = 0; ///< a (CPU, GPU) time pair: ticks -> ns
    double ns_per_tick = 1.0;
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
};

} // namespace Vulkan
