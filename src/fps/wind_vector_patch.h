// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace MacWindVectorPatch {
constexpr std::uint32_t Site = 0x02714a60;
constexpr float AuthoredSeconds = 1.f / 60.f;
// Original 1.09: vpshufd xmm4,xmm14,0; vmulps xmm12,xmm4,xmm2.
constexpr std::array<std::uint8_t, 10> Original{0xc4,0xc1,0x79,0x70,0xe6,0x00,0xc5,0x58,0x59,0xe2};
// Include the following overwrite of xmm4 in admission, without changing it.
constexpr std::array<std::uint8_t, 14> Context{0xc4,0xc1,0x79,0x70,0xe6,0x00,0xc5,0x58,0x59,0xe2,
                                              0xc5,0xf0,0x59,0xe1};
using Rewrite = std::array<std::uint8_t, Original.size()>;

// No output changes on refusal. The m128 operand needs four readable floats;
// its lifetime and ownership belong to the installed image's timing tail.
inline bool Build(const void* original, std::size_t size, std::uint64_t instruction,
                  std::uint64_t constant, Rewrite& result) {
    if (!original || size != Context.size() || std::memcmp(original, Context.data(), size) ||
        instruction > std::numeric_limits<std::uint64_t>::max() - 8) return false;
    const auto next = instruction + 8;
    std::uint32_t displacement;
    if (constant >= next) {
        const auto distance = constant - next;
        if (distance > 0x7fffffffu) return false;
        displacement = static_cast<std::uint32_t>(distance);
    } else {
        const auto distance = next - constant;
        if (distance > 0x80000000u) return false;
        displacement = std::uint32_t(0) - static_cast<std::uint32_t>(distance);
    }
    // vmulps xmm12,xmm2,[rip+disp32]; nop; nop. xmm4 is overwritten by
    // the very next original instruction; xmm14 still drives phase/fades.
    Rewrite encoded{0xc5,0x68,0x59,0x25,0,0,0,0,0x90,0x90};
    for (unsigned k = 0; k < 4; ++k) encoded[4 + k] = static_cast<std::uint8_t>(displacement >> (8 * k));
    result = encoded;
    return true;
}
}
