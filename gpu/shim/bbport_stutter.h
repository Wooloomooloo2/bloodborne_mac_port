// bbport: stutter diagnostics (BB_FRAME_STATS), phase 0 of the stutter plan
// (performance/experiments/stutter/PLAN.md). Measures, changes nothing:
// - GPU read-backs on guest write faults: who asked, how long each phase took, and when the GPU
//   command that wrote the faulting page finished, which gives the wait a read-back straight from
//   the shared arena would have had (the "counterfactual");
// - page-protection relays to the game process by calling thread;
// - the host fault thread's time per fault;
// - the device's tick timeline (commit and completion times).
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <pthread.h>
#include <pthread/qos.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include <sys/mman.h>

#include "bbport_toggles.h"

namespace BbStutter {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

inline u64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Wall-clock seconds, printed as t= on stat lines to align them with vm_stat and other logs.
inline double WallSeconds() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

template <typename T>
void AtomicMax(std::atomic<T>& target, T value) {
    T seen = target.load(std::memory_order_relaxed);
    while (seen < value &&
           !target.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
    }
}

// ---- Threads ----

enum Class : u32 { Other, StageA, StageB, Fault, Recorder, Copy, NumClasses };
inline const char* ClassName(u32 c) {
    static constexpr const char* names[NumClasses] = {"other", "GPU command thread",
                                                      "recording thread", "fault thread",
                                                      "Metal recorder", "copy threads"};
    return c < NumClasses ? names[c] : "?";
}

inline thread_local int class_override = -1;

/// The calling thread's class: from its name, or Fault inside bbgpu_host_fault.
inline u32 CurrentClass() {
    if (class_override >= 0) {
        return u32(class_override);
    }
    static thread_local const u32 by_name = [] {
        char name[64]{};
        pthread_getname_np(pthread_self(), name, sizeof(name));
        const std::string_view n{name};
        if (n == "bb:DrawRec") {
            return u32(StageB);
        }
        if (n.find("GpuCommandProcessor") != std::string_view::npos) {
            return u32(StageA);
        }
        if (n == "bb:MtlRecorder") {
            return u32(Recorder);
        }
        if (n.starts_with("bb:Copy")) {
            return u32(Copy);
        }
        return u32(Other);
    }();
    return by_name;
}

struct ScopedClass {
    int previous;
    explicit ScopedClass(u32 c) : previous(class_override) {
        class_override = int(c);
    }
    ~ScopedClass() {
        class_override = previous;
    }
};

/// The recording thread's Mach port, for its CPU time on Spike lines.
inline std::atomic<u32> stage_b_port{0};

/// The recording thread's CPU time (user + system) in microseconds, 0 if unknown.
inline u64 StageBCpuUs() {
#ifdef __APPLE__
    const mach_port_t port = stage_b_port.load(std::memory_order_acquire);
    if (!port) {
        return 0;
    }
    thread_basic_info_data_t info{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count) !=
        KERN_SUCCESS) {
        return 0;
    }
    return u64(info.user_time.seconds + info.system_time.seconds) * 1000000 +
           info.user_time.microseconds + info.system_time.microseconds;
#else
    return 0;
#endif
}

// Per-thread accumulators for the fault thread's split (read by bbgpu_host_fault).
inline thread_local u64 tls_protect_ns = 0, tls_readback_ns = 0, tls_texture_lock_ns = 0;

// ---- Protect relays ----

struct ProtectStats {
    std::array<std::atomic<u64>, NumClasses> calls{}, revokes{}, ns{}, max_ns{};
};
inline ProtectStats protect;

inline void NoteProtect(u64 ns, bool revoke) {
    const u32 c = CurrentClass();
    protect.calls[c].fetch_add(1, std::memory_order_relaxed);
    protect.revokes[c].fetch_add(revoke ? 1 : 0, std::memory_order_relaxed);
    protect.ns[c].fetch_add(ns, std::memory_order_relaxed);
    AtomicMax(protect.max_ns[c], ns);
    if (c == Fault) {
        tls_protect_ns += ns;
    }
}

// ---- Fault thread ----

struct FaultStats {
    std::atomic<u64> faults{0}, ns{0}, max_ns{0}, readback_ns{0}, protect_ns{0},
        texture_lock_ns{0};
};
inline FaultStats faults;

// ---- Tick timeline ----

struct TickTime {
    std::atomic<u64> tick{0}, commit_ns{0}, done_ns{0};
    std::atomic<u32> gpu_us{0};
};
inline constexpr u32 TickRing = 16384;
inline std::array<TickTime, TickRing> ticks;

inline void NoteCommit(u64 tick) {
    auto& t = ticks[tick % TickRing];
    t.done_ns.store(0, std::memory_order_relaxed);
    t.gpu_us.store(0, std::memory_order_relaxed);
    t.commit_ns.store(NowNs(), std::memory_order_relaxed);
    t.tick.store(tick, std::memory_order_release);
}

inline void NoteDone(u64 tick, double gpu_seconds) {
    auto& t = ticks[tick % TickRing];
    if (t.tick.load(std::memory_order_acquire) == tick) {
        t.gpu_us.store(u32(std::max(0.0, gpu_seconds) * 1e6), std::memory_order_relaxed);
        t.done_ns.store(NowNs(), std::memory_order_release);
    }
}

struct TickInfo {
    bool known = false; ///< still in the ring
    u64 commit_ns = 0, done_ns = 0; ///< done_ns 0: not completed yet
    u32 gpu_us = 0;
};
inline TickInfo GetTick(u64 tick) {
    const auto& t = ticks[tick % TickRing];
    TickInfo info;
    if (tick == 0 || t.tick.load(std::memory_order_acquire) != tick) {
        return info;
    }
    info.known = true;
    info.commit_ns = t.commit_ns.load(std::memory_order_relaxed);
    info.done_ns = t.done_ns.load(std::memory_order_acquire);
    info.gpu_us = t.gpu_us.load(std::memory_order_relaxed);
    return info;
}

// ---- Writer tick per 4 KiB page of the arena ----

class PageTicks {
public:
    void Init(u64 base, u64 size) {
        const u64 count = size >> 12;
        void* table = mmap(nullptr, count * sizeof(u32), PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON, -1, 0);
        if (table == MAP_FAILED) {
            return;
        }
        entries = static_cast<std::atomic<u32>*>(table);
        first = base;
        pages = count;
    }
    /// Stage A/B, when a binding is marked GPU-modified: the tick of the command buffer being
    /// recorded (the low 32 bits; a session never reaches 2^32 ticks).
    void Stamp(u64 address, u64 size, u64 tick) {
        if (!entries || size == 0 || address < first) {
            return;
        }
        const u64 begin = (address - first) >> 12;
        const u64 end = std::min(pages, ((address + size - 1 - first) >> 12) + 1);
        for (u64 page = begin; page < end; ++page) {
            entries[page].store(u32(tick), std::memory_order_relaxed);
        }
    }
    /// The largest stamp over the range (0: never stamped).
    u64 Max(u64 address, u64 size) const {
        if (!entries || size == 0 || address < first) {
            return 0;
        }
        const u64 begin = (address - first) >> 12;
        const u64 end = std::min(pages, ((address + size - 1 - first) >> 12) + 1);
        u32 result = 0;
        for (u64 page = begin; page < end; ++page) {
            result = std::max(result, entries[page].load(std::memory_order_relaxed));
        }
        return result;
    }

private:
    std::atomic<u32>* entries = nullptr;
    u64 first = 0, pages = 0;
};
inline PageTicks page_ticks;

// ---- Direct read-backs (stutter plan phase 1) ----
//
// A guest write fault on a page holding GPU-written data copies that data back first. Before, the
// GPU command thread did it: it drained the draw pipe, copied on the GPU and waited for the whole
// queue (scheduler.Finish()), so the frame stopped. Now the fault thread copies straight from the
// lazy arena's CPU view when the command buffer that last wrote the page has completed (93% of
// read-backs while moving, play-lazy-2123): BufferCache::TryDirectReadBack.

/// The second number of BB_TOGGLE_FILE (all 64 bits of the first are taken): bit 0 = direct
/// read-backs off, bit 1 = verify mode instead (an in-session A/B that also checks the stamps).
/// Read by a watcher thread in the GPU process (bbgpu_host_start).
inline std::atomic<u64> toggles2{0};

/// Phase 3 switches (second toggle word, for the A/B; both on by default): bit 2 = per-KiB hoist
/// tags only (Runtime::NoteSubmissionUse), bit 3 = full page walks for reads
/// (Metal::BufferBarriers::Conflicts).
inline bool CoarseHoistTagsOff() {
    return (toggles2.load(std::memory_order_relaxed) & 4) != 0;
}
inline bool BarrierWriteIndexOff() {
    return (toggles2.load(std::memory_order_relaxed) & 8) != 0;
}

/// Phase 2 switches (second toggle word): bit 4 = no user-interactive QoS for the threads on the
/// fault and protect path, bit 5 = a fault's two protect relays kept separate (both on by
/// default); bit 6 = spin before the protect relay's wait sleeps (opt-in, host_runtime.c).
inline bool QosOff() {
    return (toggles2.load(std::memory_order_relaxed) & 16) != 0;
}
inline bool MergedFaultRelayOff() {
    return (toggles2.load(std::memory_order_relaxed) & 32) != 0;
}

/// Gives the calling thread user-interactive QoS (P cores, prompt wake-ups) or the default;
/// cheap when nothing changes. For the threads on the fault/protect relay path.
inline void ApplyQos() {
    static thread_local int applied = -1;
    const int want = QosOff() ? 0 : 1;
    if (want != applied) {
        pthread_set_qos_class_self_np(want ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_DEFAULT, 0);
        applied = want;
    }
}

/// Phase 4 switch (second toggle word): bit 7 = small guest copies stay on bb:MtlRecorder during
/// bursts too (BufferCache::SmallGuestCopy).
inline bool CopyBurstsOff() {
    return (toggles2.load(std::memory_order_relaxed) & 128) != 0;
}

enum DirectMode : int { DirectOff, DirectOn, DirectVerify };
/// BB_DIRECT_READBACK=0 (off), 1 (default) or verify (decide and snapshot, but take the old path,
/// then compare the bytes it wrote with the snapshot; for testing the writer stamps).
inline int DirectReadbackMode() {
    static const int mode = [] {
        const char* env = std::getenv("BB_DIRECT_READBACK");
        if (env && std::string_view{env} == "verify") {
            return int(DirectVerify);
        }
        return env && env[0] == '0' ? int(DirectOff) : int(DirectOn);
    }();
    if (mode == DirectOn) {
        const u64 bits = toggles2.load(std::memory_order_relaxed);
        if (bits & 1) {
            return DirectOff;
        }
        if (bits & 2) {
            return DirectVerify;
        }
    }
    return mode;
}

/// Ticks committed while a GPU write was bound but its command not yet recorded (a flush in the
/// middle of a draw's bindings): pages stamped with such a tick may be written by a later command
/// buffer. Rare; a small ring.
struct SplitTicks {
    std::mutex mutex;
    std::array<u64, 256> ring{};
    u32 next = 0;
    std::atomic<u64> count{0};
};
inline SplitTicks split_ticks;

inline void MarkSplit(u64 tick) {
    std::scoped_lock lock{split_ticks.mutex};
    split_ticks.ring[split_ticks.next++ % split_ticks.ring.size()] = tick;
    split_ticks.count.fetch_add(1, std::memory_order_relaxed);
}

/// The command buffer that holds the writes stamped `stamp` (later if `stamp` was split).
inline u64 EffectiveTick(u64 stamp) {
    if (split_ticks.count.load(std::memory_order_relaxed) == 0) {
        return stamp;
    }
    std::scoped_lock lock{split_ticks.mutex};
    for (bool again = true; again;) {
        again = false;
        for (const u64 tick : split_ticks.ring) {
            if (tick != 0 && tick == stamp) {
                ++stamp;
                again = true;
                break;
            }
        }
    }
    return stamp;
}

enum Fallback : u32 {
    FbOff, FbShape, FbNoView, FbNoRegion, FbUnstamped, FbNotComplete, FbVerify, NumFallbacks
};
inline const char* FallbackName(u32 f) {
    static constexpr const char* names[NumFallbacks] = {
        "off", "not one region", "no CPU view", "no region", "unstamped", "writer not complete",
        "verify"};
    return f < NumFallbacks ? names[f] : "?";
}
struct DirectStats {
    std::atomic<u64> count{0}, ns{0}, pages{0}, bytes{0}, other_pages_left{0};
    std::array<std::atomic<u64>, NumFallbacks> fallbacks{};
    std::atomic<u64> verified{0}, verify_mismatches{0}, verify_skipped{0};
};
inline DirectStats direct;

// ---- Read-backs ----

enum Caller : u32 { FromFault, FromOther, Inline, NumCallers };

struct Readback {
    u64 start_ns = 0;   ///< request on the calling thread
    u64 total_ns = 0;   ///< until the caller continued
    u64 pickup_ns = 0;  ///< until the GPU command thread ran it (includes its draw-pipe drain)
    u64 finish_ns = 0;  ///< scheduler.Finish() inside the download
    u64 bytes = 0;
    u64 page_tick = 0;   ///< writer of the faulting page
    u64 window_tick = 0; ///< latest writer of the downloaded ranges
    u32 caller = FromOther;
    u32 windows_waited = 0; ///< summaries that found the writer not completed yet
    bool direct = false;    ///< copied by the fault thread from the arena's CPU view
};

/// The read-back the current thread is running (DownloadMemory fills bytes, finish, window).
inline thread_local Readback* tls_readback = nullptr;

struct ReadbackLog {
    std::mutex mutex;
    std::vector<Readback> pending;
};
inline ReadbackLog readbacks;

/// Bytes of small guest copies sent to the copy threads because of a burst (phase 4).
inline std::atomic<u64> burst_copy_bytes{0};

// Per-frame totals for the Stall block.
inline std::atomic<u64> readback_count{0}, readback_ns{0}, readback_finish_ns{0};

inline void NoteReadback(const Readback& r) {
    readback_count.fetch_add(1, std::memory_order_relaxed);
    readback_ns.fetch_add(r.total_ns, std::memory_order_relaxed);
    readback_finish_ns.fetch_add(r.finish_ns, std::memory_order_relaxed);
    if (r.caller == FromFault) {
        tls_readback_ns += r.total_ns;
    }
    std::scoped_lock lock{readbacks.mutex};
    if (readbacks.pending.size() < 65536) {
        readbacks.pending.push_back(r);
    }
}

/// Wait a direct read-back from the shared arena would have had, given the writer tick: none if
/// the writer had completed by the request, else until it completed. False while unknown.
inline bool PredictedWait(u64 tick, u64 start_ns, u64& wait_ns, int& kind) {
    const TickInfo info = GetTick(tick);
    if (tick == 0 || !info.known) {
        kind = 3; // never stamped or out of the ring
        wait_ns = 0;
        return true;
    }
    if (info.done_ns == 0) {
        return false;
    }
    wait_ns = info.done_ns > start_ns ? info.done_ns - start_ns : 0;
    kind = info.done_ns <= start_ns ? 0 : (info.commit_ns <= start_ns ? 1 : 2);
    return true;
}

/// Every 5 s (driver.cpp): read-backs, protect relays and the fault thread, then reset.
inline void PrintWindow(double window_s, u64 frames) {
    // The recording thread's CPU time per frame (phase 3 A/B).
    static u64 last_stage_b_us = 0;
    const u64 stage_b_us = StageBCpuUs();
    if (last_stage_b_us && frames) {
        std::printf("Recording thread: %.2f ms CPU per frame (%llu frames) | t=%.3f\n",
                    (stage_b_us - last_stage_b_us) / 1e3 / double(frames),
                    static_cast<unsigned long long>(frames), WallSeconds());
    }
    last_stage_b_us = stage_b_us;
    std::vector<Readback> done;
    {
        std::scoped_lock lock{readbacks.mutex};
        std::vector<Readback> keep;
        for (auto& r : readbacks.pending) {
            u64 wait = 0;
            int kind = 0;
            const bool known_page = PredictedWait(r.page_tick, r.start_ns, wait, kind);
            const bool known_window = PredictedWait(r.window_tick, r.start_ns, wait, kind);
            if ((known_page && known_window) || ++r.windows_waited > 2) {
                done.push_back(r);
            } else {
                keep.push_back(r);
            }
        }
        readbacks.pending.swap(keep);
    }
    const double t = WallSeconds();
    // Direct read-backs (phase 1) and why the others took the old path.
    if (const u64 n_direct = direct.count.exchange(0, std::memory_order_relaxed);
        n_direct || direct.fallbacks[FbNotComplete].load(std::memory_order_relaxed) ||
        direct.verified.load(std::memory_order_relaxed)) {
        std::string reasons;
        for (u32 f = 0; f < NumFallbacks; ++f) {
            if (const u64 c = direct.fallbacks[f].exchange(0, std::memory_order_relaxed)) {
                char part[64];
                std::snprintf(part, sizeof(part), "%s%s %llu", reasons.empty() ? "" : ", ",
                              FallbackName(f), static_cast<unsigned long long>(c));
                reasons += part;
            }
        }
        std::printf("Direct read-backs (%.1f s): %llu, %.2f ms, %llu pages, %.1f MB, %llu window pages "
                    "left GPU-modified; old path: %s; verify: %llu checked, %llu mismatches, %llu "
                    "skipped | t=%.3f\n",
                    window_s, static_cast<unsigned long long>(n_direct),
                    direct.ns.exchange(0, std::memory_order_relaxed) / 1e6,
                    static_cast<unsigned long long>(direct.pages.exchange(0, std::memory_order_relaxed)),
                    direct.bytes.exchange(0, std::memory_order_relaxed) / 1e6,
                    static_cast<unsigned long long>(
                        direct.other_pages_left.exchange(0, std::memory_order_relaxed)),
                    reasons.empty() ? "none" : reasons.c_str(),
                    static_cast<unsigned long long>(direct.verified.exchange(0, std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        direct.verify_mismatches.exchange(0, std::memory_order_relaxed)),
                    static_cast<unsigned long long>(
                        direct.verify_skipped.exchange(0, std::memory_order_relaxed)),
                    t);
    }
    if (!done.empty()) {
        u64 by_caller[NumCallers]{};
        u64 total = 0, max_total = 0, pickup = 0, finish = 0, bytes = 0;
        // Kinds: 0 writer completed before the request, 1 committed but running, 2 not yet
        // committed, 3 unknown (never stamped or out of the ring).
        u64 page_kind[4]{}, window_kind[4]{};
        u64 page_pred = 0, page_pred_max = 0, window_pred = 0, window_pred_max = 0;
        u64 fault_total = 0, fault_page_pred = 0, fault_window_pred = 0;
        for (const auto& r : done) {
            ++by_caller[r.caller];
            total += r.total_ns;
            max_total = std::max(max_total, r.total_ns);
            pickup += r.pickup_ns;
            finish += r.finish_ns;
            bytes += r.bytes;
            u64 wait = 0;
            int kind = 3;
            if (PredictedWait(r.page_tick, r.start_ns, wait, kind)) {
                ++page_kind[kind];
                page_pred += wait;
                page_pred_max = std::max(page_pred_max, wait);
                if (r.caller == FromFault) {
                    fault_page_pred += wait;
                }
            } else {
                ++page_kind[3];
            }
            wait = 0;
            kind = 3;
            if (PredictedWait(r.window_tick, r.start_ns, wait, kind)) {
                ++window_kind[kind];
                window_pred += wait;
                window_pred_max = std::max(window_pred_max, wait);
                if (r.caller == FromFault) {
                    fault_window_pred += wait;
                }
            } else {
                ++window_kind[3];
            }
            if (r.caller == FromFault) {
                fault_total += r.total_ns;
            }
        }
        std::printf("Read-backs (%.1f s): %zu (fault thread %llu, other threads %llu, inline %llu), "
                    "%.1f MB; waited %.1f ms (max %.1f): pickup %.1f, Finish %.1f | writer of the "
                    "faulting page: completed %llu, running %llu, uncommitted %llu, unknown %llu; "
                    "of the whole window: %llu/%llu/%llu/%llu | a direct read would have waited "
                    "%.1f ms (max %.1f) deciding on the page, %.1f ms (max %.1f) on the window; "
                    "fault thread only: %.1f ms now, %.1f / %.1f ms direct | t=%.3f\n",
                    window_s, done.size(), static_cast<unsigned long long>(by_caller[FromFault]),
                    static_cast<unsigned long long>(by_caller[FromOther]),
                    static_cast<unsigned long long>(by_caller[Inline]), bytes / 1e6, total / 1e6,
                    max_total / 1e6, pickup / 1e6, finish / 1e6,
                    static_cast<unsigned long long>(page_kind[0]),
                    static_cast<unsigned long long>(page_kind[1]),
                    static_cast<unsigned long long>(page_kind[2]),
                    static_cast<unsigned long long>(page_kind[3]),
                    static_cast<unsigned long long>(window_kind[0]),
                    static_cast<unsigned long long>(window_kind[1]),
                    static_cast<unsigned long long>(window_kind[2]),
                    static_cast<unsigned long long>(window_kind[3]), page_pred / 1e6,
                    page_pred_max / 1e6, window_pred / 1e6, window_pred_max / 1e6,
                    fault_total / 1e6, fault_page_pred / 1e6, fault_window_pred / 1e6, t);
    }
    // Protect relays by thread.
    std::string line;
    for (u32 c = 0; c < NumClasses; ++c) {
        const u64 calls = protect.calls[c].exchange(0, std::memory_order_relaxed);
        const u64 revokes = protect.revokes[c].exchange(0, std::memory_order_relaxed);
        const u64 ns = protect.ns[c].exchange(0, std::memory_order_relaxed);
        const u64 max_ns = protect.max_ns[c].exchange(0, std::memory_order_relaxed);
        if (calls == 0) {
            continue;
        }
        char part[160];
        std::snprintf(part, sizeof(part), "%s%s %llu (%llu revoking) %.1f ms, max %.2f ms",
                      line.empty() ? "" : "; ", ClassName(c),
                      static_cast<unsigned long long>(calls),
                      static_cast<unsigned long long>(revokes), ns / 1e6, max_ns / 1e6);
        line += part;
    }
    if (!line.empty()) {
        std::printf("Protect relays (%.1f s): %s | t=%.3f\n", window_s, line.c_str(), t);
    }
    const u64 n = faults.faults.exchange(0, std::memory_order_relaxed);
    if (n) {
        const u64 ns = faults.ns.exchange(0, std::memory_order_relaxed);
        const u64 rb = faults.readback_ns.exchange(0, std::memory_order_relaxed);
        const u64 pr = faults.protect_ns.exchange(0, std::memory_order_relaxed);
        const u64 tl = faults.texture_lock_ns.exchange(0, std::memory_order_relaxed);
        std::printf("Fault thread (%.1f s): %llu write faults, %.1f ms (max %.2f): read-backs %.1f, "
                    "protect relays %.1f, texture-cache lock %.1f, rest %.1f | t=%.3f\n",
                    window_s, static_cast<unsigned long long>(n), ns / 1e6,
                    faults.max_ns.exchange(0, std::memory_order_relaxed) / 1e6, rb / 1e6, pr / 1e6,
                    tl / 1e6, (ns > rb + pr + tl ? ns - rb - pr - tl : 0) / 1e6, t);
    }
}

} // namespace BbStutter
