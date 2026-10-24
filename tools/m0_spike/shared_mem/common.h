// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
/* M0 feasibility spike: shared guest memory + IPC between an x86-64 (Rosetta)
 * parent and a native arm64 child. Shared by parent.c and child.c.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <os/os_sync_wait_on_address.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define GUEST_BASE   0x7000000000ULL
#define REGION_SIZE  (1ULL << 30) /* 1 GiB */

/* layout inside the shared region */
#define CTL_OFF      0x0ULL       /* control block */
#define RING_A_OFF   0x100000ULL  /* parent -> child ring */
#define RING_B_OFF   0x200000ULL  /* child -> parent ring */
#define DATA_OFF     0x1000000ULL /* 16 MiB: test data */
#define DATA_SIZE    (64ULL << 20)

#define HS_MSG_ID     0x4d305348 /* 'M0SH' child hello */
#define HS_REPLY_ID   0x4d305250
#define PING_MSG_ID   0x4d305049

#define CL 128 /* Apple cache line */

enum {
    CMD_NONE = 0,
    CMD_VERIFY_PATTERN, /* a0=off a1=len a2=seed -> result = #mismatches */
    CMD_WRITE_PATTERN,  /* a0=off a1=len a2=seed */
    CMD_CHECK_ZERO,     /* a0=off a1=len -> result = #nonzero 8-byte words */
    CMD_RING_CONSUME,   /* a0=count a1=buggy  -> result=violations, result2=ns */
    CMD_RING_PRODUCE,   /* a0=count a1=buggy  -> result2=ns */
    CMD_PINGPONG,       /* a0=iters a1=mech */
    CMD_FAULT_SERVER,   /* a1=mech: serve fault relays until fault_addr==~0 */
    CMD_MAP_ENTRY_AT,   /* a0=off a1=len: re-map the entry over [off,off+len) */
    CMD_EXIT,
    CMD_PUNCH_MADV,     /* a0=off a1=len: child madvise(MADV_ZERO) -> result=errno, result2=ns */
    CMD_PUNCH_BEHAVIOR, /* a0=off a1=len: child mach_vm_behavior_set(VM_BEHAVIOR_ZERO) */
};

enum { MECH_OSSYNC = 0, MECH_SEM, MECH_MACH, MECH_SPIN, MECH_COUNT };
static const char *const mech_name[MECH_COUNT] = {
    "os_sync_wait_on_address (shared)", "Mach semaphore", "Mach message", "spin (shared word)"};

typedef struct {
    /* command channel (parent writes cmd_seq, child acks) */
    _Alignas(CL) uint32_t cmd_seq;
    uint32_t cmd;
    uint64_t a0, a1, a2;
    _Alignas(CL) uint32_t ack_seq;
    uint64_t result, result2;
    /* ping-pong / fault relay words */
    _Alignas(CL) uint32_t ping;
    _Alignas(CL) uint32_t pong;
    _Alignas(CL) uint64_t fault_addr;
    uint32_t child_page_size;
    uint32_t child_ready;
    uint32_t bootstrap_lookup; /* 1 ok, 2 error, 3 hung */
} ctl_t;

#define RING_SLOTS 256
typedef struct {
    uint64_t seq;
    uint64_t d[6];
    uint64_t sum;
} slot_t; /* 64 bytes */

typedef struct {
    _Alignas(CL) uint64_t head; /* written by producer */
    _Alignas(CL) uint64_t tail; /* written by consumer */
    _Alignas(CL) slot_t slots[RING_SLOTS];
} ring_t;

#define CTL     ((ctl_t *)(GUEST_BASE + CTL_OFF))
#define RING_A  ((ring_t *)(GUEST_BASE + RING_A_OFF))
#define RING_B  ((ring_t *)(GUEST_BASE + RING_B_OFF))

/* handshake reply: memory entry + 2 semaphores + real bootstrap port */
typedef struct {
    mach_msg_header_t h;
    mach_msg_body_t b;
    mach_msg_port_descriptor_t entry, sem_p2c, sem_c2p, bootstrap;
    uint64_t size;
} hs_reply_t;
typedef struct {
    hs_reply_t m;
    mach_msg_max_trailer_t t;
} hs_reply_rcv_t;
typedef struct {
    mach_msg_header_t h;
    uint64_t pid;
} hs_hello_t;
typedef struct {
    hs_hello_t m;
    mach_msg_max_trailer_t t;
} hs_hello_rcv_t;
typedef struct {
    mach_msg_header_t h;
    mach_msg_max_trailer_t t;
} ping_rcv_t;

#define DIE(...) do { fprintf(stderr, "[%s] FATAL: ", ROLE); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)
#define KCHECK(kr, what) do { kern_return_t _k = (kr); if (_k != KERN_SUCCESS) DIE("%s: %s (0x%x)", what, mach_error_string(_k), _k); } while (0)

static inline uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

static inline uint64_t ld_acq64(const uint64_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline uint32_t ld_acq32(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void st_rel64(uint64_t *p, uint64_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static inline void st_rel32(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

static inline void cpu_relax(void) {
#if defined(__aarch64__)
    __asm__ volatile("yield");
#else
    __asm__ volatile("pause");
#endif
}

/* Block until *p == want, using the shared os_sync futex after a short spin. */
static inline void wait_eq32(uint32_t *p, uint32_t want) {
    for (int i = 0; i < 2000; i++) {
        if (ld_acq32(p) == want) return;
        cpu_relax();
    }
    for (;;) {
        uint32_t cur = ld_acq32(p);
        if (cur == want) return;
        if (os_sync_wait_on_address(p, cur, 4, OS_SYNC_WAIT_ON_ADDRESS_SHARED) < 0 && errno != EINTR &&
            errno != EFAULT)
            DIE("os_sync_wait_on_address: %s", strerror(errno));
    }
}
static inline void store_wake32(uint32_t *p, uint32_t v) {
    st_rel32(p, v);
    os_sync_wake_by_address_any(p, 4, OS_SYNC_WAKE_BY_ADDRESS_SHARED); /* ENOENT if no waiter: fine */
}

/* deterministic pattern */
static inline uint64_t pat(uint64_t seed, uint64_t i) {
    uint64_t x = (seed ^ 0x9e3779b97f4a7c15ULL) + i * 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 31; x *= 0x94d049bb133111ebULL; x ^= x >> 29;
    return x | 1; /* never zero */
}
static inline void write_pattern(uint64_t off, uint64_t len, uint64_t seed) {
    volatile uint64_t *p = (volatile uint64_t *)(GUEST_BASE + off);
    for (uint64_t i = 0; i < len / 8; i++) p[i] = pat(seed, i);
}
static inline uint64_t verify_pattern_at(uint64_t base, uint64_t len, uint64_t seed) {
    volatile uint64_t *p = (volatile uint64_t *)base;
    uint64_t bad = 0;
    for (uint64_t i = 0; i < len / 8; i++) bad += p[i] != pat(seed, i);
    return bad;
}
static inline uint64_t verify_pattern(uint64_t off, uint64_t len, uint64_t seed) {
    return verify_pattern_at(GUEST_BASE + off, len, seed);
}
static inline uint64_t count_nonzero(uint64_t off, uint64_t len) {
    volatile uint64_t *p = (volatile uint64_t *)(GUEST_BASE + off);
    uint64_t n = 0;
    for (uint64_t i = 0; i < len / 8; i++) n += p[i] != 0;
    return n;
}

/* ---- SPSC ring ------------------------------------------------------------
 * Correct variant: payload with plain stores, head published with release;
 * consumer loads head with acquire, reads payload, publishes tail with release.
 * Buggy variant: relaxed index load/store. A compiler-only fence
 * (atomic_signal_fence) keeps the compiler from reordering, so any violation
 * seen is from the hardware (or Rosetta's translation). */
static inline void slot_fill(volatile slot_t *s, uint64_t seq) {
    uint64_t sum = seq;
    s->seq = seq;
    for (int k = 0; k < 6; k++) { uint64_t v = pat(seq, (uint64_t)k); s->d[k] = v; sum += v * (uint64_t)(k + 3); }
    s->sum = sum;
}
static inline int slot_check(volatile slot_t *s, uint64_t seq) {
    uint64_t q = s->seq, sum = q, bad = q != seq;
    for (int k = 0; k < 6; k++) { uint64_t v = s->d[k]; bad |= v != pat(seq, (uint64_t)k); sum += v * (uint64_t)(k + 3); }
    bad |= s->sum != sum;
    return (int)bad;
}

static inline void ring_produce(ring_t *r, uint64_t count, int buggy) {
    uint64_t head = r->head;
    for (uint64_t i = 0; i < count; i++, head++) {
        if (buggy) {
            while (head - __atomic_load_n(&r->tail, __ATOMIC_RELAXED) >= RING_SLOTS) cpu_relax();
            __atomic_signal_fence(__ATOMIC_SEQ_CST);
        } else {
            while (head - ld_acq64(&r->tail) >= RING_SLOTS) cpu_relax();
        }
        slot_fill(&r->slots[head % RING_SLOTS], head);
        if (buggy) {
            __atomic_signal_fence(__ATOMIC_SEQ_CST);
            __atomic_store_n(&r->head, head + 1, __ATOMIC_RELAXED);
        } else {
            st_rel64(&r->head, head + 1);
        }
    }
}
static inline uint64_t ring_consume(ring_t *r, uint64_t count, int buggy) {
    uint64_t tail = r->tail, end = tail + count, viol = 0;
    while (tail < end) {
        uint64_t head;
        if (buggy) {
            head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
            __atomic_signal_fence(__ATOMIC_SEQ_CST);
        } else {
            head = ld_acq64(&r->head);
        }
        if (head == tail) { cpu_relax(); continue; }
        for (; tail < head && tail < end; tail++) {
            viol += (uint64_t)slot_check(&r->slots[tail % RING_SLOTS], tail);
            if (buggy) {
                __atomic_signal_fence(__ATOMIC_SEQ_CST);
                __atomic_store_n(&r->tail, tail + 1, __ATOMIC_RELAXED);
            } else {
                st_rel64(&r->tail, tail + 1);
            }
        }
    }
    return viol;
}

/* ---- stats ---------------------------------------------------------------- */
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
static inline void report_lat(const char *name, uint64_t *v, size_t n) {
    qsort(v, n, sizeof *v, cmp_u64);
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += (double)v[i];
    printf("  %-38s n=%-7zu median %7.2f us  p99 %7.2f us  p99.9 %7.2f us  mean %7.2f us  min %6.2f us\n", name, n,
           v[n / 2] / 1e3, v[(size_t)(n * 0.99)] / 1e3, v[(size_t)(n * 0.999)] / 1e3, sum / n / 1e3, v[0] / 1e3);
}
