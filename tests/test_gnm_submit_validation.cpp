// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>
#include "bbport_gnm_submit.h"

int main() {
    std::array<uint32_t, 128> first{}, last{};
    last.fill(0xa5a5a5a5); // unused marker words are not zero-initialized by the engine
    last[64] = 0xc03e1000; last[65] = 0x68750777;
    const uint32_t* dcbs[] = {first.data(), last.data()};
    uint32_t sizes[] = {4, sizeof(last)};
    const auto valid = [&](bool flip = true) {
        return BbGnm::ValidSubmit(2, dcbs, sizes, nullptr, nullptr, flip);
    };
    assert(valid());
    const auto before = last;
    assert(!BbGnm::ValidSubmit(0, nullptr, nullptr, nullptr, nullptr, true));
    assert(!BbGnm::ValidSubmit(0, dcbs, sizes, nullptr, nullptr, true));
    assert(BbGnm::ValidSubmit(0, dcbs, sizes, nullptr, nullptr, false));
    for (uint32_t bad : {0u, 1u, 3u, 5u, 255u, BbGnm::MaxCommandBytes + 4,
                         std::numeric_limits<uint32_t>::max()}) {
        sizes[0] = bad; assert(!valid());
    }
    sizes[0] = 4;
    for (uint32_t bad : {4u, 248u, 252u}) {
        sizes[1] = bad; assert(!valid()); assert(valid(false));
    }
    sizes[1] = sizeof(last);
    dcbs[0] = nullptr; assert(!valid());
    dcbs[0] = reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(first.data()) + 1);
    assert(!valid()); dcbs[0] = first.data();
    uint32_t ccb_sizes[] = {0, 4};
    const uint32_t* ccbs[] = {nullptr, first.data()};
    assert(!BbGnm::ValidSubmit(2, dcbs, sizes, nullptr, ccb_sizes, false));
    assert(BbGnm::ValidSubmit(2, dcbs, sizes, ccbs, ccb_sizes, true));
    ccbs[1] = nullptr;
    assert(!BbGnm::ValidSubmit(2, dcbs, sizes, ccbs, ccb_sizes, false));
    ccbs[1] = first.data(); ccb_sizes[1] = 3;
    assert(!BbGnm::ValidSubmit(2, dcbs, sizes, ccbs, ccb_sizes, false));
    assert(last == before); // validation cannot rewrite a previously admitted region
    last[64] ^= 1; assert(!valid()); last[64] ^= 1;
    last[65] = 0x68750778; last[66] = 0x123;
    assert(!valid()); last[66] = 0x120; assert(valid());
    puts("PASS: GNM whole-batch validation, empty flips, marker capacity and optional CCBs");
}
