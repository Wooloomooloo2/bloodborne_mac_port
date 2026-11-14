// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace BbGnm {
inline constexpr uint32_t MaxCommandBytes = 0x3ffffc;
inline constexpr uint32_t FlipReservationWords = 64;

// Validate the complete batch before waiting, publishing work, or patching a flip.
// Optional CCBs may be empty; every nonempty region has a DWORD-aligned address
// and byte count. The last DCB of a flip owns the engine's 64-word reservation.
inline bool ValidSubmit(uint32_t count, const uint32_t* const* dcbs, const uint32_t* sizes,
                        const uint32_t* const* ccbs, const uint32_t* ccb_sizes, bool flip) {
    if (!dcbs || !sizes || (flip && count == 0) || (ccb_sizes && !ccbs)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (!dcbs[i] || (reinterpret_cast<uintptr_t>(dcbs[i]) & 3) ||
            !sizes[i] || (sizes[i] & 3) || sizes[i] > MaxCommandBytes) return false;
        if (ccb_sizes && ((ccb_sizes[i] & 3) || ccb_sizes[i] > MaxCommandBytes ||
            (ccb_sizes[i] && (!ccbs[i] || (reinterpret_cast<uintptr_t>(ccbs[i]) & 3))))) return false;
    }
    if (!flip) return true;
    const uint32_t bytes = sizes[count - 1];
    if (bytes < FlipReservationWords * sizeof(uint32_t)) return false;
    const uint32_t* marker = dcbs[count - 1] + bytes / sizeof(uint32_t) - FlipReservationWords;
    if (marker[0] != 0xc03e1000) return false;
    // The label variant carries an aligned address. Ordinary markers only
    // initialize the first two words: their untouched tail must stay opaque.
    return marker[1] != 0x68750778 || (marker[2] & 3) == 0;
}
} // namespace BbGnm
