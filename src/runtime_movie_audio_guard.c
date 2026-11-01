// SPDX-License-Identifier: GPL-2.0-or-later
/* #123: 1.09's movie-audio start/stop dereference a missing output device.
 * Return the original failed-start/already-closed result before that load, so
 * their callers still release their locks. This does not create an audio device
 * or explain why initialization left it absent. Non-null devices run unchanged.
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
enum { START = 0x2822790, STOP = 0x2822750, START_MOVED = 20, STOP_MOVED = 16 };
static const unsigned char start_signature[] = {
    0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x50,0x48,0x89,0xfb,
    0x80,0xbb,0xc4,0x00,0x01,0x00,0x00,0x74,0x08,0x45,0x31,0xff,
    0xe9,0xcb,0x00,0x00,0x00,0x89,0x53,0x08,0x89,0x4b,0x0c
};
static const unsigned char stop_signature[] = {
    0x55,0x48,0x89,0xe5,0x53,0x50,0x48,0x89,0xfb,
    0x80,0xbb,0xc4,0x00,0x01,0x00,0x00,0xb0,0x01,0x74,0x1a,
    0x8b,0x3b,0x31,0xf6,0xe8,0xeb,0xd7,0x39,0x00
};
static _Atomic uint64_t missing_starts, missing_stops;

static uint8_t ABI reject_missing_start(void) {
    uint64_t count = atomic_fetch_add_explicit(&missing_starts, 1, memory_order_relaxed) + 1;
    if (count <= 8)
        fprintf(stderr, "Movie audio guard: missing output device (#123), start rejected, occurrence %" PRIu64 "\n", count);
    return 0;
}

static uint8_t ABI stop_missing_device(void) {
    uint64_t count = atomic_fetch_add_explicit(&missing_stops, 1, memory_order_relaxed) + 1;
    if (count <= 8)
        fprintf(stderr, "Movie audio guard: missing output device (#123), stop already closed, occurrence %" PRIu64 "\n", count);
    return 1; /* The original stop's result when the open flag is already zero. */
}

static void put_jump(unsigned char *at, uintptr_t address) {
    static const unsigned char jump[] = {0xff,0x25,0,0,0,0};
    memcpy(at, jump, sizeof(jump));
    memcpy(at + sizeof(jump), &address, sizeof(address));
}

static void make_stub(unsigned char *stub, unsigned char *entry, unsigned moved, uintptr_t handler) {
    /* test rdi,rdi; jnz past the absolute null-handler jump. The moved prologue
     * has no relative operands, and its cmp restores flags before the original
     * branch. Keep every original argument, including the volume in XMM0. */
    const unsigned char check[] = {0x48,0x85,0xff,0x75,14};
    memcpy(stub, check, sizeof(check));
    put_jump(stub + sizeof(check), handler);
    memcpy(stub + 19, entry, moved);
    put_jump(stub + 19 + moved, (uintptr_t)(entry + moved));
}

int runtime_install_movie_audio_guard(unsigned char *image, uint64_t size) {
    const char *setting = getenv("BB_MOVIE_AUDIO_GUARD");
    if (setting && setting[0] == '0') {
        printf("Movie audio guard: off (BB_MOVIE_AUDIO_GUARD=0)\n");
        return 0;
    }
    if (!image || size < START + sizeof(start_signature) ||
        memcmp(image + START, start_signature, sizeof(start_signature)) ||
        memcmp(image + STOP, stop_signature, sizeof(stop_signature))) {
        printf("Movie audio guard: start/stop do not match Bloodborne 1.09; not hooked\n");
        return 0;
    }
    unsigned char *stub = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stub == MAP_FAILED) {
        fprintf(stderr, "Movie audio guard: no stub page; not hooked\n");
        return 0;
    }
    make_stub(stub, image + START, START_MOVED, (uintptr_t)reject_missing_start);
    make_stub(stub + 64, image + STOP, STOP_MOVED, (uintptr_t)stop_missing_device);
    if (mprotect(stub, 4096, PROT_READ | PROT_EXEC)) {
        munmap(stub, 4096);
        fprintf(stderr, "Movie audio guard: stub not executable; not hooked\n");
        return 0;
    }
    put_jump(image + START, (uintptr_t)stub);
    memset(image + START + 14, 0xcc, START_MOVED - 14);
    put_jump(image + STOP, (uintptr_t)(stub + 64));
    memset(image + STOP + 14, 0xcc, STOP_MOVED - 14);
    printf("Movie audio guard: null output device start/stop hooked (#123)\n");
    return 1;
}
#else
int runtime_install_movie_audio_guard(unsigned char *image, uint64_t size) {
    (void)image;
    (void)size;
    return 0;
}
#endif
