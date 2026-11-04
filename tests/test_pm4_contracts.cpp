// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include "bbport_pm4_bounds.h"

int main() {
    // Literal extents from Paleblood's engine writers: a seven-word counter wait
    // and a 64-word reservation that only initializes its first two words.
    constexpr std::array<uint32_t, 7> wait = {0xc0053c00, 0x15, 0x12345678, 0xabcd,
                                              0x80000000, 0xffffffff, 10};
    static_assert(BbPm4::PacketWords(wait[0]) == 7);
    static_assert(BbPm4::PacketWords(0xc03e1000) == 64);
    for (size_t n = 0; n < wait.size(); ++n) assert(!BbPm4::CompletePacket(wait[0], n));
    assert(BbPm4::CompletePacket(wait[0], wait.size()));
    assert(BbPm4::CompletePacket(wait[0], 65));
    assert(!BbPm4::CompletePacket(0xc0003c00, 100));
    assert(!BbPm4::CompletePacket(0xc0054900, 100));
    assert(BbPm4::CompletePacket(0xc0064900, 8));
    std::array<uint32_t, 64> marker;
    marker.fill(0xa5a5a5a5);
    marker[0] = 0xc03e1000; marker[1] = 0x68750777;
    assert(BbPm4::CompletePacket(marker[0], marker.size()));
    assert(!BbPm4::CompletePacket(marker[0], marker.size() - 1));
    for (uint32_t h : {0x80000000u, 0xbfffffffu})
        assert(BbPm4::PacketWords(h) == 1 && BbPm4::CompletePacket(h, 1));
    assert(!BbPm4::CompletePacket(0x40000000, 100));
    assert(BbPm4::CompletePacket(0, 2) && !BbPm4::CompletePacket(0, 1));
    assert(BbPm4::CompletePacket(0xffff1000, 16385));
    assert(!BbPm4::CompletePacket(0xffff1000, 16384));
    puts("PASS: original packet extents, marker tails and truncated wait/release rejection");
}
