// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
/* Guest function hooks: one lock around the two engine paths that write the immediate graphics
 * context (load race, MACOS_BACKLOG "Load crash: root cause").
 *
 * During a load the main thread records cloth/texture setup dispatches (0xebb960, called from
 * 0x2276501) straight into GXRenderThread's immediate context, with no lock: on the PS4 that
 * step ends before the render thread's next vblank-paced frame job (0x2199af0) starts. Under
 * Rosetta the step can outlast the vblank, both threads then write the same command buffer and
 * the GPU gets interleaved packets. Both functions are hooked at their entry and run under one
 * lock, so the frame job waits for the setup step and the other way round. A wait gives up
 * after LOCK_TIMEOUT_MS (reported) and runs the function anyway: a hidden dependency between
 * the two must not hang the game.
 *
 * A hooked function's first instructions (position-independent, checked byte for byte) move
 * into a stub that jumps back to the rest of the function; its entry becomes an absolute jump
 * to the common hook, which takes the lock, calls the stub and releases the lock. Neither
 * function takes stack arguments (checked in the disassembly), so the extra return address the
 * hook puts on the stack does not move any. BB_CONTEXT_LOCK=0 leaves the game unpatched.
 *
 * Observer hooks use the same entry but take no lock: they look at the arguments before the call
 * and the return value after it. They watch the render thread's per-frame command pool (#34,
 * #37, #38, #54): worker threads copy deferred draw records into a list whose payload comes from
 * a fixed 256 KiB linear heap per frame (carved once by the renderer, 0x2195c09/0x2195c46). The
 * flip (0x2198fd0) swaps the two lists and resets the other one's heap; an allocation from a full
 * heap returns NULL (0xe453b0), and the draw command at 0x21d5a80 copies into it unchecked: the
 * memmove crash. The observers log the pool's high-water mark, and an allocation that fails on
 * one of the two frame heaps gets memory from a spare pool of ours instead (16 MiB per heap,
 * reused once the game resets that heap: its next allocation starts at the heap's beginning).
 * Normal frames use a few KiB of the 256 (Forbidden Woods: 8 KiB peak), so the crashing frames
 * are rare spikes; the spare pool turns them into a log line. BB_FRAME_POOL=0 leaves the hooks
 * out; BB_FRAME_POOL_TEST=<KiB> treats frame heap allocations past that much as failed, to
 * exercise the spare pool. */
#define _GNU_SOURCE
#include "runtime.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#if defined(__x86_64__)

#define LOCK_TIMEOUT_MS 100
#define MAX_REPORTS 24

/* The argument registers as bb_hook_common saved them (lowest address first). */
typedef struct HookArgs {
    uint64_t r9, r8, rcx, rdx, rsi, rdi;
} HookArgs;

typedef struct Hook {
    void *original;            /* the moved prologue + jump back: read by the asm at offset 0 */
    const char *name;
    uint64_t offset;           /* image offset of the function */
    unsigned char prologue[24];
    unsigned prologue_size;
    _Atomic uint64_t calls, waits, wait_ns, timeouts;
    /* Observer hooks (no lock): called before the original with its arguments, and after it with
     * its return value. */
    void (*before)(const HookArgs *args);
    uint64_t (*after)(uint64_t ret); /* returns the value the caller gets */
} Hook;

static void frame_pool_flip(const HookArgs *args);
static void linear_heap_alloc_args(const HookArgs *args);
static uint64_t linear_heap_alloc_result(uint64_t ret);

static Hook hooks[] = {
    /* push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp,0x58 */
    {NULL, "main thread's GPU setup", 0xebb960,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83,
      0xec, 0x58},
     17, 0, 0, 0, 0, NULL, NULL},
    /* push rbp; mov rbp,rsp; push r15; push r14; push rbx; sub rsp,0x38; mov rbx,rsi */
    {NULL, "GXRenderThread frame", 0x2199af0,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x38, 0x48, 0x89,
      0xf3},
     16, 0, 0, 0, 0, NULL, NULL},
    /* The render thread's other job: its loop (0x21b69ea) runs the task at this+0x20 through
     * its first virtual method, outside the frame job, and this one records the frame's draw
     * list into the same immediate context (0x219d271 -> 0x21b2fb0). The overlap that still got
     * through the lock (MACOS_BACKLOG 2026-10-17; caught again 2026-10-19 loading a save: the
     * main thread in 0xebb960 under the lock, the render thread at 0x219d276 called from
     * 0x21b6a58) came this way. (this, esi): no stack arguments.
     * push rbp; mov rbp,rsp; push r15; push r14; push r12; push rbx; mov r14d,esi; mov r12,rdi */
    {NULL, "GXRenderThread task", 0x219d220,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x41, 0x89, 0xf6, 0x49,
      0x89, 0xfc},
     17, 0, 0, 0, 0, NULL, NULL},
    /* The main thread's frame work on GXRenderThread's immediate context: (rdi, rsi, rdx), no
     * stack arguments. During the opening movie of a new game the main thread records into that
     * context from here (directly at 0x28feb09 / 0x28feb40, and through 0x21696c0 -> 0x26bc2e0
     * -> 0x26bc320, the draw list), outside 0xebb960, while GXRenderThread's frame job uses it.
     * Slowed down (16 busy processes on an M4 Max, 2026-10-25) that froze the game (#46: no new
     * frame for 30 s) or crashed the draw list on a null item (#84, #24: signal 11 at 0x26c01c3,
     * address 0xd2); 16-18 GB Macs hit it without help. Locking 0x21696c0 alone still crashed.
     * push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx; and rsp,-0x20 */
    {NULL, "main thread's frame on the render context", 0x28fe7c0,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81,
      0xe4, 0xe0, 0xff, 0xff, 0xff},
     20, 0, 0, 0, 0, NULL, NULL},
    /* The render frame flip: (R), no stack arguments. Before it runs, R->0x360[R->0x380] is the
     * list this frame filled.
     * push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp,0x3088 */
    {NULL, "render frame flip", 0x2198fd0,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81,
      0xec, 0x88, 0x30, 0x00, 0x00},
     20, 0, 0, 0, 0, frame_pool_flip, NULL},
    /* Linear heap allocate: (heap, size, align) -> pointer or NULL, no stack arguments.
     * push rbp; mov rbp,rsp; push r15; push r14; push r12; push rbx; mov r15,rdx */
    {NULL, "linear heap allocate", 0xe453b0,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x49, 0x89, 0xd7},
     14, 0, 0, 0, 0, linear_heap_alloc_args, linear_heap_alloc_result},
};
#define HOOK_COUNT (sizeof(hooks) / sizeof(hooks[0]))

static pthread_mutex_t lock_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lock_cond = PTHREAD_COND_INITIALIZER;
static pthread_t lock_holder;
static int lock_held;
static unsigned lock_depth;
static const Hook *lock_holder_hook;
static _Atomic unsigned reports;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* The frame pool, read at each flip. A linear heap: +0x08 begin, +0x18 cursor, +0x20 limit,
 * +0x30 allocations; a list: +0x38 records, +0x3c capacity, +0x78 its chunked arena (+0 the
 * heap, +0x10 chunks, +0x14 allocations). Pointers are checked against the guest's user range
 * before they are read: a layout surprise must not crash the game. */
static int guest_pointer(uint64_t p) { return p >= 0x400000 && p < 0x10000000000ull && !(p & 7); }

static _Atomic uint64_t pool_flips, pool_peak_used, pool_peak_records, pool_failures;
static _Atomic uint64_t pool_size;
static uint64_t pool_last_report_ns;
static unsigned pool_reported_tenths;

/* The two frame lists' heaps, as the flip finds them, each with its spare pool. */
#define SPARE_SIZE (16ull << 20)
typedef struct Spare {
    _Atomic uint64_t heap;
    _Atomic uint64_t base, used, served;
} Spare;
static Spare spares[2];
static _Atomic uint64_t spare_allocations, spare_bytes, spare_resets;
static uint64_t pool_test_limit;

static Spare *frame_heap_spare(uint64_t heap) {
    for (int i = 0; i < 2; ++i)
        if (atomic_load_explicit(&spares[i].heap, memory_order_acquire) == heap) return &spares[i];
    return NULL;
}

static void frame_pool_flip(const HookArgs *args) {
    uint64_t r = args->rdi;
    atomic_fetch_add_explicit(&pool_flips, 1, memory_order_relaxed);
    if (!guest_pointer(r)) return;
    int32_t index = *(const int32_t *)(uintptr_t)(r + 0x380);
    if (index != 0 && index != 1) return;
    for (int i = 0; i < 2; ++i) {
        uint64_t l = *(const uint64_t *)(uintptr_t)(r + 0x360 + 8 * (uint64_t)i);
        uint64_t h = guest_pointer(l) ? *(const uint64_t *)(uintptr_t)(l + 0x78) : 0;
        if (guest_pointer(h) && atomic_load_explicit(&spares[i].heap, memory_order_relaxed) != h)
            atomic_store_explicit(&spares[i].heap, h, memory_order_release);
    }
    uint64_t list = *(const uint64_t *)(uintptr_t)(r + 0x360 + 8 * (uint64_t)index);
    if (!guest_pointer(list)) return;
    uint64_t heap = *(const uint64_t *)(uintptr_t)(list + 0x78);
    if (!guest_pointer(heap)) return;
    uint64_t begin = *(const uint64_t *)(uintptr_t)(heap + 0x08);
    uint64_t cursor = *(const uint64_t *)(uintptr_t)(heap + 0x18);
    uint64_t limit = *(const uint64_t *)(uintptr_t)(heap + 0x20);
    if (cursor < begin || limit < cursor || limit - begin > (64ull << 20)) return;
    uint64_t used = cursor - begin, size = limit - begin;
    uint64_t records = (uint32_t) * (const int32_t *)(uintptr_t)(list + 0x38);
    uint64_t capacity = (uint32_t) * (const int32_t *)(uintptr_t)(list + 0x3c);
    atomic_store_explicit(&pool_size, size, memory_order_relaxed);
    if (records > atomic_load_explicit(&pool_peak_records, memory_order_relaxed))
        atomic_store_explicit(&pool_peak_records, records, memory_order_relaxed);
    if (used <= atomic_load_explicit(&pool_peak_used, memory_order_relaxed) || !size) return;
    atomic_store_explicit(&pool_peak_used, used, memory_order_relaxed);
    /* A new high-water mark: reported when it crosses the next tenth of the heap, or at most
     * every 10 s otherwise. */
    unsigned tenths = (unsigned)(used * 10 / size);
    uint64_t now = now_ns();
    if (tenths > pool_reported_tenths || now - pool_last_report_ns > 10000000000ull) {
        pool_reported_tenths = tenths > pool_reported_tenths ? tenths : pool_reported_tenths;
        pool_last_report_ns = now;
        fprintf(stderr, "Frame pool: new peak %" PRIu64 " of %" PRIu64 " KiB (%u%%), %" PRIu64 " of %" PRIu64
                " records, frame %" PRIu64 "\n", used >> 10, size >> 10, (unsigned)(used * 100 / size), records,
                capacity, atomic_load_explicit(&pool_flips, memory_order_relaxed));
    }
}

static _Thread_local HookArgs heap_args;

static void linear_heap_alloc_args(const HookArgs *args) {
    heap_args = *args;
    /* The first allocation after the game reset a frame heap: the records the spare pool served
     * for that heap's previous frame are gone with it, so the spare pool starts over too. */
    Spare *spare = frame_heap_spare(args->rdi);
    if (spare && atomic_load_explicit(&spare->used, memory_order_relaxed) &&
        *(const uint64_t *)(uintptr_t)(args->rdi + 0x18) == *(const uint64_t *)(uintptr_t)(args->rdi + 0x08)) {
        atomic_store_explicit(&spare->used, 0, memory_order_relaxed);
        atomic_fetch_add_explicit(&spare_resets, 1, memory_order_relaxed);
    }
}

/* Bump allocation from a frame heap's spare pool (allocated on first use); 0 when it is full. */
static uint64_t spare_alloc(Spare *spare, uint64_t size, uint64_t align) {
    uint64_t base = atomic_load_explicit(&spare->base, memory_order_acquire);
    if (!base) {
        void *memory = runtime_low_alloc_aligned(SPARE_SIZE, 4096);
        if (!memory) return 0;
        uint64_t expected = 0;
        if (!atomic_compare_exchange_strong(&spare->base, &expected, (uint64_t)(uintptr_t)memory))
            base = expected; /* another thread won; this block stays unused */
        else
            base = (uint64_t)(uintptr_t)memory;
    }
    if (!align || (align & (align - 1))) align = 16;
    uint64_t used = atomic_load_explicit(&spare->used, memory_order_relaxed), start;
    do {
        start = (used + align - 1) & ~(align - 1);
        if (start + size > SPARE_SIZE || start + size < start) return 0;
    } while (!atomic_compare_exchange_weak(&spare->used, &used, start + size));
    return base + start;
}

static uint64_t linear_heap_alloc_result(uint64_t ret) {
    uint64_t heap = heap_args.rdi;
    Spare *spare = frame_heap_spare(heap);
    if (!spare) return ret;
    if (ret && pool_test_limit) {
        uint64_t begin = *(const uint64_t *)(uintptr_t)(heap + 0x08);
        if (ret + heap_args.rsi - begin <= pool_test_limit) return ret;
    } else if (ret) {
        return ret;
    }
    uint64_t served = spare_alloc(spare, heap_args.rsi, heap_args.rdx);
    if (served) {
        atomic_fetch_add_explicit(&spare_allocations, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&spare_bytes, heap_args.rsi, memory_order_relaxed);
    }
    uint64_t failures = atomic_fetch_add_explicit(&pool_failures, 1, memory_order_relaxed) + 1;
    if (failures <= 16 || !served) {
        uint64_t begin = *(const uint64_t *)(uintptr_t)(heap + 0x08);
        uint64_t cursor = *(const uint64_t *)(uintptr_t)(heap + 0x18);
        uint64_t limit = *(const uint64_t *)(uintptr_t)(heap + 0x20);
        char thread[64] = "?";
        if (pthread_getname_np(pthread_self(), thread, sizeof(thread)) || !thread[0])
            snprintf(thread, sizeof(thread), "%p", (void *)pthread_self());
        fprintf(stderr, "Frame pool: frame heap 0x%" PRIx64 " full: %" PRIu64 " bytes (align %" PRIu64 ") asked, %" PRIu64
                " of %" PRIu64 " KiB used, thread %s, frame %" PRIu64 "%s\n", heap, heap_args.rsi, heap_args.rdx,
                (cursor - begin) >> 10, (limit - begin) >> 10, thread,
                atomic_load_explicit(&pool_flips, memory_order_relaxed),
                served ? ": served from the spare pool" : ": the spare pool is full too");
    }
    return served ? served : ret;
}

/* Tokens: 0 = nothing to release (timed out), 1 = taken, 2 = taken again by its holder,
 * 3 = an observer (no lock). */
uint64_t bb_hook_enter(Hook *hook, const HookArgs *args);
uint64_t bb_hook_enter(Hook *hook, const HookArgs *args) {
    atomic_fetch_add_explicit(&hook->calls, 1, memory_order_relaxed);
    if (hook->before) {
        hook->before(args);
        return 3;
    }
    pthread_t self = pthread_self();
    pthread_mutex_lock(&lock_mutex);
    if (lock_held && pthread_equal(lock_holder, self)) {
        ++lock_depth;
        pthread_mutex_unlock(&lock_mutex);
        return 2;
    }
    uint64_t token = 1;
    if (lock_held) {
        const Hook *other = lock_holder_hook;
        uint64_t start = now_ns(), deadline = start + (uint64_t)LOCK_TIMEOUT_MS * 1000000u;
        while (lock_held) {
            uint64_t now = now_ns();
            if (now >= deadline) break;
            uint64_t left = deadline - now;
            struct timespec wait = {(time_t)(left / 1000000000u), (long)(left % 1000000000u)};
#ifdef __APPLE__
            pthread_cond_timedwait_relative_np(&lock_cond, &lock_mutex, &wait);
#else
            struct timespec abs;
            clock_gettime(CLOCK_REALTIME, &abs);
            abs.tv_sec += wait.tv_sec;
            abs.tv_nsec += wait.tv_nsec;
            if (abs.tv_nsec >= 1000000000) { abs.tv_sec++; abs.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&lock_cond, &lock_mutex, &abs);
#endif
        }
        uint64_t waited = now_ns() - start;
        atomic_fetch_add_explicit(&hook->waits, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&hook->wait_ns, waited, memory_order_relaxed);
        if (lock_held) {
            token = 0;
            atomic_fetch_add_explicit(&hook->timeouts, 1, memory_order_relaxed);
        }
        if (atomic_fetch_add_explicit(&reports, 1, memory_order_relaxed) < MAX_REPORTS) {
            fprintf(stderr, "Context lock: %s waited %.2f ms for %s%s (calls %" PRIu64 ", waits %" PRIu64 ")\n",
                    hook->name, (double)waited / 1e6, other ? other->name : "?",
                    token ? "" : ": timed out, running anyway",
                    atomic_load_explicit(&hook->calls, memory_order_relaxed),
                    atomic_load_explicit(&hook->waits, memory_order_relaxed));
        }
    }
    if (token) {
        lock_held = 1;
        lock_holder = self;
        lock_depth = 1;
        lock_holder_hook = hook;
    }
    pthread_mutex_unlock(&lock_mutex);
    return token;
}

/* Returns the value the guest caller gets in rax. */
uint64_t bb_hook_leave(Hook *hook, uint64_t token, uint64_t ret);
uint64_t bb_hook_leave(Hook *hook, uint64_t token, uint64_t ret) {
    if (token == 3) return hook->after ? hook->after(ret) : ret;
    if (!token) return ret;
    pthread_mutex_lock(&lock_mutex);
    if (lock_held && pthread_equal(lock_holder, pthread_self()) && --lock_depth == 0) {
        lock_held = 0;
        lock_holder_hook = NULL;
        pthread_cond_broadcast(&lock_cond);
    }
    pthread_mutex_unlock(&lock_mutex);
    return ret;
}

/* Entry: r11 = Hook*, the guest's return address on the stack (rsp = 8 mod 16). Saves the
 * argument registers around bb_hook_enter (which gets them as a HookArgs at rbp - 64), calls the original through Hook.original (no stack
 * arguments to move), keeps the return registers around bb_hook_leave, and returns rax as
 * bb_hook_leave gives it back. */
__asm__(
    ".text\n"
    ".p2align 4\n"
#ifdef __APPLE__
    "_bb_hook_common:\n"
#else
    "bb_hook_common:\n"
#endif
    "    pushq %rbp\n"
    "    movq %rsp, %rbp\n"
    "    pushq %r11\n"              /* -8: hook */
    "    pushq %rax\n"              /* -16: token (al: vector count of a variadic call) */
    "    pushq %rdi\n"
    "    pushq %rsi\n"
    "    pushq %rdx\n"
    "    pushq %rcx\n"
    "    pushq %r8\n"
    "    pushq %r9\n"
    "    subq $128, %rsp\n"
    "    movdqu %xmm0, 0(%rsp)\n"
    "    movdqu %xmm1, 16(%rsp)\n"
    "    movdqu %xmm2, 32(%rsp)\n"
    "    movdqu %xmm3, 48(%rsp)\n"
    "    movdqu %xmm4, 64(%rsp)\n"
    "    movdqu %xmm5, 80(%rsp)\n"
    "    movdqu %xmm6, 96(%rsp)\n"
    "    movdqu %xmm7, 112(%rsp)\n"
    "    movq %r11, %rdi\n"
    "    leaq -64(%rbp), %rsi\n"
#ifdef __APPLE__
    "    callq _bb_hook_enter\n"
#else
    "    callq bb_hook_enter\n"
#endif
    "    movq -16(%rbp), %r11\n"    /* the caller's rax back, the token in its slot */
    "    movq %rax, -16(%rbp)\n"
    "    movq %r11, %rax\n"
    "    movdqu 0(%rsp), %xmm0\n"
    "    movdqu 16(%rsp), %xmm1\n"
    "    movdqu 32(%rsp), %xmm2\n"
    "    movdqu 48(%rsp), %xmm3\n"
    "    movdqu 64(%rsp), %xmm4\n"
    "    movdqu 80(%rsp), %xmm5\n"
    "    movdqu 96(%rsp), %xmm6\n"
    "    movdqu 112(%rsp), %xmm7\n"
    "    addq $128, %rsp\n"
    "    popq %r9\n"
    "    popq %r8\n"
    "    popq %rcx\n"
    "    popq %rdx\n"
    "    popq %rsi\n"
    "    popq %rdi\n"               /* rsp = rbp - 16: 0 mod 16 */
    "    movq -8(%rbp), %r11\n"
    "    callq *(%r11)\n"
    "    pushq %rax\n"
    "    pushq %rdx\n"
    "    subq $32, %rsp\n"
    "    movdqu %xmm0, 0(%rsp)\n"
    "    movdqu %xmm1, 16(%rsp)\n"
    "    movq %rax, %rdx\n"
    "    movq -8(%rbp), %rdi\n"
    "    movq -16(%rbp), %rsi\n"
#ifdef __APPLE__
    "    callq _bb_hook_leave\n"
#else
    "    callq bb_hook_leave\n"
#endif
    "    movq %rax, -24(%rbp)\n"   /* the return value the caller gets (an observer may set it) */
    "    movdqu 0(%rsp), %xmm0\n"
    "    movdqu 16(%rsp), %xmm1\n"
    "    addq $32, %rsp\n"
    "    popq %rdx\n"
    "    popq %rax\n"
    "    movq %rbp, %rsp\n"
    "    popq %rbp\n"
    "    retq\n");
extern char bb_hook_common[];

/* jmp qword [rip+0]; .quad target */
static unsigned char *put_jump(unsigned char *at, uint64_t target) {
    static const unsigned char jmp[6] = {0xff, 0x25, 0, 0, 0, 0};
    memcpy(at, jmp, sizeof(jmp));
    memcpy(at + 6, &target, 8);
    return at + 14;
}

static void print_stats(void) {
    if (atomic_load(&pool_flips)) {
        uint64_t size = atomic_load(&pool_size), used = atomic_load(&pool_peak_used);
        fprintf(stderr, "Frame pool: peak %" PRIu64 " of %" PRIu64 " KiB (%u%%), %" PRIu64 " records, %" PRIu64
                " frames, %" PRIu64 " failed allocations (%" PRIu64 " served from the spare pool, %" PRIu64
                " KiB; %" PRIu64 " spare pool reuses)\n", used >> 10, size >> 10,
                size ? (unsigned)(used * 100 / size) : 0, (uint64_t)atomic_load(&pool_peak_records),
                (uint64_t)atomic_load(&pool_flips), (uint64_t)atomic_load(&pool_failures),
                (uint64_t)atomic_load(&spare_allocations), (uint64_t)atomic_load(&spare_bytes) >> 10,
                (uint64_t)atomic_load(&spare_resets));
    }
    for (size_t i = 0; i < HOOK_COUNT; ++i) {
        const Hook *h = &hooks[i];
        if (!h->original || h->before) continue;
        uint64_t waits = atomic_load(&h->waits);
        fprintf(stderr, "Context lock: %s: %" PRIu64 " calls, %" PRIu64 " waits (%.1f ms in all), %" PRIu64 " timeouts\n",
                h->name, (uint64_t)atomic_load(&h->calls), waits, (double)atomic_load(&h->wait_ns) / 1e6,
                (uint64_t)atomic_load(&h->timeouts));
    }
}

void runtime_install_context_lock(unsigned char *image, uint64_t image_size) {
    const char *env = getenv("BB_CONTEXT_LOCK");
    const int lock_off = env && env[0] == '0';
    if (lock_off) printf("Context lock: off (BB_CONTEXT_LOCK=0)\n");
    env = getenv("BB_FRAME_POOL");
    const int pool_off = env && env[0] == '0';
    /* BB_MOVIE_LOCK=0: leave the main thread's frame (0x28fe7c0) unlocked (A/B, fallback). */
    env = getenv("BB_MOVIE_LOCK");
    const int movie_off = env && env[0] == '0';
    if (movie_off && !lock_off) printf("Context lock: main thread's frame not hooked (BB_MOVIE_LOCK=0)\n");
    env = getenv("BB_FRAME_POOL_TEST");
    if (env && !pool_off) {
        pool_test_limit = strtoull(env, NULL, 10) << 10;
        printf("Frame pool: test mode, frame heap allocations past %" PRIu64 " KiB go to the spare pool\n",
               pool_test_limit >> 10);
    }
    for (size_t i = 0; i < HOOK_COUNT; ++i) {
        Hook *h = &hooks[i];
        if (h->before ? pool_off : lock_off) continue;
        if (h->offset == 0x28fe7c0 && movie_off) continue;
        const char *what = h->before ? "Frame pool" : "Context lock";
        if (h->offset + 14 > image_size || h->offset + h->prologue_size > image_size ||
            memcmp(image + h->offset, h->prologue, h->prologue_size)) {
            printf("%s: %s at 0x%" PRIx64 " does not match Bloodborne 1.09; not hooked\n", what, h->name,
                   h->offset);
            continue;
        }
        /* Stub page (one per hook, executable): mov r11, hook; jmp common, then the moved
         * prologue and a jump back. Absolute jumps: the page can be anywhere. */
        unsigned char *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) {
            printf("%s: no stub page for %s; not hooked\n", what, h->name);
            continue;
        }
        unsigned char *p = page;
        unsigned char *entry = p;
        *p++ = 0x49; *p++ = 0xbb; /* movabs r11, imm64 */
        uint64_t hook_address = (uint64_t)(uintptr_t)h;
        memcpy(p, &hook_address, 8);
        p += 8;
        p = put_jump(p, (uint64_t)(uintptr_t)bb_hook_common);
        p = (unsigned char *)(((uintptr_t)p + 15) & ~(uintptr_t)15);
        unsigned char *original = p;
        memcpy(p, h->prologue, h->prologue_size);
        p += h->prologue_size;
        p = put_jump(p, (uint64_t)(uintptr_t)(image + h->offset + h->prologue_size));
        if (mprotect(page, 4096, PROT_READ | PROT_EXEC)) {
            printf("%s: stub page for %s not executable; not hooked\n", what, h->name);
            munmap(page, 4096);
            continue;
        }
        h->original = original;
        unsigned char *target = image + h->offset;
        put_jump(target, (uint64_t)(uintptr_t)entry);
        for (unsigned b = 14; b < h->prologue_size; ++b) target[b] = 0xcc; /* never reached */
        printf("%s: %s (0x%" PRIx64 ") hooked\n", what, h->name, h->offset);
    }
    atexit(print_stats);
}

void runtime_context_lock_stats(void) { print_stats(); }

/* Diagnostics for gnmdriver's context check: the hook holding the context lock when that is
 * another thread, else NULL. Read without the mutex (a snapshot for a log line). */
const char *runtime_context_lock_other(void) {
    if (!__atomic_load_n(&lock_held, __ATOMIC_RELAXED)) return NULL;
    pthread_t holder = __atomic_load_n(&lock_holder, __ATOMIC_RELAXED);
    if (pthread_equal(holder, pthread_self())) return NULL;
    const Hook *hook = __atomic_load_n(&lock_holder_hook, __ATOMIC_RELAXED);
    return hook ? hook->name : "?";
}

#else

void runtime_install_context_lock(unsigned char *image, uint64_t image_size) {
    (void)image;
    (void)image_size;
}
void runtime_context_lock_stats(void) {}
const char *runtime_context_lock_other(void) { return NULL; }

#endif
