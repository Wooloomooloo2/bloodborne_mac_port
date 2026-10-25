// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
#include "bbport_write_log.h"
// bbport: glue between the C loader and the vendored shadPS4 video core.
#include "bbport_overlay.h"
#include "bbport_settings.h"
#include <sys/resource.h>
#include "bbport_toggles.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <boost/asio/io_context.hpp>
#include "common/polyfill_thread.h"
#include "video_core/renderdoc.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <memory>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <dispatch/dispatch.h>
#include <CoreFoundation/CoreFoundation.h>
#endif
#include "../bbgpu.h"
#include "bbport_split.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/rdtsc.h"
#include "common/signal_context.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"
#include "core/memory.h"
#include "core/signals.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

extern "C" {
// runtime_memory.c
int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size);
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size);
int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end, int* mapped);
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
typedef void (*RuntimeGpuRange)(uintptr_t address, uint64_t size);
// runtime_kernel.c: one clock for guest and GPU timestamps
uint64_t runtime_process_time_us(void);
uint64_t runtime_process_time_counter(void);
uint64_t runtime_tsc_frequency(void);
int32_t* runtime_errno(void);
void runtime_memory_set_gpu_hooks(RuntimeGpuRange map, RuntimeGpuRange unmap, RuntimeGpuRange invalidate);
}

Frontend::WindowSDL* g_window = nullptr;
extern std::unique_ptr<Vulkan::Presenter> presenter; // gnmdriver.cpp

namespace Libraries::GnmDriver {
void RegisterLib(Core::Loader::SymbolsResolver* sym);
bool BbSplitHost(const BbGpuRecord* record);
} // namespace Libraries::GnmDriver
namespace Libraries::AvPlayer { void RegisterLib(Core::Loader::SymbolsResolver* sym); }
namespace Libraries::VideoOut {
void RegisterLib(Core::Loader::SymbolsResolver* sym);
bool BbSplitHost(const BbGpuRecord* record);
u64 BbSplitLabelAddress();
} // namespace Libraries::VideoOut
namespace Libraries::Kernel { void RegisterEventQueue(Core::Loader::SymbolsResolver* sym); }

namespace {
struct Symbol {
    std::string nid, library, module;
    u64 address;
};
std::vector<Symbol> g_symbols;
u32 g_sdk_version;
} // namespace

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid, const char* library, const char* module, SymbolType,
                                u64 address) {
    g_symbols.push_back({nid, library, module, address});
}
} // namespace Core::Loader

// ElfInfo's fields are private to Core::Emulator; the port fills them here.
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

void MemoryManager::SetRasterizer(Vulkan::Rasterizer* rasterizer_) {
    rasterizer = rasterizer_;
    // Existing guest mappings are replayed by the runtime when hooks are installed.
    runtime_memory_set_gpu_hooks(
        [](uintptr_t address, uint64_t size) {
            auto* r = Memory::Instance()->GetRasterizer();
            r->RegisterMemory(address, size);
            r->MapMemory(address, size);
        },
        [](uintptr_t address, uint64_t size) { Memory::Instance()->GetRasterizer()->UnmapMemory(address, size); },
        [](uintptr_t address, uint64_t size) {
            Memory::Instance()->GetRasterizer()->InvalidateMemory(address, size);
        });
}
void MemoryManager::InvalidateMemory(VAddr address, u64 size) {
    // bbport: in the split the copies to invalidate are the host's (AvPlayer frames, design P8).
    if (BbSplit::Remote()) {
        const BbGpuRange record{{BBGPU_REC_INVALIDATE, sizeof(BbGpuRange)}, address, size};
        BbSplit::Push(&record);
        return;
    }
    if (rasterizer) rasterizer->InvalidateMemory(address, size);
}
u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    return runtime_memory_clamp(virtual_addr, size);
}
static void CopySparseSerial(VAddr source, u8* dest, u64 size) {
    while (size) {
        uintptr_t start = 0, end = 0;
        int mapped = 0;
        if (!runtime_memory_region(source, &start, &end, &mapped)) {
            end = source + size;
        }
        const u64 n = std::min<u64>(size, end - source);
        if (mapped) std::memcpy(dest, reinterpret_cast<const void*>(source), n);
        else std::memset(dest, 0, n);
        source += n; dest += n; size -= n;
    }
}
void MemoryManager::CopySparseMemory(VAddr source, u8* dest, u64 size) {
    // bbport: large uploads (streaming) are split across the copy threads.
    constexpr u64 Chunk = 512 * 1024;
    if (size < 4 * Chunk || !BbCopy::Enabled()) {
        return CopySparseSerial(source, dest, size);
    }
    BbCopy::ParallelFor((size + Chunk - 1) / Chunk, [&](std::size_t i) {
        const u64 offset = i * Chunk;
        CopySparseSerial(source + offset, dest + offset, std::min(Chunk, size - offset));
    });
}
bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    BbWriteLog::Note(reinterpret_cast<uintptr_t>(address), data, size, BbWriteLog::Backing);
    return runtime_memory_write_backing(reinterpret_cast<uintptr_t>(address), data, size) != 0;
}
void AddressSpace::Protect(VAddr virtual_addr, u64 size, MemoryPermission perms) {
    BbStats::Timer timer{BbStats::t_protect};
    const u64 pages = (size + 4095) / 4096;
    BbStats::protect_calls.fetch_add(1, std::memory_order_relaxed);
    BbStats::protect_pages.fetch_add(pages, std::memory_order_relaxed);
    if (!True(perms & MemoryPermission::Write)) {
        BbStats::protect_revoke_calls.fetch_add(1, std::memory_order_relaxed);
        BbStats::protect_revoke_pages.fetch_add(pages, std::memory_order_relaxed);
    }
    runtime_memory_gpu_protect(virtual_addr, size, True(perms & MemoryPermission::Read),
                               True(perms & MemoryPermission::Write));
}
boost::icl::interval_set<VAddr> AddressSpace::GetUsableRegions() {
    boost::icl::interval_set<VAddr> set;
    set += boost::icl::interval<VAddr>::right_open(0x10'0000'0000ULL, 0xfc'0000'0000ULL);
    return set;
}
} // namespace Core

// Kernel services the vendored libraries call directly.
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() { return runtime_tsc_frequency(); }
u64 PS4_SYSV_ABI sceKernelReadTsc() { return Common::FencedRDTSC(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTime() { return runtime_process_time_us(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounter() { return runtime_process_time_counter(); }
u64 PS4_SYSV_ABI sceKernelGetProcessTimeCounterFrequency() { return 1'000'000'000; }
s32 PS4_SYSV_ABI sceKernelIsNeoMode() { return 0; } // base PS4
s32 PS4_SYSV_ABI sceKernelGetCompiledSdkVersion(s32* ver) {
    if (!ver) return ORBIS_KERNEL_ERROR_EINVAL;
    *ver = s32(g_sdk_version);
    return ORBIS_OK;
}
s32 PS4_SYSV_ABI sceKernelUsleep(u32 microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
    return ORBIS_OK;
}
} // namespace Libraries::Kernel

namespace {
// SDL video must be driven from one thread: the window lives on its own host thread.
std::thread g_window_thread;
std::mutex g_window_mutex;
std::condition_variable g_window_cv;
bool g_window_ready;
} // namespace

u32 BbDisplayRefreshHz() {
    static const u32 hz = [] {
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        const u32 rate = mode && mode->refresh_rate > 0 ? u32(mode->refresh_rate + 0.5f) : 60;
        std::printf("GPU: vblank follows the display refresh rate, %u Hz\n", rate);
        return std::max<u32>(rate, 60);
    }();
    return hz;
}

#ifdef BB_PGO_GENERATE
extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
// Instrumented build (BB_PGO=generate): the game often ends through _exit (watchdog, guest
// exit), which skips the profile write at exit. Write every 30 s and reset: the files sum the
// intervals.
static void StartProfileWriter() {
    std::thread([] {
        Common::SetCurrentThreadName("bb:pgo");
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            __gcov_dump();
            __gcov_reset();
            std::printf("PGO: profile written\n");
        }
    }).detach();
}
#endif

static void OpenWindow(const BbGpuConfig* config);

extern "C" int bbgpu_init(const BbGpuConfig* config) {
    BbSettings::Load();
#ifdef BB_PGO_GENERATE
    StartProfileWriter();
#endif
    g_sdk_version = config->sdk_version;
    if (config->user_dir) setenv("BB_GPU_USER_DIR", config->user_dir, 0);
    Core::Emulator::FillElfInfo(*config);
    OpenWindow(config);
    Core::Loader::SymbolsResolver resolver;
    // GnmDriver creates the presenter that the VideoOut present thread uses.
    Libraries::GnmDriver::RegisterLib(&resolver);
    Libraries::VideoOut::RegisterLib(&resolver);
    return 0;
}

extern "C" int bbgpu_host_start(const BbGpuConfig* config, void* shared) {
    BbSplit::InitHost(static_cast<BbGpuShared*>(shared));
    const int result = bbgpu_init(config);
    if (result == 0) {
        auto* s = static_cast<BbGpuShared*>(shared);
        s->label_address = Libraries::VideoOut::BbSplitLabelAddress();
        s->hdr_supported = presenter && presenter->IsHDRSupported() ? 1 : 0;
        BbSplit::ForwardInterrupts();
        // Keyboard for the game's pad emulation (input proper moves here in M5).
        std::thread([s] {
            Common::SetCurrentThreadName("bb:KeyBridge");
            for (;;) {
                int count = 0;
                const bool* keys = SDL_GetKeyboardState(&count);
                if (keys) {
                    std::memcpy(s->keys, keys, std::min<size_t>(size_t(count), sizeof s->keys));
                    __atomic_store_n(&s->keys_valid, 1u, __ATOMIC_RELEASE);
                }
                __atomic_store_n(&s->overlay_captures, BbOverlay::CapturesInput() ? 1u : 0u,
                                 __ATOMIC_RELAXED);
                // Text entry (IME dialog): publish the text and its state while it is open.
                if (__atomic_load_n(&s->text_active, __ATOMIC_ACQUIRE) && g_window) {
                    std::string text;
                    const int state = g_window->PollTextInput(text);
                    const size_t n = std::min(text.size(), sizeof s->text - 1);
                    std::memcpy(s->text, text.data(), n);
                    s->text[n] = 0;
                    __atomic_store_n(&s->text_state, static_cast<u32>(state), __ATOMIC_RELEASE);
                    __atomic_fetch_add(&s->text_seq, 1u, __ATOMIC_RELEASE);
                    if (state != 0) {
                        __atomic_store_n(&s->text_active, 0u, __ATOMIC_RELEASE);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        }).detach();
    }
    return result;
}

extern "C" void bbgpu_split_post_restart(void) {
    const BbGpuRecord record{BBGPU_EV_RESTART, sizeof(BbGpuRecord)};
    BbSplit::Post(&record);
}

extern "C" int bbgpu_host_fault(uint64_t address, int write) {
    auto* rasterizer = Core::Memory::Instance()->GetRasterizer();
    if (!rasterizer) {
        return 0;
    }
    if (write) {
        BbStats::Timer timer{BbStats::t_write_faults};
        return rasterizer->OnWriteFault(address, false) ? 1 : 0;
    }
    BbStats::read_faults.fetch_add(1, std::memory_order_relaxed);
    BbStats::Timer timer{BbStats::t_read_faults};
    return rasterizer->ReadMemory(address, 8, false) ? 1 : 0;
}

extern "C" int bbgpu_host_dispatch(const void* record) {
    const auto* r = static_cast<const BbGpuRecord*>(record);
    if (r->type == BBGPU_REC_TEXT_BEGIN) {
        auto* t = static_cast<const BbGpuTextBegin*>(record);
        BbGpuShared* s = BbSplit::Shared();
        int opened = 0;
        if (g_window) {
            const std::string initial(t->initial, strnlen(t->initial, sizeof t->initial));
            const std::string prompt(t->prompt, strnlen(t->prompt, sizeof t->prompt));
            g_window->BeginTextInput(initial, prompt.empty() ? "Text" : prompt, (t->flags & 1u) != 0);
            __atomic_store_n(&s->text_state, 0u, __ATOMIC_RELAXED);
            __atomic_store_n(&s->text_active, 1u, __ATOMIC_RELEASE);
            opened = 1;
        }
        bbgpu_reply_send(s, t->slot, opened);
        return 1;
    }
    if (r->type == BBGPU_REC_INVALIDATE) {
        const auto* range = static_cast<const BbGpuRange*>(record);
        Core::Memory::Instance()->InvalidateMemory(range->address, range->size);
        return 1;
    }
    return Libraries::GnmDriver::BbSplitHost(r) || Libraries::VideoOut::BbSplitHost(r) ? 1 : 0;
}

extern "C" void bbgpu_split_attach(void* shared, int host_pid) {
    auto* s = static_cast<BbGpuShared*>(shared);
    s->host_pid = host_pid;
    s->game_pid = getpid();
    BbSplit::Attach(s);
}

extern "C" const bool* bbgpu_split_keyboard(void) {
    if (!BbSplit::Remote() || !__atomic_load_n(&BbSplit::Shared()->keys_valid, __ATOMIC_ACQUIRE)) {
        return nullptr;
    }
    static_assert(sizeof(bool) == 1);
    return reinterpret_cast<const bool*>(BbSplit::Shared()->keys);
}

extern "C" uint32_t bbgpu_bench_frame_draws(void) {
    return BbSplit::Remote() ? __atomic_load_n(&BbSplit::Shared()->bench_frame_draws, __ATOMIC_RELAXED)
                             : 0;
}

extern "C" uint64_t bbgpu_bench_host_flips(void) {
    return BbSplit::Remote() ? __atomic_load_n(&BbSplit::Shared()->host_flips, __ATOMIC_RELAXED) : 0;
}

extern "C" void bbgpu_bench_set_phase(uint32_t phase, const char* error) {
    if (!BbSplit::Remote()) {
        return;
    }
    BbGpuShared* s = BbSplit::Shared();
    if (error) {
        std::snprintf(s->bench_error, sizeof(s->bench_error), "%s", error);
    }
    __atomic_store_n(&s->bench_phase, phase, __ATOMIC_RELEASE);
}

extern "C" void bbgpu_split_push(const void* record) {
    BbSplit::Push(record);
}

extern "C" int bbgpu_init_remote(const BbGpuConfig* config, void* shared_block) {
    auto* s = static_cast<BbGpuShared*>(shared_block);
    BbSettings::Load();
    g_sdk_version = config->sdk_version;
    Core::Emulator::FillElfInfo(*config);
    BbSplit::InitRemote(s);
    std::snprintf(s->title, sizeof s->title, "%s", config->title ? config->title : "Bloodborne");
    std::snprintf(s->serial, sizeof s->serial, "%s", config->serial ? config->serial : "UNKNOWN");
    std::snprintf(s->user_dir, sizeof s->user_dir, "%s", config->user_dir ? config->user_dir : "user");
    s->sdk_version = config->sdk_version;
    s->psf_attributes = config->psf_attributes;
    s->width = config->width;
    s->height = config->height;
    s->tsc_hz = runtime_tsc_frequency();
    s->process_start_ns = 0; // the host's process clock: same CLOCK_MONOTONIC origin as ours
    {
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        s->process_start_ns = u64(now.tv_sec) * 1'000'000'000ull + u64(now.tv_nsec) -
                              runtime_process_time_counter();
    }
    BbGpuStart start{{BBGPU_REC_START, sizeof(BbGpuStart)}, 0, 0};
    const s64 result = BbSplit::Request(&start, &start.slot);
    if (result != 0) {
        std::printf("GPU split: the GPU host could not start the renderer (%lld)\n",
                    static_cast<long long>(result));
        return -1;
    }
    Core::Loader::SymbolsResolver resolver;
    Libraries::GnmDriver::RegisterLib(&resolver);
    Libraries::VideoOut::RegisterLib(&resolver);
    BbSplit::StartEventDispatcher();
    std::printf("GPU split: rendering in the GPU host (labels at %#llx, HDR %s)\n",
                static_cast<unsigned long long>(s->label_address), s->hdr_supported ? "yes" : "no");
    return 0;
}

#ifdef __APPLE__
static s32 g_window_width, g_window_height;
#endif

// bbgpu-host (and bb-probe) end through _exit, which runs no destructors: the pipeline archive
// is written here first. Every caller waits for the same flush (the first starts it on a thread
// of its own), at most `timeout_ms`; a write cut short by the _exit that follows leaves the old
// file intact.
extern "C" int bbgpu_flush_caches(unsigned timeout_ms) {
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        bool done{};
        bool ok{true};
    };
    static State* const state = new State; // never destroyed: the flush thread may outlive exit
    static std::once_flag once;
    const auto start = std::chrono::steady_clock::now();
    std::call_once(once, [] {
        std::thread([] {
            Common::SetCurrentThreadName("bb:ExitFlush");
            const bool ok = presenter ? presenter->FlushPipelineArchive() : true;
            {
                std::scoped_lock lock{state->mutex};
                state->done = true;
                state->ok = ok;
            }
            state->cv.notify_all();
        }).detach();
    });
    std::unique_lock lock{state->mutex};
    const bool done = state->cv.wait_for(lock, std::chrono::milliseconds{timeout_ms},
                                         [] { return state->done; });
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!done) {
        std::printf("Exit: the pipeline archive write did not finish within %u ms; exiting "
                    "without it\n",
                    timeout_ms);
    } else {
        std::printf("Exit: pipeline archive flushed in %.0f ms%s\n", ms,
                    state->ok ? "" : " (the write failed)");
    }
    std::fflush(stdout);
    return done ? (state->ok ? 0 : -1) : 1;
}

static void OpenWindow(const BbGpuConfig* config) {
    const std::string title = config->title ? config->title : "Bloodborne";
    const s32 width = config->width, height = config->height;
#ifdef __APPLE__
    g_window_width = width;
    g_window_height = height;
#endif
#ifndef __APPLE__
    auto window_loop = [title, width, height] {
        Common::SetCurrentThreadName("bb:window");
        auto* window = new Frontend::WindowSDL(width, height, title.c_str());
        {
            std::scoped_lock lock{g_window_mutex};
            g_window = window;
            g_window_ready = true;
        }
        g_window_cv.notify_all();
        while (window->PollEvents()) {
            SDL_Delay(2);
        }
        LOG_INFO(Frontend, "Window closed by user");
        bbgpu_flush_caches(BBGPU_EXIT_FLUSH_MS);
        std::_Exit(0);
    };
#endif
#ifdef __APPLE__
    // Cocoa only works on the main thread: bb-probe runs the game on another thread and runs
    // the main run loop on the main thread, so the window goes to the main queue. Its events
    // are polled from a 2 ms run loop timer there, not a loop that never returns: AppKit finishes some
    // work (the full-screen transition) in blocks of its own on the main queue, which a
    // never-ending block starves (the window stuck off screen at its old size).
    dispatch_async_f(dispatch_get_main_queue(), new std::string(title), [](void* ctx) {
        const std::unique_ptr<std::string> title{static_cast<std::string*>(ctx)};
        auto* window = new Frontend::WindowSDL(g_window_width, g_window_height, title->c_str());
        {
            std::scoped_lock lock{g_window_mutex};
            g_window = window;
            g_window_ready = true;
        }
        g_window_cv.notify_all();
        // A run loop timer (unlike a dispatch timer) is a source of the main run loop: it keeps
        // CFRunLoopRun() running, and in the common modes it also fires during live resizes.
        CFRunLoopTimerContext context{0, window, nullptr, nullptr, nullptr};
        CFRunLoopTimerRef timer = CFRunLoopTimerCreate(
            kCFAllocatorDefault, CFAbsoluteTimeGetCurrent(), 0.002, 0, 0,
            [](CFRunLoopTimerRef, void* ctx) {
                if (!static_cast<Frontend::WindowSDL*>(ctx)->PollEvents()) {
                    LOG_INFO(Frontend, "Window closed by user");
                    bbgpu_flush_caches(BBGPU_EXIT_FLUSH_MS);
                    std::_Exit(0);
                }
            },
            &context);
        CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
    });
#else
    g_window_thread = std::thread(std::move(window_loop));
    g_window_thread.detach();
#endif
    {
        std::unique_lock lock{g_window_mutex};
        g_window_cv.wait(lock, [] { return g_window_ready; });
    }
}

namespace Libraries::Kernel { void StartKernelService(); }
extern "C" void bbgpu_register_kernel(void) {
    Libraries::Kernel::StartKernelService();
    Core::Loader::SymbolsResolver resolver;
    Libraries::Kernel::RegisterEventQueue(&resolver);
    Libraries::AvPlayer::RegisterLib(&resolver);
}

extern "C" uintptr_t bbgpu_resolve(const char* scoped_nid) {
    const char* hash = std::strchr(scoped_nid, '#');
    const size_t length = hash ? size_t(hash - scoped_nid) : std::strlen(scoped_nid);
    for (const auto& symbol : g_symbols) {
        if (symbol.nid.size() == length && !std::memcmp(symbol.nid.data(), scoped_nid, length)) {
            return uintptr_t(symbol.address);
        }
    }
    return 0;
}

extern "C" int bbgpu_handle_fault(void* ucontext, void* address) {
    if (BbSplit::Remote()) {
        // M4: GPU page tracking lives in the host; it handles the fault and unprotects.
        return BbSplit::RelayFault(reinterpret_cast<u64>(address), Common::IsWriteError(ucontext))
                   ? 1
                   : 0;
    }
    return Core::Signals::Instance()->DispatchAccessViolation(ucontext, address) ? 1 : 0;
}

extern "C" unsigned bbgpu_symbol_count(void) {
    return unsigned(g_symbols.size());
}

extern "C" uint64_t bbgpu_get_flip_count(void) {
    return BbStats::flips.load(std::memory_order_relaxed);
}

extern "C" uint64_t bbgpu_get_submit_count(void) {
    if (BbSplit::Remote()) {
        return __atomic_load_n(&BbSplit::Shared()->host_submits, __ATOMIC_RELAXED);
    }
    return BbStats::submissions.load(std::memory_order_relaxed);
}

namespace Libraries::VideoOut {
void GetFrametimeStats(double* avg_fps, double* p95_ms, double* p99_ms);
}

extern "C" void bbgpu_get_frametime_percentiles(double* avg_fps, double* p95_ms, double* p99_ms) {
    Libraries::VideoOut::GetFrametimeStats(avg_fps, p95_ms, p99_ms);
}

extern "C" void bbgpu_dump_host_threads_hang(void* file_handle) {
    FILE* f = static_cast<FILE*>(file_handle);
    if (!f) return;
    std::fprintf(f, "=== HOST GPU / PRESENTER THREADS ===\n");
    std::fprintf(f, "GPU Stats:\n");
    std::fprintf(f, "  Submissions: %llu, Flips: %llu, Draws: %llu, Dispatches: %llu\n",
                 (unsigned long long)BbStats::submissions.load(std::memory_order_relaxed),
                 (unsigned long long)BbStats::flips.load(std::memory_order_relaxed),
                 (unsigned long long)BbStats::draws.load(std::memory_order_relaxed),
                 (unsigned long long)BbStats::dispatches.load(std::memory_order_relaxed));
    std::fprintf(f, "  GPU Idle Time: %.2f ms\n",
                 (double)BbStats::gpu_idle_ns.load(std::memory_order_relaxed) / 1000000.0);
    std::fprintf(f, "  Scheduler Sync Wait: %.2f ms\n",
                 (double)BbStats::sync_recording_ns.load(std::memory_order_relaxed) / 1000000.0);
    std::fprintf(f, "  Host Copies Wait: %.2f ms\n",
                 (double)BbStats::host_copies_wait_ns.load(std::memory_order_relaxed) / 1000000.0);
    std::fprintf(f, "  GPU Tick Wait: %.2f ms\n",
                 (double)BbStats::tick_wait_ns.load(std::memory_order_relaxed) / 1000000.0);
    std::fprintf(f, "  Host Wait (fences/queue): %.2f ms\n",
                 (double)BbStats::t_host_wait.load(std::memory_order_relaxed) / 1000000.0);
    std::fprintf(f, "  Texture Creation Time: %.2f ms, Staging Time: %.2f ms\n",
                 (double)BbStats::t_image_create.load(std::memory_order_relaxed) / 1000000.0,
                 (double)BbStats::t_staging.load(std::memory_order_relaxed) / 1000000.0);
    std::fprintf(f, "  Page Protection Revoke Calls: %llu (%llu pages)\n",
                 (unsigned long long)BbStats::protect_revoke_calls.load(std::memory_order_relaxed),
                 (unsigned long long)BbStats::protect_revoke_pages.load(std::memory_order_relaxed));
    std::fprintf(f, "====================================\n\n");
    std::fflush(f);
}

// Stuck-GPU diagnostics: the Metal device's tick counters (the old breadcrumb buffer markers
// need VK_AMD_buffer_marker; a Metal stall shows as a gap between the tick being waited on and
// the last tick known done).
extern "C" void bbgpu_dump_breadcrumbs(void* file_handle) {
    if (!file_handle) return;
    FILE* f = static_cast<FILE*>(file_handle);
    std::fprintf(f, "\n=== GPU STATE ===\n");
    if (const auto* p = presenter.get()) {
        const Metal::Device& device = p->GetDevice();
        std::fprintf(f,
                     "Metal device %s: next tick %llu, last tick done %llu, command buffer "
                     "errors %llu\n",
                     device.Name().c_str(), (unsigned long long)device.CurrentTick(),
                     (unsigned long long)device.KnownGpuTick(),
                     (unsigned long long)device.ErrorCount());
    } else {
        std::fprintf(f, "no presenter (the renderer is not running in this process)\n");
    }
    std::fprintf(f, "=================\n\n");
    std::fflush(f);
}

// Kernel service thread: runs boost::asio timers for equeue timer events.
namespace Libraries::Kernel {
boost::asio::io_context io_context;
static std::mutex m_asio_req;
static std::condition_variable_any cv_asio_req;
static std::atomic<u32> asio_requests;
static std::jthread service_thread;

void KernelSignalRequest() {
    std::unique_lock lock{m_asio_req};
    ++asio_requests;
    cv_asio_req.notify_one();
}

static void KernelServiceThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("bb:kernel_service");
    while (!stoken.stop_requested()) {
        {
            std::unique_lock lock{m_asio_req};
            cv_asio_req.wait(lock, stoken, [] { return asio_requests != 0; });
        }
        if (stoken.stop_requested()) break;
        io_context.run();
        io_context.restart();
        asio_requests = 0;
    }
}

void StartKernelService() {
    service_thread = std::jthread{KernelServiceThread};
}

int* PS4_SYSV_ABI __Error() {
    return runtime_errno();
}
} // namespace Libraries::Kernel

// RenderDoc capture hooks: not used by the port.
namespace VideoCore {
void LoadRenderDoc() {}
void StartCapture() {}
void EndCapture() {}
void TriggerCapture() {}
void SetOutputDir(const std::filesystem::path&, const std::string&) {}
bool IsRenderDocLoaded() { return false; }
void RequestScreenshot(ScreenshotRequest) {}
u32 ConsumeGameOnlyScreenshotRequests() { return 0; }
u32 ConsumeWithOverlaysScreenshotRequests() { return 0; }
ScreenshotRequests ConsumeScreenshotRequests() { return {}; }
} // namespace VideoCore

extern "C" int bbgpu_overlay_captures_input(void) {
    if (BbSplit::Remote()) {
        return __atomic_load_n(&BbSplit::Shared()->overlay_captures, __ATOMIC_RELAXED) ? 1 : 0;
    }
    return BbOverlay::CapturesInput() ? 1 : 0;
}

extern "C" int bbgpu_mouse_look_enabled(void) {
    return BbSettings::Get().mouse_look ? 1 : 0;
}

extern "C" int bbgpu_text_input_begin(const char* initial, const char* prompt, int password) {
    if (BbSplit::Remote()) {
        BbGpuTextBegin record{};
        record.r = {BBGPU_REC_TEXT_BEGIN, sizeof(BbGpuTextBegin)};
        record.flags = password ? 1u : 0u;
        std::snprintf(record.initial, sizeof record.initial, "%s", initial ? initial : "");
        std::snprintf(record.prompt, sizeof record.prompt, "%s", prompt ? prompt : "Text");
        return BbSplit::Request(&record, &record.slot) != 0 ? 1 : 0;
    }
    if (!g_window) return 0;
    g_window->BeginTextInput(initial ? initial : "", prompt ? prompt : "Text", password != 0);
    return 1;
}

extern "C" int bbgpu_text_input_poll(char* out, uint64_t size) {
    if (BbSplit::Remote()) {
        BbGpuShared* s = BbSplit::Shared();
        const u32 state = __atomic_load_n(&s->text_state, __ATOMIC_ACQUIRE);
        if (size) {
            const size_t n = std::min<size_t>(strnlen(s->text, sizeof s->text), size - 1);
            std::memcpy(out, s->text, n);
            out[n] = 0;
        }
        return static_cast<int>(state);
    }
    if (!g_window) return 2;
    std::string text;
    const int state = g_window->PollTextInput(text);
    if (size) {
        const size_t n = std::min<size_t>(text.size(), size - 1);
        std::memcpy(out, text.data(), n);
        out[n] = 0;
    }
    return state;
}
