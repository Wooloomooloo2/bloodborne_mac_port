// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: explicit ordering of untracked buffers (Metal-native renderer). Pure C++: which
// MTLFences a Metal encoder waits on when it opens and updates when it ends.
//
// Why: Metal's automatic hazard tracking on the guest arena and the fault buffer (bound writable
// to nearly every encoder) made each encoder wait for the one before it (docs/FINDINGS.md,
// 2026-10-17). Device-local buffers are therefore created untracked (mtl_buffer.mm) and ordered
// with fences. Textures and shared (CPU-visible) buffers stay tracked: Metal orders those.
//
// The rule: an encoder waits only for the encoders it conflicts with.
//   - Every Metal encoder of the in-order stream that touches untracked memory of a class gets a
//     sequence number and a slot (its number modulo Slots): the fences it updates when it ends,
//     and for a render encoder one more after its vertex stage. The tracker remembers per slot
//     its kind (render, copy, compute), its command buffer's tick and the renderer's commands it
//     holds ([first, last], Encoder::SetOrderCommand).
//   - The renderer (mtl_buffer_barriers.h) asks per command for the earlier commands it
//     conflicts with, per kind (Request). The encoder that holds the command waits for the
//     encoders holding those commands, of that kind, at their end or (reads at the vertex stage
//     only) after their vertex stage; nothing else. When the open encoder holds one of them, or
//     does not already wait for one, it ends: the next one waits.
//   - Transitive pruning: every slot keeps the set of encoders it is ordered after (its
//     closure: those it waited for, and theirs). A wait already in the closure of the encoder
//     being opened is dropped; waits are added newest first, so a wait on an encoder that
//     waited for older ones drops those.
//   - Encoders of command buffers known complete are dropped (never waited for).
//   - Slot reuse: when the slot of a new encoder still holds one in flight, the new encoder
//     waits for it first (its end) and takes over its commands, kinds and closure: waiting for
//     that slot's fence then also waits for the older encoder (BusyReuses counts these).
//   - Barriers (Barrier) are for what the renderer does not number: every encoder opened after
//     one waits for the encoders it names (render ends, their vertex stages, copies, compute
//     dispatches) until they complete or the new encoder is ordered after them anyway. Used by
//     staging round trips inside one command, isolated writes and object motion.
//   - A write the renderer does not order (Access::Write: an untracked buffer other than the
//     guest arena, the page table and device-local staging) is isolated: full barriers before
//     and after it.
//   - The uploads hoisted ahead of the in-order stream's command buffer (OpenAhead) are chained
//     through one fence that every Memory encoder waits for when it opens; a request naming the
//     uploads ends the open encoder when uploads were encoded since it opened (it may not have
//     seen them). Uploads whose ranges may still be in use by work in flight are not hoisted
//     (Runtime::HoistCopy).
// Object motion's position history is its own class (one fence per encoder, after its vertex
// stage): its passes do not conflict within a frame, and a full barrier at frame start orders
// one frame's after the last one's.
//
// Fences are global to one command queue and are evaluated in commit order ("at kernel
// submission", MTLComputeCommandEncoder.h), so the rule holds across command buffers as long as
// the in-order stream commits in the order it opens encoders (one Encoder, one thread).
// Within one Metal encoder nothing is ordered (as with hazard tracking).

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <memory>
#include <mutex>
#include "common/types.h"
#include "video_core/renderer_metal/mtl_buffer_barriers.h"

namespace Metal {

/// Untracked resources ordered as a whole.
enum class HazardClass : u8 {
    Memory,        ///< every untracked buffer (guest arena, page table, staging, fault buffer)
    MotionHistory, ///< object motion's position history (vertex stage; ordered per frame)
};
constexpr u32 NumHazardClasses = 2;

enum class Access : u8 { None, Read, Write };

/// The kind of a Metal encoder, for requests and barriers (OrderRequest, BarrierScope).
enum class Lane : u8 {
    Render,  ///< render encoders
    Copy,    ///< blit encoders, and compute encoders of copies (the copy shader, fills)
    Compute, ///< compute encoders of dispatches
};

/// Slots per hazard class: the encoders that can be waited for by number, until their command
/// buffer is known complete. The city scene opens ~900 Metal encoders a frame in two command
/// buffers, and up to four are in flight; an encoder whose slot is still in flight waits for it
/// and takes it over (BB_FRAME_STATS "busy-slot reuses"; 512 slots: every encoder).
constexpr u32 OrderSlots = 2048;
/// MTLFences: per class and slot an end and a vertex-stage fence, then the uploads' fence.
constexpr u32 OrderFences = NumHazardClasses * OrderSlots * 2 + 1;

/// A set of slots, a bit each.
struct SlotSet {
    static constexpr u32 Words = OrderSlots / 64;
    std::array<u64, Words> w{};

    void Set(u32 k) noexcept { w[k / 64] |= 1ull << (k % 64); }
    void Reset(u32 k) noexcept { w[k / 64] &= ~(1ull << (k % 64)); }
    [[nodiscard]] bool Test(u32 k) const noexcept { return (w[k / 64] >> (k % 64)) & 1; }
    [[nodiscard]] bool Any() const noexcept {
        for (u64 x : w) {
            if (x) {
                return true;
            }
        }
        return false;
    }
    SlotSet& operator|=(const SlotSet& o) noexcept {
        for (u32 i = 0; i < Words; ++i) w[i] |= o.w[i];
        return *this;
    }
    SlotSet& operator&=(const SlotSet& o) noexcept {
        for (u32 i = 0; i < Words; ++i) w[i] &= o.w[i];
        return *this;
    }
    friend SlotSet operator|(SlotSet a, const SlotSet& b) noexcept { return a |= b; }
    friend SlotSet operator&(SlotSet a, const SlotSet& b) noexcept { return a &= b; }
    /// Removes the slots of `o`.
    SlotSet& Remove(const SlotSet& o) noexcept {
        for (u32 i = 0; i < Words; ++i) w[i] &= ~o.w[i];
        return *this;
    }
    void Clear() noexcept { w = {}; }
    /// The slots of sequence numbers [from, to) (to - from <= OrderSlots).
    [[nodiscard]] static SlotSet Range(u64 from, u64 to) noexcept;
    /// Every slot, in the order of sequence numbers counting down from slot `start` (start,
    /// start - 1, ..., 0, OrderSlots - 1, ..., start + 1): newest first.
    template <typename F>
    void ForEachDown(u32 start, F&& f) const {
        const auto down = [&](u32 hi, u32 lo) { // slots hi..lo, inclusive, hi >= lo
            for (u32 i = hi / 64 + 1; i-- > lo / 64;) {
                u64 x = w[i];
                if (i == hi / 64 && hi % 64 != 63) {
                    x &= (2ull << (hi % 64)) - 1;
                }
                if (i == lo / 64) {
                    x &= ~0ull << (lo % 64);
                }
                for (; x; x &= ~(1ull << (63 - std::countl_zero(x)))) {
                    f(i * 64 + 63 - u32(std::countl_zero(x)));
                }
            }
        };
        down(start, 0);
        if (start + 1 < OrderSlots) {
            down(OrderSlots - 1, start + 1);
        }
    }
};

/// The fence waits to encode when an encoder opens and the updates to encode when it ends
/// (indices into the device's fence table, OrderTracker::NumFences).
struct OrderPlan {
    /// Updates: a render slot's two fences per class.
    static constexpr u32 MaxUpdates = 4;
    std::array<u64, (OrderFences + 63) / 64> waits{}; ///< a bit per fence
    std::array<u16, MaxUpdates> updates{};
    u32 num_waits{}, num_updates{};

    [[nodiscard]] bool Waits(u32 fence) const noexcept {
        return (waits[fence / 64] >> (fence % 64)) & 1;
    }
    /// The fences waited for, in index order.
    template <typename F>
    void ForEachWait(F&& f) const {
        for (u32 i = 0; i < waits.size(); ++i) {
            for (u64 x = waits[i]; x; x &= x - 1) {
                f(i * 64 + u32(std::countr_zero(x)));
            }
        }
    }
    [[nodiscard]] bool Updates(u32 fence) const noexcept;
    [[nodiscard]] bool Empty() const noexcept {
        return num_waits == 0 && num_updates == 0;
    }
};

class OrderTracker {
public:
    static constexpr u32 Slots = OrderSlots; ///< per class
    enum FenceStage : u32 { VertexDone, EncoderDone };
    /// Updated by every encoder committed ahead of the in-order stream.
    static constexpr u32 AheadFence = NumHazardClasses * Slots * 2;
    static constexpr u32 NumFences = AheadFence + 1;
    static_assert(NumFences == OrderFences);

    static constexpr u32 SlotFence(HazardClass c, u32 k, FenceStage s = EncoderDone) {
        return (u32(c) * Slots + k) * 2 + s;
    }
    /// The class of a fence (AheadFence: Memory), and whether it is a vertex-stage fence.
    static constexpr HazardClass ClassOf(u32 fence) {
        return fence >= AheadFence ? HazardClass::Memory : HazardClass(fence / (Slots * 2));
    }
    static constexpr bool IsVertexFence(u32 fence) {
        return fence < AheadFence && (fence & 1) == VertexDone;
    }

    using Accesses = std::array<Access, NumHazardClasses>;

    /// An encoder of the in-order stream opens with `accesses` (per class): Read takes a slot,
    /// Write is isolated (full barriers before and after it), None takes no fence. A render
    /// encoder also updates a fence after its vertex stage (Memory class). `tick`: the device
    /// tick of the command buffer it is in; `completed`: the highest tick known complete (their
    /// slots need no waiting; 0: unknown); `command`: the renderer's command it opens for (the
    /// lowest it holds). Waits for what requests and barriers left for it.
    OrderPlan Open(const Accesses& accesses, Lane lane = Lane::Compute, u64 tick = 0,
                   u64 completed = 0, u64 command = 0);
    /// The open encoder ends; it held the renderer's commands [first, last].
    void Close(u64 first, u64 last);
    /// What the command about to be encoded must wait for (BufferBarriers::Conflicts): the
    /// encoders that hold those commands, of the kinds named. Every encoder opened until
    /// EndCommand waits for them. The open encoder (holding commands [open_first, open_last])
    /// ends when it holds one of them (end_open: the next one waits for it); else it waits now
    /// for those it is not ordered after yet (waits), so a pass does not break.
    struct RequestPlan {
        bool end_open{}; ///< the open encoder holds one of the commands: end it
        OrderPlan waits; ///< else what the open encoder waits for now (before the command)
    };
    RequestPlan Request(const OrderRequest& request, u64 completed = 0, u64 open_first = 0,
                        u64 open_last = 0);
    /// The command the requests were for is encoded: encoders opened from now on no longer wait
    /// for what they asked (every encoder the command opened did).
    void EndCommand();
    /// Every encoder opened from now on waits for the encoders opened so far of the kinds
    /// `scopes` names (ScopeNone: nothing), until they complete (or it is ordered after them).
    void Barrier(HazardClass c, BarrierScopes scopes = ScopeAll);
    /// An encoder of a command buffer committed before the in-order stream's current one (the
    /// scheduler's hoisted uploads, recorded while that one is): waits for the uploads before it,
    /// then updates AheadFence. (Its ranges are not in use by work in flight: Runtime::HoistCopy.)
    /// Every fence must have been updated once before (Device primes them).
    OrderPlan OpenAhead();
    /// Encoders that found their slot still in flight and waited for it (a false dependency;
    /// BB_FRAME_STATS).
    [[nodiscard]] u64 BusyReuses(HazardClass c) const noexcept {
        return busy_reuses[u32(c)].load(std::memory_order_relaxed);
    }
    /// Fence waits encoded since the last call (at encoder opens and requests), the encoders
    /// opened, and the most in one plan (BB_FRAME_STATS).
    struct WaitStats {
        u64 waits, encoders;
        u32 max;
    };
    WaitStats TakeWaitStats() noexcept;

private:
    struct Slot {
        u64 seq{};             ///< the encoder's sequence number
        u64 tick{};            ///< its command buffer's device tick (0: unknown)
        u64 first{}, last{};   ///< the renderer's commands it holds
        u64 ahead{};           ///< uploads ahead encoded before it opened (it waits for them)
        SlotSet after_end;     ///< encoders whose end it is ordered after (by slot, its window)
        SlotSet after_vertex;  ///< ... whose vertex stage it is ordered after
        u8 kinds{};            ///< EncoderKinds of the encoders it stands for
        bool vertex{};         ///< its vertex fence stands for it (a render encoder, not merged)
    };
    struct ClassState {
        /// Per slot (on the heap: ~0.5 MiB per class).
        std::unique_ptr<Slot[]> slots = std::make_unique<Slot[]>(Slots);
        u64 next{};   ///< the sequence number of the next encoder
        u64 oldest{}; ///< the oldest encoder not known complete
        bool open{};  ///< encoder `next - 1` is open
        // Waits the next encoder takes (requests made while none is open, or for the next one).
        SlotSet pending_end, pending_vertex;
        // Barriers: waited for by every encoder opened, until complete.
        SlotSet floor_end, floor_vertex;
        bool isolate{}; ///< the last encoder wrote unordered memory: full barrier before the next
    };
    enum Need : u8 { NeedNone, NeedVertex, NeedEnd };
    static Need Needs(const Slot& slot, u64 first, u64 last, const OrderRequest& r);
    /// Drops the encoders of command buffers up to `completed`.
    static void Retire(ClassState& s, u64 completed);
    void BarrierLocked(ClassState& s, BarrierScopes scopes);
    /// Whether the encoder with sequence number `holder` (its slot's sets) is ordered after
    /// slot `k`'s end / vertex stage.
    static bool After(const Slot& holder, u32 k, Need need);
    /// Adds to `plan` the waits on the encoders of `wait_end` / `wait_vertex` that encoder
    /// `holder` (whose sets are `sets`) is not ordered after yet, newest first (a wait drops the
    /// older ones it stands for), and adds them to its sets.
    void WaitFor(ClassState& s, HazardClass c, u64 holder, Slot& sets, const SlotSet& wait_end,
                 const SlotSet& wait_vertex, OrderPlan& plan);
    void CountWaits(const OrderPlan& plan, bool open);
    std::mutex mutex;
    std::array<ClassState, NumHazardClasses> classes{};
    std::atomic<u64> ahead_uploads{};
    std::array<std::atomic<u64>, NumHazardClasses> busy_reuses{};
    std::atomic<u64> stat_waits{}, stat_encoders{};
    std::atomic<u32> stat_max{};
};

} // namespace Metal
