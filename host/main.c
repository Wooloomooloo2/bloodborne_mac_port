// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
/* bbgpu-host: the native arm64 GPU process (docs/DESIGN_NATIVE_GPU_PROCESS.md), M3.
 * Spawned by bb-probe (src/gpu_process.c). It reserves the guest range,
 * maps the guest memory entry and the shared control block it receives in the handshake
 * (host/bbgpu_protocol.h, host/bbgpu_shared.h), then serves the submit ring on its own thread:
 * guest view events (mirrored at the runtime's addresses), mirror verification, and, after the
 * START request, the GPU work, handed to the GPU library (gpu/, arm64) which renders here. The
 * main thread runs the Cocoa run loop: the window lives on it. It exits when bb-probe exits or
 * execs, when the control port dies, or when its window is closed.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#include "host.h"
#include "gpu/bbgpu.h"
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <inttypes.h>
#include <mach/mach_vm.h>
#include <mach/notify.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <libproc.h>
#include <mach-o/loader.h>

void say(const char *format, ...) {
    char line[512];
    int n = snprintf(line, sizeof line, "bbgpu-host: ");
    va_list args;
    va_start(args, format);
    n += vsnprintf(line + n, sizeof line - (size_t)n, format, args);
    va_end(args);
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    (void)!write(1, line, (size_t)n);
}

BbGpuShared *g_shared;
static mach_port_t host_port = MACH_PORT_NULL, ctl_port = MACH_PORT_NULL, entry = MACH_PORT_NULL,
                   shared_entry = MACH_PORT_NULL;
static uint64_t entry_size;
static pid_t parent_pid;
static int started; /* renderer running (ring thread only) */

/* The host ends through _exit (no destructors): only the pipeline archive is written first,
 * with a time bound (bbgpu_flush_caches; a no-op before the renderer started). */
static void finish(int code, const char *why) {
    say("exiting: %s", why);
    fflush(stdout);
    bbgpu_flush_caches(BBGPU_EXIT_FLUSH_MS);
    _exit(code);
}

static void on_sigterm(void *unused) {
    (void)unused;
    finish(0, "SIGTERM");
}

/* ---- faults: GPU page tracking first, then recovery points (the SRT walker), else fatal ---- */
extern __thread sigjmp_buf *runtime_fault_recover; /* host_runtime.c */
static void fault(int sig, siginfo_t *info, void *context) {
    if (bbgpu_handle_fault(context, info->si_addr)) return;
    if (runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    const ucontext_t *uc = context;
    const uintptr_t pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
    char line[512];
    Dl_info where;
    int n;
    /* The process is ending: dladdr/snprintf/backtrace are acceptable here. One plain line
     * first: the launcher's crash report leads with it. */
    if (dladdr((void *)pc, &where) && where.dli_fname && where.dli_sname)
        n = snprintf(line, sizeof line, "Crash: the GPU process crashed (signal %d in %s)\n", sig, where.dli_sname);
    else
        n = snprintf(line, sizeof line, "Crash: the GPU process crashed (signal %d)\n", sig);
    (void)!write(1, line, (size_t)n);
    if (dladdr((void *)pc, &where) && where.dli_fname)
        n = snprintf(line, sizeof line, "bbgpu-host: fatal fault (signal %d) at %s+0x%lx (%s), address %p\n", sig,
                     where.dli_fname, (unsigned long)(pc - (uintptr_t)where.dli_fbase),
                     where.dli_sname ? where.dli_sname : "?", info->si_addr);
    else
        n = snprintf(line, sizeof line, "bbgpu-host: fatal fault (signal %d) at pc 0x%lx, address %p\n", sig,
                     (unsigned long)pc, info->si_addr);
    (void)!write(1, line, (size_t)n);
    void *frames[32];
    const int count = backtrace(frames, 32);
    backtrace_symbols_fd(frames, count, 1);
    signal(sig, SIG_DFL); /* the default action ends the process (raise: a signal sent with kill
                           * would not fault again on return) */
    raise(sig);
}

/* ---- mirror verification (M2), answered on the control port ---------------------------- */
static void verify(const BbGpuVerifyRecord *v) {
    BbGpuVerifyReply r;
    memset(&r, 0, sizeof r);
    r.seq = v->seq;
    host_view_stats(&r.views, &r.bytes, &r.events, &r.failures);
    r.count = v->count <= BBGPU_SAMPLES_MAX ? v->count : BBGPU_SAMPLES_MAX;
    for (uint32_t i = 0; i < r.count; ++i) {
        const uint64_t a = v->samples[i].address, size = v->samples[i].size;
        if (size % 8 || a + size < a || !host_view_covered(a, a + size)) continue;
        r.mapped[i] = 1; /* read through the mirror, at the runtime's address */
        r.checksums[i] = bbgpu_checksum(BBGPU_CHECKSUM_SEED, (const void *)(uintptr_t)a, size);
    }
    if (v->events != r.events)
        say("verify #%" PRIu64 ": runtime sent %" PRIu64 " events, %" PRIu64 " applied", v->seq, v->events, r.events);
    r.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    r.h.msgh_size = sizeof r;
    r.h.msgh_remote_port = ctl_port;
    r.h.msgh_id = BBGPU_MSG_VERIFY_REPLY;
    kern_return_t kr = mach_msg(&r.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof r, 0, MACH_PORT_NULL, 2000, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS) finish(1, mach_error_string(kr));
}

/* ---- the submit ring ------------------------------------------------------------------- */
static void start_renderer(const BbGpuStart *request) {
    BbGpuShared *s = g_shared;
    s->title[sizeof s->title - 1] = s->serial[sizeof s->serial - 1] = s->user_dir[sizeof s->user_dir - 1] = 0;
    const BbGpuConfig config = {s->title, s->serial, s->user_dir, s->sdk_version, s->psf_attributes, s->width, s->height};
    const uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    const int rc = started ? -1 : bbgpu_host_start(&config, s);
    __atomic_store_n(&started, rc == 0, __ATOMIC_RELEASE);
    say("renderer %s in %.0f ms (%s, %dx%d)", started ? "started" : "FAILED to start",
        (clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0) / 1e6, config.serial, config.width, config.height);
    bbgpu_reply_send(s, request->slot, rc);
}

static void *ring_thread(void *unused) {
    (void)unused;
    BbGpuShared *s = g_shared;
    uint64_t dropped = 0;
    for (;;) {
        const BbGpuRecord *r = bbgpu_ring_peek(s, &s->submit, UINT64_C(1000000000));
        if (!r) continue;
        switch (r->type) {
        case BBGPU_REC_VIEW: {
            const BbGpuViewRecord *v = (const BbGpuViewRecord *)r;
            host_view_apply(v->op, v->prot, v->address, v->size, v->phys);
            break;
        }
        case BBGPU_REC_VERIFY:
            verify((const BbGpuVerifyRecord *)r);
            break;
        case BBGPU_REC_START:
            start_renderer((const BbGpuStart *)r);
            break;
        default:
            if (started) {
                bbgpu_host_dispatch(r);
            } else if (dropped++ < 10) {
                say("record %u before the renderer started: dropped", r->type);
            }
        }
        bbgpu_ring_pop(&s->submit, r);
    }
    return NULL;
}

/* ---- M4: faults the game took on GPU-protected pages ----------------------------------------
 * The renderer handles them as in one process (mark the range CPU-dirty or download GPU data,
 * then unprotect through the protect relay) and answers; the game then retries the access. Its
 * own thread: a fault must never wait behind GPU work on the submit ring. */
static void *fault_thread(void *unused) {
    (void)unused;
    BbGpuShared *s = g_shared;
    for (;;) {
        const BbGpuRecord *r = bbgpu_ring_peek(s, &s->faults, UINT64_C(1000000000));
        if (!r) continue;
        const BbGpuFault fault = *(const BbGpuFault *)r;
        bbgpu_ring_pop(&s->faults, r);
        const int handled = __atomic_load_n(&started, __ATOMIC_ACQUIRE) ? bbgpu_host_fault(fault.address, (int)fault.write) : 0;
        bbgpu_reply_send(s, fault.slot, handled);
    }
    return NULL;
}

/* ---- Mach control: shutdown and peer death ---------------------------------------------- */
typedef union {
    mach_msg_header_t h;
    BbGpuSetup setup;
    mach_dead_name_notification_t dead;
} Message;
typedef struct { Message m; mach_msg_max_trailer_t trailer; } MessageBuffer;

static void *receiver(void *unused) {
    (void)unused;
    static MessageBuffer in;
    for (;;) {
        kern_return_t kr = mach_msg(&in.m.h, MACH_RCV_MSG, 0, sizeof in, host_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (kr != KERN_SUCCESS) finish(1, mach_error_string(kr));
        switch (in.m.h.msgh_id) {
        case BBGPU_MSG_SHUTDOWN:
            finish(0, "shutdown requested");
            break;
        case MACH_NOTIFY_DEAD_NAME:
            finish(0, "control port died (bb-probe exited)");
            break;
        default:
            mach_msg_destroy(&in.m.h);
        }
    }
}

/* bb-probe exiting or exec'ing (runtime_restart) ends the host too. */
static void *parent_watch(void *unused) {
    (void)unused;
    int kq = kqueue();
    struct kevent change, event;
    EV_SET(&change, parent_pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT | NOTE_EXEC, 0, NULL);
    if (kq < 0 || kevent(kq, &change, 1, &event, 1, NULL) < 0 || (event.flags & EV_ERROR)) finish(0, "bb-probe is gone");
    finish(0, event.fflags & NOTE_EXEC ? "bb-probe exec'd (restart)" : "bb-probe exited");
    return NULL;
}

static int translated(void) {
    int value = 0;
    size_t size = sizeof value;
    return sysctlbyname("sysctl.proc_translated", &value, &size, NULL, 0) == 0 && value;
}

static int handshake(mach_port_t *parent) {
    const char *marker = getenv("BBGPU_HANDSHAKE");
    if (!marker || atoi(marker) != getppid()) {
        say("not started by bb-probe (bb-probe starts it; run the game with run.sh)");
        return -1;
    }
    mach_port_array_t ports = NULL;
    mach_msg_type_number_t n = 0;
    kern_return_t kr = mach_ports_lookup(mach_task_self(), &ports, &n);
    if (kr != KERN_SUCCESS || n < 1 || ports[0] == MACH_PORT_NULL) {
        say("no handshake port inherited");
        return -1;
    }
    *parent = ports[0];
    for (mach_msg_type_number_t i = 1; i < n; ++i) if (ports[i]) mach_port_deallocate(mach_task_self(), ports[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)ports, n * sizeof *ports);
    mach_ports_register(mach_task_self(), NULL, 0); /* not passed on to anything this spawns */

    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &host_port) != KERN_SUCCESS) return -1;
    BbGpuHello hello;
    memset(&hello, 0, sizeof hello);
    hello.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND);
    hello.h.msgh_size = sizeof hello;
    hello.h.msgh_remote_port = *parent;
    hello.h.msgh_local_port = host_port;
    hello.h.msgh_id = BBGPU_MSG_HELLO;
    hello.version = BBGPU_PROTOCOL_VERSION;
    hello.pid = (uint32_t)getpid();
    kr = mach_msg(&hello.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof hello, 0, MACH_PORT_NULL, 5000, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS) { say("hello: %s", mach_error_string(kr)); return -1; }

    static MessageBuffer in;
    for (;;) {
        kr = mach_msg(&in.m.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof in, host_port, 10000, MACH_PORT_NULL);
        if (kr != KERN_SUCCESS) { say("setup: %s", mach_error_string(kr)); return -1; }
        if (in.m.h.msgh_id == BBGPU_MSG_SETUP) break;
        mach_msg_destroy(&in.m.h);
    }
    const BbGpuSetup *s = &in.m.setup;
    if (!(s->h.msgh_bits & MACH_MSGH_BITS_COMPLEX) || s->body.msgh_descriptor_count != 3 || s->version != BBGPU_PROTOCOL_VERSION ||
        s->guest_min != BBGPU_GUEST_MIN || s->guest_max != BBGPU_GUEST_MAX) {
        say("bad setup message (protocol %u, expected %u)", s->version, BBGPU_PROTOCOL_VERSION);
        mach_msg_destroy(&in.m.h);
        return -1;
    }
    entry = s->entry.name;
    ctl_port = s->control.name;
    shared_entry = s->shared.name;
    entry_size = s->entry_size;
    parent_pid = (pid_t)s->pid;
    return 0;
}

static void send_ready(int status) {
    BbGpuReady r;
    memset(&r, 0, sizeof r);
    r.h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    r.h.msgh_size = sizeof r;
    r.h.msgh_remote_port = ctl_port;
    r.h.msgh_id = BBGPU_MSG_READY;
    r.pid = (uint32_t)getpid();
    r.page_size = (uint32_t)getpagesize();
    r.native = !translated();
    r.window = 0; /* the window opens with the renderer (START) */
    r.status = status;
    kern_return_t kr = mach_msg(&r.h, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof r, 0, MACH_PORT_NULL, 2000, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS) say("ready: %s", mach_error_string(kr));
}

static int map_shared(void) {
    mach_vm_address_t at = 0;
    kern_return_t kr = mach_vm_map(mach_task_self(), &at, BBGPU_SHARED_SIZE, 0, VM_FLAGS_ANYWHERE, shared_entry, 0, FALSE,
                                   VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) { say("cannot map the control block: %s", mach_error_string(kr)); return kr; }
    g_shared = (BbGpuShared *)(uintptr_t)at;
    if (g_shared->magic != BBGPU_SHARED_MAGIC || g_shared->version != BBGPU_SHARED_VERSION) {
        say("control block version %u, expected %u", g_shared->version, BBGPU_SHARED_VERSION);
        return EINVAL;
    }
    return 0;
}

/* Names what already occupies the guest range (the reservation failed on an M2 Max, issue #2). */
static void report_guest_range(void) {
    mach_vm_address_t at = BBGPU_GUEST_MIN;
    for (int n = 0; n < 16; ++n) {
        mach_vm_size_t size = 0;
        vm_region_extended_info_data_t info;
        mach_msg_type_number_t count = VM_REGION_EXTENDED_INFO_COUNT;
        mach_port_t object = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &at, &size, VM_REGION_EXTENDED_INFO, (vm_region_info_t)&info, &count,
                           &object) != KERN_SUCCESS || at >= BBGPU_GUEST_MAX)
            return;
        char file[PATH_MAX] = "";
        proc_regionfilename(getpid(), at, file, sizeof file);
        say("  in the way: 0x%" PRIx64 "-0x%" PRIx64 " (%" PRIu64 " MiB) tag %u prot %d %s", (uint64_t)at,
            (uint64_t)(at + size), (uint64_t)(size >> 20), info.user_tag, info.protection, file);
        at += size;
    }
}

/* The guest range, mapped PROT_NONE at its exact addresses. In bbgpu-host-macos27 the
 * __GUESTRSV segment (host/guest_reservation.s) holds it from exec, slid by the image's ASLR
 * slide; from macOS 27 on, libmalloc would otherwise have put regions there before main. The
 * range is taken over only if nothing but that segment is in it, so no foreign mapping is
 * overwritten. The plain bbgpu-host (no segment: on macOS 26 a pinned zero-fill segment breaks
 * dyld) keeps the mmap hint, which is enough there. bb-probe picks the host by macOS version. */
static int reserve_guest_range(void) {
    const uint64_t span = BBGPU_GUEST_MAX - BBGPU_GUEST_MIN;
    const struct mach_header_64 *image = (const struct mach_header_64 *)_dyld_get_image_header(0);
    const struct segment_command_64 *seg = NULL;
    const struct load_command *cmd = (const struct load_command *)(image + 1);
    for (uint32_t i = 0; i < image->ncmds; ++i, cmd = (const struct load_command *)((const char *)cmd + cmd->cmdsize))
        if (cmd->cmd == LC_SEGMENT_64 && !strncmp(((const struct segment_command_64 *)cmd)->segname, "__GUESTRSV", 16))
            seg = (const struct segment_command_64 *)cmd;
    if (!seg) { /* built without the segment: the reservation from main() alone */
        void *reserved = mmap((void *)BBGPU_GUEST_MIN, span, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (reserved == (void *)BBGPU_GUEST_MIN) return 0;
        say("cannot reserve the guest range 0x%" PRIx64 "-0x%" PRIx64 " (got %p)", BBGPU_GUEST_MIN, BBGPU_GUEST_MAX, reserved);
        if (reserved != MAP_FAILED) munmap(reserved, span);
        return ENOMEM;
    }
    const uint64_t own_lo = seg->vmaddr + (uint64_t)_dyld_get_image_vmaddr_slide(0), own_hi = own_lo + seg->vmsize;
    for (mach_vm_address_t at = BBGPU_GUEST_MIN; at < BBGPU_GUEST_MAX;) {
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &at, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count,
                           &object) != KERN_SUCCESS || at >= BBGPU_GUEST_MAX)
            break;
        if (at < own_lo || at + size > own_hi || info.protection != VM_PROT_NONE) {
            say("cannot reserve the guest range 0x%" PRIx64 "-0x%" PRIx64 ": 0x%llx-0x%llx is in use",
                BBGPU_GUEST_MIN, BBGPU_GUEST_MAX, (unsigned long long)at, (unsigned long long)(at + size));
            return ENOMEM;
        }
        at += size;
    }
    void *reserved =
        mmap((void *)BBGPU_GUEST_MIN, span, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0);
    if (reserved == (void *)BBGPU_GUEST_MIN) return 0;
    say("cannot reserve the guest range 0x%" PRIx64 "-0x%" PRIx64 ": %s", BBGPU_GUEST_MIN, BBGPU_GUEST_MAX, strerror(errno));
    return ENOMEM;
}

int main(int argc, char **argv) {
    /* bbgpu-host --msl-check <spv dir> [msl cache dir]: the Metal back end's shader check. */
    if (argc >= 3 && !strcmp(argv[1], "--msl-check")) return bbgpu_msl_check(argv[2], argc >= 4 ? argv[3] : NULL);
    /* stdout is the launcher's pipe (shared with bb-probe): if the launcher goes away, say()
     * fails with EPIPE (a lost log line) instead of SIGPIPE ending the host. */
    signal(SIGPIPE, SIG_IGN);
    /* The renderer's printf lines (GPU arena, GPU memory, Frame stats) share that pipe with
     * bb-probe's unbuffered output. A pipe's stdout is fully buffered: they arrived minutes late,
     * out of order with the game's lines, or never when the host ended with _Exit (a failed
     * check). Line-buffered, each line is written as it is printed. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* A signal ends the host as a signal, not as SDL's polite quit event (exit status 0). */
    setenv("SDL_NO_SIGNAL_HANDLERS", "1", 1);
    say("pid %d, %s, page %d bytes", (int)getpid(), translated() ? "TRANSLATED (Rosetta)" : "native arm64", getpagesize());
    /* The guest range first, before anything (Metal, the window server) maps memory there. The
     * kernel hole below 0x7000000000 is the same in arm64 processes. */
    /* BBGPU_TEST_BLOCK_RANGE=1 (testing) occupies the start of the range, as seen on an M2 Max. */
    const char *block = getenv("BBGPU_TEST_BLOCK_RANGE");
    if (block && !strcmp(block, "1"))
        mmap((void *)BBGPU_GUEST_MIN, 1 << 20, PROT_READ, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    int status = reserve_guest_range();
    if (status) report_guest_range();
    mach_port_t parent = MACH_PORT_NULL;
    if (handshake(&parent)) return 1;
    mach_port_deallocate(mach_task_self(), parent); /* the handshake port; control goes through ctl_port */
    say("handshake done: parent pid %d, guest entry 0x%" PRIx64 " bytes, guest range 0x%" PRIx64 "-0x%" PRIx64 " reserved",
        (int)parent_pid, entry_size, BBGPU_GUEST_MIN, BBGPU_GUEST_MAX);
    if (!status) status = map_shared();
    /* The entry must be mappable here (send right valid, size as announced). */
    if (!status) {
        mach_vm_address_t at = 0;
        kern_return_t kr = mach_vm_map(mach_task_self(), &at, BBGPU_GUEST_PAGE, 0, VM_FLAGS_ANYWHERE, entry,
                                       entry_size - BBGPU_GUEST_PAGE, FALSE, VM_PROT_READ | VM_PROT_WRITE,
                                       VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS) { say("cannot map the guest entry: %s", mach_error_string(kr)); status = kr; }
        else mach_vm_deallocate(mach_task_self(), at, BBGPU_GUEST_PAGE);
    }
    host_runtime_init(entry, entry_size);
    mach_port_t previous = MACH_PORT_NULL;
    if (mach_port_request_notification(mach_task_self(), ctl_port, MACH_NOTIFY_DEAD_NAME, 0, host_port,
                                       MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous) != KERN_SUCCESS) {
        say("bb-probe's control port is already dead");
        return 0;
    }
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = fault;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    /* SIGTERM (bb-probe's stop_host, kill): exit through finish, off the signal handler. */
    signal(SIGTERM, SIG_IGN);
    dispatch_source_t term = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0,
                                                    dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    dispatch_source_set_event_handler_f(term, on_sigterm);
    dispatch_resume(term);

    pthread_t thread;
    pthread_create(&thread, NULL, parent_watch, NULL);
    pthread_detach(thread);
    pthread_create(&thread, NULL, receiver, NULL);
    pthread_detach(thread);
    if (!status) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 16u << 20);
        pthread_create(&thread, &attr, ring_thread, NULL);
        pthread_detach(thread);
        pthread_create(&thread, &attr, fault_thread, NULL);
        pthread_detach(thread);
    }
    send_ready(status);
    if (status) { say("setup failed; exiting"); return 1; }
    say("ready: serving the submit ring");
    /* Cocoa: the renderer's window and its event loop run on the main thread. SDL stops the
     * main run loop at times (entering full screen): run it again; the window closing exits. */
    for (;;) {
        if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0e10, false) == kCFRunLoopRunFinished)
            usleep(1000); /* no sources yet */
    }
}
