// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: Metal::Device (Metal-native renderer, spec component 1).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_metal/mtl_device_impl.h"

namespace Metal {

namespace {

void RaiseTo(std::atomic<u64>& value, u64 tick) {
    u64 known = value.load(std::memory_order_acquire);
    while (known < tick &&
           !value.compare_exchange_weak(known, tick, std::memory_order_acq_rel)) {
    }
}

/// BB_METAL_HAZARD_TRACKING=1: tracked buffers, no fences (read once, logged once).
bool HazardTrackingRequested() {
    static const bool requested = [] {
        const char* env = std::getenv("BB_METAL_HAZARD_TRACKING");
        const bool on = env && std::strcmp(env, "1") == 0;
        if (on) {
            std::printf("Metal: hazard tracking on every buffer (BB_METAL_HAZARD_TRACKING=1); "
                        "no explicit ordering\n");
        }
        return on;
    }();
    return requested;
}

/// BB_GPU_WATCHDOG_MS (default 3000, 0 = off): how long committed work may make no progress.
/// A GPU that stops (a fence or event wait that is never satisfied) also stalls WindowServer,
/// whose own watchdog then restarts it and takes the whole session down; exiting the process
/// first makes the driver drop our work.
u64 WatchdogLimitMs() {
    const char* env = std::getenv("BB_GPU_WATCHDOG_MS");
    return env ? std::strtoull(env, nullptr, 10) : 3000;
}

} // namespace

void Device::StartWatchdog() {
    const u64 limit_ms = WatchdogLimitMs();
    if (limit_ms == 0) {
        return;
    }
    // Keep the event alive until this detached observer exits. A completed GPU signal can
    // precede CPU completion handlers (including TrackGpuTime and resource retirement).
    id<MTLSharedEvent> event = impl->event;
    std::thread([shared = counters, event, limit_ms] {
        using Clock = std::chrono::steady_clock;
        u64 last_known = 0;
        Clock::time_point since = Clock::now(), last_check = since;
        while (!shared->stop_watchdog.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const u64 handlers = shared->gpu_tick.load(std::memory_order_acquire);
            const u64 signaled = event.signaledValue;
            // Observe GPU progress without retiring resources early. The handler counter
            // still covers failed command buffers that never execute their event signal.
            const u64 known = std::max(handlers, signaled);
            const u64 committed = shared->committed.load(std::memory_order_acquire);
            const Clock::time_point now = Clock::now();
            // A late wake-up (system sleep, the process stopped) is not a GPU stall.
            const bool woke_late = now - last_check > std::chrono::seconds(1);
            last_check = now;
            if (known >= committed || known != last_known || woke_late) {
                last_known = known;
                since = now;
                continue;
            }
            const auto stalled =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - since).count();
            if (u64(stalled) >= limit_ms) {
                std::fprintf(stderr, "Crash: the GPU stopped making progress (%lld ms)\n",
                             static_cast<long long>(stalled));
                std::fprintf(stderr,
                             "STOP: GPU watchdog: tick %llu committed, %llu complete, no progress "
                             "for %lld ms; exiting so the GPU does not stall WindowServer "
                             "(BB_GPU_WATCHDOG_MS; GPU event %llu, handlers %llu)\n",
                             static_cast<unsigned long long>(committed),
                             static_cast<unsigned long long>(known),
                             static_cast<long long>(stalled),
                             static_cast<unsigned long long>(signaled),
                             static_cast<unsigned long long>(handlers));
                std::fflush(stderr);
                _exit(75);
            }
        }
    }).detach();
}

Device::Device(void* mtl_device, void* mtl_queue) : impl{std::make_unique<Impl>()} {
    @autoreleasepool {
        impl->device = mtl_device ? (__bridge id<MTLDevice>)mtl_device : MTLCreateSystemDefaultDevice();
        ASSERT_MSG(impl->device, "No Metal device");
        impl->queue = mtl_queue ? (__bridge id<MTLCommandQueue>)mtl_queue : [impl->device newCommandQueue];
        ASSERT_MSG(impl->queue, "No Metal command queue");
        impl->event = [impl->device newSharedEvent];
        ASSERT_MSG(impl->event, "No Metal shared event");
        MTLResidencySetDescriptor* rd = [MTLResidencySetDescriptor new];
        rd.label = @"bbport arena and address-reached buffers";
        NSError* error = nil;
        impl->residency = [impl->device newResidencySetWithDescriptor:rd error:&error];
        ASSERT_MSG(impl->residency, "No Metal residency set: {}",
                   error ? error.localizedDescription.UTF8String : "unknown");
        [impl->queue addResidencySet:impl->residency];
        name = impl->device.name.UTF8String;
        limits.max_buffer_length = impl->device.maxBufferLength;
        limits.recommended_working_set = impl->device.recommendedMaxWorkingSetSize;
        hazard_tracking = HazardTrackingRequested();
        if (!hazard_tracking) {
            // Every fence gets one update before any wait, so waits never depend on whether a
            // fence was used yet (OrderTracker::OpenAhead waits on all of them).
            id<MTLCommandBuffer> prime = [impl->queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [prime blitCommandEncoder];
            for (u32 i = 0; i < OrderTracker::NumFences; ++i) {
                impl->fences[i] = [impl->device newFence];
                ASSERT_MSG(impl->fences[i], "No Metal fence");
                [blit updateFence:impl->fences[i]];
            }
            [blit endEncoding];
            [prime commit];
            [prime waitUntilCompleted];
        }
    }
    StartWatchdog();
}

Device::~Device() {
    Finish();
    counters->stop_watchdog.store(true, std::memory_order_release);
}

u64 Device::AllocatedSize() const {
    return impl->device.currentAllocatedSize;
}

void* Device::NativeDevice() const noexcept {
    return (__bridge void*)impl->device;
}

void* Device::NativeQueue() const noexcept {
    return (__bridge void*)impl->queue;
}

bool Device::IsFree(u64 tick) const {
    return KnownGpuTick() >= tick;
}

void Device::Wait(u64 tick) {
    ASSERT_MSG(tick < CurrentTick(), "Waiting for tick {} that was never committed", tick);
    // Completion handlers raise the known tick and wake waiters (also for failed command buffers).
    for (u64 known = KnownGpuTick(); known < tick; known = KnownGpuTick()) {
        counters->gpu_tick.wait(known, std::memory_order_acquire);
    }
}

void Device::Finish() {
    const u64 last = CurrentTick() - 1;
    if (last != 0) {
        Wait(last);
    }
}

void Device::CommitUntracked(void* command_buffer) {
    TrackGpuTime(command_buffer);
    [(__bridge id<MTLCommandBuffer>)command_buffer commit];
}

void Device::TrackGpuTime(void* command_buffer) {
    std::shared_ptr<Counters> shared = counters;
    [(__bridge id<MTLCommandBuffer>)command_buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
      const double start = done.GPUStartTime, end = done.GPUEndTime;
      if (!(end > start) || start <= 0) {
          return; // never ran (error) or no work
      }
      // BB_LOG_LONG_CB_MS=<ms>: command buffers whose GPU time reaches <ms> (diagnostics), with
      // the time the GPU sat idle before them (completion order, so roughly).
      static const double long_ms = [] {
          const char* env = std::getenv("BB_LOG_LONG_CB_MS");
          return env ? std::atof(env) : 0.0;
      }();
      if (long_ms > 0) {
          static std::atomic<double> last_end{0.0};
          const double gpu_ms = (end - start) * 1e3;
          const double idle_ms = (start - last_end.exchange(end)) * 1e3;
          if (gpu_ms >= long_ms || idle_ms >= long_ms) {
              LOG_WARNING(Render,
                          "Metal command buffer '{}': GPU {:.2f} ms, scheduled {:.2f} ms after "
                          "the kernel saw it, GPU idle {:.2f} ms before it",
                          done.label ? done.label.UTF8String : "", gpu_ms,
                          (start - done.kernelStartTime) * 1e3, idle_ms);
          }
      }
      std::scoped_lock lock{shared->busy_mutex};
      shared->gpu_busy_ns.fetch_add(u64(shared->busy.Add(start, end) * 1e9),
                                    std::memory_order_relaxed);
    }];
}

void Device::MakeResident(void* buffer) {
    std::scoped_lock lock{residency_mutex};
    id<MTLBuffer> b = (__bridge id<MTLBuffer>)buffer;
    [impl->residency addAllocation:b];
    [impl->residency commit];
    if (resident.emplace(buffer, b.length).second) {
        resident_bytes += b.length;
    }
}

void Device::Evict(void* buffer) {
    std::scoped_lock lock{residency_mutex};
    [impl->residency removeAllocation:(__bridge id<MTLBuffer>)buffer];
    [impl->residency commit];
    if (const auto it = resident.find(buffer); it != resident.end()) {
        resident_bytes -= it->second;
        resident.erase(it);
    }
}

u64 Device::ResidentBytes() const {
    std::scoped_lock lock{residency_mutex};
    return resident_bytes;
}

u64 Device::Commit(void* command_buffer) {
    id<MTLCommandBuffer> cmd = (__bridge id<MTLCommandBuffer>)command_buffer;
    const u64 tick = current_tick.fetch_add(1, std::memory_order_acq_rel);
    [cmd encodeSignalEvent:impl->event value:tick];
    // Before the tick handler: when Wait(tick) returns, this command buffer's time is counted.
    TrackGpuTime(command_buffer);
    std::shared_ptr<Counters> shared = counters;
    [cmd addCompletedHandler:^(id<MTLCommandBuffer> done) {
        if (done.status == MTLCommandBufferStatusError) {
            shared->errors.fetch_add(1, std::memory_order_relaxed);
            if (done.error.code == MTLCommandBufferErrorPageFault) {
                shared->page_faults.fetch_add(1, std::memory_order_relaxed);
            }
            LOG_ERROR(Render, "Metal command buffer of tick {} failed: {}", tick,
                      done.error ? done.error.localizedDescription.UTF8String : "unknown error");
        }
        RaiseTo(shared->gpu_tick, tick);
        shared->gpu_tick.notify_all();
    }];
    [cmd commit];
    // Commit can spend time on the CPU before Metal receives the buffer. Do not arm the
    // GPU watchdog for that interval. Commit callers are serialized (Device's contract).
    counters->committed.store(tick, std::memory_order_release);
    return tick;
}

} // namespace Metal
