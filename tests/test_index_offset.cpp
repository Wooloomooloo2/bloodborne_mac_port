// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// Real Metal indexed draws from low, high CPU-owned and high GPU-produced guest ranges.
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
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_metal/mtl_buffer.h"
#include "bbport_toggles.h"

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
}

namespace {
constexpr uintptr_t Base = 0x7000000000ULL;
constexpr uintptr_t HighBase = Base + (1ull << 32);
constexpr size_t MappedSize = 4 * 1024 * 1024;
constexpr u32 Size = 64 * 1024; // above the small, uncached stream-buffer threshold
bool Covered(uintptr_t p, uint64_t n) {
    return (p >= Base && p <= Base + MappedSize && n <= Base + MappedSize - p) ||
           (p >= HighBase && p <= HighBase + MappedSize && n <= HighBase + MappedSize - p);
}
int failures = 0;
void Check(bool ok, const char* message) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}
}
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
    if (!Covered(p, 1)) return 0;
    if (prot) *prot = 0x33; // CPU and GPU read/write
    if (type) *type = -1;
    if (end) *end = (p >= HighBase ? HighBase : Base) + MappedSize;
    return 1;
}
int runtime_memory_region(uintptr_t p, uintptr_t* a, uintptr_t* b, int* mapped) {
    if (!Covered(p, 1)) return 0;
    *a = p >= HighBase ? HighBase : Base; *b = *a + MappedSize; *mapped = 1; return 1;
}
void runtime_memory_set_gpu_hooks(void (*map)(uintptr_t, uint64_t), void (*)(uintptr_t, uint64_t), void (*)(uintptr_t, uint64_t)) {
    map(Base, MappedSize);
    map(HighBase, MappedSize);
}
// Production split mode protects the separate guest process, never this host mirror.
void runtime_memory_gpu_protect(uintptr_t p, uint64_t n, int, int) { assert(Covered(p,n)); }
int runtime_memory_write_backing(uintptr_t p, const void* data, uint64_t n) {
    if (!Covered(p,n)) return 0;
    std::memcpy(reinterpret_cast<void*>(p), data, n); return 1;
}
int runtime_memory_read_backing(uintptr_t p, void* data, uint64_t n) {
    if (!Covered(p,n)) return 0;
    std::memcpy(data, reinterpret_cast<void*>(p), n); return 1;
}
}
#pragma GCC visibility pop
bool RenderIndexTriangle(Metal::Device& device, Metal::BufferRef indices);
bool RenderIndirectTriangle(Metal::Device& device, Metal::BufferRef args);
unsigned RunIndirectDispatch(Metal::Device& device, Metal::BufferRef args);

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setenv("BB_NULL_GPU", "1", 1);
    setenv("BB_DRAW_PIPE", "0", 1);
    setenv("BB_PIPELINE_CACHE", "0", 1);
    setenv("BB_READBACKS", "2", 1);
    // The single arena (the default on Macs with more than 24 GB): its 4 GiB windows are tested.
    setenv("BB_ARENA_LAYOUT", "legacy", 1);
    void* mapped = mmap(reinterpret_cast<void*>(Base), MappedSize, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(mapped == reinterpret_cast<void*>(Base));
    void* high_mapped = mmap(reinterpret_cast<void*>(HighBase), MappedSize,
                            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(high_mapped == reinterpret_cast<void*>(HighBase));
    const BbGpuConfig config = {"Index offset test", "TEST", nullptr, 0x05050031, 0, 640, 360};
    Core::Emulator::FillElfInfo(config);
    Metal::Device device;
    Vulkan::Scheduler scheduler(device, false);
    Vulkan::Runtime runtime(device, scheduler);
    auto liverpool = std::make_unique<AmdGpu::Liverpool>();
    auto owner = std::make_unique<Vulkan::Rasterizer>(device, scheduler, runtime, liverpool.get());
    auto& rasterizer = *owner;
    auto& cache = rasterizer.GetBufferCache();
    // Real indexed draws, with zeros at the corresponding wrapped low address.
    // CPU-owned and GPU-produced high indices must both be rebased without losing data.
    const uintptr_t high_indices = HighBase + 12 * Size;
    auto* indices = reinterpret_cast<u16*>(high_indices);
    std::fill_n(indices, Size / sizeof(u16), 0);
    indices[0] = 0; indices[1] = 1; indices[2] = 2;
    const auto [raw, raw_offset] = cache.ObtainBuffer(high_indices, Size, false);
    scheduler.Finish();
    std::printf("Diagnostic: original high-offset indexed triangle %s\n",
                RenderIndexTriangle(device, raw->Ref(raw_offset)) ? "visible" : "MISSING");
    const auto [safe, safe_offset] = cache.ObtainIndexBuffer(high_indices, Size);
    scheduler.Finish();
    // The encoder draws through Metal::LowOffset (a window of the arena, no copy).
    const Metal::BufferRef safe_ref = Metal::LowOffset(safe->Ref(safe_offset));
    Check(safe_ref.offset + Size <= (1ull << 32), "CPU index binding stays below 4 GiB");
    const Metal::BufferRef mid = Metal::LowOffset(safe->Ref(u64{5} << 29));
    Check(mid.buffer == safe->Ref().buffer && mid.offset == (u64{5} << 29),
          "offsets below 3.5 GiB keep the arena (no window)");
    Check(safe_ref.offset < (u64{512} << 20), "windowed offsets are below 512 MiB");
    Check(RenderIndexTriangle(device, safe_ref), "high CPU-owned indices render correctly");
    const uintptr_t high_gpu = HighBase + 13 * Size;
    std::memset(reinterpret_cast<void*>(high_gpu), 0, Size);
    const auto [gpu_indices, gpu_index_offset] = cache.ObtainBuffer(high_gpu, Size, true);
    const VideoCore::BufferCopy index_copy{safe_offset, gpu_index_offset, Size};
    runtime.CopyBuffer(*safe, *gpu_indices, std::span{&index_copy, 1});
    const auto [safe_gpu, safe_gpu_offset] = cache.ObtainIndexBuffer(high_gpu, Size);
    scheduler.Finish();
    Check(reinterpret_cast<u16*>(high_gpu)[1] == 0, "GPU index test keeps the CPU mirror stale");
    const Metal::BufferRef gpu_ref = Metal::LowOffset(safe_gpu->Ref(safe_gpu_offset));
    Check(gpu_ref.offset + Size <= (1ull << 32), "GPU index binding stays below 4 GiB");
    Check(RenderIndexTriangle(device, gpu_ref), "high GPU-produced indices render correctly");
    const auto [low, low_offset] = cache.ObtainBuffer(Base + 14 * Size, Size, false);
    const auto [low_index, low_index_offset] = cache.ObtainIndexBuffer(Base + 14 * Size, Size);
    Check(low == low_index && low_offset == low_index_offset, "low index bindings retain the existing path");
    scheduler.Finish();

    // bbport: indirect arguments. drawPrimitives:indirectBuffer and
    // dispatchThreadgroupsWithIndirectBuffer take 32-bit offsets too; past 4 GiB of the arena the
    // arguments are copied (ObtainDrawArgumentBuffer), CPU-owned ones from guest memory and
    // GPU-produced ones in command order. Ranges above 16 KiB: smaller read-only ones take the
    // stream buffer. The arena's own high range is drawn once as a diagnostic only (whether this
    // Mac wraps it).
    constexpr u64 Limit = u64{1} << 32;
    const uintptr_t high_args = HighBase + 16 * Size;
    auto* draw_args = reinterpret_cast<u32*>(high_args);
    std::fill_n(draw_args, Size / sizeof(u32), 0u);
    draw_args[0] = 3; // vertexCount
    draw_args[1] = 1; // instanceCount
    const auto [raw_args, raw_args_offset] = cache.ObtainBuffer(high_args, Size, false);
    scheduler.Finish();
    std::printf("Diagnostic: indirect draw straight from the arena past 4 GiB %s\n",
                RenderIndirectTriangle(device, raw_args->Ref(raw_args_offset)) ? "drawn" : "MISSING");
    const u64 cpu_copies = BbStats::indirect_copies_cpu.load();
    const auto [cpu_args, cpu_args_offset] = cache.ObtainDrawArgumentBuffer(high_args, Size);
    scheduler.Finish();
    Check(cpu_args_offset + Size <= Limit, "CPU-owned indirect arguments are bound below 4 GiB");
    Check(BbStats::indirect_copies_cpu.load() == cpu_copies + 1, "and counted as a CPU copy");
    Check(RenderIndirectTriangle(device, cpu_args->Ref(cpu_args_offset)),
          "high CPU-owned indirect draw arguments draw");

    const uintptr_t high_gpu_args = HighBase + 17 * Size;
    std::memset(reinterpret_cast<void*>(high_gpu_args), 0, Size);
    const auto [gpu_args_dst, gpu_args_dst_offset] = cache.ObtainBuffer(high_gpu_args, Size, true);
    const VideoCore::BufferCopy args_copy{raw_args_offset, gpu_args_dst_offset, Size};
    runtime.CopyBuffer(*raw_args, *gpu_args_dst, std::span{&args_copy, 1});
    const u64 gpu_copies = BbStats::indirect_copies_gpu.load();
    const auto [gpu_args, gpu_args_offset] = cache.ObtainDrawArgumentBuffer(high_gpu_args, Size);
    scheduler.Finish();
    Check(reinterpret_cast<u32*>(high_gpu_args)[0] == 0,
          "GPU argument test keeps the CPU mirror stale");
    Check(gpu_args_offset + Size <= Limit, "GPU-produced indirect arguments are bound below 4 GiB");
    Check(BbStats::indirect_copies_gpu.load() == gpu_copies + 1, "and counted as a GPU copy");
    Check(RenderIndirectTriangle(device, gpu_args->Ref(gpu_args_offset)),
          "high GPU-produced indirect draw arguments draw");

    const uintptr_t high_groups = HighBase + 18 * Size;
    auto* groups = reinterpret_cast<u32*>(high_groups);
    std::fill_n(groups, Size / sizeof(u32), 0u);
    groups[0] = 5; // thread groups x, y, z
    groups[1] = 1;
    groups[2] = 1;
    const auto [raw_groups, raw_groups_offset] = cache.ObtainBuffer(high_groups, Size, false);
    scheduler.Finish();
    std::printf("Diagnostic: indirect dispatch straight from the arena past 4 GiB ran %u groups\n",
                RunIndirectDispatch(device, raw_groups->Ref(raw_groups_offset)));
    const auto [dispatch_args, dispatch_offset] = cache.ObtainDrawArgumentBuffer(high_groups, Size);
    scheduler.Finish();
    Check(dispatch_offset + Size <= Limit, "indirect dispatch arguments are bound below 4 GiB");
    Check(RunIndirectDispatch(device, dispatch_args->Ref(dispatch_offset)) == 5,
          "high indirect dispatch arguments run 5 thread groups");
    const auto [low_args, low_args_offset] = cache.ObtainBuffer(Base + 19 * Size, Size, false);
    const auto [low_draw, low_draw_offset] = cache.ObtainDrawArgumentBuffer(Base + 19 * Size, Size);
    Check(low_args == low_draw && low_args_offset == low_draw_offset,
          "low indirect arguments keep the arena range");

    // Requests that stopped the game in GetArena: a size-0 range at the arena's 4 GiB page
    // boundary (it ended before it started there) and past the arena; a read-back past the arena.
    const auto [boundary, boundary_offset] = cache.ObtainBuffer(HighBase, 0, true);
    Check(boundary == low_args && boundary_offset == (u64{1} << 32) &&
              BbStats::size0_sinks.load() == 0,
          "a size-0 write at the arena's 4 GiB boundary keeps the arena");
    const auto [past, past_offset] = cache.ObtainBuffer(Base + (u64{7} << 30), 0, true);
    Check(past != nullptr && past != low_args && BbStats::size0_sinks.load() == 1,
          "a size-0 write past the arena goes to the sink");
    const auto [inside, inside_offset] = cache.ObtainBuffer(Base + 20 * Size, 0, true);
    Check(inside == low_args && BbStats::size0_sinks.load() == 1,
          "a size-0 write inside the arena keeps the arena");
    cache.ReadMemory(Base + (u64{7} << 30), 8, false, true);
    Check(BbStats::uncovered_readbacks.load() == 1, "a read-back past the arena is declined");
    scheduler.Finish();
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
