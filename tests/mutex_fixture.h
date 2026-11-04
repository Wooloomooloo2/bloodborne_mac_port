// SPDX-License-Identifier: GPL-2.0-or-later
/* Link the production mutex implementation without a GPU or game process. */
#ifndef BB_MUTEX_FIXTURE_H
#define BB_MUTEX_FIXTURE_H
#include "runtime.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

void *runtime_low_malloc(size_t size) {
    static atomic_uint pages;
    const size_t page = (size_t)getpagesize();
    assert(size <= page);
    const uintptr_t hint = UINT64_C(0x800000000) + atomic_fetch_add(&pages, 1) * page;
    void *p = mmap((void *)hint, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(p != MAP_FAILED && (uintptr_t)p < (UINT64_C(1) << 40));
    return p;
}
void runtime_low_free(void *p) { assert(munmap(p, (size_t)getpagesize()) == 0); }
void runtime_thread_set_blocked(const char *what, uint64_t resource) { (void)what; (void)resource; }
void runtime_thread_clear_blocked(void) {}

typedef int32_t (ABI *FixtureOp)(void **);
typedef int32_t (ABI *FixtureAttrType)(void **, int);
typedef int32_t (ABI *FixtureInit)(void **, void **, const char *);
typedef int32_t (ABI *FixtureWait)(void **, void **);
typedef int32_t (ABI *FixtureTimedWait)(void **, void **, uint32_t);
#define FN(type, nid) ((type)runtime_mutex_resolve(nid))
#define SCE_ERROR(n) ((int32_t)(UINT32_C(0x80020000) | (n)))
#define LOCK FN(FixtureOp, "9UK1vLZQft4#p#J")
#define UNLOCK FN(FixtureOp, "tn3VlD0hG60#p#J")
#define TRYLOCK FN(FixtureOp, "upoVrzMHFeE#p#J")
#define DESTROY FN(FixtureOp, "2Of0f+3mhhE#p#J")
#define WAIT FN(FixtureWait, "WKAXJ4XBPQ4#p#J")
#define TIMEDWAIT FN(FixtureTimedWait, "BmMjYxmew1w#p#J")
#define SIGNAL FN(FixtureOp, "kDh-NfxgMtE#p#J")
#define COND_DESTROY FN(FixtureOp, "g+PZd2hiacg#p#J")
/* The exact exported name is checked rather than calling file-local helpers. */
static inline void fixture_mutex(void **mutex, int type) {
    void *attr = NULL;
    assert(FN(FixtureOp, "F8bUHwAG284#p#J")(&attr) == 0);
    assert(FN(FixtureAttrType, "iMp8QpE+XO4#p#J")(&attr, type) == 0);
    assert(FN(FixtureInit, "cmo1RIYva9o#p#J")(mutex, &attr, "fixture") == 0);
    assert(FN(FixtureOp, "smWEktiyyG0#p#J")(&attr) == 0);
}
#endif
