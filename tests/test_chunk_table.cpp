// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// VideoCore::ChunkTable (the guest memory buffers' lookup): lookups, rejected overlaps, and one
// publisher adding chunks while readers look them up (build with -fsanitize=thread).
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "video_core/buffer_cache/chunk_table.h"

namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}
const VideoCore::Buffer* Tag(u64 i) {
    return reinterpret_cast<const VideoCore::Buffer*>(0x1000 * (i + 1));
}
} // namespace

int main() {
    using VideoCore::ChunkTable;
    constexpr VAddr Base = 0x7000000000ULL;
    constexpr u64 MiB = u64{1} << 20;
    {
        ChunkTable t;
        Check(t.Find(Base, 1) == nullptr && t.Chunks().empty(), "an empty table finds nothing");
        Check(t.Add(Base + 916 * MiB, Base + (916 + 3325) * MiB, Tag(0)), "a run is added");
        const VAddr s = Base + 916 * MiB, e = Base + (916 + 3325) * MiB;
        Check(t.Find(s, 1) && t.Find(s, 1)->buffer == Tag(0), "its first byte");
        Check(t.Find(e - 1, 1) != nullptr, "its last byte");
        Check(t.Find(e, 1) == nullptr && t.Find(s - 1, 1) == nullptr, "not the bytes around it");
        Check(t.Find(s, e - s) != nullptr && t.Find(s, e - s + 1) == nullptr,
              "a range ending at its end, not one past it");
        Check(t.Find(s, 0) != nullptr && t.Find(e, 0) == nullptr, "size 0 counts as one byte");
        Check(t.Find(~VAddr{0} - 4, 16) == nullptr, "a range wrapping the address space");
        Check(!t.Add(s - MiB, s + MiB, Tag(1)) && !t.Add(e - MiB, e + MiB, Tag(1)) &&
                  !t.Add(s + MiB, s + 2 * MiB, Tag(1)) && !t.Add(s, s, Tag(1)),
              "overlapping and empty chunks are refused");
        Check(t.Add(e, e + 13 * MiB, Tag(2)), "an adjacent chunk is added");
        Check(t.Find(e - 1, 2) == nullptr, "a range across two chunks is not in one");
        Check(t.Add(Base, Base + MiB, Tag(3)), "a chunk below is added");
        const auto chunks = t.Chunks();
        Check(chunks.size() == 3 && chunks[0].buffer == Tag(3) && chunks[1].buffer == Tag(0) &&
                  chunks[2].buffer == Tag(2),
              "the chunks stay sorted");
    }
    {
        // One publisher, four readers: every chunk a reader finds is the right one, and every
        // chunk becomes visible.
        ChunkTable t;
        constexpr u64 N = 512;
        std::atomic<bool> done{false};
        std::atomic<u64> wrong{0}, seen_last{0};
        std::vector<std::thread> readers;
        for (int r = 0; r < 4; ++r) {
            readers.emplace_back([&, r] {
                u64 i = r;
                while (!done.load(std::memory_order_acquire)) {
                    i = (i * 6364136223846793005ULL + 1442695040888963407ULL);
                    const u64 k = (i >> 33) % N;
                    if (const auto* c = t.Find(Base + k * 2 * MiB + 1234, 4096)) {
                        if (c->buffer != Tag(k) || c->start != Base + k * 2 * MiB) {
                            wrong.fetch_add(1);
                        }
                    }
                    for (const auto& c : t.Chunks()) {
                        if (c.end - c.start != MiB) {
                            wrong.fetch_add(1);
                        }
                    }
                }
                if (t.Find(Base + (N - 1) * 2 * MiB, 1)) {
                    seen_last.fetch_add(1);
                }
            });
        }
        for (u64 k = 0; k < N; ++k) {
            const u64 slot = (k * 7) % N; // out of order
            t.Add(Base + slot * 2 * MiB, Base + slot * 2 * MiB + MiB, Tag(slot));
        }
        done.store(true, std::memory_order_release);
        for (auto& th : readers) {
            th.join();
        }
        Check(wrong.load() == 0, "concurrent readers only find the right chunks");
        Check(seen_last.load() == 4 && t.Chunks().size() == N, "every chunk is visible at the end");
    }
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
