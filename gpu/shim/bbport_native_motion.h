// SPDX-FileCopyrightText: Copyright 2026 Ronny Stiftel
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
namespace BbNativeMotion {
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_METAL_NATIVE_MOTION");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

// Provenance belongs to one image backing and one rendered frame. Any other
// writer or CPU refresh invalidates it. This ports native_velocity.inc's
// producer contract; unknown dilation passes deliberately keep the fallback.
struct State {
    std::uint64_t frame = ~0ull;
    bool cleared = false, produced = false;
    std::array<float, 2> scale{};
    std::shared_ptr<std::atomic<bool>> encoded;
};
inline bool CanonicalClear(const std::array<float, 4>& c) {
    return std::abs(c[0]-127.f/255) < 1e-6f && std::abs(c[1]-127.f/255) < 1e-6f &&
           c[2] == 1 && c[3] == 0;
}
inline bool Producer(std::uint64_t hash) {
    return hash == 0xaf04e37b2bb2a2b6ull || hash == 0x570e1a1bf6b555a0ull;
}
inline bool Vertex(std::uint64_t hash) {
    return hash == 0xc8b56cc8164c9750ull || hash == 0x5ffe56ee585d1a5full ||
           hash == 0x0c51d94fb1dade98ull || hash == 0x8fd61d8ed1143963ull ||
           hash == 0x2a56114753454010ull;
}
inline bool DecodeScale(const std::array<float, 2>& producer, std::array<float, 2>& uv) {
    if (!std::isfinite(producer[0]) || !std::isfinite(producer[1]) ||
        producer[0] < .01f || producer[0] > 1024 ||
        producer[1] > -.01f || producer[1] < -1024) return false;
    uv = {-1/producer[0], 1/producer[1]};
    return true;
}
}
