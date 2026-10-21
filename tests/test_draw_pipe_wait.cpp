// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone CPU-only exercise of the actual production DrawPipe implementation.
#include <cassert>
#include <cstdio>
#include <ctime>
#include "video_core/renderer_vulkan/vk_draw_pipe.h"

void assert_fail_impl() { std::abort(); }
namespace Common { void SetCurrentThreadName(const char*) {} }

struct State { u64 next = 0; bool slow = false; };
static void Consume(void* raw, const u8* packet, u32 size) {
    auto& state = *static_cast<State*>(raw);
    u64 value;
    assert(size >= sizeof value);
    std::memcpy(&value, packet, sizeof value);
    assert(value == state.next++);
    if (state.slow) std::this_thread::sleep_for(std::chrono::milliseconds(80));
}

int main() {
    State state;
    {
        Vulkan::DrawPipe pipe(Consume, &state);
        state.slow = true;
        const u64 value = 0;
        std::memcpy(pipe.Begin(sizeof value), &value, sizeof value);
        const auto wall_start = std::chrono::steady_clock::now();
        const auto cpu_start = std::clock();
        pipe.Commit(sizeof value);
        pipe.Drain();
        const double cpu_ms = 1000.0 * (std::clock() - cpu_start) / CLOCKS_PER_SEC;
        const double wall_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - wall_start).count();
        std::printf("80 ms blocked handler: %.2f ms CPU, %.2f ms elapsed\n", cpu_ms, wall_ms);
        const char* mode = std::getenv("BB_PIPE_BLOCKING_WAIT");
        if (!mode || mode[0] != '0') assert(cpu_ms < 25.0);
        assert(state.next == 1 && pipe.Idle());
        state.slow = false;
        // Small and large packets, repeated full-ring backpressure and wrapping.
        for (u64 value = 1; value <= 50000; ++value) {
            const u32 size = value % 7 == 0 ? 131072 : 8;
            std::memcpy(pipe.Begin(size), &value, sizeof value);
            pipe.Commit(size);
            if (value % 97 == 0) pipe.Drain();
        }
        pipe.Drain();
        assert(state.next == 50001 && pipe.Reached(pipe.Head()));
    }
    // Repeated idle/start/stop transitions test wakeups and destruction.
    for (unsigned n = 0; n < 100; ++n) {
        State small;
        Vulkan::DrawPipe pipe(Consume, &small);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const u64 value = 0;
        std::memcpy(pipe.Begin(sizeof value), &value, sizeof value);
        pipe.Commit(sizeof value);
    }
    std::puts("DrawPipe ordering, wrap, backpressure, wake and shutdown tests passed");
}
