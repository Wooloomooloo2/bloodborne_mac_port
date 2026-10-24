// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <vector>

#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {
/// Samples in the counter buffer (all slices); the device may allow fewer.
constexpr u32 WantedSamples = 32768;
} // namespace

void GpuProfiler::Init(Metal::Device& device, Scheduler& scheduler) {
    const char* env = std::getenv("BB_GPU_PROFILE");
    if (!env || env[0] != '1' || instance_ptr) {
        return;
    }
    auto* profiler = new GpuProfiler(device, scheduler);
    if (!profiler->samples.Valid()) {
        std::printf("GPU profile: off (no GPU timestamps at encoder boundaries)\n");
        delete profiler;
        return;
    }
    instance_ptr = profiler;
    scheduler.Record([profiler](Metal::Encoder& e) { e.SetTimestampSink(profiler); });
}

GpuProfiler::GpuProfiler(Metal::Device& device_, Scheduler& scheduler_)
    : device{device_}, scheduler{scheduler_}, samples{device_, WantedSamples} {
    slice_samples = samples.Count() / NumSlices;
    for (auto& s : slices) {
        s.encoders.reserve(slice_samples / 2);
    }
    Metal::TimestampBuffer::Correlate(device, cpu_ns0, gpu_ticks0);
    if (samples.Valid()) {
        std::printf("GPU profile: on (%u timestamps per frame, encoder boundaries)\n",
                    slice_samples);
    }
}

void GpuProfiler::Label(u64 key, std::string name) {
    // Outside render passes: a pass ends where a new one starts anyway; a dispatch or transfer
    // gets encoders of its own.
    scheduler.EndRendering();
    current = key;
    scheduler.Record([this, key, name = std::move(name)](Metal::Encoder& e) {
        if (!name.empty()) {
            names.emplace(key, name);
        }
        label = key;
        if (!e.InRenderPass()) {
            e.EndEncoding();
        }
    });
}

bool GpuProfiler::Reserve(void*& sample_buffer, u32& index, u32 count) {
    Slice& s = slices[slice];
    if (s.used + count > slice_samples) {
        ++s.unsampled;
        return false;
    }
    sample_buffer = samples.Native();
    index = slice * slice_samples + s.used;
    s.encoders.push_back({label, index, count});
    s.used += count;
    return true;
}

void GpuProfiler::BeginFrame() {
    // Commands recorded now go into the submission that signals the current tick: the frame's
    // encoders are all in it or before it.
    scheduler.Record([this, tick = scheduler.CurrentTick()](Metal::Encoder&) { EndFrame(tick); });
}

void GpuProfiler::EndFrame(u64 tick) {
    Slice& done = slices[slice];
    done.tick = tick;
    done.pending = !done.encoders.empty();
    ++frames_ended;
    // GPU ticks -> ns from the pair taken at the start (the longer the span, the better).
    u64 cpu_ns = 0, gpu_ticks = 0;
    Metal::TimestampBuffer::Correlate(device, cpu_ns, gpu_ticks);
    if (gpu_ticks > gpu_ticks0 + 1'000'000 && cpu_ns > cpu_ns0) {
        ns_per_tick = double(cpu_ns - cpu_ns0) / double(gpu_ticks - gpu_ticks0);
    }
    slice = (slice + 1) % NumSlices;
    // The oldest slice: its frame was submitted four frames ago.
    Slice& next = slices[slice];
    if (next.pending) {
        if (device.IsFree(next.tick)) {
            Collect(slice);
        } else {
            ++dropped; // the GPU lags four frames: skip rather than wait
        }
    }
    unsampled += next.unsampled;
    next.encoders.clear();
    next.used = 0;
    next.pending = false;
    next.unsampled = 0;
    Print();
}

void GpuProfiler::Collect(u32 which) {
    const Slice& s = slices[which];
    std::vector<u64> t;
    samples.Resolve(which * slice_samples, s.used, t);
    const u32 base = which * slice_samples;
    u64 first = std::numeric_limits<u64>::max(), last = 0;
    // Segments: from the start of a label's first encoder to the start of the next label's
    // (the Vulkan profiler's timestamps on MoltenVK measured the same).
    u64 segment_key = 0, segment_start = 0;
    // BB_GPU_PROFILE_SLOW_MS=<ms>: frames whose span reaches <ms> print their own segments.
    static const double slow_ms = [] {
        const char* env = std::getenv("BB_GPU_PROFILE_SLOW_MS");
        return env ? std::atof(env) : 0.0;
    }();
    std::unordered_map<u64, double> frame_ms;
    for (const Sampled& enc : s.encoders) {
        const u32 i = enc.index - base;
        u64 start = t[i], end = t[i + 1];
        if (enc.count == 3) {
            // A render encoder with no draws runs no vertex stage.
            const u64 fs_start = t[i + 1];
            start = start && fs_start ? std::min(start, fs_start) : (start ? start : fs_start);
            end = t[i + 2];
        }
        if (!start || !end || end < start) {
            ++unsampled;
            continue;
        }
        auto& total = totals[enc.key];
        total.encoder_ms += double(end - start) * ns_per_tick / 1e6;
        ++total.encoders;
        if (segment_start == 0) {
            segment_key = enc.key;
            segment_start = start;
        } else if (enc.key != segment_key && start >= segment_start) {
            totals[segment_key].ms += double(start - segment_start) * ns_per_tick / 1e6;
            frame_ms[segment_key] += double(start - segment_start) * ns_per_tick / 1e6;
            segment_key = enc.key;
            segment_start = start;
        }
        first = std::min(first, start);
        last = std::max(last, end);
    }
    if (segment_start != 0 && last > segment_start) {
        totals[segment_key].ms += double(last - segment_start) * ns_per_tick / 1e6;
    }
    if (last > first) {
        span_ms += double(last - first) * ns_per_tick / 1e6;
    }
    if (slow_ms > 0 && last > first && double(last - first) * ns_per_tick / 1e6 >= slow_ms) {
        if (segment_start != 0 && last > segment_start) {
            frame_ms[segment_key] += double(last - segment_start) * ns_per_tick / 1e6;
        }
        std::vector<std::pair<u64, double>> sorted(frame_ms.begin(), frame_ms.end());
        std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second > b.second; });
        std::printf("GPU profile: slow frame (tick %llu) %.2f ms first encoder start to last end, "
                    "%zu encoders:",
                    static_cast<unsigned long long>(s.tick),
                    double(last - first) * ns_per_tick / 1e6, s.encoders.size());
        for (size_t i = 0; i < std::min<size_t>(sorted.size(), 10); ++i) {
            const auto name = names.find(sorted[i].first);
            std::printf(" %.2f %s;", sorted[i].second,
                        name != names.end() ? name->second.c_str() : "(before the first mark)");
        }
        std::printf("\n");
    }
    ++frames;
}

void GpuProfiler::Print() {
    const auto now = std::chrono::steady_clock::now();
    if (now - window < std::chrono::seconds(5) || frames == 0) {
        return;
    }
    window = now;
    const u64 busy = device.GpuBusyNs();
    const u64 busy_frames = std::max<u64>(1, frames_ended - window_frames);
    const double busy_ms = double(busy - window_busy_ns) / 1e6 / double(busy_frames);
    window_busy_ns = busy;
    window_frames = frames_ended;
    std::vector<std::pair<u64, Total>> sorted(totals.begin(), totals.end());
    std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
    double sum = 0, encoder_sum = 0, encoders = 0;
    for (const auto& [key, total] : sorted) {
        sum += total.ms;
        encoder_sum += total.encoder_ms;
        encoders += double(total.encoders);
    }
    std::printf("GPU profile: %.2f ms/frame first encoder start to last end (segments %.2f), "
                "command buffers busy %.2f; %.0f sampled encoders/frame summing %.2f ms "
                "(overlapping) over %llu frames; %zu labels, %llu frames skipped, %llu encoders "
                "unsampled\n",
                span_ms / frames, sum / frames, busy_ms, encoders / frames, encoder_sum / frames,
                static_cast<unsigned long long>(frames), sorted.size(),
                static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(unsampled));
    std::printf("  segment ms/frame (encoder ms) encoders/frame  label\n");
    for (size_t i = 0; i < std::min<size_t>(sorted.size(), 30); ++i) {
        const auto& [key, total] = sorted[i];
        const auto name = names.find(key);
        std::printf("  %6.3f (%6.3f) %5.1f  %s\n", total.ms / frames, total.encoder_ms / frames,
                    double(total.encoders) / frames,
                    name != names.end() ? name->second.c_str() : "(before the first mark)");
    }
    totals.clear();
    frames = 0;
    dropped = 0;
    unsampled = 0;
    span_ms = 0;
}

} // namespace Vulkan
