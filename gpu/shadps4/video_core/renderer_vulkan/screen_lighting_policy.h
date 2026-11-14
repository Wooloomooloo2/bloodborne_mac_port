// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace Vulkan::ScreenLightingPolicy {
// Verified game 1.09 opaque scene composite. Unknown variants leave the game intact;
// a late post-process fallback could incorrectly darken particles and fog.
constexpr bool OpaqueComposite(uint64_t vs, uint64_t ps, uint32_t outputs, uint32_t vertices,
                               uint32_t instances, bool blended, uint32_t write_mask = 15) {
    return vs == 0x8747a367 && ps == 0x32e47788 && outputs == 1 &&
           (vertices == 3 || vertices == 4) && instances == 1 && !blended && (write_mask & 7) == 7;
}
inline std::array<uint32_t, 2> TraceSize(uint32_t width, uint32_t height) {
    const double scale =
        std::min({0.5, 1280.0 / std::max(1u, width), 720.0 / std::max(1u, height)});
    return {std::max(1u, uint32_t(std::ceil(width * scale))),
            std::max(1u, uint32_t(std::ceil(height * scale)))};
}
} // namespace Vulkan::ScreenLightingPolicy
