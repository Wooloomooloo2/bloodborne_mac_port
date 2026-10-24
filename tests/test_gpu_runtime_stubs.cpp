// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <setjmp.h>
// Renderer tests have no guest process. Clock/host-thread services work; guest accesses abort.
extern "C" {
// __thread, not thread_local: a C++ thread_local is not exported for the library's lookup.
__thread sigjmp_buf* runtime_fault_recover = nullptr;
uint64_t runtime_disabled_optimizations = 0;
uint64_t runtime_tsc_frequency() { return 1000000000; }
int runtime_file_translate(const char*, char*, size_t) { std::abort(); }
uint64_t runtime_memory_clamp(uintptr_t, uint64_t) { std::abort(); }
uint64_t runtime_process_time_us() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int runtime_memory_region(uintptr_t, uintptr_t*, uintptr_t*, int*) { std::abort(); }
void runtime_memory_set_gpu_hooks(void (*)(uintptr_t, uint64_t),
    void (*)(uintptr_t, uint64_t), void (*)(uintptr_t, uint64_t)) { std::abort(); }
void runtime_thread_attach_host(const char*) {}
void runtime_memory_gpu_protect(uintptr_t, uint64_t, int, int) { std::abort(); }
void runtime_restart() { std::abort(); }
uint64_t runtime_process_time_counter() { return runtime_process_time_us() * 1000; }
int32_t* runtime_errno() { std::abort(); }
int runtime_memory_write_backing(uintptr_t, const void*, uint64_t) { std::abort(); }
int runtime_memory_read_backing(uintptr_t, void*, uint64_t) { std::abort(); }
uint64_t runtime_thread_cpu_ns(int) { return 0; }
void runtime_game_cpu_since_last(uint64_t* total_ns, uint64_t* busiest_ns, char busiest_name[32]) {
    *total_ns = *busiest_ns = 0;
    busiest_name[0] = 0;
}
void runtime_thread_set_blocked(const char*, uint64_t) {}
void runtime_thread_clear_blocked() {}
}

