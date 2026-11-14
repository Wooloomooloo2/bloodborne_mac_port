// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <limits>
#include <optional>

namespace BbUpload {
struct Reservation { uint64_t offset, end; };

// Positions are monotonic; retired only advances after the associated GPU tick.
// Reject impossible sizes and arithmetic overflow before publishing a position.
inline std::optional<Reservation> Reserve(uint64_t position, uint64_t retired,
                                         uint64_t size, uint64_t alignment, uint64_t capacity) {
    constexpr auto max = std::numeric_limits<uint64_t>::max();
    if (!size || size > capacity || !alignment || (alignment & (alignment - 1)) ||
        alignment > capacity || capacity % alignment || retired > position ||
        position > max - (alignment - 1)) return std::nullopt;
    uint64_t at = (position + alignment - 1) & ~(alignment - 1);
    const uint64_t offset = at % capacity;
    if (offset > capacity - size) {
        const uint64_t padding = capacity - offset;
        if (at > max - padding) return std::nullopt;
        at += padding;
    }
    if (at > max - size) return std::nullopt;
    const uint64_t end = at + size;
    if (end > capacity && end - capacity > retired) return std::nullopt;
    return Reservation{at % capacity, end};
}
} // namespace BbUpload
