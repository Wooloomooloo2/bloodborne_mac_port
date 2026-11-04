// SPDX-License-Identifier: GPL-2.0-or-later
// Actual public entry points must refuse invalid work before any GPU state is used.
#include <array>
#include <cassert>
#include <cstdio>
#include "core/libraries/gnmdriver/gnmdriver.h"

int main() {
    using namespace Libraries::GnmDriver;
    constexpr auto invalid = static_cast<s32>(0x80d11000u);
    assert(sceGnmSubmitAndFlipCommandBuffers(0, nullptr, nullptr, nullptr, nullptr, 0, 0, 0, 0) == invalid);
    std::array<u32, 64> commands{};
    u32* dcbs[] = {commands.data()};
    u32 sizes[] = {252};
    assert(sceGnmSubmitAndFlipCommandBuffers(1, dcbs, sizes, nullptr, nullptr, 0, 0, 0, 0) == invalid);
    sizes[0] = sizeof(commands);
    assert(sceGnmSubmitAndFlipCommandBuffers(1, dcbs, sizes, nullptr, nullptr, 0, 0, 0, 0) == invalid);
    const u32* const_dcbs[] = {nullptr};
    assert(sceGnmSubmitCommandBuffers(1, const_dcbs, sizes, nullptr, nullptr) == invalid);
    const_dcbs[0] = commands.data(); sizes[0] = 3;
    assert(sceGnmSubmitCommandBuffers(1, const_dcbs, sizes, nullptr, nullptr) == invalid);
    puts("PASS: public GNM APIs reject invalid batches without initializing a GPU");
}
