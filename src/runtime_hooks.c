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
 * hook puts on the stack does not move any. BB_CONTEXT_LOCK=0 leaves the game unpatched. */
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

typedef struct Hook {
    void *original;            /* the moved prologue + jump back: read by the asm at offset 0 */
    const char *name;
    uint64_t offset;           /* image offset of the function */
    unsigned char prologue[24];
    unsigned prologue_size;
    _Atomic uint64_t calls, waits, wait_ns, timeouts;
} Hook;

static Hook hooks[] = {
    /* push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp,0x58 */
    {NULL, "main thread's GPU setup", 0xebb960,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83,
      0xec, 0x58},
     17, 0, 0, 0, 0},
    /* push rbp; mov rbp,rsp; push r15; push r14; push rbx; sub rsp,0x38; mov rbx,rsi */
    {NULL, "GXRenderThread frame", 0x2199af0,
     {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x38, 0x48, 0x89,
      0xf3},
     16, 0, 0, 0, 0},
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
     17, 0, 0, 0, 0},
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

/* Tokens: 0 = nothing to release (timed out), 1 = taken, 2 = taken again by its holder. */
uint64_t bb_hook_enter(Hook *hook);
uint64_t bb_hook_enter(Hook *hook) {
    atomic_fetch_add_explicit(&hook->calls, 1, memory_order_relaxed);
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

void bb_hook_leave(Hook *hook, uint64_t token);
void bb_hook_leave(Hook *hook, uint64_t token) {
    (void)hook;
    if (!token) return;
    pthread_mutex_lock(&lock_mutex);
    if (lock_held && pthread_equal(lock_holder, pthread_self()) && --lock_depth == 0) {
        lock_held = 0;
        lock_holder_hook = NULL;
        pthread_cond_broadcast(&lock_cond);
    }
    pthread_mutex_unlock(&lock_mutex);
}

/* Entry: r11 = Hook*, the guest's return address on the stack (rsp = 8 mod 16). Saves the
 * argument registers around bb_hook_enter, calls the original through Hook.original (no stack
 * arguments to move), keeps the return registers around bb_hook_leave. */
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
    "    movq -8(%rbp), %rdi\n"
    "    movq -16(%rbp), %rsi\n"
#ifdef __APPLE__
    "    callq _bb_hook_leave\n"
#else
    "    callq bb_hook_leave\n"
#endif
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
    for (size_t i = 0; i < HOOK_COUNT; ++i) {
        const Hook *h = &hooks[i];
        if (!h->original) continue;
        uint64_t waits = atomic_load(&h->waits);
        fprintf(stderr, "Context lock: %s: %" PRIu64 " calls, %" PRIu64 " waits (%.1f ms in all), %" PRIu64 " timeouts\n",
                h->name, (uint64_t)atomic_load(&h->calls), waits, (double)atomic_load(&h->wait_ns) / 1e6,
                (uint64_t)atomic_load(&h->timeouts));
    }
}

void runtime_install_context_lock(unsigned char *image, uint64_t image_size) {
    const char *env = getenv("BB_CONTEXT_LOCK");
    if (env && env[0] == '0') {
        printf("Context lock: off (BB_CONTEXT_LOCK=0)\n");
        return;
    }
    for (size_t i = 0; i < HOOK_COUNT; ++i) {
        Hook *h = &hooks[i];
        if (h->offset + 14 > image_size || h->offset + h->prologue_size > image_size ||
            memcmp(image + h->offset, h->prologue, h->prologue_size)) {
            printf("Context lock: %s at 0x%" PRIx64 " does not match Bloodborne 1.09; not hooked\n", h->name,
                   h->offset);
            continue;
        }
        /* Stub page (one per hook, executable): mov r11, hook; jmp common, then the moved
         * prologue and a jump back. Absolute jumps: the page can be anywhere. */
        unsigned char *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) {
            printf("Context lock: no stub page for %s; not hooked\n", h->name);
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
            printf("Context lock: stub page for %s not executable; not hooked\n", h->name);
            munmap(page, 4096);
            continue;
        }
        h->original = original;
        unsigned char *target = image + h->offset;
        put_jump(target, (uint64_t)(uintptr_t)entry);
        for (unsigned b = 14; b < h->prologue_size; ++b) target[b] = 0xcc; /* never reached */
        printf("Context lock: %s (0x%" PRIx64 ") hooked\n", h->name, h->offset);
    }
    atexit(print_stats);
}

void runtime_context_lock_stats(void) { print_stats(); }

#else

void runtime_install_context_lock(unsigned char *image, uint64_t image_size) {
    (void)image;
    (void)image_size;
}
void runtime_context_lock_stats(void) {}

#endif
