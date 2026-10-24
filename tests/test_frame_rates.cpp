// SPDX-FileCopyrightText: Copyright 2026 Ronny Stiftel
// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_frame_rates.h"
#include <cassert>
#include <cstdio>
#include <limits>
#include <thread>

using BbFrameRates::Tracker;

static bool Near(double actual, double expected) { return std::abs(actual - expected) < .01; }

int main() {
    Tracker rates;
    assert(rates.Read(1).real_fps == 0 && rates.Read(1).displayed_fps == 0);
    for (int i = 0; i <= 60; ++i) {
        const double now = 1 + i / 60.0;
        rates.RealFrame(now);
        rates.RealFrame(now + .001, true); // redisplaying an old game frame is not rendering one
        rates.RealFrame(now + .002, false, false); // blank presentation
        rates.Displayed(101 + i / 60.0, now); // independent presentation clock domain
    }
    auto sample = rates.Read(2);
    assert(Near(sample.real_fps, 60) && Near(sample.displayed_fps, 60));

    // Actual 2x delivery, including callbacks arriving in the opposite order.
    for (int i = 1; i <= 60; ++i) {
        const double now = 2 + i / 60.0;
        rates.RealFrame(now);
        rates.Displayed(102 + i / 60.0, now);
        rates.Displayed(102 + (i - .5) / 60.0, now);
    }
    sample = rates.Read(3);
    assert(Near(sample.real_fps, 60) && Near(sample.displayed_fps, 120));

    // If interpolation falls back to real frames only, displayed FPS must fall too.
    for (int i = 1; i <= 120; ++i) {
        const double now = 3 + i / 60.0;
        rates.RealFrame(now);
        rates.Displayed(103 + i / 60.0, now);
    }
    sample = rates.Read(5);
    assert(Near(sample.real_fps, 60) && Near(sample.displayed_fps, 60));
    rates.Displayed(105, 5.1); // duplicate callback cannot inflate or refresh the rate
    rates.Displayed(0, 5.1);   // never displayed / occluded
    rates.Displayed(std::numeric_limits<double>::quiet_NaN(), 5.1);
    rates.Displayed(std::numeric_limits<double>::infinity(), 5.1);
    assert(Near(rates.Read(5.2).displayed_fps, 60));
    assert(rates.Read(6.01).displayed_fps == 0 && rates.Read(6.01).real_fps == 0);
    rates.RealFrame(7);
    rates.Displayed(107, 7);
    assert(rates.Read(7).real_fps == 0 && rates.Read(7).displayed_fps == 0);
    rates.RealFrame(7 + 1.0 / 30);
    rates.Displayed(107 + 1.0 / 30, 7 + 1.0 / 30);
    assert(Near(rates.Read(7.04).real_fps, 30) && Near(rates.Read(7.04).displayed_fps, 30));

    // Display refresh can limit delivery despite 60 real frames and a requested 2x mode.
    Tracker limited;
    for (int i = 0; i <= 60; ++i) limited.RealFrame(1 + i / 60.0);
    for (int i = 0; i <= 90; ++i) limited.Displayed(51 + i / 90.0, 1 + i / 90.0);
    assert(Near(limited.Read(2).real_fps, 60) && Near(limited.Read(2).displayed_fps, 90));

    // High rates stay bounded, with present-thread samples and display callbacks concurrent.
    Tracker concurrent;
    std::thread real([&] {
        for (int i = 0; i <= 4000; ++i) concurrent.RealFrame(1 + i / 2000.0);
    });
    std::thread displayed([&] {
        for (int i = 0; i <= 4000; ++i) concurrent.Displayed(101 + i / 2000.0, 1 + i / 2000.0);
    });
    real.join(); displayed.join();
    assert(Near(concurrent.Read(3).real_fps, 2000));
    assert(Near(concurrent.Read(3).displayed_fps, 2000));
    std::puts("PASS: real and displayed frame rates");
}
