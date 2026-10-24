// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/arch.h"

#ifdef _MSC_VER
#include <intrin.h>
#endif

#include "common/types.h"

namespace Common {

#ifdef _MSC_VER
__forceinline static u64 FencedRDTSC() {
#ifdef ARCH_X86_64
    _mm_lfence();
    _ReadWriteBarrier();
    const u64 result = __rdtsc();
    _mm_lfence();
    _ReadWriteBarrier();
    return result;
#else
#error "Missing FencedRDTSC() implementation for target CPU architecture."
#endif
}
#else
static inline u64 FencedRDTSC() {
#ifdef ARCH_X86_64
    u64 eax;
    u64 edx;
    asm volatile("lfence\n\t"
                 "rdtsc\n\t"
                 "lfence\n\t"
                 : "=a"(eax), "=d"(edx));
    return (edx << 32) | eax;
#elif defined(ARCH_ARM64)
    // bbport: the native arm64 GPU host must produce the same "TSC" as the game process, where
    // the guest's RDTSC is translated by Rosetta 2. Measured on the M4 Max: CNTFRQ_EL0 is 1 GHz
    // (fixed from ARMv8.6 on) and Rosetta's RDTSC returns CNTVCT_EL0 itself, so the counter is
    // used as is. Older cores (M1-M3) count at 24 MHz while Rosetta reports a 1 GHz TSC; scale
    // to 1 GHz there (assumed, not measured: no such machine at hand). Units must also match
    // runtime_tsc_frequency(), which the runtime measures from RDTSC in the game process.
    u64 ret;
    asm volatile("isb\n\t"
                 "mrs %0, cntvct_el0\n\t"
                 "isb\n\t"
                 : "=r"(ret)::"memory");
    static const u64 frequency = [] {
        u64 f;
        asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
        return f;
    }();
    if (frequency != 1'000'000'000) [[unlikely]] {
        return static_cast<u64>(static_cast<unsigned __int128>(ret) * 1'000'000'000 / frequency);
    }
    return ret;
#else
#error "Missing FencedRDTSC() implementation for target CPU architecture."
#endif
}
#endif

u64 EstimateRDTSCFrequency();

} // namespace Common
