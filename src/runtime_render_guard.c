// SPDX-License-Identifier: GPL-2.0-or-later
/* #99: a deferred graphics bind can reach 1.09's resource dispatcher with RDX=NULL.
 * Its identity accessor returns that pointer, then 0x2169edc dereferences it. Treat
 * the absent record like the dispatcher's existing unsupported-type branch: no
 * binding update, but still run its context-finalization tail. This contains that
 * exact fault; it does not recreate the missing resource or suppress other faults.
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
       IDENTITY_OFFSET = 0x26a94c0,
       MOVED_BYTES = 17 };
static const unsigned char bind_signature[] = {
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x50,0x49,0x89,0xff,0x4d,0x8d,0x77,0x10,
    0x48,0x89,0xd7,0xe8,0xe7,0xf5,0x53,0x00,0x48,0x89,0xc3,0x8b,0x03,0x83,0xf8,0x01
};
static const unsigned char finish_signature[] = {
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x58
};
static const unsigned char identity_signature[] = {0x48,0x89,0xf8,0xc3};
static const unsigned char tail_signature[] = {
    0x4c,0x89,0xf7,0x48,0x83,0xc4,0x08,0x5b,0x41,0x5e,0x41,0x5f,0x5d,0xe9,0xda,0x56,0x54,0x00
};
static uint64_t (ABI *finish_context)(void *context);
static _Atomic uint64_t null_binds;

static uint64_t ABI finish_null_bind(void *context) {
    uint64_t count = atomic_fetch_add_explicit(&null_binds, 1, memory_order_relaxed) + 1;
    if (count <= 8)
        fprintf(stderr, "Render guard: absent shader bind record (#99), context %p, occurrence %" PRIu64 "\n",
                context, count);
    return finish_context((unsigned char *)context + 0x10);
}

static unsigned char *put_jump(unsigned char *at, uint64_t address) {
    static const unsigned char jump[] = {0xff,0x25,0,0,0,0};
    memcpy(at, jump, sizeof(jump));
    memcpy(at + sizeof(jump), &address, sizeof(address));
    return at + sizeof(jump) + sizeof(address);
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
        printf("Render guard: shader bind does not match Bloodborne 1.09; not hooked\n");
        return 0;
    }
    unsigned char *stub = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stub == MAP_FAILED) {
        fprintf(stderr, "Render guard: no stub page; not hooked\n");
        return 0;
    }
    unsigned char *p = stub;
    /* test rdx,rdx; jnz past the absolute null-handler jump. No registers, stack
     * arguments or original instructions change on the non-null path. */
    const unsigned char check[] = {0x48,0x85,0xd2,0x75,14};
    memcpy(p, check, sizeof(check));
    p += sizeof(check);
    p = put_jump(p, (uint64_t)(uintptr_t)finish_null_bind);
    memcpy(p, image + BIND_OFFSET, MOVED_BYTES);
    p += MOVED_BYTES;
    put_jump(p, (uint64_t)(uintptr_t)(image + BIND_OFFSET + MOVED_BYTES));
    if (mprotect(stub, 4096, PROT_READ | PROT_EXEC)) {
        munmap(stub, 4096);
        fprintf(stderr, "Render guard: stub not executable; not hooked\n");
        return 0;
    }
    finish_context = (uint64_t (ABI *)(void *))(image + FINISH_OFFSET);
    put_jump(image + BIND_OFFSET, (uint64_t)(uintptr_t)stub);
    memset(image + BIND_OFFSET + 14, 0xcc, MOVED_BYTES - 14);
    printf("Render guard: null shader bind (0x2169ec0) hooked\n");
    return 1;
}
#else
int runtime_install_render_guard(unsigned char *image, uint64_t image_size) {
    (void)image;
    (void)image_size;
    return 0;
}
#endif
