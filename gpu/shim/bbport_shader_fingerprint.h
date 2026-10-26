// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <span>
namespace BbShader {
// Instruction-word FNV-1a, independent of the retail header and of each
// recompiler's fetch-shader expansion. Reject a missing terminator.
inline std::uint64_t Fingerprint(std::span<const std::uint32_t> words) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (auto word : words) {
        hash = (hash ^ word) * 0x100000001b3ull;
        if (word == 0xbf810000u) return hash;
    }
    return 0;
}
}
