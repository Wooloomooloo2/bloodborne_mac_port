// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <deque>
#include <limits>
#include "bbport_upload_reservation.h"

int main() {
    constexpr uint64_t block = 65536, capacity = 4 * block;
    uint64_t position = 0, retired = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const auto r = BbUpload::Reserve(position, retired, block, 256, capacity);
        assert(r && r->offset == i * block);
        position = r->end;
    }
    assert(!BbUpload::Reserve(position, retired, block, 256, capacity));
    retired = block;
    const auto reused = BbUpload::Reserve(position, retired, block, 256, capacity);
    assert(reused && reused->offset == 0 && reused->end == capacity + block);
    for (uint64_t size : {0ULL, capacity + 1})
        assert(!BbUpload::Reserve(0, 0, size, 256, capacity));
    for (uint64_t alignment : {0ULL, 3ULL, capacity * 2})
        assert(!BbUpload::Reserve(0, 0, block, alignment, capacity));
    assert(!BbUpload::Reserve(0, 1, 1, 1, capacity));
    assert(!BbUpload::Reserve(std::numeric_limits<uint64_t>::max() - 3,
                             std::numeric_limits<uint64_t>::max() - 3, 8, 256, capacity));

    // Independent live-region oracle: no accepted physical range may overlap
    // any allocation whose simulated GPU submission has not completed.
    struct Live { uint64_t offset, size, end; };
    std::deque<Live> live;
    position = retired = 0;
    uint32_t random = 0x625b0147;
    for (unsigned i = 0; i < 20000; ++i) {
        random = random * 1664525 + 1013904223;
        if (!live.empty() && ((random >> 24) & 3) == 0) {
            retired = live.front().end; live.pop_front();
        }
        const uint64_t size = 1 + (random % block);
        const auto r = BbUpload::Reserve(position, retired, size, 256, capacity);
        if (!r) continue;
        assert(r->offset % 256 == 0 && r->offset + size <= capacity && r->end > position);
        for (const auto& item : live)
            assert(r->offset + size <= item.offset || item.offset + item.size <= r->offset);
        live.push_back({r->offset, size, r->end});
        position = r->end;
    }
    puts("PASS: 64 KiB uploads, completion-gated reuse and 20000 live-region checks");
}
