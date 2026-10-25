// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The frame rate as code that runs on the game's side of the thunks reads it:
// the decomp camera (decomp/camera/follow_camera.cpp) is a leaf, which calls
// nothing of the host's and touches no host TLS, so it reads plain globals and
// does its arithmetic inline (no libm: powf could set errno). Written by
// engine/frame_rate.cpp; header-defined so the eboot kit test links without
// it (there, as at the game's own 30, nothing is converted).

#include <atomic>
#include <cstdint>
#include <cstring>

namespace frame_rate_step {

// This frame's step at a converted rate - the measured frame time clamped to
// [target, 1/30], what the game's task step is - and 0 at the game's own 30.
inline std::atomic<float> g_frame_seconds{0.0f};
// The rate the follow camera's per-frame weights are tuned for at a converted
// rate (the Camera tuning option, engine/frame_rate.cpp): 60 with FPS++, Kyo's
// 60 fps values in (the 60 FPS++ sites' parameter stores and the constructor's
// defaults), 30 with Original, the game's own; a weight is converted from a
// frame of that rate to this frame's length. 0: nothing is converted.
inline std::atomic<int> g_camera_tuned_fps{0};

// The functions are always inlined and local to each file that uses them, so
// the leaf's copy is built with the leaf's flags (no stack protector), never
// a host file's.
//
// x^y for x in [0, 1] and y > 0, as exp(y ln x) in double: ln through
// 2 atanh((m - 1) / (m + 1)) on the mantissa (to t^15, |t| < 0.172) and exp
// through 2^n e^r (to r^13, |r| <= ln 2 / 2) - both within a few units of the
// double's last place, far below a float's.
[[gnu::always_inline]] static inline double pow_unit(double x, double y) {
    if (!(x > 0.0)) return 0.0;  // 0^y, and NaN
    if (x >= 1.0) return 1.0;
    std::uint64_t bits;
    std::memcpy(&bits, &x, 8);
    int e = static_cast<int>((bits >> 52) & 0x7ff) - 1023;
    if (e == -1023) {  // subnormal: make it normal first
        x *= 4503599627370496.0;  // 2^52
        std::memcpy(&bits, &x, 8);
        e = static_cast<int>((bits >> 52) & 0x7ff) - 1023 - 52;
    }
    bits = (bits & 0x000fffffffffffffull) | 0x3ff0000000000000ull;
    double m;
    std::memcpy(&m, &bits, 8);
    if (m > 1.4142135623730951) {
        m *= 0.5;
        ++e;
    }
    const double t = (m - 1.0) / (m + 1.0), t2 = t * t;
    double s = 1.0 / 15.0;
    s = s * t2 + 1.0 / 13.0;
    s = s * t2 + 1.0 / 11.0;
    s = s * t2 + 1.0 / 9.0;
    s = s * t2 + 1.0 / 7.0;
    s = s * t2 + 1.0 / 5.0;
    s = s * t2 + 1.0 / 3.0;
    s = s * t2 + 1.0;
    constexpr double kLn2 = 0.6931471805599453;
    const double z = y * (2.0 * t * s + static_cast<double>(e) * kLn2);  // ln of the result
    if (z < -700.0) return 0.0;
    if (z >= 0.0) return 1.0;
    const double nf = z * 1.4426950408889634 - 0.5;
    const auto n = static_cast<std::int64_t>(nf);  // truncation toward 0: round(z / ln 2) for z < 0
    const double r = z - static_cast<double>(n) * kLn2;
    double p = 1.0 / 6227020800.0;  // 1/13!
    p = p * r + 1.0 / 479001600.0;
    p = p * r + 1.0 / 39916800.0;
    p = p * r + 1.0 / 3628800.0;
    p = p * r + 1.0 / 362880.0;
    p = p * r + 1.0 / 40320.0;
    p = p * r + 1.0 / 5040.0;
    p = p * r + 1.0 / 720.0;
    p = p * r + 1.0 / 120.0;
    p = p * r + 1.0 / 24.0;
    p = p * r + 1.0 / 6.0;
    p = p * r + 0.5;
    p = p * r + 1.0;
    p = p * r + 1.0;
    const std::uint64_t scale_bits = static_cast<std::uint64_t>(n + 1023) << 52;  // n >= -1010 here
    double scale;
    std::memcpy(&scale, &scale_bits, 8);
    return p * scale;
}

// A weight a frame (value += (target - value) * w) tuned for frames of a
// given length, for a frame `frames` of those long: 1 - (1 - w)^frames, so a
// second moves as far at any frame rate. `frames` 0 or 1, and a weight
// outside (0, 1) (a snap, none, or not a weight): w as it is.
[[gnu::always_inline]] static inline float weight(float w, float frames) {
    if (!(frames > 0.0f) || frames == 1.0f || !(w > 0.0f) || !(w < 1.0f)) return w;
    return static_cast<float>(1.0 - pow_unit(1.0 - static_cast<double>(w), static_cast<double>(frames)));
}

// A share kept a frame (difference *= k): k^frames; the same cases as is.
[[gnu::always_inline]] static inline float keep(float k, float frames) {
    if (!(frames > 0.0f) || frames == 1.0f || !(k > 0.0f) || !(k < 1.0f)) return k;
    return static_cast<float>(pow_unit(static_cast<double>(k), static_cast<double>(frames)));
}

}  // namespace frame_rate_step
