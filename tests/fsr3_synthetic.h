// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: synthetic FSR 3 input frames shared by the Vulkan golden tool (tests/data/fsr3/README)
// and the Metal comparison test (tests/test_ffx_metal.mm, Fsr3MatchesVulkan), with the one set of
// dispatch constants and golden cases both use (Metal-native M6 Task 3). The scene: a checkerboard
// times a colour ramp, moving 1.25 render pixels per frame to the right, with matching motion
// vectors, a horizontal depth gradient, Halton(2, 3) jitter as vk_temporal_upscaler.cpp, and resets
// at frames 0 and 5. Plain C++.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Fsr3Synthetic {

// Dispatch constants (vk_temporal_upscaler.cpp's values; near, fov and frame time are fixed for
// synthetic frames).
constexpr float kFrameTimeMs = 16.6f;
constexpr float kCameraNear = 0.1f;
constexpr float kCameraFar = 3000.0f;
constexpr float kCameraFovY = 1.0f; // radians
constexpr float kViewSpaceToMeters = 1.0f;
constexpr float kPreExposure = 1.0f;
constexpr float kMotionScale = 1.0f; // motionVectorScale {1, 1}: vectors in render pixels
constexpr float kSpeed = 1.25f;      // render pixels per frame, to the right

constexpr uint32_t kFrames = 8;
constexpr uint32_t kGoldenFrames[] = {4, 7};

// FFX_FSR3UPSCALER_ENABLE_* bits (also FFX_VK_PORTABLE_CONTEXT_*: the same values).
constexpr uint32_t kFlagHdr = 1u << 0;
constexpr uint32_t kFlagAutoExposure = 1u << 5;

struct Fsr3Case {
    const char* name;
    uint32_t render_width, render_height;
    uint32_t output_width, output_height;
    uint32_t flags;
    bool sharpen;
    float sharpness;
};

constexpr Fsr3Case kCases[] = {
    {"quality_hdr", 256, 144, 384, 216, kFlagHdr | kFlagAutoExposure, true, 0.3f},
    {"odd_up", 171, 96, 512, 288, 0, false, 0.0f},
};

struct Fsr3Frame {
    std::vector<uint16_t> color;  ///< RGBA16F, render size
    std::vector<float> depth;     ///< R32F, render size
    std::vector<uint16_t> motion; ///< RG16F, render pixels (previous = current + motion)
    float jitter_x, jitter_y;     ///< pixels
    bool reset;
};

inline uint16_t Half(float value) {
    const _Float16 half = _Float16(value);
    uint16_t bits;
    std::memcpy(&bits, &half, sizeof(bits));
    return bits;
}

inline float FromHalf(uint16_t bits) {
    _Float16 half;
    std::memcpy(&half, &bits, sizeof(half));
    return float(half);
}

inline float Halton(uint32_t index, uint32_t base) {
    float f = 1.0f, result = 0.0f;
    for (uint32_t i = index; i > 0; i /= base) {
        f /= float(base);
        result += f * float(i % base);
    }
    return result;
}

inline float Fract(float value) {
    return value - std::floor(value);
}

inline Fsr3Frame MakeFrame(uint32_t index, uint32_t w, uint32_t h) {
    Fsr3Frame frame;
    frame.jitter_x = Halton(index % 8 + 1, 2) - 0.5f;
    frame.jitter_y = Halton(index % 8 + 1, 3) - 0.5f;
    frame.reset = index == 0 || index == 5;
    frame.color.resize(size_t(w) * h * 4);
    frame.depth.resize(size_t(w) * h);
    frame.motion.resize(size_t(w) * h * 2);
    const float shift = kSpeed * float(index);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t i = size_t(y) * w + x;
            // The scene point this jittered sample sees: the scene moves right by `shift`.
            const float sx = float(x) + 0.5f - frame.jitter_x - shift;
            const float sy = float(y) + 0.5f - frame.jitter_y;
            const bool odd = ((int(std::floor(sx / 8.0f)) + int(std::floor(sy / 8.0f))) & 1) != 0;
            const float checker = odd ? 0.85f : 0.15f;
            const float u = Fract(sx / 64.0f), v = Fract(sy / float(h));
            frame.color[i * 4 + 0] = Half(checker * (0.3f + 0.7f * u));
            frame.color[i * 4 + 1] = Half(checker * (0.3f + 0.7f * v));
            frame.color[i * 4 + 2] = Half(0.5f * checker + 0.25f * u);
            frame.color[i * 4 + 3] = Half(1.0f);
            frame.depth[i] = 0.1f + 0.8f * (float(x) + 0.5f) / float(w);
            frame.motion[i * 2 + 0] = Half(-kSpeed);
            frame.motion[i * 2 + 1] = Half(0.0f);
        }
    }
    return frame;
}

} // namespace Fsr3Synthetic
