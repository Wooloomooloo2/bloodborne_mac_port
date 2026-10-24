// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
/* Wire protocol between bb-probe (x86-64, Rosetta) and bbgpu-host (native arm64): Mach
 * messages over ports exchanged at launch (docs/DESIGN_NATIVE_GPU_PROCESS.md 3.2, 3.6).
 * Shared by src/gpu_process.c and host/main.c; both sides are little-endian LP64, so the
 * structures have the same layout (asserted below).
 *
 * Handshake (M0 mechanism, tools/m0_spike/shared_mem):
 *   1. bb-probe allocates a receive port, mach_ports_register()s it just before posix_spawn of
 *      bbgpu-host (arm64 slice), then restores the previously registered ports.
 *   2. bbgpu-host mach_ports_lookup()s it, clears its own registration, and sends HELLO with a
 *      MAKE_SEND right to its own receive port (the host port) in msgh_local_port.
 *   3. bb-probe replies (to the host port) with SETUP: send rights to the pool memory entry and
 *      to a fresh control port (host -> runtime), plus the pool size and the guest range.
 *   4. bbgpu-host reserves the guest range, checks the entry, and sends READY on the control port.
 * Then (M3): view events, verification requests and all GPU work go through the submit ring of
 * the shared control block (host/bbgpu_shared.h), in one order; VERIFY_REPLY still comes back on
 * the control port. Each side requests a dead-name notification for the other's port, so either
 * exits/falls back when the peer dies. (M2 sent EVENTS/VERIFY as Mach messages to the host port.)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <mach/mach.h>
#include <stdint.h>

#define BBGPU_PROTOCOL_VERSION 2u /* 2: shared control block entry in SETUP (M3) */
#define BBGPU_GUEST_MIN UINT64_C(0x7000000000)
#define BBGPU_GUEST_MAX UINT64_C(0xfc00000000)
#define BBGPU_GUEST_PAGE UINT64_C(16384)

enum {
    BBGPU_MSG_HELLO = 0x42424730,  /* host -> runtime (handshake port) */
    BBGPU_MSG_SETUP,               /* runtime -> host: entry + control port */
    BBGPU_MSG_READY,               /* host -> runtime (control port) */
    BBGPU_MSG_EVENTS,              /* runtime -> host: view events */
    BBGPU_MSG_VERIFY,              /* runtime -> host: snapshot to compare */
    BBGPU_MSG_VERIFY_REPLY,        /* host -> runtime */
    BBGPU_MSG_SHUTDOWN,            /* runtime -> host */
};

/* View events, applied in order. Addresses and sizes are multiples of BBGPU_GUEST_PAGE. */
enum { BBGPU_EV_MAP = 1, BBGPU_EV_UNMAP = 2, BBGPU_EV_PROTECT = 3 };
typedef struct {
    uint32_t op, prot;           /* prot: PS4 protection bits (map, protect) */
    uint64_t address, size, phys; /* phys: offset in the pool entry (map) */
} BbGpuEvent;

#define BBGPU_EVENTS_MAX 64
#define BBGPU_SAMPLES_MAX 8

typedef struct {
    mach_msg_header_t h;
    uint32_t version, pid;
} BbGpuHello;

typedef struct {
    mach_msg_header_t h;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t entry, control, shared; /* shared: control block (bbgpu_shared.h) */
    uint32_t version, pid;
    uint64_t entry_size, guest_min, guest_max;
} BbGpuSetup;

typedef struct {
    mach_msg_header_t h;
    uint32_t pid, page_size;
    int32_t native, window;      /* native: not translated by Rosetta; window: SDL window opened */
    int32_t status;              /* 0 ok, else a kern_return_t / errno of the failed step */
    uint32_t pad;
} BbGpuReady;

typedef struct {
    mach_msg_header_t h;
    uint32_t count, pad;
    BbGpuEvent events[BBGPU_EVENTS_MAX];
} BbGpuEvents;

typedef struct { uint64_t address, size, checksum; } BbGpuSample;

typedef struct {
    mach_msg_header_t h;
    uint64_t seq;
    uint64_t views, bytes;        /* runtime: non-reserved views at the snapshot */
    uint64_t events;              /* runtime: events sent before this request */
    uint32_t count, pad;
    BbGpuSample samples[BBGPU_SAMPLES_MAX];
} BbGpuVerify;

typedef struct {
    mach_msg_header_t h;
    uint64_t seq;
    uint64_t views, bytes, events; /* host: mirrored views/bytes, events applied */
    uint64_t failures;             /* host: events whose map/unmap failed */
    uint32_t count, pad;
    uint64_t checksums[BBGPU_SAMPLES_MAX];
    uint32_t mapped[BBGPU_SAMPLES_MAX]; /* 1: the sample range is fully mirrored */
} BbGpuVerifyReply;

_Static_assert(sizeof(BbGpuEvent) == 32, "event layout");
_Static_assert(sizeof(BbGpuEvents) == sizeof(mach_msg_header_t) + 8 + 32 * BBGPU_EVENTS_MAX, "events layout");

/* Checksum of guest bytes (size a multiple of 8), streamed: state starts at BBGPU_CHECKSUM_SEED. */
#define BBGPU_CHECKSUM_SEED UINT64_C(0xcbf29ce484222325)
static inline uint64_t bbgpu_checksum(uint64_t h, const void *data, uint64_t size) {
    const uint64_t *p = (const uint64_t *)data;
    for (uint64_t i = 0; i < size / 8; ++i) {
        h ^= p[i] + (h << 6) + (h >> 2);
        h *= UINT64_C(0x100000001b3);
    }
    return h;
}
