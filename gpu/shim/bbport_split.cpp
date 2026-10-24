// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the two-process split of the GPU library (bbport_split.h).
#include "bbport_split.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <signal.h>
#include <unistd.h>
#include "common/thread.h"
#include "core/platform.h"
#include "video_core/amdgpu/liverpool.h"

extern std::unique_ptr<AmdGpu::Liverpool> liverpool; // gnmdriver.cpp

extern "C" void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write);
extern "C" void runtime_restart(void); // bb-probe (probe.c)

namespace Libraries::VideoOut {
void BbSplitApplyEvent(const BbGpuRecord* record);
}

namespace BbSplit {
namespace {

std::atomic<Mode> g_mode{Mode::Single};
BbGpuShared* g_shared;
std::mutex g_push_mutex;  // producers on this side of each ring
std::mutex g_fault_mutex; // game: producers of the fault ring (fault handlers)
std::atomic<u64> g_gpu_idle_seq;

constexpr u64 Second = 1'000'000'000;

/// The peer process is gone: nothing can make progress any more.
[[noreturn]] void PeerLost(const char* what) {
    std::printf("GPU split: the %s process is gone; exiting\n", what);
    std::fflush(stdout);
    _exit(1);
}

bool PeerAlive(s32 pid) {
    return pid <= 0 || kill(pid, 0) == 0;
}

void PushTo(BbGpuRing& ring, const void* record, s32 peer_pid, const char* peer) {
    std::scoped_lock lock{g_push_mutex};
    for (u32 waited = 0;; ++waited) {
        if (bbgpu_ring_push(g_shared, &ring, record, Second) == 0) {
            return;
        }
        if (!PeerAlive(peer_pid)) {
            PeerLost(peer);
        }
        if (waited == 10) {
            std::printf("GPU split: the %s has not taken records for 10 s (ring full)\n", peer);
        }
    }
}

} // namespace

Mode GetMode() {
    return g_mode.load(std::memory_order_relaxed);
}

BbGpuShared* Shared() {
    return g_shared;
}

void Attach(BbGpuShared* shared) {
    g_shared = shared;
}

void InitRemote(BbGpuShared* shared) {
    g_shared = shared;
    g_mode = Mode::Remote;
    // M4: the host's page protection requests, applied here and acknowledged in order.
    std::thread([] {
        Common::SetCurrentThreadName("bb:GpuProtect");
        BbGpuShared* s = g_shared;
        for (;;) {
            const BbGpuRecord* r = bbgpu_ring_peek(s, &s->protect, Second);
            if (!r) {
                if (!PeerAlive(s->host_pid)) {
                    PeerLost("GPU host");
                }
                continue;
            }
            const auto* p = reinterpret_cast<const BbGpuProtect*>(r);
            runtime_memory_gpu_protect(p->address, p->size, static_cast<int>(p->read),
                                       static_cast<int>(p->write));
            const u32 seq = p->seq;
            bbgpu_ring_pop(&s->protect, r);
            __atomic_store_n(&s->protect_done, seq, __ATOMIC_RELEASE);
            bbgpu_wake_word(&s->protect_done);
        }
    }).detach();
}

void InitHost(BbGpuShared* shared) {
    g_shared = shared;
    g_mode = Mode::Host;
}

void Push(const void* record) {
    PushTo(g_shared->submit, record, g_shared->host_pid, "GPU host");
}

s64 Request(void* record, u32* slot) {
    const int claimed = bbgpu_reply_claim(g_shared);
    *slot = static_cast<u32>(claimed);
    Push(record);
    s64 result = 0;
    for (u32 waited = 0; bbgpu_reply_wait(g_shared, claimed, &result, Second) != 0; ++waited) {
        if (!PeerAlive(g_shared->host_pid)) {
            PeerLost("GPU host");
        }
        if (waited == 30) {
            std::printf("GPU split: request %u unanswered for 30 s\n",
                        static_cast<const BbGpuRecord*>(record)->type);
        }
    }
    return result;
}

bool RelayFault(u64 address, bool write) {
    if (address < BBGPU_GUEST_MIN || address >= BBGPU_GUEST_MAX) {
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    const int slot = bbgpu_reply_claim(g_shared);
    const BbGpuFault fault{{BBGPU_REC_FAULT, sizeof(BbGpuFault)}, address, static_cast<u32>(slot),
                           write ? 1u : 0u};
    {
        std::scoped_lock lock{g_fault_mutex};
        while (bbgpu_ring_push(g_shared, &g_shared->faults, &fault, Second) != 0) {
            if (!PeerAlive(g_shared->host_pid)) {
                PeerLost("GPU host");
            }
        }
    }
    s64 handled = 0;
    while (bbgpu_reply_wait(g_shared, slot, &handled, Second) != 0) {
        if (!PeerAlive(g_shared->host_pid)) {
            PeerLost("GPU host");
        }
    }
    const auto waited = std::chrono::steady_clock::now() - start;
    __atomic_add_fetch(&g_shared->game_faults, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_shared->game_fault_ns,
                       u64(std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count()),
                       __ATOMIC_RELAXED);
    return handled != 0;
}

u64 GpuIdleSeq() {
    return g_gpu_idle_seq.load(std::memory_order_acquire);
}

void StartEventDispatcher() {
    std::thread([] {
        Common::SetCurrentThreadName("bb:GpuEvents");
        BbGpuShared* s = g_shared;
        for (;;) {
            const BbGpuRecord* r = bbgpu_ring_peek(s, &s->events, Second);
            if (!r) {
                if (!PeerAlive(s->host_pid)) {
                    PeerLost("GPU host");
                }
                continue;
            }
            switch (r->type) {
            case BBGPU_EV_IRQ:
                if (reinterpret_cast<const BbGpuIrq*>(r)->id ==
                    static_cast<u32>(Platform::InterruptId::GpuIdle)) {
                    g_gpu_idle_seq.store(reinterpret_cast<const BbGpuIrq*>(r)->seq,
                                         std::memory_order_release);
                }
                // The game's own registrations (sceGnmAddEqEvent, the submission lock) run here
                // as they ran on the GPU thread in a single process.
                Platform::IrqC::Instance()->Signal(
                    static_cast<Platform::InterruptId>(reinterpret_cast<const BbGpuIrq*>(r)->id));
                break;
            case BBGPU_EV_FLIP:
            case BBGPU_EV_EOP_FLIP_QUEUED:
            case BBGPU_EV_VBLANK:
                Libraries::VideoOut::BbSplitApplyEvent(r);
                break;
            case BBGPU_EV_RESTART:
                runtime_restart(); // execs run.sh; the host follows bb-probe's exec and exits
                break;
            default:
                std::printf("GPU split: unknown event %u\n", r->type);
            }
            bbgpu_ring_pop(&s->events, r);
        }
    }).detach();
}

void Post(const void* record) {
    PushTo(g_shared->events, record, g_shared->game_pid, "game");
}

void ForwardInterrupts() {
    using Platform::InterruptId;
    static constexpr InterruptId ids[] = {
        InterruptId::Compute0RelMem, InterruptId::Compute1RelMem, InterruptId::Compute2RelMem,
        InterruptId::Compute3RelMem, InterruptId::Compute4RelMem, InterruptId::Compute5RelMem,
        InterruptId::Compute6RelMem, InterruptId::GfxEop,         InterruptId::GpuIdle,
    };
    static int uid; // one registration per interrupt
    for (const InterruptId id : ids) {
        Platform::IrqC::Instance()->Register(
            id,
            [](InterruptId irq) {
                // GpuIdle carries the SubmitDone it follows (gnmdriver ResetSubmissionLock).
                const u32 seq = irq == InterruptId::GpuIdle && liverpool
                                    ? static_cast<u32>(liverpool->SubmitDonesHandled())
                                    : 0;
                const BbGpuIrq event{{BBGPU_EV_IRQ, sizeof(BbGpuIrq)}, static_cast<u32>(irq), seq};
                Post(&event);
            },
            &uid);
    }
}

} // namespace BbSplit
