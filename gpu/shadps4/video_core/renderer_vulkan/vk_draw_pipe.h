// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: two-stage draw pipeline (docs/parallel_gpu.md, "Two-stage draw pipeline").
//
// The GPU command thread (stage A) decodes PM4, keeps the register file and selects pipelines;
// for a direct draw it writes a packet (the register blocks changed since the previous packet,
// the stages' user data, the draw parameters) into this ring and goes on decoding. The draw
// recording thread (stage B) owns the caches, the barriers and the command recording while
// packets are in flight: it applies each packet to its own copy of the registers and runs the
// rest of the draw. Anything else stage A does on that state waits for stage B to run dry
// (Drain) and then runs on stage A as before.

#pragma once

#include <cstdlib>
#include <cstdio>
#include "bbport_stutter.h"
#include "bbport_threads.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include "bbport_cpu.h"
#include "bbport_toggles.h"

#include "common/assert.h"
#include "common/thread.h"
#include "common/types.h"

namespace Vulkan {

class DrawPipe {
public:
    /// Runs a packet on stage B.
    using Handler = void (*)(void* context, const u8* packet, u32 size);

    DrawPipe(Handler handler_, void* context_) : handler{handler_}, context{context_} {
        const char* legacy = std::getenv("BB_PIPE_BLOCKING_WAIT");
        blocking_wait = !legacy || legacy[0] != '0';
        std::printf("Draw pipeline: %s producer waits\n", blocking_wait ? "blocking" : "legacy spin");
        ring = std::make_unique<u8[]>(Capacity);
        thread = std::jthread([this](std::stop_token stop) { Run(stop); });
    }

    ~DrawPipe() {
        Drain();
        thread.request_stop();
        published.fetch_add(0, std::memory_order_seq_cst);
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }

    /// True on the draw recording thread.
    [[nodiscard]] static bool OnStageB() noexcept {
        return on_stage_b;
    }

    [[nodiscard]] u32 StageBThreadId() const noexcept {
        return stage_b_tid.load(std::memory_order_acquire);
    }

    /// Stage A: space for a packet of `size` bytes (8-byte aligned), valid until Commit().
    u8* Begin(u32 size) {
        size = Align(size + sizeof(Header));
        ASSERT(size <= Capacity / 4);
        u64 at = head;
        const u64 offset = at % Capacity;
        if (offset + size > Capacity) {
            // Wrap: the rest of the ring is skipped (a header with zero size marks it).
            WaitForSpace(at, Capacity - offset);
            reinterpret_cast<Header*>(ring.get() + offset)->size = 0;
            at += Capacity - offset;
            head = at;
            published.store(at, std::memory_order_release);
        }
        WaitForSpace(at, size);
        pending_size = size;
        auto* header = reinterpret_cast<Header*>(ring.get() + at % Capacity);
        header->size = size;
        return reinterpret_cast<u8*>(header + 1);
    }

    /// Stage A: hands the packet from Begin() to stage B.
    void Commit(u32 payload_size) {
        reinterpret_cast<Header*>(ring.get() + head % Capacity)->payload = payload_size;
        head += pending_size;
        published.store(head, std::memory_order_seq_cst);
        ++packets;
        if (sleeping.load(std::memory_order_seq_cst)) {
            wake.fetch_add(1, std::memory_order_seq_cst);
            wake.notify_one();
        }
    }

    /// Why stage A drained (statistics): a PM4 opcode, or one of these.
    enum Reason : u32 {
        ReasonConstRam = 256,
        ReasonCommands,
        ReasonCompute,
        ReasonSubmissionEnd,
        ReasonDraw,
        ReasonRasterizer,
        NumReasons,
    };
    std::array<u64, NumReasons> drains_by_reason{};
    std::array<u64, NumReasons> cycles_by_reason{};

    /// Stage A: waits until stage B has run every committed packet.
    void Drain(u32 reason = ReasonRasterizer) {
        if (consumed.load(std::memory_order_acquire) == head) {
            return;
        }
        ++drains;
        const u32 slot = reason < NumReasons ? reason : ReasonRasterizer;
        ++drains_by_reason[slot];
        const u64 start = bbport::CpuCycles();
        for (u32 spins = 0; consumed.load(std::memory_order_acquire) != head; ++spins) {
            if (spins < 4096) {
                bbport::CpuPause();
            } else {
                // Stage B can be blocked in a GPU fence for a full frame. A
                // yield loop consumes a CPU core throughout that wait on macOS.
                // Keep the short-latency spin, then sleep until B makes progress.
                const u64 seen = consumed.load(std::memory_order_acquire);
                if (seen != head) {
                    WaitForProgress(seen);
                }
            }
        }
        const u64 waited = bbport::CpuCycles() - start;
        BbStats::drain_ns.fetch_add(bbport::CyclesToNs(waited), std::memory_order_relaxed);
        drain_cycles += waited;
        cycles_by_reason[slot] += waited;
    }

    /// Stage A: position after the last committed packet; Reached(position) once B ran it.
    [[nodiscard]] u64 Head() const noexcept {
        return head;
    }
    [[nodiscard]] bool Reached(u64 position) const noexcept {
        return consumed.load(std::memory_order_acquire) >= position;
    }

    [[nodiscard]] bool Idle() const noexcept {
        return consumed.load(std::memory_order_acquire) == head;
    }

    /// Statistics (stage A): packets, drains that had to wait, cycles waited, cycles stage B
    /// spent running packets.
    u64 packets = 0, drains = 0, drain_cycles = 0;
    std::atomic<u64> busy_cycles{0};

private:
    struct Header {
        u32 size;    ///< bytes to the next packet (0: wrap to the ring start)
        u32 payload; ///< bytes written by the producer
    };
    static constexpr u64 Capacity = 16ull << 20;

    static constexpr u32 Align(u32 size) {
        return (size + 63) & ~63u;
    }

    void WaitForSpace(u64 at, u64 size) {
        for (u32 spins = 0;; ++spins) {
            const u64 seen = consumed.load(std::memory_order_acquire);
            if (at + size - seen <= Capacity) {
                return;
            }
            if (spins < 4096) {
                bbport::CpuPause();
            } else {
                WaitForProgress(seen);
            }
        }
    }

    void WaitForProgress(u64 seen) {
        if (!blocking_wait) {
            std::this_thread::yield();
            return;
        }
        producer_sleeping.store(true, std::memory_order_seq_cst);
        // Paired seq_cst operations with PublishConsumed prevent a missed wake
        // between announcing the waiter and checking the producer's predicate.
        if (consumed.load(std::memory_order_seq_cst) == seen) {
            consumed.wait(seen, std::memory_order_acquire);
        }
        producer_sleeping.store(false, std::memory_order_seq_cst);
    }

    void PublishConsumed(u64 at) {
        consumed.store(at, std::memory_order_seq_cst);
        if (producer_sleeping.load(std::memory_order_seq_cst)) {
            consumed.notify_one();
        }
    }

    void Run(std::stop_token stop) {
        Common::SetCurrentThreadName("bb:DrawRec");
        on_stage_b = true;
#ifdef __APPLE__
        BbStutter::stage_b_port.store(pthread_mach_thread_np(pthread_self()),
                                      std::memory_order_release);
#endif
#ifdef _WIN32
        stage_b_tid.store(static_cast<u32>(GetCurrentThreadId()), std::memory_order_release);
#else
        stage_b_tid.store(static_cast<u32>(gettid()), std::memory_order_release);
#endif
        u64 at = 0;
        while (true) {
            BbStutter::ApplyQos(); // user-interactive unless switched off (stutter plan phase 2)
            // Packets follow each other within microseconds while a frame is decoded: spin,
            // and sleep only after a longer pause (between frames).
            u64 available = published.load(std::memory_order_acquire);
            if (available == at) {
                const u64 idle_start = bbport::CpuCycles();
                // With few hardware threads (Steam Deck: 8) a long spin takes time from the
                // game's own threads. BB_PIPE_SPIN_US overrides.
                static const auto spin_time = std::chrono::microseconds([] {
                    if (const char* env = std::getenv("BB_PIPE_SPIN_US")) {
                        return std::max(0, std::atoi(env));
                    }
                    return BbThreads::Available() >= 12 ? 200 : 50;
                }());
                const auto spin_until = std::chrono::steady_clock::now() + spin_time;
                for (u32 spins = 1; available == at; ++spins) {
                    if (stop.stop_requested()) {
                        return;
                    }
                    bbport::CpuPause();
                    if (!(spins & 255) && std::chrono::steady_clock::now() >= spin_until) {
                        const u32 seen = wake.load(std::memory_order_seq_cst);
                        sleeping.store(true, std::memory_order_seq_cst);
                        if (published.load(std::memory_order_seq_cst) == at &&
                            !stop.stop_requested()) {
                            wake.wait(seen, std::memory_order_seq_cst);
                        }
                        sleeping.store(false, std::memory_order_relaxed);
                    }
                    available = published.load(std::memory_order_acquire);
                }
                BbStats::rec_idle_ns.fetch_add(bbport::CyclesToNs(bbport::CpuCycles() - idle_start),
                                               std::memory_order_relaxed);
            }
            const auto* header = reinterpret_cast<const Header*>(ring.get() + at % Capacity);
            if (header->size == 0) {
                at += Capacity - at % Capacity;
                PublishConsumed(at);
                continue;
            }
            const u64 start = bbport::CpuCycles();
            handler(context, reinterpret_cast<const u8*>(header + 1), header->payload);
            const u64 busy = bbport::CpuCycles() - start;
            busy_cycles.fetch_add(busy, std::memory_order_relaxed);
            BbStats::rec_busy_ns.fetch_add(bbport::CyclesToNs(busy), std::memory_order_relaxed);
            at += header->size;
            PublishConsumed(at);
        }
    }

    static inline thread_local bool on_stage_b = false;
    Handler handler;
    void* context;
    std::unique_ptr<u8[]> ring;
    u64 head = 0; ///< stage A's write position
    u32 pending_size = 0;
    alignas(64) std::atomic<u64> published{0};
    alignas(64) std::atomic<u64> consumed{0};
    alignas(64) std::atomic<u32> wake{0};
    std::atomic<bool> sleeping{false};
    std::atomic<bool> producer_sleeping{false};
    bool blocking_wait = true; ///< set before thread creation; rollback/A-B switch
    std::atomic<u32> stage_b_tid{0};
    std::jthread thread;
};

} // namespace Vulkan
