// SPDX-License-Identifier: GPL-2.0-or-later
/* Synthetic addresses only: no game image or private game bytes are required. */
#include "runtime_hang_list.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint64_t image = 0x800000000ull, owner = 0x7000000000ull, first = 0x7100000000ull;
static unsigned char signature[] = {0x4d,0x8b,0x7f,0x10,0x4d,0x39,0xe7,0x75,0xa5};
static unsigned char header[0x30], nodes[65][0x28];
static unsigned reads;
static uint64_t deny;
static void put64(void *p, uint64_t v) { memcpy(p, &v, sizeof(v)); }
static void put32(void *p, uint32_t v) { memcpy(p, &v, sizeof(v)); }
static uint64_t address(unsigned i) { return first + i * 0x28; }
static int read_memory(void *ctx, uint64_t at, void *out, size_t size) {
    (void)ctx;
    ++reads;
    assert(reads <= 67); /* signature + header + at most 64 nodes, including a failing read */
    if (at == deny) { memset(out, 0xa5, size); return 0; }
    const void *source = NULL;
    if (at == image + 0x24a41b2 && size == sizeof(signature)) source = signature;
    if (at == owner + 0x68 && size == sizeof(header)) source = header;
    for (unsigned i = 0; i < 65; ++i)
        if (at == address(i) && size == 0x24) source = nodes[i];
    if (!source) return 0;
    memcpy(out, source, size);
    return 1;
}
static void init(unsigned count) {
    memset(header, 0, sizeof(header)); memset(nodes, 0, sizeof(nodes));
    put64(header, count ? first : owner + 0x58); put64(header + 0x18, first);
    put32(header + 0x28, 2); header[0x2c] = 4;
    for (unsigned i = 0; i < count; ++i) {
        put64(nodes[i] + 0x10, i + 1 < count ? address(i + 1) : owner + 0x58);
        put64(nodes[i] + 0x18, i ? address(i - 1) : owner + 0x58);
        put32(nodes[i] + 0x20, 0x1000001u + i);
    }
    reads = 0; deny = 0;
}
static RuntimeHangList capture(uint64_t pc, uint64_t o, uint64_t h, uint64_t cur) {
    RuntimeHangList result = {0}; reads = 0;
    runtime_hang_list_capture(&result, image, 0x2500000, pc, o, h, cur, read_memory, NULL);
    return result;
}
static RuntimeHangList collect(uint64_t current) {
    return capture(image + 0x24a41b6, owner, owner + 0x58, current);
}
int main(void) {
    init(3);
    unsigned char before[sizeof(nodes)]; memcpy(before, nodes, sizeof(before));
    RuntimeHangList s = collect(first);
    assert(s.status == HANG_LIST_HEAD && s.count == 3 && s.stopped_at == owner + 0x58);
    assert(s.first == first && s.cursor == first && s.generation == 2 && s.flags == 4);
    assert(s.nodes[1].previous == first && s.nodes[2].flags == 0x1000003);
    assert(!memcmp(before, nodes, sizeof(before)));
    s = collect(owner + 0x58); assert(s.status == HANG_LIST_HEAD && !s.count);
    put64(nodes[2] + 0x10, address(1));
    s = collect(first);
    assert(s.status == HANG_LIST_CYCLE && s.count == 3 && s.repeated == 1 && s.stopped_at == address(1));
    put64(nodes[0] + 0x10, first);
    s = collect(first); assert(s.status == HANG_LIST_CYCLE && s.count == 1 && !s.repeated);
    init(64); s = collect(first); assert(s.status == HANG_LIST_HEAD && s.count == 64);
    put64(nodes[63] + 0x10, first);
    s = collect(first); assert(s.status == HANG_LIST_CYCLE && s.count == 64);
    init(65); s = collect(first); assert(s.status == HANG_LIST_LIMIT && s.count == 64);
    assert(reads == 66);
    puts("PASS: terminating walks, self/subcycles and exact bounds; traversal never writes nodes");

    init(3); deny = address(1); s = collect(first);
    assert(s.status == HANG_LIST_UNREADABLE && s.count == 1 && s.stopped_at == deny);
    deny = owner + 0x68; s = collect(first);
    assert(s.status == HANG_LIST_UNREADABLE && !s.count && s.stopped_at == deny);
    deny = 0;
    const uint64_t bad[] = {0, 1, first + 1, UINT64_MAX - 7};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        s = collect(bad[i]); assert(s.status == HANG_LIST_UNREADABLE && !s.count);
    }
    s = capture(image + 0x24a41b6, owner, owner + 0x60, first);
    assert(s.status == HANG_LIST_BAD_REGISTERS && reads == 1);
    s = capture(image + 0x24a41b6, UINT64_MAX - 7, 0x50, first);
    assert(s.status == HANG_LIST_BAD_REGISTERS);
    puts("PASS: unreadable, unaligned and overflowing addresses remain explicit failures");

    init(1);
    for (unsigned i = 0; i < sizeof(signature); ++i) {
        signature[i] ^= 1; s = collect(first);
        assert(s.status == HANG_LIST_NONE && reads == 1); signature[i] ^= 1;
    }
    deny = image + 0x24a41b2; s = collect(first);
    assert(s.status == HANG_LIST_NONE && reads == 1); deny = 0;
    const uint64_t unrelated[] = {image - 1, image + 0x24a415f, image + 0x24a41bb};
    for (unsigned i = 0; i < sizeof(unrelated)/sizeof(unrelated[0]); ++i) {
        s = capture(unrelated[i], owner, owner + 0x58, first);
        assert(s.status == HANG_LIST_NONE && !reads);
    }
    s = (RuntimeHangList){0}; reads = 0;
    runtime_hang_list_capture(&s, image, 0x24a41ba, image + 0x24a41b6, owner,
                              owner + 0x58, first, read_memory, NULL);
    assert(s.status == HANG_LIST_NONE && !reads);
    runtime_hang_list_capture(&s, UINT64_MAX - 10, 0x2500000, 0, owner,
                              owner + 0x58, first, read_memory, NULL);
    assert(s.status == HANG_LIST_NONE && !reads);
    puts("PASS: unrelated PCs, short images and every signature mismatch skip layout decoding");
}
