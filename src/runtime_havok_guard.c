// SPDX-License-Identifier: GPL-2.0-or-later
/* 1.09's Havok buffer upload borrows a graphics context, but its validation and
 * mapping failures skip the return-to-pool tail. Enough failures strand all
 * callers in the yield loop at 0x1067fc0 (the stack in #110). Return only a new
 * borrow left by a failed call; keep its failure result and the successful path.
 * The report does not establish which operation failed in the Rom transition.
 */
#define _GNU_SOURCE
#include "runtime.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#if defined(__x86_64__)
enum { UPLOAD = 0x1067f60, GET_POOL = 0x1057350, GIVE = 0x1056990,
       POOL_SLOT = 0x54b8788, ACTIVE = 0x120, MOVED_BYTES = 20 };
static const unsigned char entry[] = {
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x18,0x49,0x89,0xfe
};
static const struct {
    uint64_t offset;
    unsigned size;
    unsigned char bytes[32];
} checks[] = {
    {GET_POOL,8,{0x48,0x8b,0x05,0x31,0x14,0x46,0x04,0xc3}},
    {0x1067f82,8,{0xe8,0xc9,0xf3,0xfe,0xff,0x49,0x89,0xc4}},
    /* Borrow, the two failure exits, and the original successful release. */
    {0x1067fc5,20,{0x4c,0x89,0xe7,0xe8,0x53,0xe9,0xfe,0xff,0x49,0x89,0x86,0x20,0x01,0x00,0x00,0x48,0x85,0xc0,0x74,0xe7}},
    {0x1067fff,25,{0x85,0xc0,0x74,0x15,0x48,0x8b,0x1d,0xd6,0xbb,0x37,0x04,0xe9,0x83,0x00,0x00,0x00,0x48,0x8b,0x1d,0xca,0xbb,0x37,0x04,0xeb,0x7a}},
    {0x1068031,16,{0xe8,0xfa,0x1b,0x10,0x01,0x85,0xc0,0x48,0x8b,0x1d,0xa1,0xbb,0x37,0x04,0x75,0x51}},
    {0x1068075,29,{0x49,0x8b,0xb6,0x20,0x01,0x00,0x00,0x4c,0x89,0xe7,0xe8,0x0c,0xe9,0xfe,0xff,0x49,0xc7,0x86,0x20,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x45,0x31,0xff}},
    {GIVE,14,{0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x50,0x49,0x89,0xff,0x41}},
    {0x10569d8,31,{0x49,0x8b,0x47,0x50,0xc7,0x04,0x98,0x01,0x00,0x00,0x00,0x49,0x8b,0x47,0x70,0x4c,0x89,0xf7,0x48,0x83,0xc4,0x08,0x5b,0x41,0x5e,0x41,0x5f,0x5d,0xff,0x60,0x28}},
};
static uint32_t (ABI *upload)(void *owner);
static void *(ABI *get_pool)(void);
static void (ABI *give)(void *pool, void *node);
static _Atomic uint64_t recovered;

static uint32_t ABI guarded_upload(void *owner) {
    void *before, *after;
    /* The original getter is a pure load of the process-wide pool. Capture the
     * pool before the call and refuse cleanup if its identity changed. */
    void *pool = get_pool();
    if (!pool) return upload(owner);
    memcpy(&before, (unsigned char *)owner + ACTIVE, sizeof(before));
    uint32_t result = upload(owner);
    memcpy(&after, (unsigned char *)owner + ACTIVE, sizeof(after));
    if (result == 1 && !before && after && get_pool() == pool) {
        give(pool, after); /* the game's synchronized return-to-pool operation */
        memset((unsigned char *)owner + ACTIVE, 0, sizeof(after));
        uint64_t count = atomic_fetch_add_explicit(&recovered, 1, memory_order_relaxed) + 1;
        if (count <= 8)
            fprintf(stderr, "Havok upload: returned a context after failure, owner %p, occurrence %" PRIu64 "\n",
                    owner, count);
    }
    return result;
}

static void put_jump(unsigned char *p, uintptr_t target) {
    static const unsigned char op[] = {0xff,0x25,0,0,0,0};
    memcpy(p, op, sizeof(op));
    memcpy(p + sizeof(op), &target, sizeof(target));
}

int runtime_install_havok_guard(unsigned char *image, uint64_t size) {
    const char *setting = getenv("BB_HAVOK_POOL_GUARD");
    if (setting && setting[0] == '0') {
        printf("Havok upload: pool guard off (BB_HAVOK_POOL_GUARD=0)\n");
        return 0;
    }
    if (!image || size < POOL_SLOT + sizeof(void *) || memcmp(image + UPLOAD, entry, sizeof(entry)))
        goto mismatch;
    for (unsigned i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i)
        if (size < checks[i].offset + checks[i].size ||
            memcmp(image + checks[i].offset, checks[i].bytes, checks[i].size))
            goto mismatch;
    unsigned char *stub = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stub == MAP_FAILED) return 0;
    /* These complete prologue instructions have no PC-relative operands. */
    memcpy(stub, image + UPLOAD, MOVED_BYTES);
    put_jump(stub + MOVED_BYTES, (uintptr_t)(image + UPLOAD + MOVED_BYTES));
    if (mprotect(stub, 4096, PROT_READ | PROT_EXEC)) {
        munmap(stub, 4096);
        return 0;
    }
    upload = (uint32_t (ABI *)(void *))stub;
    get_pool = (void *(ABI *)(void))(image + GET_POOL);
    give = (void (ABI *)(void *, void *))(image + GIVE);
    put_jump(image + UPLOAD, (uintptr_t)guarded_upload);
    memset(image + UPLOAD + 14, 0xcc, MOVED_BYTES - 14);
    printf("Havok upload: failed-borrow cleanup (0x1067f60) hooked\n");
    return 1;
mismatch:
    printf("Havok upload: pool path does not match Bloodborne 1.09; not hooked\n");
    return 0;
}
#else
int runtime_install_havok_guard(unsigned char *image, uint64_t size) {
    (void)image;
    (void)size;
    return 0;
}
#endif
