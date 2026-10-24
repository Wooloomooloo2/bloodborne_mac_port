// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
/* Shared control block rings (host/bbgpu_shared.h) between an x86-64 parent under Rosetta and a
 * native arm64 child, the bb-probe / bbgpu-host pair. The block is a Mach memory entry mapped at
 * different addresses on each side. Checks variable-size records in order (with wrap-around),
 * the event ring the other way at the same time, request/reply slots, and doorbell latency.
 * Build: the same source as both architectures (build_macos.sh --test).
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "host/bbgpu_shared.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define RECORDS 2000000u
#define EVENTS 500000u
#define REQUESTS 20000u
#define SECOND UINT64_C(1000000000)

static int translated(void) {
    int value = 0;
    size_t size = sizeof value;
    return sysctlbyname("sysctl.proc_translated", &value, &size, NULL, 0) == 0 && value;
}

static uint64_t mix(uint64_t x) {
    x ^= x >> 33;
    x *= UINT64_C(0xff51afd7ed558ccd);
    x ^= x >> 33;
    return x;
}

static BbGpuShared *map_entry(mach_port_t entry) {
    mach_vm_address_t at = 0;
    kern_return_t kr = mach_vm_map(mach_task_self(), &at, BBGPU_SHARED_SIZE, 0, VM_FLAGS_ANYWHERE, entry, 0, FALSE,
                                   VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "mach_vm_map: %s\n", mach_error_string(kr));
        exit(1);
    }
    return (BbGpuShared *)(uintptr_t)at;
}

/* A submit-like record: count pairs derived from the sequence number, contents checkable. */
static uint32_t make_record(BbGpuSubmit *s, uint32_t seq) {
    const uint32_t count = 1 + (uint32_t)(mix(seq) % BBGPU_SUBMIT_MAX);
    memset(s, 0, sizeof *s);
    s->r.type = BBGPU_REC_SUBMIT_GFX;
    s->count = count;
    s->vo_handle = seq;
    for (uint32_t i = 0; i < count; ++i) {
        s->dcb[i] = mix(seq * 131u + i);
        s->ccb[i] = ~s->dcb[i];
        s->dcb_bytes[i] = (uint32_t)s->dcb[i];
        s->ccb_bytes[i] = seq + i;
    }
    bbgpu_submit_pack(s);
    s->r.bytes = bbgpu_submit_bytes(count);
    return s->r.bytes;
}

/* ---- child (arm64): consumes the submit ring, produces events, answers requests ---- */
static BbGpuShared *child_block;

static void *child_events(void *unused) {
    (void)unused;
    for (uint32_t i = 0; i < EVENTS; ++i) {
        BbGpuFlipEvent e = {{BBGPU_EV_FLIP, sizeof e}, 1, (int32_t)i, i & 1, 0, (int64_t)mix(i), i, ~(uint64_t)i};
        if (bbgpu_ring_push(child_block, &child_block->events, &e, 5 * SECOND)) {
            fprintf(stderr, "child: event ring stuck at %u\n", i);
            exit(2);
        }
    }
    return NULL;
}

static int child(void) {
    if (translated()) {
        fprintf(stderr, "child: runs under Rosetta, expected native arm64\n");
        return 1;
    }
    mach_port_array_t ports = NULL;
    mach_msg_type_number_t n = 0;
    if (mach_ports_lookup(mach_task_self(), &ports, &n) != KERN_SUCCESS || n < 1 || !ports[0]) {
        fprintf(stderr, "child: no entry inherited\n");
        return 1;
    }
    BbGpuShared *s = child_block = map_entry(ports[0]);
    if (s->magic != BBGPU_SHARED_MAGIC || s->version != BBGPU_SHARED_VERSION) {
        fprintf(stderr, "child: bad block\n");
        return 1;
    }
    pthread_t events;
    pthread_create(&events, NULL, child_events, NULL);
    BbGpuSubmit got, want;
    uint32_t seq = 0, requests = 0, errors = 0;
    for (;;) {
        const BbGpuRecord *r = bbgpu_ring_peek(s, &s->submit, 5 * SECOND);
        if (!r) {
            fprintf(stderr, "child: submit ring idle at record %u\n", seq);
            return 2;
        }
        if (r->type == BBGPU_REC_SUBMIT_GFX) {
            bbgpu_submit_unpack(r, &got);
            make_record(&want, seq);
            BbGpuSubmit unpacked;
            bbgpu_submit_unpack(&want.r, &unpacked);
            if (got.count != unpacked.count || got.vo_handle != seq ||
                memcmp(got.dcb, unpacked.dcb, got.count * 8) || memcmp(got.ccb, unpacked.ccb, got.count * 8) ||
                memcmp(got.dcb_bytes, unpacked.dcb_bytes, got.count * 4) ||
                memcmp(got.ccb_bytes, unpacked.ccb_bytes, got.count * 4)) {
                if (errors++ < 5) fprintf(stderr, "child: record %u corrupt\n", seq);
            }
            ++seq;
        } else if (r->type == BBGPU_REC_VO_OPEN) {
            const BbGpuVoOpen *o = (const BbGpuVoOpen *)r;
            bbgpu_reply_send(s, o->slot, (int64_t)o->user_id * 3 + 1);
            ++requests;
        } else if (r->type == BBGPU_REC_SUBMIT_DONE) {
            bbgpu_ring_pop(&s->submit, r);
            break;
        }
        bbgpu_ring_pop(&s->submit, r);
    }
    pthread_join(events, NULL);
    printf("child (arm64): %u records verified, %u corrupt, %u requests answered\n", seq, errors, requests);
    return errors || seq != RECORDS || requests != REQUESTS ? 3 : 0;
}

/* ---- parent (x86-64 under Rosetta): produces submits and requests, consumes events ---- */
static BbGpuShared *parent_block;
static pthread_mutex_t submit_lock = PTHREAD_MUTEX_INITIALIZER;

static void push_locked(const void *record) {
    pthread_mutex_lock(&submit_lock);
    if (bbgpu_ring_push(parent_block, &parent_block->submit, record, 5 * SECOND)) {
        fprintf(stderr, "parent: submit ring stuck\n");
        exit(2);
    }
    pthread_mutex_unlock(&submit_lock);
}

static uint64_t request_ns_total, request_ns_max;
static void *parent_requests(void *unused) {
    (void)unused;
    for (uint32_t i = 0; i < REQUESTS; ++i) {
        const uint64_t t0 = bbgpu_now_ns();
        const int slot = bbgpu_reply_claim(parent_block);
        BbGpuVoOpen o = {{BBGPU_REC_VO_OPEN, sizeof o}, (uint32_t)slot, i, 0, 0};
        push_locked(&o);
        int64_t result = 0;
        if (bbgpu_reply_wait(parent_block, slot, &result, 5 * SECOND) || result != (int64_t)i * 3 + 1) {
            fprintf(stderr, "parent: request %u got %lld\n", i, (long long)result);
            exit(2);
        }
        const uint64_t ns = bbgpu_now_ns() - t0;
        request_ns_total += ns;
        if (ns > request_ns_max) request_ns_max = ns;
    }
    return NULL;
}

static int parent(const char *child_path) {
    if (!translated()) {
        fprintf(stderr, "parent: run it as x86-64 (arch -x86_64), the bb-probe side\n");
        return 1;
    }
    mach_port_t entry = MACH_PORT_NULL;
    memory_object_size_t size = BBGPU_SHARED_SIZE;
    if (mach_make_memory_entry_64(mach_task_self(), &size, 0, MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE,
                                  &entry, MACH_PORT_NULL) != KERN_SUCCESS) {
        fprintf(stderr, "parent: memory entry\n");
        return 1;
    }
    BbGpuShared *s = parent_block = map_entry(entry);
    bbgpu_shared_init(s);

    mach_ports_register(mach_task_self(), &entry, 1);
    pid_t pid;
    char *argv[] = {(char *)child_path, (char *)"child", NULL};
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    cpu_type_t arm64 = CPU_TYPE_ARM64;
    size_t done = 0;
    posix_spawnattr_setbinpref_np(&attr, 1, &arm64, &done);
    const int err = posix_spawn(&pid, child_path, NULL, &attr, argv, environ);
    mach_ports_register(mach_task_self(), NULL, 0);
    if (err) {
        fprintf(stderr, "parent: spawn %s: %s\n", child_path, strerror(err));
        return 1;
    }

    pthread_t requests;
    pthread_create(&requests, NULL, parent_requests, NULL);
    const uint64_t t0 = bbgpu_now_ns();
    uint32_t events = 0, event_errors = 0;
    BbGpuSubmit record;
    for (uint32_t seq = 0; seq < RECORDS; ++seq) {
        make_record(&record, seq);
        push_locked(&record);
        /* drain events as they come, without blocking */
        for (const BbGpuRecord *r; (r = bbgpu_ring_peek(s, &s->events, 0));) {
            const BbGpuFlipEvent *e = (const BbGpuFlipEvent *)r;
            if (r->type != BBGPU_EV_FLIP || e->index != (int32_t)events || e->flip_arg != (int64_t)mix(events) ||
                e->tsc != ~(uint64_t)events)
                ++event_errors;
            ++events;
            bbgpu_ring_pop(&s->events, r);
        }
    }
    pthread_join(requests, NULL);
    const double seconds = (double)(bbgpu_now_ns() - t0) / 1e9;
    BbGpuRecord done_record = {BBGPU_REC_SUBMIT_DONE, sizeof done_record};
    push_locked(&done_record);
    while (events < EVENTS) {
        const BbGpuRecord *r = bbgpu_ring_peek(s, &s->events, 5 * SECOND);
        if (!r) break;
        const BbGpuFlipEvent *e = (const BbGpuFlipEvent *)r;
        if (r->type != BBGPU_EV_FLIP || e->index != (int32_t)events || e->tsc != ~(uint64_t)events) ++event_errors;
        ++events;
        bbgpu_ring_pop(&s->events, r);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    const int child_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("parent (x86-64/Rosetta): %u records in %.2f s (%.1f M/s, ring wrapped %llu times), %u events "
           "(%u wrong), %u requests (mean %.1f us, max %.1f us)\n",
           RECORDS, seconds, RECORDS / seconds / 1e6,
           (unsigned long long)(s->submit.head / s->submit.capacity), events, event_errors, REQUESTS,
           request_ns_total / 1e3 / REQUESTS, request_ns_max / 1e3);
    if (!child_ok || events != EVENTS || event_errors) {
        printf("FAIL: shared rings\n");
        return 1;
    }
    printf("PASS: shared rings x86-64 <-> arm64 (records in order across wrap-around, events, requests)\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "child")) return child();
    return parent(argc > 1 ? argv[1] : argv[0]);
}
