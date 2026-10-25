// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: which earlier commands an access to the guest arena must wait for (Metal-native
// renderer). Pure C++.
//
// The arena is untracked (mtl_buffer.mm) and nearly every command reads it, so ordering it per
// Metal encoder serialises the frame (docs/FINDINGS.md, 2026-10-17). The renderer knows the
// ranges each command touches instead and orders those, per command:
//   - every command of the in-order stream gets a number (BeginCommand; the Encoder learns it
//     through Scheduler::SetOrderCommand, so it knows which Metal encoders hold a command);
//   - per 256-byte block the last write (its command and kinds of encoder) and the window of
//     commands that read the block since (lowest and highest, and their kinds; for reads in
//     render passes, the stage) are recorded;
//   - an access asks (Conflicts) for an OrderRequest: per kind of encoder, the window of earlier
//     commands it conflicts with:
//       - a read conflicts with the block's last write (RAW): earlier writes of the block were
//         ordered before that one (WAW), and the reads before it too (WAR);
//       - a write conflicts with the last write (WAW) and with the reads since (WAR): their
//         ends, or for reads made only at the vertex stage of render passes (vertex, index and
//         indirect-argument fetches, pre-rasterisation shaders) their vertex stages;
//       - reads never conflict with reads (RAR);
//   - the Encoder makes the command's Metal encoder wait for the encoders that hold commands of
//     those windows, of the matching kind only, and for nothing else
//     (Metal::OrderTracker::Request).
// Nothing is retired by a barrier: a later command that conflicts with the same writer asks
// again (its encoder need not be ordered after the first asker's). Commands of command buffers
// known complete are retired (Retire) and never asked for.
// Writers are named exactly (a few per kind; past those, and for the readers of a block, a
// window of command numbers: the lowest and the highest, so a request may name commands between
// them that do not touch the range: a wait too many, never one too few).
// Use, on the thread that records the commands (accesses are in recording order):
//   Conflicts(...) for every access of the next command (OR the requests); BeginCommand();
//   record the request (when not empty) and the command; Mark(...) its accesses.
// Granularity: 256-byte blocks (constant tables of neighbouring draws share pages, and
// whole-page tracking made their reads conflict with the copies between them); ranges of whole
// 1 MiB chunks are marked per chunk (vertex buffers, big copies), whole buffers (direct memory
// access) once.
// Command numbers are stored as their low 32 bits and expanded against the current command: a
// tag 2^32 commands old (hours) may look recent again, which only adds a wait.

#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <unordered_map>
#include "common/types.h"

namespace Metal {

/// A set of kinds of encoder (a mask; 0 = none): what OrderTracker::Barrier waits for, and
/// OrderRequest::Scopes (which kinds a request names).
enum BarrierScope : u8 {
    ScopeNone = 0,
    ScopeRenderVertex = 1, ///< the vertex stage of render encoders
    ScopeRenderEnd = 2,    ///< render encoders (implies ScopeRenderVertex)
    ScopeCopyEnd = 4,      ///< copy encoders (blits, the copy-shader and fill kernels)
    ScopeComputeEnd = 8,   ///< compute dispatches
    ScopeAhead = 16,       ///< the uploads hoisted ahead of the command buffer
    ScopeAll = ScopeRenderEnd | ScopeCopyEnd | ScopeComputeEnd | ScopeAhead,
};
using BarrierScopes = u8;

/// When a read happens in the render pass that makes it.
enum class ReadStage : u8 {
    Vertex, ///< vertex fetch, index fetch, indirect arguments, pre-rasterisation shaders
    Late,   ///< fragment shaders (and everything in compute and blit encoders)
};

/// Which kinds of Metal encoder make an access (a mask: a tessellated draw reads its inputs in
/// compute kernels and in its render pass).
enum EncoderKinds : u8 {
    EncoderRender = 1,
    EncoderCopy = 2,    ///< blits, and compute encoders of copies (the copy shader, fills)
    EncoderCompute = 4, ///< compute dispatches (guest and host kernels)
    EncoderAhead = 8,   ///< uploads hoisted ahead of the command buffer (Encoder Ordering::Ahead)
};

/// A window of command numbers (inclusive); empty when lo > hi.
struct CommandWindow {
    u64 lo = ~0ull, hi = 0;

    [[nodiscard]] bool Empty() const noexcept {
        return lo > hi;
    }
    void Add(u64 first, u64 last) noexcept {
        lo = std::min(lo, first);
        hi = std::max(hi, last);
    }
    void Add(const CommandWindow& o) noexcept {
        if (!o.Empty()) {
            Add(o.lo, o.hi);
        }
    }
    /// Whether commands [first, last] (an encoder's) meet the window.
    [[nodiscard]] bool Meets(u64 first, u64 last) const noexcept {
        return lo <= hi && first <= hi && last >= lo;
    }
    bool operator==(const CommandWindow&) const = default;
};

/// Earlier commands to wait for: a few exact ones (the last writers of the blocks an access
/// touches, usually one or two) and a window (reads since a write, and exact ones past the few).
struct CommandSet {
    static constexpr u32 MaxExact = 6;
    std::array<u64, MaxExact> exact{};
    u32 count{};
    CommandWindow window;

    [[nodiscard]] bool Empty() const noexcept {
        return count == 0 && window.Empty();
    }
    void Add(u64 command) noexcept {
        for (u32 i = 0; i < count; ++i) {
            if (exact[i] == command) {
                return;
            }
        }
        if (count < MaxExact) {
            exact[count++] = command;
        } else {
            window.Add(command, command);
        }
    }
    void Add(u64 first, u64 last) noexcept {
        if (first == last) {
            Add(first);
        } else {
            window.Add(first, last);
        }
    }
    void Add(const CommandSet& o) noexcept {
        for (u32 i = 0; i < o.count; ++i) {
            Add(o.exact[i]);
        }
        window.Add(o.window);
    }
    /// Whether one of its commands is among [first, last] (an encoder's).
    [[nodiscard]] bool Meets(u64 first, u64 last) const noexcept {
        if (window.Meets(first, last)) {
            return true;
        }
        for (u32 i = 0; i < count; ++i) {
            if (exact[i] >= first && exact[i] <= last) {
                return true;
            }
        }
        return false;
    }
    /// The lowest and the highest command (empty: lo > hi).
    [[nodiscard]] CommandWindow Span() const noexcept {
        CommandWindow span = window;
        for (u32 i = 0; i < count; ++i) {
            span.Add(exact[i], exact[i]);
        }
        return span;
    }
};

/// What a command must wait for: per kind of Metal encoder, the earlier commands whose encoders
/// it waits for (Metal::Encoder::OrderWait).
struct OrderRequest {
    CommandSet render_end;    ///< render encoders, their ends
    CommandSet render_vertex; ///< render encoders, their vertex stages
    CommandSet copy;          ///< copy encoders
    CommandSet compute;       ///< compute dispatches
    /// The uploads hoisted ahead (one fence stands for them all; OrderTracker::Request).
    bool ahead{};

    [[nodiscard]] bool Empty() const noexcept {
        return render_end.Empty() && render_vertex.Empty() && copy.Empty() && compute.Empty() &&
               !ahead;
    }
    /// The lowest command named (~0: none).
    [[nodiscard]] u64 Lowest() const noexcept {
        return std::min({render_end.Span().lo, render_vertex.Span().lo, copy.Span().lo,
                         compute.Span().lo});
    }
    /// Whether it names a command from `first` on (the uploads ahead are not commands).
    [[nodiscard]] bool NamesFrom(u64 first) const noexcept {
        return render_end.Meets(first, ~0ull) || render_vertex.Meets(first, ~0ull) ||
               copy.Meets(first, ~0ull) || compute.Meets(first, ~0ull);
    }
    /// The kinds named (diagnostics and tests).
    [[nodiscard]] BarrierScopes Scopes() const noexcept {
        return BarrierScopes((render_end.Empty() ? 0 : ScopeRenderEnd) |
                             (render_vertex.Empty() ? 0 : ScopeRenderVertex) |
                             (copy.Empty() ? 0 : ScopeCopyEnd) |
                             (compute.Empty() ? 0 : ScopeComputeEnd) | (ahead ? ScopeAhead : 0));
    }
    OrderRequest& operator|=(const OrderRequest& o) noexcept {
        render_end.Add(o.render_end);
        render_vertex.Add(o.render_vertex);
        copy.Add(o.copy);
        compute.Add(o.compute);
        ahead |= o.ahead;
        return *this;
    }
    friend OrderRequest operator|(OrderRequest a, const OrderRequest& b) noexcept {
        return a |= b;
    }
};

class BufferBarriers {
public:
    static constexpr u32 PageBits = 12;
    static constexpr u32 BlockBits = 8; ///< 16 blocks per page
    static constexpr u32 ChunkBits = 20;

    /// Tracks `buffer` (its first `size` bytes; accesses past them are clamped). The tag arrays
    /// are reserved, not committed (~7% of `size`, committed as accesses touch them). When
    /// they cannot be allocated (logged), the buffer is ordered conservatively: every access
    /// to it waits for every command not retired.
    void Register(const void* buffer, u64 size);
    void Unregister(const void* buffer) {
        buffers.erase(buffer);
    }
    [[nodiscard]] bool Registered(const void* buffer) const {
        return buffers.contains(buffer);
    }
    /// Numbers the next command (Mark marks its accesses); returns its number (never 0, nor a
    /// multiple of 2^32: a stored 0 means "none").
    u64 BeginCommand() noexcept {
        if (u32(++command) == 0) {
            ++command;
        }
        return command;
    }
    /// The current command (0: none yet).
    [[nodiscard]] u64 Command() const noexcept {
        return command;
    }
    /// Commands up to `cmd` are complete: never waited for again.
    void Retire(u64 cmd) noexcept {
        retired = std::max(retired, cmd);
    }
    [[nodiscard]] u64 Retired() const noexcept {
        return retired;
    }
    /// The commands an access must wait for (unregistered buffers: none; the Encoder orders
    /// those itself).
    [[nodiscard]] OrderRequest Conflicts(const void* buffer, u64 offset, u64 size,
                                         bool write) const;
    /// Records the access by the current command, made by `encoders` (reads in render passes
    /// at `stage`).
    void Mark(const void* buffer, u64 offset, u64 size, bool write, u8 encoders,
              ReadStage stage = ReadStage::Late);

private:
    /// Kinds of read (Block::read_kinds).
    enum ReadKind : u8 {
        ReadRenderVertex = 1,
        ReadRenderLate = 2,
        ReadCopy = 4,
        ReadCompute = 8,
        ReadAhead = 16,
    };
    /// The accesses of a block (or of a whole chunk, or of the whole buffer). Command tags: the
    /// low 32 bits, 0 = none.
    struct Block {
        u32 write;      ///< the last write
        u32 read_lo;    ///< the reads since it, lowest
        u32 read_hi;    ///< ... and highest
        u8 write_kinds; ///< EncoderKinds of the write
        u8 read_kinds;  ///< ReadKind of the reads
    };
    /// calloc'd: zero (never accessed) without committing memory; large blocks come from fresh
    /// zero pages the OS commits as accesses touch them.
    struct FreeDeleter {
        void operator()(void* p) const noexcept;
    };
    template <typename T>
    using ZeroArray = std::unique_ptr<T[], FreeDeleter>;
    /// The accesses of a page's blocks together, for a query covering the whole page: the
    /// window of their writes (lowest, highest) and of their reads (since the oldest live one;
    /// writes do not reset them here).
    struct Summary {
        u32 write_lo, write_hi;
        u32 read_lo, read_hi;
        u8 write_kinds, read_kinds;
    };
    struct Tags {
        ZeroArray<Block> blocks;    ///< per 256-byte block
        ZeroArray<Block> page;      ///< accesses of a whole page
        ZeroArray<Summary> summary; ///< per page, its blocks' accesses together
        ZeroArray<u32> page_touch;  ///< the newest command that marked the page (or a block)
        ZeroArray<Block> chunk;     ///< accesses of a whole chunk
        ZeroArray<u32> chunk_touch; ///< the newest command that marked anything in the chunk
        Block whole{};              ///< accesses of the whole buffer (direct memory access)
        u32 touch{};                ///< the newest command that marked anything
        u64 size{};
        /// The arrays could not be allocated: every access waits for every live command.
        bool conservative{};
    };
    /// The command a stored tag stands for (0: none).
    [[nodiscard]] u64 Expand(u32 tag) const noexcept {
        return tag == 0 ? 0 : command - u32(u32(command) - tag);
    }
    [[nodiscard]] bool Live(u32 tag) const noexcept {
        return Expand(tag) > retired;
    }
    /// Adds what an access (`write`) must wait for among `b`'s accesses.
    void Collect(const Block& b, bool write, OrderRequest& r) const;
    void Collect(const Summary& s, bool write, OrderRequest& r) const;
    void MarkBlock(Block& b, bool write, u8 encoders, ReadStage stage) const;
    void MarkSummary(Summary& s, bool write, u8 encoders, ReadStage stage) const;
    /// Adds read kinds `kinds` (ReadKind) of commands [lo, hi] to `r`.
    static void AddReads(u8 kinds, u64 lo, u64 hi, OrderRequest& r);
    static void AddWrites(u8 kinds, u64 lo, u64 hi, OrderRequest& r);
    static u8 ReadKinds(u8 encoders, ReadStage stage);
    /// Every live command, of every kind.
    [[nodiscard]] OrderRequest Everything() const;

    std::unordered_map<const void*, Tags> buffers;
    u64 command{};
    u64 retired{};
};

} // namespace Metal
