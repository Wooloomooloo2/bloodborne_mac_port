// SPDX-FileCopyrightText: Copyright 2026 Ronny Stiftel
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace BbFrameGen {
enum class BudgetState { Scene, Measuring, Ready, Pressure };

// Producer-thread admission based on completed GPU work, independent of diagnostic logging.
// Measure real-only gameplay first. The optional interpolation submission supplies its GPU
// cost; resumption requires spare time for that cost plus a margin.
// There are no periodic interpolation probes on an already saturated GPU.
class GpuBudget {
public:
    bool Observe(double now, uint64_t busy_ns, bool scene, uint32_t cap, uint64_t config,
                 double generation_seconds = 0, uint64_t generation_sequence = 0) {
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
        // A first-use upload/compilation or one slow command buffer must not become the
        // permanent resume cost: once paused there are no new interpolation measurements.
        // Count distinct completed submissions, not repeated reads of the last timestamp.
        if (allowed && generation_sequence != last_generation_sequence &&
            std::isfinite(generation_seconds) && generation_seconds > 0) {
            last_generation_sequence = generation_sequence;
            costs[cost_index++ % costs.size()] = generation_seconds;
            cost_count = std::min(cost_count + 1, costs.size());
            if (cost_count >= 5) {
                auto sorted = costs;
                std::sort(sorted.begin(), sorted.begin() + cost_count);
                extra = sorted[cost_count / 2];
            }
        }
        last_time = now;
        // Loading/menu frames must not advertise their spare time as gameplay headroom.
        if (!scene) {
            state = BudgetState::Scene;
            allowed = false;
            overloads = 0;
            Begin(now, busy_ns);
            return false;
        }
        if (!allowed && state == BudgetState::Scene) state = BudgetState::Measuring;
        ++frames;
        const double elapsed = now - window_start;
        if (elapsed < .5 || frames < 15) return allowed;
        const double gpu = double(busy_ns - window_busy) * 1e-9 / frames;
        real_seconds = gpu;
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
        state = allowed ? BudgetState::Ready : BudgetState::Pressure;
        Begin(now, busy_ns);
        return allowed;
    }

    [[nodiscard]] BudgetState State() const { return state; }
    [[nodiscard]] double RealSeconds() const { return real_seconds; }
    [[nodiscard]] double GenerationSeconds() const { return extra; }

private:
    void Begin(double now, uint64_t busy_ns) {
        last_time = window_start = now;
        last_busy = window_busy = busy_ns;
        frames = 0;
    }
    bool allowed = false;
    BudgetState state = BudgetState::Measuring;
    uint32_t real_cap = 0, frames = 0, overloads = 0;
    uint64_t configuration = 0, last_busy = 0, window_busy = 0;
    double last_time = 0, window_start = 0, extra = 0;
    double real_seconds = 0;
    std::array<double, 9> costs{};
    size_t cost_index = 0, cost_count = 0;
    uint64_t last_generation_sequence = 0;
};
} // namespace BbFrameGen
