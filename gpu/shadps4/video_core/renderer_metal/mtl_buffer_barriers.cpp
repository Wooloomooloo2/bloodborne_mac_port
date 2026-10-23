// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: which earlier commands an access to the guest arena must wait for
// (mtl_buffer_barriers.h).

#include <algorithm>
#include <bit>
#include <cstdlib>
#include "common/logging/log.h"
#include "bbport_stutter.h"
#include "video_core/renderer_metal/mtl_buffer_barriers.h"

namespace Metal {

namespace {
constexpr u64 PageSize = 1ull << BufferBarriers::PageBits;
constexpr u64 BlockSize = 1ull << BufferBarriers::BlockBits;
constexpr u64 ChunkSize = 1ull << BufferBarriers::ChunkBits;
constexpr u64 BlocksPerPage = PageSize / BlockSize;
static_assert(BlocksPerPage == 16);
} // namespace

void BufferBarriers::FreeDeleter::operator()(void* p) const noexcept {
    std::free(p);
}

void BufferBarriers::Register(const void* buffer, u64 size) {
    const u64 pages = (size + PageSize - 1) >> PageBits;
    const u64 chunks = (size + ChunkSize - 1) >> ChunkBits;
    Tags t;
    t.size = size;
    // calloc: zero (never accessed); arrays this large are fresh zero-filled pages the OS
    // commits only as accesses touch them (make_unique<T[]> would write every byte).
    const auto zero = []<typename T>(ZeroArray<T>& array, u64 count) {
        array.reset(static_cast<T*>(std::calloc(count, sizeof(T))));
    };
    zero(t.blocks, pages * BlocksPerPage);
    zero(t.page, pages);
    zero(t.summary, pages);
    zero(t.page_touch, pages);
    zero(t.chunk, chunks);
    zero(t.chunk_touch, chunks);
    zero(t.write_pages, (pages + 63) / 64);
    if (!t.blocks || !t.page || !t.summary || !t.page_touch || !t.chunk || !t.chunk_touch ||
        !t.write_pages) {
        LOG_ERROR(Render, "Buffer ordering: no memory for the access tags of a {:#x}-byte buffer; "
                          "every access to it waits for all work before it",
                  size);
        t = Tags{};
        t.size = size;
        t.conservative = true;
    }
    buffers[buffer] = std::move(t);
}

OrderRequest BufferBarriers::Everything() const {
    OrderRequest r;
    if (command > retired) {
        for (CommandSet* set : {&r.render_end, &r.copy, &r.compute}) {
            set->window = {retired + 1, command};
        }
        r.ahead = true;
    }
    return r;
}

void BufferBarriers::AddWrites(u8 kinds, u64 lo, u64 hi, OrderRequest& r) {
    if (kinds & EncoderRender) {
        r.render_end.Add(lo, hi);
    }
    if (kinds & EncoderCopy) {
        r.copy.Add(lo, hi);
    }
    if (kinds & EncoderCompute) {
        r.compute.Add(lo, hi);
    }
    r.ahead |= (kinds & EncoderAhead) != 0;
}

void BufferBarriers::AddReads(u8 kinds, u64 lo, u64 hi, OrderRequest& r) {
    if (kinds & ReadRenderLate) {
        r.render_end.Add(lo, hi);
    } else if (kinds & ReadRenderVertex) {
        r.render_vertex.Add(lo, hi);
    }
    if (kinds & ReadCopy) {
        r.copy.Add(lo, hi);
    }
    if (kinds & ReadCompute) {
        r.compute.Add(lo, hi);
    }
    r.ahead |= (kinds & ReadAhead) != 0;
}

u8 BufferBarriers::ReadKinds(u8 encoders, ReadStage stage) {
    u8 kinds = 0;
    if (encoders & EncoderRender) {
        kinds |= stage == ReadStage::Vertex ? ReadRenderVertex : ReadRenderLate;
    }
    kinds |= (encoders & EncoderCopy) ? ReadCopy : 0;
    kinds |= (encoders & EncoderCompute) ? ReadCompute : 0;
    kinds |= (encoders & EncoderAhead) ? ReadAhead : 0;
    return kinds;
}

void BufferBarriers::Collect(const Block& b, bool write, OrderRequest& r) const {
    // RAW and WAW: the last write (the earlier ones and their readers are ordered before it).
    if (const u64 w = Expand(b.write); w > retired) {
        AddWrites(b.write_kinds, w, w, r);
    }
    if (!write || b.read_kinds == 0) {
        return; // RAR: nothing more
    }
    // WAR: the reads since, their ends (or vertex stages for render reads made only there).
    const u64 hi = Expand(b.read_hi);
    if (hi <= retired) {
        return;
    }
    u64 lo = Expand(b.read_lo);
    if (lo <= retired || lo > hi) {
        lo = retired + 1; // (a tag 2^32 commands old expands past `hi`: every live command)
    }
    AddReads(b.read_kinds, lo, hi, r);
}

void BufferBarriers::Collect(const Summary& s, bool write, OrderRequest& r) const {
    if (const u64 hi = Expand(s.write_hi); hi > retired) {
        u64 lo = Expand(s.write_lo);
        if (lo <= retired || lo > hi) {
            lo = retired + 1;
        }
        AddWrites(s.write_kinds, lo, hi, r);
    }
    if (!write || s.read_kinds == 0) {
        return;
    }
    if (const u64 hi = Expand(s.read_hi); hi > retired) {
        u64 lo = Expand(s.read_lo);
        if (lo <= retired || lo > hi) {
            lo = retired + 1;
        }
        AddReads(s.read_kinds, lo, hi, r);
    }
}

void BufferBarriers::MarkBlock(Block& b, bool write, u8 encoders, ReadStage stage) const {
    const u32 tag = u32(command);
    if (write) {
        if (b.write != tag) {
            // The write waited for the reads since the last one (WAR): they are ordered before
            // it, and a later conflict with it stands for them. Its own reads stay.
            if (b.read_hi == tag) {
                b.read_lo = tag;
            } else {
                b.read_lo = b.read_hi = 0;
                b.read_kinds = 0;
            }
            b.write = tag;
            b.write_kinds = 0;
        }
        b.write_kinds |= encoders;
        return;
    }
    if (b.read_kinds == 0 || !Live(b.read_hi)) {
        b.read_lo = tag;
        b.read_kinds = 0;
    }
    b.read_hi = tag;
    b.read_kinds |= ReadKinds(encoders, stage);
}

void BufferBarriers::MarkSummary(Summary& s, bool write, u8 encoders, ReadStage stage) const {
    const u32 tag = u32(command);
    if (write) {
        if (s.write_kinds == 0 || !Live(s.write_hi)) {
            s.write_lo = tag;
            s.write_kinds = 0;
        }
        s.write_hi = tag;
        s.write_kinds |= encoders;
        return;
    }
    if (s.read_kinds == 0 || !Live(s.read_hi)) {
        s.read_lo = tag;
        s.read_kinds = 0;
    }
    s.read_hi = tag;
    s.read_kinds |= ReadKinds(encoders, stage);
}

OrderRequest BufferBarriers::Conflicts(const void* buffer, u64 offset, u64 size,
                                       bool write) const {
    const auto it = buffers.find(buffer);
    if (it == buffers.end() || size == 0 || offset >= it->second.size) {
        return {};
    }
    const Tags& t = it->second;
    if (t.conservative) {
        return Everything();
    }
    OrderRequest r;
    if (!Live(t.touch)) {
        return r; // nothing since the last complete command buffer
    }
    const u64 end = std::min(offset + size, t.size);
    if (offset == 0 && end >= t.size) {
        return Everything(); // a whole-buffer access (direct memory access): no walk
    }
    Collect(t.whole, write, r);
    const u64 first_page = offset >> PageBits, last_page = (end - 1) >> PageBits;
    const auto collect_page = [&](u64 p) {
        Collect(t.page[p], write, r);
        const u64 page_begin = p << PageBits;
        if (offset <= page_begin && end >= page_begin + PageSize) {
            Collect(t.summary[p], write, r); // the whole page: its blocks together
            return;
        }
        const u64 b0 = (std::max(offset, page_begin) - page_begin) >> BlockBits;
        const u64 b1 = (std::min(end, page_begin + PageSize) - 1 - page_begin) >> BlockBits;
        const Block* blocks = &t.blocks[p * BlocksPerPage];
        for (u64 b = b0; b <= b1; ++b) {
            Collect(blocks[b], write, r);
        }
    };
    // bbport: a read waits only for writes (RAR orders nothing), and pages whose page and block
    // marks hold no write add nothing for it: visit the pages with a write bit, and clear the
    // bits of pages whose writes all retired. Exact; second toggle word bit 3 walks every page.
    const bool reads_by_index = !write && !BbStutter::BarrierWriteIndexOff();
    for (u64 chunk = offset >> ChunkBits; chunk <= (end - 1) >> ChunkBits; ++chunk) {
        if (!Live(t.chunk_touch[chunk])) {
            continue;
        }
        Collect(t.chunk[chunk], write, r);
        const u64 lo = std::max(first_page, chunk << (ChunkBits - PageBits));
        const u64 hi = std::min(last_page, ((chunk + 1) << (ChunkBits - PageBits)) - 1);
        if (reads_by_index) {
            for (u64 word = lo / 64; word <= hi / 64; ++word) {
                u64 bits = t.write_pages[word];
                while (bits) {
                    const u64 p = word * 64 + u64(std::countr_zero(bits));
                    bits &= bits - 1;
                    if (p < lo || p > hi) {
                        continue;
                    }
                    if (!Live(t.page[p].write) && !Live(t.summary[p].write_hi)) {
                        t.write_pages[word] &= ~(1ull << (p % 64)); // its writes retired
                        continue;
                    }
                    collect_page(p);
                }
            }
            continue;
        }
        for (u64 p = lo; p <= hi; ++p) {
            if (Live(t.page_touch[p])) {
                collect_page(p);
            }
        }
    }
    return r;
}

void BufferBarriers::Mark(const void* buffer, u64 offset, u64 size, bool write, u8 encoders,
                          ReadStage stage) {
    const auto it = buffers.find(buffer);
    if (it == buffers.end() || size == 0 || offset >= it->second.size || command == 0) {
        return;
    }
    Tags& t = it->second;
    if (t.conservative) {
        return; // every access waits for everything live
    }
    const u32 tag = u32(command);
    t.touch = tag;
    const u64 end = std::min(offset + size, t.size);
    if (offset == 0 && end >= t.size) {
        MarkBlock(t.whole, write, encoders, stage); // one tag, not a walk over every chunk
        return;
    }
    const u64 first_page = offset >> PageBits, last_page = (end - 1) >> PageBits;
    for (u64 chunk = offset >> ChunkBits; chunk <= (end - 1) >> ChunkBits; ++chunk) {
        t.chunk_touch[chunk] = tag;
        const u64 chunk_begin = chunk << ChunkBits;
        if (offset <= chunk_begin && end >= std::min(chunk_begin + ChunkSize, t.size)) {
            MarkBlock(t.chunk[chunk], write, encoders, stage);
            continue;
        }
        const u64 lo = std::max(first_page, chunk << (ChunkBits - PageBits));
        const u64 hi = std::min(last_page, ((chunk + 1) << (ChunkBits - PageBits)) - 1);
        for (u64 p = lo; p <= hi; ++p) {
            t.page_touch[p] = tag;
            if (write) {
                t.write_pages[p / 64] |= 1ull << (p % 64); // Conflicts' read index
            }
            const u64 page_begin = p << PageBits;
            if (offset <= page_begin && end >= page_begin + PageSize) {
                MarkBlock(t.page[p], write, encoders, stage); // the whole page: one tag
                continue;
            }
            const u64 b0 = (std::max(offset, page_begin) - page_begin) >> BlockBits;
            const u64 b1 = (std::min(end, page_begin + PageSize) - 1 - page_begin) >> BlockBits;
            Block* blocks = &t.blocks[p * BlocksPerPage];
            for (u64 b = b0; b <= b1; ++b) {
                MarkBlock(blocks[b], write, encoders, stage);
            }
            MarkSummary(t.summary[p], write, encoders, stage);
        }
    }
}

} // namespace Metal
