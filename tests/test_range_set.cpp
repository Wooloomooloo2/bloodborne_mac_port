// SPDX-FileCopyrightText: Copyright 2026 adurham
// SPDX-License-Identifier: GPL-2.0-or-later
// VideoCore::RangeSet (the buffer cache's GPU-modified ranges and the rasterizer's mapped ranges)
// against a byte map: random Add/Subtract sequences, then Contains, Intersects, ForEachInRange and
// ForEachNotInRange over random windows. The Boost.ICL interval_set it replaced left overlapping
// intervals behind after Add() on libc++ 22 / Apple's libc++ (std::set::lower_bound/upper_bound with
// a key overlapping several elements), which dropped GPU-written bytes from the tracker.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "video_core/buffer_cache/range_set.h"

namespace {
int failures = 0;
void Check(bool ok, const char* message) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", message);
    failures += !ok;
}
} // namespace

int main() {
    using VideoCore::RangeSet;
    constexpr VAddr Base = 0x1000;
    constexpr VAddr Span = 3000;
    long structure_errors = 0, coverage_errors = 0, query_errors = 0;
    std::srand(5);
    for (int round = 0; round < 5000; ++round) {
        RangeSet set;
        std::vector<char> bytes(Span, 0);
        for (int step = 0; step < 50; ++step) {
            const VAddr start = std::rand() % (Span - 8);
            const VAddr size = std::min<VAddr>(1 + std::rand() % 400, Span - start);
            const bool add = std::rand() % 3 != 0;
            if (add) {
                set.Add(Base + start, size);
            } else {
                set.Subtract(Base + start, size);
            }
            std::fill(bytes.begin() + start, bytes.begin() + start + size, add ? 1 : 0);
        }
        // Ordered, disjoint, non-adjacent, and exactly the bytes that were added and not subtracted.
        VAddr previous_end = 0;
        VAddr covered = 0;
        bool first = true;
        set.ForEach([&](VAddr start, VAddr end) {
            structure_errors += end <= start || (!first && start <= previous_end);
            previous_end = end;
            covered += end - start;
            first = false;
        });
        coverage_errors += covered != static_cast<VAddr>(std::count(bytes.begin(), bytes.end(), 1));
        for (int query = 0; query < 30; ++query) {
            const VAddr start = std::rand() % (Span - 8);
            const VAddr size = std::min<VAddr>(1 + std::rand() % 300, Span - start);
            bool all = true, any = false;
            for (VAddr i = start; i < start + size; ++i) {
                any |= bytes[i] != 0;
                all &= bytes[i] != 0;
            }
            query_errors += set.Contains(Base + start, size) != all;
            query_errors += set.Intersects(Base + start, size) != any;
            std::vector<char> in(Span, 0), out(Span, 0);
            set.ForEachInRange(Base + start, size, [&](VAddr s, VAddr e) {
                query_errors += s < Base + start || e > Base + start + size;
                for (VAddr i = s; i < e; ++i) {
                    in[i - Base] = 1;
                }
            });
            set.ForEachNotInRange(Base + start, size, [&](VAddr s, size_t n) {
                for (VAddr i = s; i < s + n; ++i) {
                    out[i - Base] = 1;
                }
            });
            for (VAddr i = start; i < start + size; ++i) {
                query_errors += (in[i] != 0) != (bytes[i] != 0);
                query_errors += (out[i] != 0) == (bytes[i] != 0);
            }
        }
    }
    Check(structure_errors == 0, "ranges stay ordered, disjoint and non-adjacent");
    Check(coverage_errors == 0, "coverage equals the byte map");
    Check(query_errors == 0, "Contains, Intersects, ForEachInRange and ForEachNotInRange match the byte map");
    {
        RangeSet set;
        set.Add(0x1000, 0x100);
        set.Add(0x1200, 0x100);
        set.Add(0x1400, 0x100);
        set.Add(0x1000, 0x500); // one Add spanning three ranges merges all of them
        int count = 0;
        set.ForEach([&](VAddr, VAddr) { ++count; });
        Check(count == 1 && set.Contains(0x1000, 0x500), "an Add spanning several ranges merges them");
        set.Subtract(0x1100, 0x300);
        count = 0;
        set.ForEach([&](VAddr, VAddr) { ++count; });
        Check(count == 2 && !set.Intersects(0x1100, 0x300) && set.Contains(0x1000, 0x100) &&
                  set.Contains(0x1400, 0x100),
              "a Subtract inside a range splits it");
    }
    return failures != 0;
}
