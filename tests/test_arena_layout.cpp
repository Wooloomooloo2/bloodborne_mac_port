// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// BB_ARENA_LAYOUT=runs with real Metal: one buffer per run of GPU-mapped guest memory, created by the
// GPU command thread when the runs change; bindings, index offsets, downloads and the routes for
// requests no buffer holds (a guard past a run, CPU-only memory read and written).
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <setjmp.h>
#include <sys/mman.h>
#include "common/elf_info.h"
#include "../gpu/bbgpu.h"
#include "bbport_arena_runs.h"
#include "bbport_toggles.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"

namespace Core {
class Emulator {
public:
    static void FillElfInfo(const BbGpuConfig& config) {
        auto& info = Common::ElfInfo::Instance();
        info.initialized = true;
        info.game_serial = config.serial ? config.serial : "UNKNOWN";
        info.title = config.title ? config.title : "";
        info.sdk_ver = config.sdk_version;
        info.psf_attributes.raw = config.psf_attributes;
    }
};
} // namespace Core

namespace {
constexpr uintptr_t Base = 0x7000000000ULL;
constexpr size_t MappedSize = 4 * 1024 * 1024;
// GPU run 1 at Base, CPU-only memory 64 MiB above, GPU run 2 at 4 GiB + 512 MiB.
constexpr uintptr_t CpuOnly = Base + (64ull << 20);
constexpr uintptr_t HighRun = Base + (9ull << 29);
constexpr u32 Size = 64 * 1024;
struct Region {
    uintptr_t start;
    int prot;
};
constexpr Region Regions[] = {{Base, 0x33}, {CpuOnly, 0x3}, {HighRun, 0x33}};
const Region* RegionOf(uintptr_t p) {
    for (const auto& r : Regions) {
        if (p >= r.start && p < r.start + MappedSize) {
            return &r;
        }
    }
    return nullptr;
}
bool Covered(uintptr_t p, uint64_t n) {
    const Region* r = RegionOf(p);
    return r && n <= r->start + MappedSize - p;
}
int failures = 0;
void Check(bool ok, const char* message) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}
} // namespace

#pragma GCC visibility push(default)
extern "C" {
__thread sigjmp_buf* runtime_fault_recover = nullptr;
uint64_t runtime_disabled_optimizations = 0;
uint64_t runtime_tsc_frequency() { return 1000000000; }
uint64_t runtime_process_time_us() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
uint64_t runtime_process_time_counter() { return runtime_process_time_us() * 1000; }
void runtime_thread_attach_host(const char*) {}
void runtime_thread_set_blocked(const char*, uint64_t) {}
void runtime_thread_clear_blocked() {}
uint64_t runtime_thread_cpu_ns(int) { return 0; }
void runtime_game_cpu_since_last(uint64_t* a, uint64_t* b, char n[32]) { *a = *b = 0; n[0] = 0; }
int runtime_file_translate(const char*, char*, size_t) { std::abort(); }
int32_t* runtime_errno() { std::abort(); }
void runtime_restart() { std::abort(); }
uint64_t runtime_memory_clamp(uintptr_t p, uint64_t n) { return Covered(p, n) ? n : 0; }
int runtime_memory_vma_info(uintptr_t p, int* prot, int* type, uintptr_t* end) {
    const Region* r = RegionOf(p);
    if (!r) return 0;
    if (prot) *prot = r->prot;
    if (type) *type = -1;
    if (end) *end = r->start + MappedSize;
    return 1;
}
int runtime_memory_region(uintptr_t p, uintptr_t* a, uintptr_t* b, int* mapped) {
    if (const Region* r = RegionOf(p)) {
        *a = r->start; *b = r->start + MappedSize; *mapped = 1; return 1;
    }
    // The gap up to the next region, 0 past the last (as the host's view table).
    for (const auto& r : Regions) {
        if (p < r.start) {
            *a = p; *b = r.start; *mapped = 0; return 1;
        }
    }
    return 0;
}
void runtime_memory_set_gpu_hooks(void (*map)(uintptr_t, uint64_t), void (*)(uintptr_t, uint64_t), void (*)(uintptr_t, uint64_t)) {
    for (const auto& r : Regions) map(r.start, MappedSize);
}
void runtime_memory_gpu_protect(uintptr_t p, uint64_t n, int, int) { assert(Covered(p, n)); }
int runtime_memory_write_backing(uintptr_t p, const void* data, uint64_t n) {
    if (!Covered(p, n)) return 0;
    std::memcpy(reinterpret_cast<void*>(p), data, n); return 1;
}
int runtime_memory_read_backing(uintptr_t p, void* data, uint64_t n) {
    if (!Covered(p, n)) return 0;
    std::memcpy(data, reinterpret_cast<void*>(p), n); return 1;
}
}
#pragma GCC visibility pop

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setenv("BB_NULL_GPU", "1", 1);
    setenv("BB_DRAW_PIPE", "0", 1);
    setenv("BB_PIPELINE_CACHE", "0", 1);
    setenv("BB_READBACKS", "2", 1);
    setenv("BB_ARENA_LAYOUT", "runs", 1);
    setenv("BB_ARENA_GUARD_MB", "1", 1);
    for (const auto& r : Regions) {
        void* p = mmap(reinterpret_cast<void*>(r.start), MappedSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        assert(p == reinterpret_cast<void*>(r.start));
    }
    const BbGpuConfig config = {"Arena layout test", "TEST", nullptr, 0x05050031, 0, 640, 360};
    Core::Emulator::FillElfInfo(config);
    Metal::Device device;
    Vulkan::Scheduler scheduler(device, false);
    Vulkan::Runtime runtime(device, scheduler);
    auto liverpool = std::make_unique<AmdGpu::Liverpool>();
    auto owner = std::make_unique<Vulkan::Rasterizer>(device, scheduler, runtime, liverpool.get());
    auto& cache = owner->GetBufferCache();
    // The view hooks saw the three maps (Rasterizer: MemoryManager::SetRasterizer); the ring
    // thread's Refresh publishes the runs, and the GPU command thread creates the buffers.
    BbArenaRuns::Refresh();
    liverpool->SendCommand<true>([] {});

    const auto [low, low_offset] = cache.ObtainBuffer(Base + Size, Size, false);
    const auto [high, high_offset] = cache.ObtainBuffer(HighRun + Size, Size, false);
    Check(low != nullptr && low_offset == Size, "run 1 has its own buffer, from its start");
    Check(high != nullptr && high != low && high_offset == Size,
          "run 2 has another, offsets from its start (not 4.5 GiB)");
    Check(low->SizeBytes() == MappedSize + (1u << 20) && high->buffer.HostView() != nullptr,
          "each holds its run and the guard, with a CPU view");
    const u64 copies = BbStats::index_copies_cpu.load() + BbStats::index_copies_gpu.load();
    const auto [index, index_offset] = cache.ObtainIndexBuffer(HighRun + 2 * Size, Size);
    Check(index == high && index_offset == 2 * Size &&
              BbStats::index_copies_cpu.load() + BbStats::index_copies_gpu.load() == copies,
          "indices of the run past 4 GiB need no copy");

    // GPU-written data in run 2, downloaded back into guest memory (offsets in its buffer).
    const uintptr_t written = HighRun + 3 * Size;
    std::memset(reinterpret_cast<void*>(HighRun + Size), 0x5a, Size);
    const auto [src, src_offset] = cache.ObtainBuffer(HighRun + Size, Size, false);
    const auto [dst, dst_offset] = cache.ObtainBuffer(written, Size, true);
    const VideoCore::BufferCopy copy{src_offset, dst_offset, Size};
    runtime.CopyBuffer(*src, *dst, std::span{&copy, 1});
    scheduler.Finish();
    Check(reinterpret_cast<u8*>(written)[0] == 0, "the GPU write leaves guest memory stale");
    cache.ReadMemory(written, 64, false, true);
    Check(reinterpret_cast<u8*>(written)[0] == 0x5a && reinterpret_cast<u8*>(written)[63] == 0x5a,
          "a read-back brings it to guest memory");

    // Routes for requests no buffer holds.
    // (Above 16 KiB: smaller read-only ranges take the stream buffer.)
    const auto [guarded, guarded_offset] =
        cache.ObtainBuffer(Base + MappedSize - 4096, 32 * 1024, false);
    Check(guarded == low && guarded_offset == MappedSize - 4096,
          "a range ending in the guard is bound in its run's buffer");
    const auto [past, past_offset] = cache.ObtainBuffer(Base + MappedSize - 4096, 2 << 20, false);
    Check(past != low && past != high, "one ending past the guard gets a copy");
    std::memset(reinterpret_cast<void*>(CpuOnly), 0x33, Size);
    const auto [cpu, cpu_offset] = cache.ObtainBuffer(CpuOnly, Size, false);
    scheduler.Finish();
    Check(cpu != low && cpu != high && cpu->mapped_data.size() >= cpu_offset + Size &&
              cpu->mapped_data[cpu_offset] == 0x33 && cpu->mapped_data[cpu_offset + Size - 1] == 0x33,
          "a read of CPU-only memory gets a copy of its bytes");
    const auto [scratch, scratch_offset] = cache.ObtainBuffer(CpuOnly + Size, Size, true);
    Check(scratch != nullptr && scratch != low && scratch != high,
          "a write to CPU-only memory gets a scratch range");
    cache.ReadMemory(CpuOnly, 8, false, true);
    Check(BbStats::uncovered_readbacks.load() == 1, "a read-back there is declined");
    Check(cache.GetChunks().size() == 2, "two buffers in all");
    scheduler.Finish();
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
