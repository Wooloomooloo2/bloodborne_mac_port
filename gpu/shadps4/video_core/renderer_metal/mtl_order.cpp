// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: explicit ordering of untracked buffers (mtl_order.h).

#include <algorithm>
#include <bit>
#include "common/assert.h"
#include "video_core/renderer_metal/mtl_order.h"

namespace Metal {

namespace {

void AddWait(OrderPlan& p, u32 fence) {
    DEBUG_ASSERT(fence < OrderFences);
    u64& word = p.waits[fence / 64];
    const u64 bit = 1ull << (fence % 64);
    p.num_waits += (word & bit) ? 0 : 1;
    word |= bit;
}

void AddUpdate(OrderPlan& p, u32 fence) {
    DEBUG_ASSERT(p.num_updates < OrderPlan::MaxUpdates);
    p.updates[p.num_updates++] = u16(fence);
}

static_assert(OrderPlan::MaxUpdates >= NumHazardClasses * 2);
static_assert(OrderTracker::NumFences <= 0x10000, "plans store fence indices as u16");
static_assert(OrderSlots % 64 == 0);

u8 KindOf(Lane lane) {
    switch (lane) {
    case Lane::Render:
        return EncoderRender;
    case Lane::Copy:
        return EncoderCopy;
    case Lane::Compute:
        return EncoderCompute;
    }
    return EncoderCompute;
}

} // namespace

SlotSet SlotSet::Range(u64 from, u64 to) noexcept {
    SlotSet s;
    if (to <= from) {
        return s;
    }
    if (to - from >= OrderSlots) {
        s.w.fill(~0ull);
        return s;
    }
    // Slots from % S .. (to - 1) % S, wrapping.
    const auto bits = [&](u32 lo, u32 hi) { // inclusive, lo <= hi
        for (u32 i = lo / 64; i <= hi / 64; ++i) {
            u64 x = ~0ull;
            if (i == lo / 64) {
                x &= ~0ull << (lo % 64);
            }
            if (i == hi / 64 && hi % 64 != 63) {
                x &= (2ull << (hi % 64)) - 1;
            }
            s.w[i] |= x;
        }
    };
    const u32 lo = u32(from % OrderSlots), hi = u32((to - 1) % OrderSlots);
    if (lo <= hi) {
        bits(lo, hi);
    } else {
        bits(lo, OrderSlots - 1);
        bits(0, hi);
    }
    return s;
}

bool OrderPlan::Updates(u32 fence) const noexcept {
    return std::find(updates.begin(), updates.begin() + num_updates, u16(fence)) !=
           updates.begin() + num_updates;
}

OrderTracker::Need OrderTracker::Needs(const Slot& slot, u64 first, u64 last,
                                       const OrderRequest& r) {
    if (((slot.kinds & EncoderRender) && r.render_end.Meets(first, last)) ||
        ((slot.kinds & EncoderCopy) && r.copy.Meets(first, last)) ||
        ((slot.kinds & EncoderCompute) && r.compute.Meets(first, last))) {
        return NeedEnd;
    }
    if ((slot.kinds & EncoderRender) && r.render_vertex.Meets(first, last)) {
        return slot.vertex ? NeedVertex : NeedEnd;
    }
    return NeedNone;
}

void OrderTracker::Retire(ClassState& s, u64 completed) {
    if (!completed) {
        return;
    }
    while (s.oldest < s.next && !(s.open && s.oldest == s.next - 1)) {
        const u32 k = u32(s.oldest % Slots);
        const u64 tick = s.slots[k].tick;
        if (tick == 0 || tick > completed) {
            break;
        }
        s.floor_end.Reset(k);
        s.floor_vertex.Reset(k);
        s.pending_end.Reset(k);
        s.pending_vertex.Reset(k);
        ++s.oldest;
    }
}

bool OrderTracker::After(const Slot& holder, u32 k, Need need) {
    return need == NeedEnd ? holder.after_end.Test(k)
                           : holder.after_end.Test(k) || holder.after_vertex.Test(k);
}

void OrderTracker::BarrierLocked(ClassState& s, BarrierScopes scopes) {
    if (scopes & ScopeRenderEnd) {
        scopes |= ScopeRenderVertex;
    }
    SlotSet added;
    for (u64 seq = s.oldest; seq < s.next; ++seq) {
        const u32 k = u32(seq % Slots);
        const Slot& slot = s.slots[k];
        const bool render = slot.kinds & EncoderRender;
        if ((render && (scopes & ScopeRenderEnd)) ||
            ((slot.kinds & EncoderCopy) && (scopes & ScopeCopyEnd)) ||
            ((slot.kinds & EncoderCompute) && (scopes & ScopeComputeEnd))) {
            s.floor_end.Set(k);
            added.Set(k);
        } else if (render && (scopes & ScopeRenderVertex)) {
            (slot.vertex ? s.floor_vertex : s.floor_end).Set(k);
            added.Set(k);
        }
    }
    s.floor_vertex.Remove(s.floor_end);
    if (!added.Any()) {
        return;
    }
    // What the encoders waited for stand for, among the older ones of the floor.
    SlotSet implied_end, implied_vertex;
    for (u64 seq = s.oldest; seq < s.next; ++seq) {
        const u32 k = u32(seq % Slots);
        if (!s.floor_end.Test(k) && !s.floor_vertex.Test(k)) {
            continue;
        }
        const Slot& slot = s.slots[k];
        const SlotSet older =
            SlotSet::Range(std::max(s.oldest, seq >= Slots ? seq - Slots + 1 : 0), seq);
        implied_end |= slot.after_end & older;
        implied_vertex |= (slot.after_end | slot.after_vertex) & older;
    }
    s.floor_end.Remove(implied_end);
    s.floor_vertex.Remove(implied_vertex);
}

void OrderTracker::WaitFor(ClassState& s, HazardClass c, u64 holder, Slot& sets,
                           const SlotSet& wait_end, const SlotSet& wait_vertex, OrderPlan& plan) {
    if (holder == 0) {
        return; // no encoder before the first
    }
    // Slot sets of `holder` name sequence numbers [window, holder).
    const u64 window = holder >= Slots ? holder - Slots : 0;
    const u64 lowest = std::max(s.oldest, window);
    // Newest first: a wait drops the older ones it stands for.
    (wait_end | wait_vertex).ForEachDown(u32((holder - 1) % Slots), [&](u32 k) {
        const u64 seq = holder - 1 - ((holder - 1 - k) % Slots);
        if (seq < lowest || seq >= holder) {
            return; // complete, or not an encoder's
        }
        const Need need = wait_end.Test(k) ? NeedEnd : NeedVertex;
        if (After(sets, k, need)) {
            return;
        }
        const Slot& slot = s.slots[k];
        const bool vertex = need == NeedVertex && slot.vertex;
        AddWait(plan, SlotFence(c, k, vertex ? VertexDone : EncoderDone));
        // What it is ordered after, among the sequence numbers [window, seq).
        const SlotSet older = SlotSet::Range(window, seq);
        sets.after_end |= slot.after_end & older;
        sets.after_vertex |= slot.after_vertex & older;
        (vertex ? sets.after_vertex : sets.after_end).Set(k);
    });
    sets.after_vertex.Remove(sets.after_end);
}

void OrderTracker::CountWaits(const OrderPlan& plan, bool open) {
    stat_waits.fetch_add(plan.num_waits, std::memory_order_relaxed);
    if (open) {
        stat_encoders.fetch_add(1, std::memory_order_relaxed);
    }
    u32 max = stat_max.load(std::memory_order_relaxed);
    while (plan.num_waits > max &&
           !stat_max.compare_exchange_weak(max, plan.num_waits, std::memory_order_relaxed)) {
    }
}

OrderPlan OrderTracker::Open(const Accesses& accesses, Lane lane, u64 tick, u64 completed,
                             u64 command) {
    std::scoped_lock lock{mutex};
    OrderPlan plan;
    for (u32 i = 0; i < NumHazardClasses; ++i) {
        const Access access = accesses[i];
        if (access == Access::None) {
            continue;
        }
        const HazardClass c = HazardClass(i);
        const bool memory = c == HazardClass::Memory;
        ClassState& s = classes[i];
        s.open = false; // an encoder not closed (tests): closed as it was
        Retire(s, completed);
        if (s.isolate || access == Access::Write) {
            s.isolate = false;
            BarrierLocked(s, ScopeAll); // after the isolated write / before this one
        }
        s.isolate = access == Access::Write;
        const u64 e = s.next;
        const u32 slot_index = u32(e % Slots);
        // The window of sequence numbers a slot set names relative to `e`.
        const u64 window = e >= Slots ? e - Slots : 0;
        SlotSet wait_end = s.pending_end | s.floor_end;
        SlotSet wait_vertex = s.pending_vertex | s.floor_vertex;
        // The slot still holds an encoder in flight: wait for it first, and take it over.
        const bool merged = e >= Slots && e - Slots >= s.oldest;
        if (merged) {
            wait_end.Set(slot_index);
            busy_reuses[i].fetch_add(1, std::memory_order_relaxed);
        }
        Slot fresh;
        WaitFor(s, c, e, fresh, wait_end, wait_vertex, plan);
        if (memory) {
            AddWait(plan, AheadFence);
        }
        Slot& slot = s.slots[slot_index];
        const Slot old = slot;
        slot.seq = e;
        slot.tick = tick;
        slot.first = merged ? std::min(old.first, command) : command;
        slot.last = std::max(merged ? old.last : 0, command);
        slot.ahead = ahead_uploads.load(std::memory_order_acquire);
        slot.after_end = fresh.after_end;
        slot.after_vertex = fresh.after_vertex;
        // Motion history: every encoder one fence, after its vertex stage (lane kept as compute).
        const u8 kind = memory ? KindOf(lane) : EncoderCompute;
        slot.kinds = merged ? u8(old.kinds | kind) : kind;
        slot.vertex = memory && lane == Lane::Render && !merged;
        if (merged) {
            s.oldest = e - Slots + 1; // the old encoder is the new one's now
        }
        s.next = e + 1;
        s.open = true;
        if (memory && lane == Lane::Render) {
            AddUpdate(plan, SlotFence(c, slot_index, VertexDone));
        }
        AddUpdate(plan, SlotFence(c, slot_index));
    }
    if (!plan.Empty()) {
        CountWaits(plan, true);
    }
    return plan;
}

void OrderTracker::Close(u64 first, u64 last) {
    std::scoped_lock lock{mutex};
    for (u32 i = 0; i < NumHazardClasses; ++i) {
        ClassState& s = classes[i];
        if (!s.open) {
            continue;
        }
        s.open = false;
        if (i == u32(HazardClass::Memory)) {
            Slot& slot = s.slots[(s.next - 1) % Slots];
            slot.first = std::min(slot.first, first);
            slot.last = std::max(slot.last, last);
        }
    }
}

OrderTracker::RequestPlan OrderTracker::Request(const OrderRequest& r, u64 completed,
                                                u64 open_first, u64 open_last) {
    std::scoped_lock lock{mutex};
    ClassState& s = classes[u32(HazardClass::Memory)];
    Retire(s, completed);
    RequestPlan result;
    Slot* open = s.open ? &s.slots[(s.next - 1) % Slots] : nullptr;
    const u64 open_seq = open ? s.next - 1 : ~0ull;
    SlotSet now_end, now_vertex; // older encoders the open one must wait for
    if (const u64 lowest = r.Lowest(); lowest != ~0ull) {
        // Newest first; encoders hold commands in order (the last of each grows), so the search
        // stops at the first one entirely before the request's lowest command.
        for (u64 seq = s.next; seq-- > s.oldest;) {
            const u32 k = u32(seq % Slots);
            const Slot& slot = s.slots[k];
            const bool is_open = seq == open_seq;
            const u64 first = is_open ? std::min(slot.first, open_first) : slot.first;
            const u64 last = is_open ? std::max(slot.last, open_last) : slot.last;
            if (last < lowest) {
                break;
            }
            const Need need = Needs(slot, first, last, r);
            if (need == NeedNone) {
                continue;
            }
            if (is_open) {
                result.end_open = true; // it holds one of the commands
            } else if (open && !After(*open, k, need)) {
                (need == NeedEnd ? now_end : now_vertex).Set(k);
            }
            (need == NeedEnd ? s.pending_end : s.pending_vertex).Set(k);
        }
        s.pending_vertex.Remove(s.pending_end);
    }
    if (!open || result.end_open) {
        return result; // the encoders it opens wait (pending; the uploads at every open)
    }
    // The open encoder goes on: it waits now, before the command's work (Metal may move the
    // wait to the encoder's start: earlier, never later).
    now_vertex.Remove(now_end);
    WaitFor(s, HazardClass::Memory, open_seq, *open, now_end, now_vertex, result.waits);
    if (r.ahead) {
        const u64 uploads = ahead_uploads.load(std::memory_order_acquire);
        if (open->ahead < uploads) { // uploads encoded since it opened
            AddWait(result.waits, AheadFence);
            open->ahead = uploads;
        }
    }
    CountWaits(result.waits, false);
    return result;
}

void OrderTracker::EndCommand() {
    std::scoped_lock lock{mutex};
    ClassState& s = classes[u32(HazardClass::Memory)];
    s.pending_end.Clear();
    s.pending_vertex.Clear();
}

void OrderTracker::Barrier(HazardClass c, BarrierScopes scopes) {
    std::scoped_lock lock{mutex};
    BarrierLocked(classes[u32(c)], scopes);
}

OrderPlan OrderTracker::OpenAhead() {
    ahead_uploads.fetch_add(1, std::memory_order_acq_rel);
    OrderPlan plan;
    AddWait(plan, AheadFence);
    AddUpdate(plan, AheadFence);
    return plan;
}

OrderTracker::WaitStats OrderTracker::TakeWaitStats() noexcept {
    return {stat_waits.exchange(0, std::memory_order_relaxed),
            stat_encoders.exchange(0, std::memory_order_relaxed),
            stat_max.exchange(0, std::memory_order_relaxed)};
}

} // namespace Metal
