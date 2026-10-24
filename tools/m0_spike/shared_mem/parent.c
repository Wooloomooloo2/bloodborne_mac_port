// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
/* M0 spike parent: x86-64, runs under Rosetta 2. Owns the guest memory entry,
 * spawns the native arm64 child, and drives every test.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define ROLE "parent/x86_64"
#include "common.h"

#include <libgen.h>
#include <mach-o/dyld.h>
#include <servers/bootstrap.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

extern char **environ;

static mach_port_t entry_port, parent_port, child_port, sem_p2c, sem_c2p;
static uint32_t cseq;
static size_t ppage;
static int failures;

#define CHECK(cond, ...) do { int _ok = !!(cond); printf("  [%s] ", _ok ? "PASS" : "FAIL"); printf(__VA_ARGS__); putchar('\n'); if (!_ok) failures++; } while (0)

/* ---- command channel ------------------------------------------------------ */
static void cmd_start(uint32_t c, uint64_t a0, uint64_t a1, uint64_t a2) {
    CTL->cmd = c; CTL->a0 = a0; CTL->a1 = a1; CTL->a2 = a2;
    store_wake32(&CTL->cmd_seq, ++cseq);
}
static uint64_t cmd_wait(void) { wait_eq32(&CTL->ack_seq, cseq); return CTL->result; }
static uint64_t cmd(uint32_t c, uint64_t a0, uint64_t a1, uint64_t a2) { cmd_start(c, a0, a1, a2); return cmd_wait(); }

/* ---- doorbell primitives (parent side) ------------------------------------ */
static void mach_ping_send_recv(void) {
    struct { mach_msg_header_t h; mach_msg_max_trailer_t t; } m;
    memset(&m, 0, sizeof m);
    m.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    m.h.msgh_size = sizeof(mach_msg_header_t);
    m.h.msgh_remote_port = child_port;
    m.h.msgh_id = PING_MSG_ID;
    kern_return_t kr = mach_msg(&m.h, MACH_SEND_MSG | MACH_RCV_MSG, sizeof(mach_msg_header_t), sizeof m, parent_port,
                                MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    while (kr == MACH_RCV_TOO_LARGE || (kr == KERN_SUCCESS && m.h.msgh_id != PING_MSG_ID)) {
        /* stray (e.g. XPC sent to a hijacked bootstrap port); too-large ones are dropped by the kernel */
        if (kr == KERN_SUCCESS) mach_msg_destroy(&m.h);
        kr = mach_msg(&m.h, MACH_RCV_MSG, 0, sizeof m, parent_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
    if (kr) DIE("mach_msg ping: %s", mach_error_string(kr));
}
/* one round trip: signal child (ping=i), wait for pong=i */
static inline void ring_doorbell(int mech, uint32_t i) {
    switch (mech) {
    case MECH_OSSYNC:
        store_wake32(&CTL->ping, i);
        for (;;) {
            uint32_t cur = ld_acq32(&CTL->pong);
            if (cur == i) break;
            os_sync_wait_on_address(&CTL->pong, cur, 4, OS_SYNC_WAIT_ON_ADDRESS_SHARED);
        }
        break;
    case MECH_SEM:
        st_rel32(&CTL->ping, i);
        semaphore_signal(sem_p2c);
        while (semaphore_wait(sem_c2p) != KERN_SUCCESS) {}
        break;
    case MECH_MACH:
        st_rel32(&CTL->ping, i);
        mach_ping_send_recv();
        break;
    case MECH_SPIN:
        st_rel32(&CTL->ping, i);
        while (ld_acq32(&CTL->pong) != i) cpu_relax();
        break;
    }
}

/* ---- fault handling ------------------------------------------------------- */
enum { FM_JMP, FM_LOCAL, FM_RELAY };
static volatile int fault_mode, fault_mech, last_sig;
static volatile uint32_t fault_seq;
static sigjmp_buf fault_jmp;

static void on_fault(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    last_sig = sig;
    uintptr_t a = (uintptr_t)si->si_addr;
    if (fault_mode == FM_JMP) siglongjmp(fault_jmp, 1);
    if (a < GUEST_BASE || a >= GUEST_BASE + REGION_SIZE) { signal(sig, SIG_DFL); return; }
    if (fault_mode == FM_RELAY) {
        CTL->fault_addr = a;
        ring_doorbell(fault_mech, ++fault_seq);
    }
    mprotect((void *)(a & ~(uintptr_t)(ppage - 1)), ppage, PROT_READ | PROT_WRITE);
}

/* ---- setup ---------------------------------------------------------------- */
static void create_entry(const char *mode) {
    mach_vm_address_t at = GUEST_BASE;
    memory_object_size_t sz = REGION_SIZE;
    if (!strcmp(mode, "share")) {
        /* allocate anonymous memory at the guest base, then wrap it in an entry */
        KCHECK(mach_vm_allocate(mach_task_self(), &at, REGION_SIZE, VM_FLAGS_FIXED), "mach_vm_allocate @guest base");
        KCHECK(mach_make_memory_entry_64(mach_task_self(), &sz, GUEST_BASE, VM_PROT_READ | VM_PROT_WRITE | MAP_MEM_VM_SHARE,
                                         &entry_port, MACH_PORT_NULL),
               "mach_make_memory_entry_64(MAP_MEM_VM_SHARE)");
        /* re-map it through the entry so the parent's own view is literally the entry */
        KCHECK(mach_vm_map(mach_task_self(), &at, REGION_SIZE, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, entry_port, 0, FALSE,
                           VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE),
               "mach_vm_map(entry) @guest base");
    } else {
        /* create a fresh named object, then map it at the guest base */
        KCHECK(mach_make_memory_entry_64(mach_task_self(), &sz, 0,
                                         VM_PROT_READ | VM_PROT_WRITE | MAP_MEM_NAMED_CREATE, &entry_port, MACH_PORT_NULL),
               "mach_make_memory_entry_64(MAP_MEM_NAMED_CREATE)");
        KCHECK(mach_vm_map(mach_task_self(), &at, REGION_SIZE, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, entry_port, 0, FALSE,
                           VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE),
               "mach_vm_map(entry) @guest base");
    }
    if (sz != REGION_SIZE) DIE("entry size %llu != %llu", sz, REGION_SIZE);
    printf("  memory entry (%s): 0x%llx bytes mapped at 0x%llx\n", mode, REGION_SIZE, GUEST_BASE);
}

static pid_t spawn_child(const char *xfer) {
    char exe[4096]; uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n)) DIE("exe path");
    char path[4200];
    snprintf(path, sizeof path, "%s/child", dirname(exe));

    KCHECK(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &parent_port), "port alloc");
    KCHECK(mach_port_insert_right(mach_task_self(), parent_port, parent_port, MACH_MSG_TYPE_MAKE_SEND), "insert send");

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    if (!strcmp(xfer, "bootstrap")) {
        KCHECK(posix_spawnattr_setspecialport_np(&attr, parent_port, TASK_BOOTSTRAP_PORT), "setspecialport_np");
    } else {
        KCHECK(mach_ports_register(mach_task_self(), &parent_port, 1), "mach_ports_register");
    }
    setenv("SPIKE_XFER", xfer, 1);
    char *argv[] = {path, NULL};
    pid_t pid;
    int rc = posix_spawn(&pid, path, NULL, &attr, argv, environ);
    if (rc) DIE("posix_spawn %s: %s", path, strerror(rc));
    posix_spawnattr_destroy(&attr);
    if (strcmp(xfer, "bootstrap")) mach_ports_register(mach_task_self(), NULL, 0); /* un-register */
    return pid;
}

static void handshake(void) {
    hs_hello_rcv_t in;
    for (;;) {
        memset(&in, 0, sizeof in);
        kern_return_t kr = mach_msg(&in.m.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof in, parent_port, 10000, MACH_PORT_NULL);
        if (kr) DIE("handshake receive: %s", mach_error_string(kr));
        if (in.m.h.msgh_id == HS_MSG_ID) break;
        printf("  (ignoring stray message id %d on control port)\n", in.m.h.msgh_id);
        mach_msg_destroy(&in.m.h);
    }
    child_port = in.m.h.msgh_remote_port; /* send right to the child's port */
    KCHECK(semaphore_create(mach_task_self(), &sem_p2c, SYNC_POLICY_FIFO, 0), "semaphore_create");
    KCHECK(semaphore_create(mach_task_self(), &sem_c2p, SYNC_POLICY_FIFO, 0), "semaphore_create");

    hs_reply_t r;
    memset(&r, 0, sizeof r);
    r.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
    r.h.msgh_size = sizeof r;
    r.h.msgh_remote_port = child_port;
    r.h.msgh_id = HS_REPLY_ID;
    r.b.msgh_descriptor_count = 4;
    mach_msg_port_descriptor_t *d[4] = {&r.entry, &r.sem_p2c, &r.sem_c2p, &r.bootstrap};
    mach_port_t p[4] = {entry_port, sem_p2c, sem_c2p, bootstrap_port};
    for (int i = 0; i < 4; i++) {
        d[i]->name = p[i];
        d[i]->disposition = MACH_MSG_TYPE_COPY_SEND;
        d[i]->type = MACH_MSG_PORT_DESCRIPTOR;
    }
    r.size = REGION_SIZE;
    KCHECK(mach_msg(&r.h, MACH_SEND_MSG, sizeof r, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL), "handshake send");
    wait_eq32(&CTL->child_ready, 1);
    printf("  child mapped the entry at 0x%llx (child page size %u)\n", GUEST_BASE, CTL->child_page_size);
    static const char *const lk[] = {"?", "ok", "error", "HUNG (libxpc cached the hijacked bootstrap port)"};
    CHECK(CTL->bootstrap_lookup == 1, "child bootstrap_look_up (launchd) works: %s", lk[CTL->bootstrap_lookup & 3]);
}

/* ---- (a) shared memory tests ---------------------------------------------- */
static void test_shared(void) {
    puts("\n== (a) shared memory at identical VAs ==");
    const uint64_t len = 4 << 20;

    write_pattern(DATA_OFF, len, 1);
    CHECK(cmd(CMD_VERIFY_PATTERN, DATA_OFF, len, 1) == 0, "child sees 4 MiB written by parent at the same VA");
    cmd(CMD_WRITE_PATTERN, DATA_OFF, len, 2);
    CHECK(verify_pattern(DATA_OFF, len, 2) == 0, "parent sees 4 MiB written by child at the same VA");

    /* host-style view: child maps entry offset X at a different guest VA (va != phys) */
    const uint64_t view_va_off = 0x30000000ULL;
    CHECK(cmd(CMD_MAP_ENTRY_AT, DATA_OFF, len, view_va_off) == 0, "child maps entry offset 0x%llx at VA 0x%llx",
          DATA_OFF, GUEST_BASE + view_va_off);
    write_pattern(DATA_OFF, len, 3);
    CHECK(cmd(CMD_VERIFY_PATTERN, view_va_off, len, 3) == 0, "child's offset view sees parent writes");

    /* parent aliases */
    mach_vm_address_t a1 = 0, a2 = 0; vm_prot_t cur, max;
    KCHECK(mach_vm_remap(mach_task_self(), &a1, len, 0, VM_FLAGS_ANYWHERE, mach_task_self(), GUEST_BASE + DATA_OFF, FALSE,
                         &cur, &max, VM_INHERIT_NONE), "mach_vm_remap alias");
    KCHECK(mach_vm_map(mach_task_self(), &a2, len, 0, VM_FLAGS_ANYWHERE, entry_port, DATA_OFF, FALSE,
                       VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE), "mach_vm_map alias");
    cmd(CMD_WRITE_PATTERN, DATA_OFF, len, 4);
    CHECK(verify_pattern_at(a1, len, 4) == 0, "parent mach_vm_remap alias (0x%llx) sees child writes", a1);
    CHECK(verify_pattern_at(a2, len, 4) == 0, "parent mach_vm_map(entry) alias (0x%llx) sees child writes", a2);
    for (uint64_t i = 0; i < len / 8; i++) ((volatile uint64_t *)a1)[i] = pat(5, i);
    CHECK(cmd(CMD_VERIFY_PATTERN, DATA_OFF, len, 5) == 0, "child sees writes made through parent remap alias");
    for (uint64_t i = 0; i < len / 8; i++) ((volatile uint64_t *)a2)[i] = pat(6, i);
    CHECK(cmd(CMD_VERIFY_PATTERN, DATA_OFF, len, 6) == 0, "child sees writes made through parent entry alias");

    /* protection: parent RO, child stays RW */
    const uint64_t poff = DATA_OFF + 0x200000;
    volatile uint64_t *pp = (volatile uint64_t *)(GUEST_BASE + poff);
    write_pattern(poff, 0x4000, 7);
    CHECK(mprotect((void *)pp, 0x4000, PROT_READ) == 0, "parent mprotect(16 KiB, PROT_READ)");
    cmd(CMD_WRITE_PATTERN, poff, 0x4000, 8);
    CHECK(verify_pattern(poff, 0x4000, 8) == 0, "child writes into parent-RO page; parent reads them");
    fault_mode = FM_JMP; last_sig = 0;
    int faulted = 0;
    if (sigsetjmp(fault_jmp, 1) == 0) { pp[0] = 0xdead; } else faulted = 1;
    CHECK(faulted, "parent write to its RO view faults (signal %d = %s)", last_sig,
          last_sig == SIGBUS ? "SIGBUS" : last_sig == SIGSEGV ? "SIGSEGV" : "none");
    CHECK(cmd(CMD_VERIFY_PATTERN, poff, 0x4000, 8) == 0, "child data intact after parent's faulting write");
    uint64_t alias_off = poff - DATA_OFF;
    ((volatile uint64_t *)(a1 + alias_off))[0] = 0x1234;
    CHECK(pp[0] == 0x1234, "parent alias stays writable while primary view is RO (protection is per-view)");
    mprotect((void *)pp, 0x4000, PROT_READ | PROT_WRITE);
}

/* ---- hole punching -------------------------------------------------------- */
static const char *errs(int rc) { return rc == 0 ? "ok" : strerror(errno); }
static void test_punch(void) {
    puts("\n== (a) hole punching on shared memory ==");
    const uint64_t off = DATA_OFF + 0x800000, len = 1 << 20;
    void *p = (void *)(GUEST_BASE + off);
    uint64_t t0, t1;
    int rc;

    /* P1 parent madvise(MADV_ZERO) */
    write_pattern(off, len, 11);
    t0 = now_ns(); rc = madvise(p, len, MADV_ZERO); t1 = now_ns();
    printf("  P1 parent madvise(MADV_ZERO): %s (%.1f us)\n", errs(rc), (t1 - t0) / 1e3);
    if (!rc) CHECK(cmd(CMD_CHECK_ZERO, off, len, 0) == 0 && count_nonzero(off, len) == 0, "P1 both sides see zeros");

    /* P2 parent mach_vm_behavior_set(VM_BEHAVIOR_ZERO) */
    write_pattern(off, len, 12);
    t0 = now_ns();
    kern_return_t kr = mach_vm_behavior_set(mach_task_self(), (mach_vm_address_t)p, len, VM_BEHAVIOR_ZERO);
    t1 = now_ns();
    printf("  P2 parent mach_vm_behavior_set(VM_BEHAVIOR_ZERO): %s (%.1f us)\n", mach_error_string(kr), (t1 - t0) / 1e3);
    if (!kr) CHECK(cmd(CMD_CHECK_ZERO, off, len, 0) == 0 && count_nonzero(off, len) == 0, "P2 both sides see zeros");

    /* P3/P4 child (native arm64) zeroes its view of the same object */
    for (int m = 0; m < 2; m++) {
        write_pattern(off, len, 13);
        t0 = now_ns();
        uint64_t r = cmd(m ? CMD_PUNCH_BEHAVIOR : CMD_PUNCH_MADV, off, len, 0);
        t1 = now_ns();
        printf("  P%d child %s: %s (child-side %.1f us, round trip %.1f us)\n", 3 + m,
               m ? "mach_vm_behavior_set(VM_BEHAVIOR_ZERO)" : "madvise(MADV_ZERO)",
               r ? (m ? mach_error_string((kern_return_t)r) : strerror((int)r)) : "ok", CTL->result2 / 1e3, (t1 - t0) / 1e3);
        if (!r) CHECK(count_nonzero(off, len) == 0 && cmd(CMD_CHECK_ZERO, off, len, 0) == 0, "P%d parent and child see zeros", 3 + m);
    }
    /* sub-16K granularity from the native side */
    write_pattern(off, len, 14);
    uint64_t r = cmd(CMD_PUNCH_MADV, off + 0x1000, 0x1000, 0);
    printf("  P3b child madvise(MADV_ZERO) on a 4 KiB sub-page range [+0x1000,+0x2000): %s\n", r ? strerror((int)r) : "ok");
    if (!r) {
        uint64_t z_target = count_nonzero(off + 0x1000, 0x1000), z_before = count_nonzero(off, 0x1000),
                 z_after = count_nonzero(off + 0x2000, 0x2000);
        printf("       nonzero words: target %llu/512, preceding 4K %llu/512, rest of 16K page %llu/1024\n", z_target,
               z_before, z_after);
        printf("  [INFO] P3b %s\n", z_target == 0 && z_before == 512 && z_after == 1024 ? "only the 4 KiB target was zeroed"
               : "the WHOLE 16 KiB host page was zeroed (sub-16K punches must use memset)");
    }

    /* P5 runtime-style punch: fresh anonymous pages over the parent's range */
    write_pattern(off, len, 15);
    t0 = now_ns();
    void *q = mmap(p, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    t1 = now_ns();
    printf("  P5 parent mmap(MAP_FIXED|MAP_ANON) over the range: %s (%.1f us)\n", q == p ? "ok" : strerror(errno), (t1 - t0) / 1e3);
    uint64_t child_nz = cmd(CMD_CHECK_ZERO, off, len, 0);
    CHECK(count_nonzero(off, len) == 0 && child_nz == len / 8,
          "P5 parent sees zeros but child still sees the old data (sharing broken: %llu/%llu words old)", child_nz, len / 8);
    mach_vm_address_t at = (mach_vm_address_t)p;
    KCHECK(mach_vm_map(mach_task_self(), &at, len, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, entry_port, off, FALSE,
                       VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE), "re-map entry");
    CHECK(verify_pattern(off, len, 15) == 0, "P5 re-mapping the entry over the range restores sharing (old data back)");

    /* P6 memset baseline */
    t0 = now_ns(); memset(p, 0, len); t1 = now_ns();
    printf("  P6 parent memset 1 MiB: %.1f us\n", (t1 - t0) / 1e3);
    CHECK(cmd(CMD_CHECK_ZERO, off, len, 0) == 0, "P6 child sees zeros after memset");
}

/* ---- (b) ordering ---------------------------------------------------------- */
static void test_rings(uint64_t n, int reps) {
    puts("\n== (b) SPSC ring ordering (payload plain stores, checksummed; ring of 256 x 64 B) ==");
    for (int buggy = 0; buggy < 2; buggy++) {
        for (int dir = 0; dir < 2; dir++) {
            uint64_t viol = 0, ns = 0;
            for (int r = 0; r < reps; r++) {
                ring_t *ring = dir ? RING_B : RING_A;
                memset(ring, 0, sizeof *ring);
                uint64_t t0 = now_ns();
                if (dir == 0) {
                    cmd_start(CMD_RING_CONSUME, n, buggy, 0);
                    ring_produce(ring, n, buggy);
                    viol += cmd_wait();
                } else {
                    cmd_start(CMD_RING_PRODUCE, n, buggy, 0);
                    viol += ring_consume(ring, n, buggy);
                    cmd_wait();
                }
                ns += now_ns() - t0;
            }
            printf("  %-8s %-34s %llu msgs: %llu violations  (%.1f M msg/s)\n", buggy ? "RELAXED" : "acq/rel",
                   dir ? "arm64 producer -> x86 consumer" : "x86 producer -> arm64 consumer", n * reps, viol,
                   (double)(n * reps) / (ns / 1e3));
            if (!buggy) CHECK(viol == 0, "no ordering violations with release/acquire");
        }
    }
}

/* ---- (b) doorbells -------------------------------------------------------- */
static void test_doorbells(uint32_t iters) {
    puts("\n== (b) doorbell round trip x86(Rosetta) -> arm64 -> x86 ==");
    uint64_t *v = malloc(iters * sizeof *v);
    for (int mech = 0; mech < MECH_COUNT; mech++) {
        CTL->ping = CTL->pong = 0;
        cmd_start(CMD_PINGPONG, iters, mech, 0);
        for (uint32_t i = 1; i <= iters; i++) {
            uint64_t t0 = now_ns();
            ring_doorbell(mech, i);
            v[i - 1] = now_ns() - t0;
        }
        cmd_wait();
        report_lat(mech_name[mech], v + 100, iters - 100); /* drop warm-up */
    }
    free(v);
}

/* ---- (b) fault relay ------------------------------------------------------ */
static void fault_run(const char *name, int mode, int mech, uint32_t n) {
    uint64_t *v = malloc(n * sizeof *v);
    const uint64_t base = GUEST_BASE + DATA_OFF + 0x1000000; /* 16 MiB into data, 256 pages */
    fault_mode = mode; fault_mech = mech; fault_seq = 0;
    if (mode == FM_RELAY) { CTL->ping = CTL->pong = 0; cmd_start(CMD_FAULT_SERVER, 0, mech, 0); }
    for (uint32_t i = 0; i < n; i++) {
        volatile uint64_t *pg = (volatile uint64_t *)(base + (uint64_t)(i % 256) * 0x4000);
        mprotect((void *)pg, ppage, PROT_READ);
        uint64_t t0 = now_ns();
        *pg = i;
        v[i] = now_ns() - t0;
    }
    if (mode == FM_RELAY) {
        CTL->fault_addr = ~0ULL;
        ring_doorbell(mech, ++fault_seq);
        uint64_t handled = cmd_wait();
        if (handled != n) printf("  !! child handled %llu faults, expected %u\n", handled, n);
    }
    report_lat(name, v + 100, n - 100);
    free(v);
}
static void test_fault_relay(uint32_t n) {
    puts("\n== (b) fault relay: write to RO page -> signal -> relay to arm64 -> reply -> mprotect RW -> resume ==");
    fault_run("no relay (signal + mprotect only)", FM_LOCAL, 0, n);
    for (int mech = 0; mech < MECH_COUNT; mech++) {
        char name[80];
        snprintf(name, sizeof name, "relay via %s", mech_name[mech]);
        fault_run(name, FM_RELAY, mech, n);
    }
    uint64_t t0 = now_ns();
    for (int i = 0; i < 10000; i++) {
        mprotect((void *)(GUEST_BASE + DATA_OFF), ppage, PROT_READ);
        mprotect((void *)(GUEST_BASE + DATA_OFF), ppage, PROT_READ | PROT_WRITE);
    }
    printf("  (reference: one mprotect call = %.2f us)\n", (now_ns() - t0) / 2e4 / 1e3);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *entry_mode = getenv("SPIKE_ENTRY") ? getenv("SPIKE_ENTRY") : "named";
    const char *xfer = getenv("SPIKE_XFER") ? getenv("SPIKE_XFER") : "register";
    uint64_t ring_n = argc > 1 ? strtoull(argv[1], 0, 0) : 10000000;
    int translated = 0; size_t tl = sizeof translated;
    sysctlbyname("sysctl.proc_translated", &translated, &tl, NULL, 0);
    ppage = (size_t)getpagesize();
    printf("== setup ==\n  parent pid %d, translated=%d, page size %zu (vm_page_size %lu)\n", getpid(), translated, ppage,
           (unsigned long)vm_page_size);

    struct sigaction sa = {0};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);

    create_entry(entry_mode);
    printf("  port transfer: %s\n", xfer);
    pid_t pid = spawn_child(xfer);
    handshake();

    test_shared();
    test_punch();
    test_rings(ring_n, 3);
    test_doorbells(100000);
    test_fault_relay(10000);

    cmd(CMD_EXIT, 0, 0, 0);
    int st;
    waitpid(pid, &st, 0);
    printf("\n== done: %d failure(s); child exit status %d ==\n", failures, WEXITSTATUS(st));
    return failures != 0;
}
