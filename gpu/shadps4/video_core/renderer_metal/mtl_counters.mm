// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: Metal::TimestampBuffer (mtl_counters.h).

#include <chrono>
#include "common/logging/log.h"
#include "video_core/renderer_metal/mtl_counters.h"
#include "video_core/renderer_metal/mtl_device_impl.h"

namespace Metal {

TimestampBuffer::TimestampBuffer(Device& device, u32 wanted) {
    id<MTLDevice> dev = device.GetImpl().device;
    if (![dev supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) {
        LOG_WARNING(Render, "GPU timestamps: {} cannot sample counters at stage boundaries",
                    device.Name());
        return;
    }
    id<MTLCounterSet> timestamps = nil;
    for (id<MTLCounterSet> set in dev.counterSets) {
        if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) {
            timestamps = set;
        }
    }
    if (!timestamps) {
        LOG_WARNING(Render, "GPU timestamps: {} has no timestamp counter set", device.Name());
        return;
    }
    @autoreleasepool {
        MTLCounterSampleBufferDescriptor* desc = [MTLCounterSampleBufferDescriptor new];
        desc.counterSet = timestamps;
        desc.storageMode = MTLStorageModeShared;
        desc.label = @"bbport GPU profiler timestamps";
        // The size limit is the device's: halve until it is accepted.
        for (u32 n = wanted; n >= 256 && !buffer; n /= 2) {
            desc.sampleCount = n;
            NSError* error = nil;
            id<MTLCounterSampleBuffer> b = [dev newCounterSampleBufferWithDescriptor:desc
                                                                               error:&error];
            if (b) {
                buffer = (__bridge_retained void*)b;
                count = n;
            }
        }
    }
    if (!buffer) {
        LOG_WARNING(Render, "GPU timestamps: no counter sample buffer");
    }
}

TimestampBuffer::~TimestampBuffer() {
    if (buffer) {
        CFRelease(buffer);
    }
}

void TimestampBuffer::Resolve(u32 first, u32 n, std::vector<u64>& out) const {
    out.assign(n, 0);
    if (!buffer || n == 0 || first + n > count) {
        return;
    }
    @autoreleasepool {
        NSData* data = [(__bridge id<MTLCounterSampleBuffer>)buffer
            resolveCounterRange:NSMakeRange(first, n)];
        if (!data) {
            return;
        }
        const auto* samples = static_cast<const MTLCounterResultTimestamp*>(data.bytes);
        const u32 resolved = u32(data.length / sizeof(MTLCounterResultTimestamp));
        for (u32 i = 0; i < resolved && i < n; ++i) {
            const u64 t = samples[i].timestamp;
            out[i] = t == MTLCounterErrorValue ? 0 : t;
        }
    }
}

void TimestampBuffer::Correlate(Device& device, u64& cpu_ns, u64& gpu_ticks) {
    MTLTimestamp cpu = 0, gpu = 0;
    [device.GetImpl().device sampleTimestamps:&cpu gpuTimestamp:&gpu];
    cpu_ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count());
    gpu_ticks = gpu;
}

} // namespace Metal
