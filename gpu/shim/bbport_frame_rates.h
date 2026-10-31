// SPDX-FileCopyrightText: Copyright 2026 Ronny Stiftel
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <mutex>

namespace BbFrameRates {

inline double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Sample {
    double real_fps{};
    double displayed_fps{};
};

// Original single-rate readout: exponential mean of every present interval,
// including reused/blank frames. Used only when frame generation is inactive.
class PresentAverage {
public:
    void Frame(double now) {
        const double ms = (now - last) * 1000;
        last = now;
        if (ms > 0 && ms < 1000)
            average_ms = average_ms == 0 ? ms : average_ms * .95 + ms * .05;
    }
    Sample Read() const { return {average_ms > 0 ? 1000 / average_ms : 0, 0}; }

private:
    double last{}, average_ms{};
};

// A bounded one-second cadence window. Display timestamps have their own clock
// domain; observed times use Now() only to expire stale readings. A display
// callback need not arrive in timestamp order. No GPU waits or ImGui access.
class Tracker {
public:
    void RealFrame(double now, bool reused = false, bool game_frame = true) {
        if (reused || !game_frame) return;
        std::scoped_lock lock{mutex};
        real.Add(now, now);
    }

    void Displayed(double presented, double observed) {
        std::scoped_lock lock{mutex};
        displayed.Add(presented, observed);
    }

    Sample Read(double now) const {
        std::scoped_lock lock{mutex};
        return {real.Rate(now), displayed.Rate(now)};
    }

private:
    struct Window {
        static constexpr double Seconds = 1.0;
        std::array<double, 512> times{};
        size_t count{};
        double last_observed{};

        void Add(double timestamp, double observed) {
            if (!std::isfinite(timestamp) || timestamp <= 0 ||
                !std::isfinite(observed) || observed <= 0) return;
            if (count && (observed - last_observed > Seconds ||
                          timestamp - times[count - 1] > Seconds)) count = 0;
            const double newest = count ? std::max(timestamp, times[count - 1]) : timestamp;
            if (timestamp < newest - Seconds) return;
            const auto end = times.begin() + count;
            const auto first = std::lower_bound(times.begin(), end, newest - Seconds);
            if (first != times.begin()) count = std::move(first, end, times.begin()) - times.begin();
            auto at = std::lower_bound(times.begin(), times.begin() + count, timestamp);
            if (at != times.begin() + count && *at == timestamp) return;
            if (count == times.size()) {
                if (at == times.begin()) return;
                std::move(times.begin() + 1, times.end(), times.begin());
                --count;
                --at;
            }
            std::move_backward(at, times.begin() + count, times.begin() + count + 1);
            *at = timestamp;
            ++count;
            last_observed = std::max(last_observed, observed);
        }

        double Rate(double now) const {
            if (!std::isfinite(now) || count < 2 || now - last_observed > Seconds) return 0;
            const double span = times[count - 1] - times[0];
            return span > 0 ? (count - 1) / span : 0;
        }
    };

    mutable std::mutex mutex;
    Window real, displayed;
};

} // namespace BbFrameRates
