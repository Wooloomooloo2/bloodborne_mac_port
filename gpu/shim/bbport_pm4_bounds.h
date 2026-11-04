// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

namespace BbPm4 {
// Type-2 padding is one word. Type-0/3 encode payload words minus one.
inline constexpr size_t PacketWords(uint32_t header) {
    const uint32_t type = header >> 30;
    return type == 2 ? 1 : type == 0 || type == 3 ? 2 + ((header >> 16) & 0x3fff) : 0;
}
inline constexpr bool CompletePacket(uint32_t header, size_t remaining) {
    const size_t words = PacketWords(header);
    if (!words || words > remaining) return false;
    if ((header >> 30) == 3) {
        const uint32_t opcode = (header >> 8) & 0xff;
        // These handlers read fixed layouts even if a corrupt header is short.
        if ((opcode == 0x3c && words < 7) || (opcode == 0x49 && words < 8)) return false;
    }
    return true;
}
} // namespace BbPm4
