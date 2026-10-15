/* Shared control block between bb-probe (x86-64, Rosetta) and bbgpu-host (native arm64), M3
 * (docs/DESIGN_NATIVE_GPU_PROCESS.md "M3 plan"). A separate small Mach memory entry, mapped at
 * any address on each side: it holds offsets, never pointers. Guest addresses inside records are
 * valid in both processes (guest memory is mirrored at identical addresses).
 *
 * Rings carry 8-byte-aligned records { u32 type; u32 bytes (header included); payload }. Each
 * ring has one consumer; producers on one side serialise with a lock of their own (passed in).
 * Indices are byte counters published with release stores and read with acquire loads: the arm64
 * side reorders loads, Rosetta's TSO does not protect it (M0 (b)). Doorbells are shared
 * os_sync_wait_on_address words; every wait has a timeout so a lost wake costs latency, not a
 * hang, and the caller can check that the peer is still alive.
 *
 * Used from C (src/gpu_process.c, host/main.c) and C++ (gpu/shim); both sides little-endian LP64.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <errno.h>
#include <os/os_sync_wait_on_address.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#ifdef __cplusplus
#define BBGPU_ALIGNAS(n) alignas(n)
#define BBGPU_STATIC_ASSERT static_assert
#else
#define BBGPU_ALIGNAS(n) _Alignas(n)
#define BBGPU_STATIC_ASSERT _Static_assert
#endif

/* The mirrored guest range (identical to host/bbgpu_protocol.h). */
#define BBGPU_GUEST_MIN UINT64_C(0x7000000000)
#define BBGPU_GUEST_MAX UINT64_C(0xfc00000000)

#define BBGPU_SHARED_MAGIC 0x31424742u /* "BGB1" */
#define BBGPU_SHARED_VERSION 4u /* 3: command buffer copy arena; 4: game fault counters */
#define BBGPU_CONTROL_SIZE (UINT64_C(4) << 20) /* control block and rings */
/* Command buffer copies (game side, at submit; the host parses these): the game may rewrite its
 * command buffers once the submit returns, before the host has parsed them. */
#define BBGPU_ARENA_OFFSET BBGPU_CONTROL_SIZE
#define BBGPU_ARENA_SIZE (UINT64_C(64) << 20)
#define BBGPU_SHARED_SIZE (BBGPU_CONTROL_SIZE + BBGPU_ARENA_SIZE)
#define BBGPU_SUBMIT_RING_BYTES (UINT64_C(2) << 20)
#define BBGPU_EVENT_RING_BYTES (UINT64_C(512) << 10)
#define BBGPU_REQUEST_SLOTS 32 /* requests and fault relays in flight (bit mask in a u32) */
#define BBGPU_PROTECT_RING_BYTES (UINT64_C(256) << 10)
#define BBGPU_FAULT_RING_BYTES (UINT64_C(64) << 10)

/* ---- records: game -> host (submit ring) ---- */
enum {
    BBGPU_REC_PAD = 0,             /* filler up to the end of the ring */
    BBGPU_REC_SUBMIT_GFX = 1,      /* BbGpuSubmit */
    BBGPU_REC_SUBMIT_GFX_FLIP,     /* BbGpuSubmit with the flip fields */
    BBGPU_REC_SUBMIT_DONE,         /* no payload */
    BBGPU_REC_DING_DONG,           /* BbGpuDingDong */
    BBGPU_REC_MAP_COMPUTE_QUEUE,   /* BbGpuMapComputeQueue (request) */
    BBGPU_REC_UNMAP_COMPUTE_QUEUE, /* BbGpuScalar: a = vqid */
    BBGPU_REC_VO_OPEN,             /* BbGpuVoOpen (request) */
    BBGPU_REC_VO_CLOSE,            /* BbGpuScalar: a = handle */
    BBGPU_REC_VO_REGISTER_BUFFERS, /* BbGpuVoRegister (request) */
    BBGPU_REC_VO_UNREGISTER_BUFFERS, /* BbGpuScalar: a = handle, b = attribute index */
    BBGPU_REC_VO_CHANGE_ATTRIBUTE, /* BbGpuVoAttribute */
    BBGPU_REC_VO_SET_FLIP_RATE,    /* BbGpuScalar: a = handle, b = rate */
    BBGPU_REC_VO_SUBMIT_FLIP,      /* BbGpuVoFlip */
    BBGPU_REC_VO_ADJUST_COLOR,     /* BbGpuScalar: a = handle, f = gamma */
    BBGPU_REC_VO_CONFIGURE_MODE,   /* BbGpuScalar: a = handle, b = colorimetry */
    BBGPU_REC_INVALIDATE,          /* BbGpuRange */
    BBGPU_REC_VIEW,                /* BbGpuViewRecord: guest view map/unmap/protect (mirror) */
    BBGPU_REC_VERIFY,              /* BbGpuVerifyRecord: mirror check, answered on the control port */
    BBGPU_REC_START,               /* BbGpuStart (request): start the renderer with the configuration */
    BBGPU_REC_TEXT_BEGIN,          /* BbGpuTextBegin (request): open the window's text entry (IME) */
    BBGPU_REC_COUNT,
};

/* ---- records: host -> game (event ring) ---- */
enum {
    BBGPU_EV_PAD = 0,
    BBGPU_EV_IRQ = 1,          /* BbGpuIrq */
    BBGPU_EV_FLIP,             /* BbGpuFlipEvent: a flip was presented */
    BBGPU_EV_EOP_FLIP_QUEUED,  /* BbGpuFlipEvent: an end-of-pipe flip entered the queue */
    BBGPU_EV_VBLANK,           /* BbGpuVblank */
    BBGPU_EV_RESTART,          /* no payload: the settings menu asked to restart the game */
};

/* ---- M4 page tracking: protect ring (host -> game), fault ring (game -> host) ---- */
enum {
    BBGPU_REC_PROTECT = 1, /* BbGpuProtect: apply GPU page protection in the game (acknowledged) */
    BBGPU_REC_FAULT = 2,   /* BbGpuFault (request): the game hit a GPU-protected page */
};

typedef struct { uint32_t type, bytes; } BbGpuRecord;

#define BBGPU_SUBMIT_MAX 64 /* command buffer pairs per submit record */
typedef struct {
    BbGpuRecord r;
    uint32_t count;
    uint32_t has_ccb_addrs, has_ccb_sizes;
    uint32_t copied;  /* dcb/ccb are byte offsets into the arena, not guest addresses */
    uint32_t game_us; /* the game's steady clock at the submit, microseconds mod 2^32 (spike log) */
    uint32_t vo_handle, buf_idx, flip_mode; /* SUBMIT_GFX_FLIP */
    int64_t flip_arg;
    uint64_t dcb[BBGPU_SUBMIT_MAX], ccb[BBGPU_SUBMIT_MAX];
    uint32_t dcb_bytes[BBGPU_SUBMIT_MAX], ccb_bytes[BBGPU_SUBMIT_MAX];
} BbGpuSubmit; /* sent truncated to bbgpu_submit_bytes(count): the arrays are packed */

typedef struct { BbGpuRecord r; uint32_t vqid, next_offs_dw; } BbGpuDingDong;
typedef struct { BbGpuRecord r; int64_t a, b; double f; } BbGpuScalar;
typedef struct { BbGpuRecord r; uint64_t address, size; } BbGpuRange;
typedef struct {
    BbGpuRecord r;
    uint32_t slot, pipe_id, queue_id, ring_size_dw;
    uint64_t ring_base, read_ptr_addr;
} BbGpuMapComputeQueue;
typedef struct { BbGpuRecord r; uint32_t slot, user_id; int32_t bus_type, index; } BbGpuVoOpen;
#define BBGPU_ATTRIBUTE_BYTES 64 /* VideoOut BufferAttribute, raw */
#define BBGPU_VO_BUFFERS_MAX 16
typedef struct {
    BbGpuRecord r;
    uint32_t slot;
    int32_t handle, start, count;
    uint8_t attribute[BBGPU_ATTRIBUTE_BYTES];
    uint64_t addresses[BBGPU_VO_BUFFERS_MAX];
} BbGpuVoRegister;
typedef struct {
    BbGpuRecord r;
    int32_t handle, index;
    uint8_t attribute[BBGPU_ATTRIBUTE_BYTES];
} BbGpuVoAttribute;
typedef struct { BbGpuRecord r; int32_t handle, index, mode, pad; int64_t flip_arg; } BbGpuVoFlip;

/* Mirror records: the same payloads as the M2 Mach messages (host/bbgpu_protocol.h). Going
 * through the ring orders them with the GPU work: memory is mapped before work that uses it. */
typedef struct { BbGpuRecord r; uint32_t op, prot; uint64_t address, size, phys; } BbGpuViewRecord;
typedef struct {
    BbGpuRecord r;
    uint64_t seq, views, bytes, events;
    uint32_t count, pad;
    struct { uint64_t address, size, checksum; } samples[8];
} BbGpuVerifyRecord;
typedef struct { BbGpuRecord r; uint32_t slot, pad; } BbGpuStart; /* configuration: in the block */
typedef struct { BbGpuRecord r; uint32_t slot, pad; char initial[512], prompt[128]; } BbGpuTextBegin;

typedef struct { BbGpuRecord r; uint32_t id, seq; } BbGpuIrq;
typedef struct { BbGpuRecord r; uint64_t address, size; uint32_t read, write, seq, pad; } BbGpuProtect;
typedef struct { BbGpuRecord r; uint64_t address; uint32_t slot, write; } BbGpuFault; /* seq: GpuIdle: SubmitDones handled */
typedef struct {
    BbGpuRecord r;
    int32_t handle, index;
    uint32_t eop, pad;
    int64_t flip_arg;
    uint64_t process_time, tsc; /* FLIP: flip status times; EOP_FLIP_QUEUED: tsc = submit_tsc */
} BbGpuFlipEvent;
typedef struct { BbGpuRecord r; int32_t handle, pad; uint64_t count, process_time, tsc; } BbGpuVblank;

/* ---- the block ---- */
#define BBGPU_CACHE_LINE 128
typedef struct {
    BBGPU_ALIGNAS(BBGPU_CACHE_LINE) uint64_t head;   /* bytes pushed (producer) */
    uint32_t head_seq;                          /* doorbell: bumped after each push */
    uint32_t consumer_waiting;
    uint64_t reserved_pad;                      /* producer only: PAD bytes of the open reservation */
    BBGPU_ALIGNAS(BBGPU_CACHE_LINE) uint64_t tail;   /* bytes consumed (consumer) */
    uint32_t tail_seq;                          /* doorbell: bumped after each pop */
    uint32_t producer_waiting;
    BBGPU_ALIGNAS(BBGPU_CACHE_LINE) uint64_t offset, capacity; /* data, from the block start */
} BbGpuRing;

typedef struct {
    uint32_t state; /* 0 free, 1 waiting for the host, 2 answered (futex word) */
    uint32_t pad;
    int64_t result;
} BbGpuReply;

typedef struct {
    uint32_t magic, version;
    uint64_t size;
    /* Launch configuration and timebase: written by the game before the host starts rendering. */
    char title[128], serial[32], user_dir[1024];
    uint32_t sdk_version, psf_attributes;
    int32_t width, height;
    uint64_t process_start_ns; /* CLOCK_MONOTONIC (system-wide) at the runtime's start */
    uint64_t tsc_hz;           /* guest rdtsc rate (Rosetta: cntvct_el0, 1 GHz on M4 Max) */
    /* Host facts: written by the host before it reports started (state 2). */
    BBGPU_ALIGNAS(BBGPU_CACHE_LINE) uint32_t host_state; /* 0 idle, 1 start requested, 2 started, 3 failed (futex word) */
    uint32_t hdr_supported;
    uint64_t label_address; /* VideoOut port buffer labels in the host (P3a) */
    uint64_t host_flips, host_submits;
    int32_t host_pid, game_pid;
    /* Keyboard state of the host's window (SDL scancodes), refreshed by the host every few ms
     * for the game's pad emulation; keys_valid once written. Input proper is M5. */
    uint32_t keys_valid, overlay_captures; /* overlay_captures: the settings menu is open */
    uint8_t keys[512];
    /* Text entry in the host window (IME dialog): text_state 0 typing, 1 confirmed, 2 cancelled;
     * text_seq bumps with each update (written by the host, the game polls). */
    uint32_t text_active, text_state, text_seq, pad1;
    char text[512];
    BBGPU_ALIGNAS(BBGPU_CACHE_LINE) BbGpuReply replies[BBGPU_REQUEST_SLOTS];
    uint32_t reply_slots_used; /* game side: bit mask of slots in use */
    /* M4: the last protect request the game applied (futex word; compare as signed u32 deltas). */
    BBGPU_ALIGNAS(BBGPU_CACHE_LINE) uint32_t protect_done;
    BbGpuRing submit, events, protect, faults;
    /* Game side, for the spike log: faults on GPU-tracked pages relayed to the host, and the
     * time the faulting game threads waited for them (steady clock ns). */
    BBGPU_ALIGNAS(BBGPU_CACHE_LINE) uint64_t game_faults, game_fault_ns;
    /* Game side, at each flip submit: CPU time (ns) so far of the game's main thread and of the
     * thread that submits flips, for the spike log ("busy or waiting"). */
    uint64_t game_main_cpu_ns, game_flip_cpu_ns;
    /* ... and over the last game frame: all guest threads' CPU time, the busiest thread's. */
    uint64_t game_frame_cpu_ns, game_busiest_ns;
    char game_busiest_name[32];
} BbGpuShared;

#define BBGPU_SUBMIT_RING_OFFSET (UINT64_C(64) << 10)
#define BBGPU_EVENT_RING_OFFSET (BBGPU_SUBMIT_RING_OFFSET + BBGPU_SUBMIT_RING_BYTES)
BBGPU_STATIC_ASSERT(sizeof(BbGpuShared) <= BBGPU_SUBMIT_RING_OFFSET, "control block fits before the rings");
#define BBGPU_PROTECT_RING_OFFSET (BBGPU_EVENT_RING_OFFSET + BBGPU_EVENT_RING_BYTES)
#define BBGPU_FAULT_RING_OFFSET (BBGPU_PROTECT_RING_OFFSET + BBGPU_PROTECT_RING_BYTES)
BBGPU_STATIC_ASSERT(BBGPU_FAULT_RING_OFFSET + BBGPU_FAULT_RING_BYTES <= BBGPU_CONTROL_SIZE, "rings fit");
BBGPU_STATIC_ASSERT(sizeof(BbGpuSubmit) % 8 == 0 && sizeof(BbGpuFlipEvent) % 8 == 0, "record alignment");

static inline uint32_t bbgpu_submit_bytes(uint32_t count) {
    /* header .. flip_arg, then the four arrays packed to `count` entries */
    const uint32_t bytes = (uint32_t)offsetof(BbGpuSubmit, dcb) + count * (8 + 8 + 4 + 4);
    return (bytes + 7) & ~7u;
}
/* Packs/unpacks the arrays of a submit record (count entries each, in dcb, ccb, dcb_bytes,
 * ccb_bytes order) at the end of the fixed part. */
static inline void bbgpu_submit_pack(BbGpuSubmit *s) {
    uint8_t *p = (uint8_t *)s + offsetof(BbGpuSubmit, dcb);
    const uint32_t n = s->count;
    memmove(p + n * 8, s->ccb, n * 8);
    memmove(p + n * 16, s->dcb_bytes, n * 4);
    memmove(p + n * 20, s->ccb_bytes, n * 4);
}
static inline void bbgpu_submit_unpack(const BbGpuRecord *record, BbGpuSubmit *out) {
    const BbGpuSubmit *s = (const BbGpuSubmit *)record;
    const uint32_t n = s->count <= BBGPU_SUBMIT_MAX ? s->count : BBGPU_SUBMIT_MAX;
    memcpy(out, s, offsetof(BbGpuSubmit, dcb));
    out->count = n;
    const uint8_t *p = (const uint8_t *)s + offsetof(BbGpuSubmit, dcb);
    memcpy(out->dcb, p, n * 8);
    memcpy(out->ccb, p + n * 8, n * 8);
    memcpy(out->dcb_bytes, p + n * 16, n * 4);
    memcpy(out->ccb_bytes, p + n * 20, n * 4);
}

/* ---- waits ---- */
static inline uint64_t bbgpu_now_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }

static inline void bbgpu_cpu_relax(void) {
#if defined(__aarch64__)
    __asm__ volatile("yield");
#else
    __asm__ volatile("pause");
#endif
}

/* Waits while *word == value, up to timeout_ns. 0 when the word changed (or was woken). */
static inline int bbgpu_wait_word(uint32_t *word, uint32_t value, uint64_t timeout_ns) {
    for (int i = 0; i < 200; ++i) {
        if (__atomic_load_n(word, __ATOMIC_ACQUIRE) != value) return 0;
        bbgpu_cpu_relax();
    }
    const int r = os_sync_wait_on_address_with_timeout(word, value, 4, OS_SYNC_WAIT_ON_ADDRESS_SHARED,
                                                       OS_CLOCK_MACH_ABSOLUTE_TIME, timeout_ns ? timeout_ns : 1);
    if (r < 0 && errno == ETIMEDOUT) return __atomic_load_n(word, __ATOMIC_ACQUIRE) == value ? -1 : 0;
    return 0;
}
static inline void bbgpu_wake_word(uint32_t *word) {
    os_sync_wake_by_address_all(word, 4, OS_SYNC_WAKE_BY_ADDRESS_SHARED); /* ENOENT without waiters */
}

/* ---- ring ---- */
static inline void bbgpu_ring_init(BbGpuShared *s, BbGpuRing *ring, uint64_t offset, uint64_t capacity) {
    memset(ring, 0, sizeof *ring);
    ring->offset = offset;
    ring->capacity = capacity;
    (void)s;
}

static inline uint8_t *bbgpu_ring_data(BbGpuShared *s, BbGpuRing *ring) {
    return (uint8_t *)s + ring->offset;
}

/* Reserves `bytes` (a multiple of 8, <= capacity / 2) for the next record and returns where to
 * write it, or NULL after timeout_ns while the ring stays full. The producer lock must be held
 * from reserve to commit. Inserts a PAD record when the record would cross the end. */
static inline void *bbgpu_ring_reserve(BbGpuShared *s, BbGpuRing *ring, uint32_t bytes, uint64_t timeout_ns) {
    const uint64_t capacity = ring->capacity, head = ring->head; /* only this producer writes head */
    const uint64_t at = head % capacity;
    const uint64_t pad = at + bytes > capacity ? capacity - at : 0;
    const uint64_t deadline = bbgpu_now_ns() + timeout_ns;
    for (;;) {
        const uint64_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);
        if (head + pad + bytes - tail <= capacity) break;
        const uint32_t seq = __atomic_load_n(&ring->tail_seq, __ATOMIC_ACQUIRE);
        __atomic_store_n(&ring->producer_waiting, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&ring->tail, __ATOMIC_SEQ_CST) == tail) {
            const uint64_t now = bbgpu_now_ns();
            if (now >= deadline) return NULL;
            bbgpu_wait_word(&ring->tail_seq, seq, deadline - now < 50000000 ? deadline - now : 50000000);
        }
    }
    uint8_t *data = bbgpu_ring_data(s, ring);
    ring->reserved_pad = pad; /* the PAD record is published with the record, in commit */
    if (pad) {
        BbGpuRecord filler = {BBGPU_REC_PAD, (uint32_t)pad};
        memcpy(data + at, &filler, sizeof filler);
        return data;
    }
    return data + at;
}

/* Publishes the record written at the reservation and rings the doorbell. */
static inline void bbgpu_ring_commit(BbGpuRing *ring, uint32_t bytes) {
    __atomic_store_n(&ring->head, ring->head + ring->reserved_pad + bytes, __ATOMIC_RELEASE);
    __atomic_fetch_add(&ring->head_seq, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&ring->consumer_waiting, __ATOMIC_SEQ_CST)) bbgpu_wake_word(&ring->head_seq);
}

/* Copies a whole record (r->bytes) into the ring. 0 on success, -1 on timeout. */
static inline int bbgpu_ring_push(BbGpuShared *s, BbGpuRing *ring, const void *record, uint64_t timeout_ns) {
    const uint32_t bytes = ((const BbGpuRecord *)record)->bytes;
    void *at = bbgpu_ring_reserve(s, ring, bytes, timeout_ns);
    if (!at) return -1;
    memcpy(at, record, bytes);
    bbgpu_ring_commit(ring, bytes);
    return 0;
}

/* The next record, or NULL after timeout_ns with the ring empty. Skips PAD records. Valid until
 * bbgpu_ring_pop. Single consumer. */
static inline const BbGpuRecord *bbgpu_ring_peek(BbGpuShared *s, BbGpuRing *ring, uint64_t timeout_ns) {
    const uint64_t deadline = bbgpu_now_ns() + timeout_ns;
    for (;;) {
        const uint64_t tail = ring->tail; /* only this consumer writes tail */
        const uint64_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);
        if (head != tail) {
            const BbGpuRecord *r = (const BbGpuRecord *)(bbgpu_ring_data(s, ring) + tail % ring->capacity);
            if (r->type != BBGPU_REC_PAD) return r;
            __atomic_store_n(&ring->tail, tail + r->bytes, __ATOMIC_RELEASE);
            continue;
        }
        const uint32_t seq = __atomic_load_n(&ring->head_seq, __ATOMIC_ACQUIRE);
        __atomic_store_n(&ring->consumer_waiting, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&ring->head, __ATOMIC_SEQ_CST) == tail) {
            const uint64_t now = bbgpu_now_ns();
            if (now >= deadline) {
                __atomic_store_n(&ring->consumer_waiting, 0, __ATOMIC_RELAXED);
                return NULL;
            }
            bbgpu_wait_word(&ring->head_seq, seq, deadline - now < 50000000 ? deadline - now : 50000000);
        }
        __atomic_store_n(&ring->consumer_waiting, 0, __ATOMIC_RELAXED);
    }
}

static inline void bbgpu_ring_pop(BbGpuRing *ring, const BbGpuRecord *record) {
    __atomic_store_n(&ring->tail, ring->tail + record->bytes, __ATOMIC_RELEASE);
    __atomic_fetch_add(&ring->tail_seq, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&ring->producer_waiting, __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&ring->producer_waiting, 0, __ATOMIC_RELAXED);
        bbgpu_wake_word(&ring->tail_seq);
    }
}

static inline void bbgpu_shared_init(BbGpuShared *s) {
    memset(s, 0, sizeof *s);
    s->magic = BBGPU_SHARED_MAGIC;
    s->version = BBGPU_SHARED_VERSION;
    s->size = BBGPU_SHARED_SIZE;
    bbgpu_ring_init(s, &s->submit, BBGPU_SUBMIT_RING_OFFSET, BBGPU_SUBMIT_RING_BYTES);
    bbgpu_ring_init(s, &s->events, BBGPU_EVENT_RING_OFFSET, BBGPU_EVENT_RING_BYTES);
    bbgpu_ring_init(s, &s->protect, BBGPU_PROTECT_RING_OFFSET, BBGPU_PROTECT_RING_BYTES);
    bbgpu_ring_init(s, &s->faults, BBGPU_FAULT_RING_OFFSET, BBGPU_FAULT_RING_BYTES);
}

/* ---- request slots (game side): claim, push the request, wait, release ---- */
static inline int bbgpu_reply_claim(BbGpuShared *s) {
    for (;;) {
        uint32_t used = __atomic_load_n(&s->reply_slots_used, __ATOMIC_ACQUIRE);
        if (used == UINT32_MAX) { bbgpu_cpu_relax(); continue; }
        const int slot = __builtin_ctz(~used);
        if (__atomic_compare_exchange_n(&s->reply_slots_used, &used, used | (1u << slot), 0, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&s->replies[slot].state, 1, __ATOMIC_RELEASE);
            return slot;
        }
    }
}
/* Waits for the host's answer; -1 when timeout_ns passes first (the result is then unknown). */
static inline int bbgpu_reply_wait(BbGpuShared *s, int slot, int64_t *result, uint64_t timeout_ns) {
    const uint64_t deadline = bbgpu_now_ns() + timeout_ns;
    while (__atomic_load_n(&s->replies[slot].state, __ATOMIC_ACQUIRE) == 1) {
        const uint64_t now = bbgpu_now_ns();
        if (now >= deadline) return -1;
        bbgpu_wait_word(&s->replies[slot].state, 1, deadline - now < 50000000 ? deadline - now : 50000000);
    }
    *result = s->replies[slot].result;
    __atomic_fetch_and(&s->reply_slots_used, ~(1u << slot), __ATOMIC_RELEASE);
    return 0;
}
/* Host side: answers a request. */
static inline void bbgpu_reply_send(BbGpuShared *s, uint32_t slot, int64_t result) {
    if (slot >= BBGPU_REQUEST_SLOTS) return;
    s->replies[slot].result = result;
    __atomic_store_n(&s->replies[slot].state, 2, __ATOMIC_RELEASE);
    bbgpu_wake_word(&s->replies[slot].state);
}
