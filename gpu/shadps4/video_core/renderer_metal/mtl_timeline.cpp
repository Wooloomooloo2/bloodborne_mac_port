// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: per-scheduler logical ticks over the shared Metal::Device (Metal-native renderer).

#include "video_core/renderer_metal/mtl_timeline.h"

#include "common/assert.h"
#include "video_core/renderer_metal/mtl_device.h"

namespace Metal {

Timeline::Timeline(Device& device_) : device{device_} {}

void Timeline::Refresh() const {
    while (!pending.empty() && device.IsFree(pending.front().device)) {
        known.store(pending.front().logical, std::memory_order_release);
        pending.pop_front();
    }
}

u64 Timeline::KnownGpuTick() const {
    std::scoped_lock lock{mutex};
    Refresh();
    return known.load(std::memory_order_acquire);
}

bool Timeline::IsFree(u64 tick) const {
    if (tick <= known.load(std::memory_order_acquire)) {
        return true;
    }
    if (tick >= CurrentTick()) {
        return false;
    }
    std::scoped_lock lock{mutex};
    Refresh();
    return tick <= known.load(std::memory_order_acquire);
}

void Timeline::Wait(u64 tick) {
    ASSERT_MSG(tick < CurrentTick(), "Waiting for logical tick {} that was never committed", tick);
    u64 device_tick;
    {
        std::scoped_lock lock{mutex};
        Refresh();
        if (tick <= known.load(std::memory_order_acquire)) {
            return;
        }
        // Logical ticks are consecutive: the front mapping is known + 1.
        device_tick = pending[tick - pending.front().logical].device;
    }
    device.Wait(device_tick);
}

u64 Timeline::Committed(u64 device_tick) {
    std::scoped_lock lock{mutex};
    ASSERT_MSG(device_tick != 0, "Timeline: an untracked commit has no device tick");
    ASSERT_MSG(pending.empty() || device_tick > pending.back().device,
               "Timeline: device tick {} is not after this timeline's last one", device_tick);
    const u64 logical = current.load(std::memory_order_relaxed);
    pending.push_back({logical, device_tick});
    current.store(logical + 1, std::memory_order_release);
    Refresh();
    return logical;
}

std::optional<u64> Timeline::DeviceTickOf(u64 logical) const {
    if (logical >= CurrentTick()) {
        return std::nullopt;
    }
    std::scoped_lock lock{mutex};
    Refresh();
    if (logical <= known.load(std::memory_order_acquire)) {
        return u64{0};
    }
    // Logical ticks are consecutive: the front mapping is known + 1.
    return pending[logical - pending.front().logical].device;
}

size_t Timeline::PendingCount() const {
    std::scoped_lock lock{mutex};
    Refresh();
    return pending.size();
}

} // namespace Metal
