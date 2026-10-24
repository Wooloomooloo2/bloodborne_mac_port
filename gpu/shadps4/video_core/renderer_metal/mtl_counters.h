// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: GPU timestamps at encoder boundaries (Metal-native renderer, M5; the GPU profiler,
// vk_gpu_profiler.h). Apple GPUs sample counters only at stage boundaries
// (MTLCounterSamplingPointAtStageBoundary): the start and end of a render, compute or blit
// encoder, through the pass descriptor's sampleBufferAttachments (Metal::Encoder does that for
// a TimestampSink, mtl_encoder.h).

#pragma once

#include <vector>
#include "common/types.h"

namespace Metal {

class Device;

/// An MTLCounterSampleBuffer of GPU timestamps.
class TimestampBuffer {
public:
    /// Up to `count` samples (fewer when the device limits the buffer size: see Count()). Not
    /// Valid() when the device cannot sample timestamps at stage boundaries.
    TimestampBuffer(Device& device, u32 count);
    ~TimestampBuffer();

    TimestampBuffer(const TimestampBuffer&) = delete;
    TimestampBuffer& operator=(const TimestampBuffer&) = delete;

    [[nodiscard]] bool Valid() const noexcept {
        return buffer != nullptr;
    }
    /// id<MTLCounterSampleBuffer>, unretained.
    [[nodiscard]] void* Native() const noexcept {
        return buffer;
    }
    [[nodiscard]] u32 Count() const noexcept {
        return count;
    }
    /// Samples [first, first + n) in GPU ticks, 0 for samples not taken. Only for samples whose
    /// command buffers completed.
    void Resolve(u32 first, u32 n, std::vector<u64>& out) const;

    /// A CPU time (steady_clock, ns) and the GPU timestamp at the same moment: two of them
    /// convert GPU ticks to nanoseconds.
    static void Correlate(Device& device, u64& cpu_ns, u64& gpu_ticks);

private:
    void* buffer{}; ///< id<MTLCounterSampleBuffer>, retained
    u32 count{};
};

} // namespace Metal
