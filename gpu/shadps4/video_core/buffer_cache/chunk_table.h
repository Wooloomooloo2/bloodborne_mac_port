// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "common/types.h"

namespace VideoCore {

class Buffer;

/// bbport: the buffers that hold guest memory for the GPU ("chunks"), each at its own guest range
/// [start, end), sorted by address and never overlapping. Today's layout has one: the arena from
/// 0x7000000000 (BufferCache). Readers on any thread (binding, read-backs, the fault thread) look
/// up the current table without a lock; a publisher copies it, adds a chunk and publishes the copy
/// (release/acquire). A published table is never freed while the cache lives (a reader may still
/// hold it); there are a few per session.
class ChunkTable {
public:
    struct Chunk {
        VAddr start;
        VAddr end;
        const Buffer* buffer;
    };

    /// The chunk holding all of [address, address + size) (size 0 as 1 byte), or null.
    [[nodiscard]] const Chunk* Find(VAddr address, u64 size) const noexcept {
        const Table* table = current.load(std::memory_order_acquire);
        const u64 bytes = std::max<u64>(size, 1);
        if (!table || address + bytes < address) {
            return nullptr;
        }
        const auto& chunks = table->chunks;
        auto it = std::upper_bound(chunks.begin(), chunks.end(), address,
                                   [](VAddr a, const Chunk& c) { return a < c.start; });
        if (it == chunks.begin()) {
            return nullptr;
        }
        --it;
        return address + bytes <= it->end ? &*it : nullptr;
    }

    /// The chunk whose range contains `address`, or null.
    [[nodiscard]] const Chunk* FindContaining(VAddr address) const noexcept {
        return Find(address, 1);
    }

    /// The first chunk starting after `address`, or null.
    [[nodiscard]] const Chunk* NextAfter(VAddr address) const noexcept {
        const Table* table = current.load(std::memory_order_acquire);
        if (!table) {
            return nullptr;
        }
        const auto& chunks = table->chunks;
        const auto it = std::upper_bound(chunks.begin(), chunks.end(), address,
                                         [](VAddr a, const Chunk& c) { return a < c.start; });
        return it != chunks.end() ? &*it : nullptr;
    }

    /// Adds a chunk and publishes the new table. False (nothing changes) when it is empty or
    /// overlaps a chunk.
    bool Add(VAddr start, VAddr end, const Buffer* buffer) {
        if (end <= start) {
            return false;
        }
        std::scoped_lock lock{publish_mutex};
        const Table* old = current.load(std::memory_order_relaxed);
        auto table = std::make_unique<Table>();
        if (old) {
            table->chunks = old->chunks;
        }
        auto& chunks = table->chunks;
        const auto it = std::upper_bound(chunks.begin(), chunks.end(), start,
                                         [](VAddr a, const Chunk& c) { return a < c.start; });
        if ((it != chunks.end() && it->start < end) ||
            (it != chunks.begin() && std::prev(it)->end > start)) {
            return false;
        }
        chunks.insert(it, Chunk{start, end, buffer});
        current.store(table.get(), std::memory_order_release);
        tables.push_back(std::move(table));
        return true;
    }

    /// The current chunks, sorted (valid while the table lives: tables are never freed).
    [[nodiscard]] std::span<const Chunk> Chunks() const noexcept {
        const Table* table = current.load(std::memory_order_acquire);
        return table ? std::span<const Chunk>{table->chunks} : std::span<const Chunk>{};
    }

private:
    struct Table {
        std::vector<Chunk> chunks;
    };
    std::atomic<const Table*> current{nullptr};
    std::vector<std::unique_ptr<const Table>> tables; ///< every table published (publish_mutex)
    std::mutex publish_mutex;
};

} // namespace VideoCore
