// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
struct SprjFlipper;
namespace PalebloodReference {
struct Clock {
    virtual std::uint64_t Now() = 0;
    virtual void Sleep(std::uint32_t microseconds) = 0;
    virtual ~Clock() = default;
};
void PaceFrame(SprjFlipper& state, Clock& clock, bool spin_only, void* notify);
}
