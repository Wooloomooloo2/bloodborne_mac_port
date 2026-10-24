// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: per-scheduler logical ticks over the shared Metal::Device (Metal-native renderer).

#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include "common/types.h"

namespace Metal {

class Device;

/// The ticks of one submitter (a Scheduler) on a Device that several submitters share. Each
/// commit gets the next logical tick (1, 2, ...) mapped to the device tick it signalled, so a
/// tick of this timeline only ever means this timeline's own command buffer. Thread-safe.
class Timeline {
public:
    explicit Timeline(Device& device);

    Timeline(const Timeline&) = delete;
    Timeline& operator=(const Timeline&) = delete;

    /// The logical tick the next commit gets (starts at 1).
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return current.load(std::memory_order_acquire);
    }
    /// The highest logical tick whose command buffer completed.
    [[nodiscard]] u64 KnownGpuTick() const;
    /// True when `tick` (and every tick before it) completed. Tick 0 always is; an uncommitted
    /// tick never is.
    [[nodiscard]] bool IsFree(u64 tick) const;
    /// Blocks until `tick` completed. `tick` must have been committed (asserted).
    void Wait(u64 tick);
    /// Records that this timeline's next commit signalled `device_tick` (a tracked
    /// Device::Commit, later than this timeline's previous one) and returns its logical tick.
    u64 Committed(u64 device_tick);
    /// The device tick `logical` signals, for GPU-side waits by other submitters
    /// (Encoder::WaitForDeviceTick). nullopt: not committed yet (Flush first). 0: already complete
    /// (tick 0, or completed and pruned), which a wait treats as a no-op.
    [[nodiscard]] std::optional<u64> DeviceTickOf(u64 logical) const;
    /// Committed ticks not yet known complete (their mappings), for tests.
    [[nodiscard]] size_t PendingCount() const;

private:
    struct Mapping {
        u64 logical;
        u64 device;
    };
    /// Drops the mappings whose device tick completed (they complete in order on one queue).
    void Refresh() const;

    Device& device;
    std::atomic<u64> current{1};
    mutable std::atomic<u64> known{0};
    mutable std::mutex mutex;
    mutable std::deque<Mapping> pending; ///< logical ticks known+1 .. current-1, in order
};

} // namespace Metal
