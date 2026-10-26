// SPDX-License-Identifier: GPL-2.0-or-later
/* #99/#113: a deferred graphics bind and its matching unbind can reach 1.09's
 * resource dispatchers with RDX=NULL. Their identity accessor returns that pointer,
 * then 0x2169edc / 0x216a08c dereferences it. Treat an absent record like each
 * dispatcher's existing unsupported-type branch: no binding update, but still run
 * its context-finalization tail. A later query uses its original unsupported-type
 * error path, retaining the optional flush. This does not recreate the resource.
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
enum { BIND_OFFSET = 0x2169ec0, FINISH_OFFSET = 0x26af700, TAIL_OFFSET = 0x216a014,
       UNBIND_OFFSET = 0x216a070, UNBIND_TAIL_OFFSET = 0x216a298,
       QUERY_OFFSET = 0x216a2f0, QUERY_TAIL_OFFSET = 0x216a717, QUERY_MOVED_BYTES = 16,
       IDENTITY_OFFSET = 0x26a94c0,
       MOVED_BYTES = 17 };
static const unsigned char bind_signature[] = {
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x50,0x49,0x89,0xff,0x4d,0x8d,0x77,0x10,
    0x48,0x89,0xd7,0xe8,0xe7,0xf5,0x53,0x00,0x48,0x89,0xc3,0x8b,0x03,0x83,0xf8,0x01
};
static const unsigned char unbind_signature[] = {
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x50,0x48,0x89,0xfb,0x4c,0x8d,0x73,0x10,
    0x48,0x89,0xd7,0xe8,0x37,0xf4,0x53,0x00,0x49,0x89,0xc7,0x41,0x8b,0x07,0x83,0xf8,0x01
};
static const unsigned char finish_signature[] = {
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x58
};
static const unsigned char identity_signature[] = {0x48,0x89,0xf8,0xc3};
static const unsigned char tail_signature[] = {
    0x4c,0x89,0xf7,0x48,0x83,0xc4,0x08,0x5b,0x41,0x5e,0x41,0x5f,0x5d,0xe9,0xda,0x56,0x54,0x00
};
static const unsigned char unbind_tail_signature[] = {
    0x4c,0x89,0xf7,0x48,0x83,0xc4,0x08,0x5b,0x41,0x5e,0x41,0x5f,0x5d,0xe9,0x56,0x54,0x54,0x00
};
static const unsigned char query_signature[] = {
    0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x49,0x89,0xce,0x48,0x89,0xd3,0x45,0x84,0xc9,
    0x74,0x09,0x48,0x83,0xc7,0x10,0xe8,0xf5,0x56,0x54,0x00,0x48,0x89,0xdf,0xe8,0xad,0xf1,0x53,0x00,
    0x48,0x89,0xc1,0xb8,0x07,0x00,0x00,0x00,0x8b,0x51,0x04,0x48,0x83,0xfa,0x0f,0x0f,0x87,0xef,0x03,0x00,0x00
};
static const unsigned char query_tail_signature[] = {0x5b,0x41,0x5e,0x5d,0xc3};
typedef uint32_t (ABI *Query)(void *, void *, const void *, void *, uint32_t, uint8_t);
static Query original_query;
static uint64_t (ABI *finish_context)(void *context);
static _Atomic uint64_t null_binds;
static _Atomic uint64_t null_unbinds;
static _Atomic uint64_t null_queries;

static uint64_t ABI finish_null_bind(void *context) {
    uint64_t count = atomic_fetch_add_explicit(&null_binds, 1, memory_order_relaxed) + 1;
    if (count <= 8)
        fprintf(stderr, "Render guard: absent shader bind record (#99), context %p, occurrence %" PRIu64 "\n",
                context, count);
    return finish_context((unsigned char *)context + 0x10);
}

static uint64_t ABI finish_null_unbind(void *context) {
    uint64_t count = atomic_fetch_add_explicit(&null_unbinds, 1, memory_order_relaxed) + 1;
    if (count <= 8)
        fprintf(stderr, "Render guard: absent shader unbind record (#113), context %p, occurrence %" PRIu64 "\n",
                context, count);
    return finish_context((unsigned char *)context + 0x10);
}

static uint32_t ABI reject_null_query(void *context, void *scratch, const void *record,
                                      void *out, uint32_t flags, uint8_t flush) {
    (void)record;
    uint64_t count = atomic_fetch_add_explicit(&null_queries, 1, memory_order_relaxed) + 1;
    if (count <= 8)
        fprintf(stderr, "Render guard: absent shader query record (#113), context %p, occurrence %" PRIu64 "\n",
                context, count);
    /* The checked query rejects type 16 with error 7 before reading any payload
     * or writing out. Running that original path also preserves its optional
     * context flush; this sentinel never represents a valid graphics resource. */
    static const uint32_t unsupported[] = {0,16};
    return original_query(context, scratch, unsupported, out, flags, flush);
}

static unsigned char *put_jump(unsigned char *at, uint64_t address) {
    static const unsigned char jump[] = {0xff,0x25,0,0,0,0};
    memcpy(at, jump, sizeof(jump));
    memcpy(at + sizeof(jump), &address, sizeof(address));
    return at + sizeof(jump) + sizeof(address);
}

static unsigned char *make_stub(unsigned char *p, unsigned char *image, uint64_t offset,
                                unsigned moved, uintptr_t handler) {
    /* test rdx,rdx; jnz past the absolute null-handler jump. The non-null path
     * keeps all incoming registers, stack arguments and original instructions. */
    const unsigned char check[] = {0x48,0x85,0xd2,0x75,14};
    memcpy(p, check, sizeof(check));
    p += sizeof(check);
    p = put_jump(p, handler);
    memcpy(p, image + offset, moved);
    put_jump(p + moved, (uintptr_t)(image + offset + moved));
    return p;
}

int runtime_install_render_guard(unsigned char *image, uint64_t image_size) {
    const char *setting = getenv("BB_NULL_BIND_GUARD");
    if (setting && setting[0] == '0') {
        printf("Render guard: off (BB_NULL_BIND_GUARD=0)\n");
        return 0;
    }
    if (!image || image_size < FINISH_OFFSET + sizeof(finish_signature) ||
        memcmp(image + BIND_OFFSET, bind_signature, sizeof(bind_signature)) ||
        memcmp(image + TAIL_OFFSET, tail_signature, sizeof(tail_signature)) ||
        memcmp(image + IDENTITY_OFFSET, identity_signature, sizeof(identity_signature)) ||
        memcmp(image + FINISH_OFFSET, finish_signature, sizeof(finish_signature))) {
        printf("Render guard: shader bind/unbind does not match Bloodborne 1.09; not hooked\n");
        return 0;
    }
    /* A mod of the unbind must not disable the existing #99 protection. */
    int unbind_matches = !memcmp(image + UNBIND_OFFSET, unbind_signature, sizeof(unbind_signature)) &&
                         !memcmp(image + UNBIND_TAIL_OFFSET, unbind_tail_signature, sizeof(unbind_tail_signature));
    int query_matches = !memcmp(image + QUERY_OFFSET, query_signature, sizeof(query_signature)) &&
                        !memcmp(image + QUERY_TAIL_OFFSET, query_tail_signature, sizeof(query_tail_signature));
    unsigned char *stub = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stub == MAP_FAILED) {
        fprintf(stderr, "Render guard: no stub page; not hooked\n");
        return 0;
    }
    make_stub(stub, image, BIND_OFFSET, MOVED_BYTES, (uintptr_t)finish_null_bind);
    if (unbind_matches)
        make_stub(stub + 64, image, UNBIND_OFFSET, MOVED_BYTES, (uintptr_t)finish_null_unbind);
    if (query_matches)
        original_query = (Query)make_stub(stub + 128, image, QUERY_OFFSET, QUERY_MOVED_BYTES, (uintptr_t)reject_null_query);
    if (mprotect(stub, 4096, PROT_READ | PROT_EXEC)) {
        munmap(stub, 4096);
        fprintf(stderr, "Render guard: stub not executable; not hooked\n");
        return 0;
    }
    finish_context = (uint64_t (ABI *)(void *))(image + FINISH_OFFSET);
    put_jump(image + BIND_OFFSET, (uint64_t)(uintptr_t)stub);
    memset(image + BIND_OFFSET + 14, 0xcc, MOVED_BYTES - 14);
    printf("Render guard: null shader bind (0x2169ec0) hooked\n");
    if (unbind_matches) {
        put_jump(image + UNBIND_OFFSET, (uintptr_t)(stub + 64));
        memset(image + UNBIND_OFFSET + 14, 0xcc, MOVED_BYTES - 14);
        printf("Render guard: null shader unbind (0x216a070) hooked\n");
    } else {
        printf("Render guard: shader unbind does not match Bloodborne 1.09; unbind not hooked\n");
    }
    if (query_matches) {
        put_jump(image + QUERY_OFFSET, (uintptr_t)(stub + 128));
        memset(image + QUERY_OFFSET + 14, 0xcc, QUERY_MOVED_BYTES - 14);
        printf("Render guard: null shader query (0x216a2f0) hooked\n");
    } else {
        printf("Render guard: shader query does not match Bloodborne 1.09; query not hooked\n");
    }
    return 1;
}
#else
int runtime_install_render_guard(unsigned char *image, uint64_t image_size) {
    (void)image;
    (void)image_size;
    return 0;
}
#endif
