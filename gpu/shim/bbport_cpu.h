// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: CPU spin-wait hint and cycle counter, portable between the x86-64 build (game process
// under Rosetta 2) and the native arm64 GPU host (docs/DESIGN_NATIVE_GPU_PROCESS.md).
//
// x86-64: exactly the previous __builtin_ia32_pause() / __rdtsc().
// arm64:  `yield` and the virtual counter CNTVCT_EL0. Measured on the M4 Max (2026-10-11):
//         CNTFRQ_EL0 = 1 GHz (fixed by ARMv8.6+) and Rosetta's RDTSC returns the same
//         CNTVCT_EL0 value (same epoch and units), so cycle counts and guest TSC values agree
//         between the two processes. Only used for statistics here; guest-visible TSC reads
//         go through sceKernelReadTsc (shim/bbgpu.cpp).

#pragma once

#include <chrono>
#include <cstdint>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace bbport {

static inline __attribute__((always_inline)) void CpuPause() {
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#else
#error "bbport::CpuPause: unsupported architecture"
#endif
}

static inline __attribute__((always_inline)) std::uint64_t CpuCycles() {
#if defined(__x86_64__)
    return __rdtsc();
#elif defined(__aarch64__)
    std::uint64_t value;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
#else
#error "bbport::CpuCycles: unsupported architecture"
#endif
}

/// CpuCycles() ticks per second. The counter runs at 1 GHz on the M4 but 24 MHz on M1-M3:
/// statistics that print cycle deltas as times must convert them (CyclesToNs). arm64 reads
/// CNTFRQ_EL0; under Rosetta RDTSC returns the same counter, timed once against the clock.
inline std::uint64_t CpuCyclesPerSecond() {
    static const std::uint64_t frequency = [] {
#if defined(__aarch64__)
        std::uint64_t value;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
        return value;
#else
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        const std::uint64_t first = CpuCycles();
        while (Clock::now() - start < std::chrono::milliseconds(2)) {
        }
        const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     Clock::now() - start).count());
        const double hz = double(CpuCycles() - first) * 1e9 / ns;
        // Snap to the known counter rates (24 MHz, 1 GHz) when close.
        for (const double known : {24e6, 1e9}) {
            if (hz > known * 0.95 && hz < known * 1.05) {
                return std::uint64_t(known);
            }
        }
        return std::uint64_t(hz);
#endif
    }();
    return frequency;
}

/// Converts a CpuCycles() delta to nanoseconds.
static inline std::uint64_t CyclesToNs(std::uint64_t cycles) {
    // 32.32 fixed point: exact for 1 GHz, 1e9/24e6 within 2^-32 otherwise.
    static const std::uint64_t scale =
        std::uint64_t((static_cast<unsigned __int128>(1000000000) << 32) / CpuCyclesPerSecond());
    return std::uint64_t((static_cast<unsigned __int128>(cycles) * scale) >> 32);
}

} // namespace bbport
