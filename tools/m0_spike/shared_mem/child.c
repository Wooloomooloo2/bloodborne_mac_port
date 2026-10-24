// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
/* M0 spike child: native arm64. Receives the memory entry from the x86-64
 * parent, maps it at the same VA and serves the parent's test commands.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define ROLE "child/arm64"
#include "common.h"

#include <servers/bootstrap.h>
#include <pthread.h>
#include <sys/mman.h>

static mach_port_t entry_port, parent_port, child_port, sem_p2c, sem_c2p;

/* ---- doorbell primitives (child side) ------------------------------------- */
static void mach_rcv(void) {
    ping_rcv_t m;
    kern_return_t kr = mach_msg(&m.h, MACH_RCV_MSG, 0, sizeof m, child_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (kr) DIE("mach_msg rcv: %s", mach_error_string(kr));
}
static void mach_send(int and_rcv) {
    ping_rcv_t m;
    memset(&m, 0, sizeof m);
    m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    m.h.msgh_size = sizeof(mach_msg_header_t);
    m.h.msgh_remote_port = parent_port;
    m.h.msgh_id = PING_MSG_ID;
    kern_return_t kr = mach_msg(&m.h, MACH_SEND_MSG | (and_rcv ? MACH_RCV_MSG : 0), sizeof(mach_msg_header_t),
                                and_rcv ? sizeof m : 0, and_rcv ? child_port : MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE,
                                MACH_PORT_NULL);
    if (kr) DIE("mach_msg send: %s", mach_error_string(kr));
}
/* wait for ping == i */
static void wait_ping(int mech, uint32_t i, int first) {
    switch (mech) {
    case MECH_OSSYNC:
        for (;;) {
            uint32_t cur = ld_acq32(&CTL->ping);
            if (cur == i) return;
            os_sync_wait_on_address(&CTL->ping, cur, 4, OS_SYNC_WAIT_ON_ADDRESS_SHARED);
        }
    case MECH_SEM:
        while (semaphore_wait(sem_p2c) != KERN_SUCCESS) {}
        break;
    case MECH_MACH:
        if (first) mach_rcv(); /* later receives are folded into the reply send */
        break;
    case MECH_SPIN:
        while (ld_acq32(&CTL->ping) != i) cpu_relax();
        return;
    }
    if (ld_acq32(&CTL->ping) != i) DIE("doorbell %d: ping %u != %u", mech, CTL->ping, i);
}
/* reply pong = i; for Mach, also receive the next ping unless last */
static void reply_pong(int mech, uint32_t i, int last) {
    switch (mech) {
    case MECH_OSSYNC: store_wake32(&CTL->pong, i); break;
    case MECH_SEM: st_rel32(&CTL->pong, i); semaphore_signal(sem_c2p); break;
    case MECH_MACH: st_rel32(&CTL->pong, i); mach_send(!last); break;
    case MECH_SPIN: st_rel32(&CTL->pong, i); break;
    }
}

static volatile int lookup_done;
static kern_return_t lookup_kr;
static void *lookup_thread(void *arg) {
    (void)arg;
    mach_port_t svc;
    lookup_kr = bootstrap_look_up(bootstrap_port, "com.apple.system.notification_center", &svc);
    __atomic_store_n(&lookup_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void handshake(void) {
    const char *xfer = getenv("SPIKE_XFER");
    mach_port_t boot_special = MACH_PORT_NULL;
    if (xfer && !strcmp(xfer, "bootstrap")) {
        KCHECK(task_get_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, &boot_special), "task_get_special_port");
        parent_port = boot_special;
    } else {
        mach_port_array_t ports; mach_msg_type_number_t n = 0;
        KCHECK(mach_ports_lookup(mach_task_self(), &ports, &n), "mach_ports_lookup");
        if (n < 1 || ports[0] == MACH_PORT_NULL) DIE("no registered port inherited (n=%u)", n);
        parent_port = ports[0];
        mach_ports_register(mach_task_self(), NULL, 0); /* don't pass it on further */
    }
    KCHECK(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &child_port), "port alloc");

    hs_hello_t h;
    memset(&h, 0, sizeof h);
    h.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND);
    h.h.msgh_size = sizeof h;
    h.h.msgh_remote_port = parent_port;
    h.h.msgh_local_port = child_port;
    h.h.msgh_id = HS_MSG_ID;
    h.pid = (uint64_t)getpid();
    KCHECK(mach_msg(&h.h, MACH_SEND_MSG, sizeof h, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL), "hello send");

    hs_reply_rcv_t r;
    memset(&r, 0, sizeof r);
    KCHECK(mach_msg(&r.m.h, MACH_RCV_MSG, 0, sizeof r, child_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL), "reply rcv");
    if (r.m.h.msgh_id != HS_REPLY_ID || r.m.b.msgh_descriptor_count != 4) DIE("bad handshake reply");
    entry_port = r.m.entry.name;
    sem_p2c = r.m.sem_p2c.name;
    sem_c2p = r.m.sem_c2p.name;
    if (boot_special) {
        /* restore the real bootstrap port so launchd/XPC (Metal, SDL...) work */
        KCHECK(task_set_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, r.m.bootstrap.name), "task_set_special_port");
        bootstrap_port = r.m.bootstrap.name;
    } else {
        mach_port_deallocate(mach_task_self(), r.m.bootstrap.name);
    }

    mach_vm_address_t at = GUEST_BASE;
    KCHECK(mach_vm_map(mach_task_self(), &at, r.m.size, 0, VM_FLAGS_FIXED, entry_port, 0, FALSE,
                       VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE),
           "child mach_vm_map(entry) @guest base");
    CTL->child_page_size = (uint32_t)getpagesize();

    /* launchd lookups must still work (the real host needs WindowServer/Metal XPC).
     * Run one on a thread with a 2 s timeout: with the bootstrap-port transfer it hangs. */
    pthread_t th;
    pthread_create(&th, NULL, lookup_thread, NULL);
    for (int i = 0; i < 200 && !__atomic_load_n(&lookup_done, __ATOMIC_ACQUIRE); i++) usleep(10000);
    int done = __atomic_load_n(&lookup_done, __ATOMIC_ACQUIRE);
    CTL->bootstrap_lookup = done ? (lookup_kr ? 2 : 1) : 3;
    store_wake32(&CTL->child_ready, 1);
}

int main(void) {
    handshake();
    uint32_t seen = 0;
    for (;;) {
        wait_eq32(&CTL->cmd_seq, seen + 1);
        seen++;
        uint64_t a0 = CTL->a0, a1 = CTL->a1, a2 = CTL->a2, res = 0, res2 = 0;
        uint32_t c = CTL->cmd;
        switch (c) {
        case CMD_VERIFY_PATTERN: res = verify_pattern(a0, a1, a2); break;
        case CMD_WRITE_PATTERN: write_pattern(a0, a1, a2); break;
        case CMD_CHECK_ZERO: res = count_nonzero(a0, a1); break;
        case CMD_RING_CONSUME: res = ring_consume(RING_A, a0, (int)a1); break;
        case CMD_RING_PRODUCE: ring_produce(RING_B, a0, (int)a1); break;
        case CMD_PINGPONG:
            for (uint32_t i = 1; i <= a0; i++) {
                wait_ping((int)a1, i, i == 1);
                reply_pong((int)a1, i, i == a0);
            }
            break;
        case CMD_FAULT_SERVER: {
            uint64_t handled = 0;
            for (uint32_t i = 1;; i++) {
                wait_ping((int)a1, i, i == 1);
                uint64_t a = CTL->fault_addr;
                int last = a == ~0ULL;
                if (!last) { /* "handle": look at the page through our (writable) view */
                    volatile uint64_t *p = (volatile uint64_t *)(a & ~0x3fffULL);
                    (void)p[0];
                    handled++;
                }
                reply_pong((int)a1, i, last);
                if (last) break;
            }
            res = handled;
            break;
        }
        case CMD_MAP_ENTRY_AT: {
            mach_vm_address_t at = GUEST_BASE + a2;
            res = mach_vm_map(mach_task_self(), &at, a1, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, entry_port, a0, FALSE,
                              VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
            break;
        }
        case CMD_PUNCH_MADV: {
            uint64_t t0 = now_ns();
            res = madvise((void *)(GUEST_BASE + a0), a1, MADV_ZERO) ? (uint64_t)errno : 0;
            res2 = now_ns() - t0;
            break;
        }
        case CMD_PUNCH_BEHAVIOR: {
            uint64_t t0 = now_ns();
            res = (uint64_t)mach_vm_behavior_set(mach_task_self(), GUEST_BASE + a0, a1, VM_BEHAVIOR_ZERO);
            res2 = now_ns() - t0;
            break;
        }
        case CMD_EXIT: break;
        default: DIE("unknown command %u", c);
        }
        CTL->result = res;
        CTL->result2 = res2;
        store_wake32(&CTL->ack_seq, seen);
        if (c == CMD_EXIT) return 0;
    }
}
