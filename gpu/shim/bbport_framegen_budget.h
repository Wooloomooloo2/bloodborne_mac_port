// SPDX-FileCopyrightText: Copyright 2026 Ronny Stiftel
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cmath>
#include <cstdint>

namespace BbFrameGen {
// Producer-thread admission based on completed GPU work, independent of diagnostic logging.
// Measure real-only gameplay first. The optional interpolation submission supplies its GPU
// cost; resumption requires spare time for that cost plus a margin.
// There are no periodic interpolation probes on an already saturated GPU.
class GpuBudget {
public:
    bool Observe(double now, uint64_t busy_ns, bool scene, uint32_t cap, uint64_t config,
                 double generation_seconds = 0) {
        if (!cap || !std::isfinite(now) || now <= 0) return allowed = false;
        const double period = 1. / cap;
        if (!last_time || config != configuration || cap != real_cap ||
            now <= last_time || now - last_time > .25 || busy_ns < last_busy) {
            *this = {};
            configuration = config;
            real_cap = cap;
            extra = period * .15;
            Begin(now, busy_ns);
            return false;
        }
        if (allowed && std::isfinite(generation_seconds) && generation_seconds > 0)
            extra = generation_seconds;
        last_time = now;
        // Loading/menu frames must not advertise their spare time as gameplay headroom.
        if (!scene) {
            allowed = false;
            overloads = 0;
            Begin(now, busy_ns);
            return false;
        }
        ++frames;
        const double elapsed = now - window_start;
        if (elapsed < .5 || frames < 15) return allowed;
        const double gpu = double(busy_ns - window_busy) * 1e-9 / frames;
        if (allowed) {
            // Two overloaded windows reject sustained pressure, rather than one compilation
            // or snapshot hitch. GPU pressure is required: a CPU stall alone is not a reason.
            const bool pressure = gpu > period * .95 ||
                (elapsed / frames > period * 1.03 && gpu > period * .85);
            overloads = pressure ? overloads + 1 : 0;
            if (overloads >= 2) {
                allowed = false;
                overloads = 0;
            }
        } else {
            // Separate admission/rejection thresholds avoid oscillating near the limit.
            allowed = gpu > 0 && gpu + extra < period * .85;
        }
        Begin(now, busy_ns);
        return allowed;
    }

private:
    void Begin(double now, uint64_t busy_ns) {
        last_time = window_start = now;
        last_busy = window_busy = busy_ns;
        frames = 0;
    }
    bool allowed = false;
    uint32_t real_cap = 0, frames = 0, overloads = 0;
    uint64_t configuration = 0, last_busy = 0, window_busy = 0;
    double last_time = 0, window_start = 0, extra = 0;
};
} // namespace BbFrameGen
