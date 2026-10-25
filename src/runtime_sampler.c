// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
/* BB_SAMPLE_MS=<n>: where the game's main thread spends long frames. Every n ms a sampler thread
 * suspends the main thread, reads its x86 registers (thread_get_state; Rosetta provides the
 * translated state, as for .NET's GC) and up to three rbp-chain return addresses from its own
 * stack, and resumes it. At each flip (gnmdriver.cpp) the frame's samples go to the "long frame"
 * histogram when the main thread used more than BB_SAMPLE_LONG_MS (25) of CPU in it, else they
 * are dropped. Every 60 s and at exit the top locations print as "Sampler:" lines: guest image
 * offsets (g:0x...) or host symbols. Diagnostics only: off unless the variable is set.
 * Not a signal: SIGPROF on the main thread made the game's texture streaming fail
 * (CSGXTextureCreateFragment panic) within minutes, every time (2026-10-21). While the thread is
 * suspended the sampler takes no lock and allocates nothing (the thread may hold the lock). */
#include "runtime.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

int runtime_main_pthread(pthread_t *out);

enum { Depth = 8, Ring = 1 << 14, Slots = 4096 };
typedef struct { uintptr_t pc[Depth]; } Sample;
static Sample ring[Ring];
static _Atomic uint32_t ring_head; /* written by the sampler thread */
static uint32_t ring_tail;         /* read at flips (the flip thread) */
static uintptr_t image_lo, image_hi;
static int interval_ms, long_ns_ms = 25;
static pthread_t main_thread;

/* Histogram of long-frame samples by location (the top frame, and the first caller inside the
 * guest image for host frames). Open addressing; touched by the flip thread only. */
typedef struct { uintptr_t key, caller; uint32_t count; } Slot;
static Slot slots[Slots];
static uint64_t long_frames, long_samples, frames_seen, last_report_ns;
static FILE *dump; /* BB_SAMPLE_DUMP=<file>: every long-frame sample, "frame pc0 pc1 pc2 pc3" (guest offsets as g:) */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Frames are read with a kernel call from the sampler thread (an unmapped rbp fails instead of
 * faulting); the main thread runs on a stack the game allocated, not its pthread stack. */
static int read_frame(uintptr_t rbp, uintptr_t frame[2]) {
#ifdef __APPLE__
    mach_vm_size_t got = 0;
    return rbp && !(rbp & 7) &&
           mach_vm_read_overwrite(mach_task_self(), rbp, 2 * sizeof(uintptr_t),
                                  (mach_vm_address_t)(uintptr_t)frame, &got) == KERN_SUCCESS &&
           got == 2 * sizeof(uintptr_t);
#else
    (void)rbp; (void)frame;
    return 0;
#endif
}

static void take_sample(mach_port_t port) {
#if defined(__APPLE__) && defined(__x86_64__)
    if (thread_suspend(port) != KERN_SUCCESS) return;
    x86_thread_state64_t state;
    mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
    const kern_return_t got = thread_get_state(port, x86_THREAD_STATE64, (thread_state_t)&state, &count);
    Sample sample = {{0}};
    if (got == KERN_SUCCESS) {
        sample.pc[0] = (uintptr_t)state.__rip;
        uintptr_t rbp = (uintptr_t)state.__rbp;
        for (int i = 1; i < Depth; ++i) {
            uintptr_t frame[2];
            if (!read_frame(rbp, frame) || frame[0] <= rbp) break;
            sample.pc[i] = frame[1];
            rbp = frame[0];
        }
    }
    thread_resume(port);
    if (got != KERN_SUCCESS) return;
    const uint32_t head = atomic_load_explicit(&ring_head, memory_order_relaxed);
    ring[head & (Ring - 1)] = sample;
    atomic_store_explicit(&ring_head, head + 1, memory_order_release);
#else
    (void)port;
#endif
}

static void *sampler(void *arg) {
    (void)arg;
    pthread_setname_np("bb:Sampler");
    const mach_port_t port = pthread_mach_thread_np(main_thread);
    const struct timespec gap = {0, (long)interval_ms * 1000000L};
    for (;;) {
        nanosleep(&gap, NULL);
        take_sample(port);
    }
    return NULL;
}

static int in_image(uintptr_t pc) { return pc >= image_lo && pc < image_hi; }

static void add(const Sample *s) {
    /* A guest location keeps its own offset; a host one (our runtime, libc) also records the
     * first guest frame up the chain, which says what the game asked for. */
    uintptr_t key = s->pc[0], caller = 0;
    if (!in_image(key))
        for (int i = 1; i < Depth; ++i) if (in_image(s->pc[i])) { caller = s->pc[i]; break; }
    uint32_t h = (uint32_t)((key ^ (caller * 31)) >> 2) & (Slots - 1);
    for (int probe = 0; probe < Slots; ++probe, h = (h + 1) & (Slots - 1)) {
        if (!slots[h].count) { slots[h].key = key; slots[h].caller = caller; }
        if (slots[h].key == key && slots[h].caller == caller) { ++slots[h].count; return; }
    }
}

static void describe(uintptr_t pc, char *out, size_t size) {
    Dl_info where = {0};
    if (in_image(pc)) { snprintf(out, size, "g:%#lx", (unsigned long)(pc - image_lo)); return; }
    if (!dladdr((void *)pc, &where)) { snprintf(out, size, "%#lx", (unsigned long)pc); return; }
    if (where.dli_sname) { snprintf(out, size, "%s", where.dli_sname); return; }
    const char *name = where.dli_fname ? where.dli_fname : "?";
    const char *slash = strrchr(name, '/');
    snprintf(out, size, "%s+%#lx", slash ? slash + 1 : name, (unsigned long)(pc - (uintptr_t)where.dli_fbase));
}

static int by_count(const void *a, const void *b) {
    const Slot *x = a, *y = b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

static void report(void) {
    static Slot sorted[Slots];
    memcpy(sorted, slots, sizeof(slots));
    qsort(sorted, Slots, sizeof(Slot), by_count);
    printf("Sampler: %llu of %llu frames over %d ms of main-thread CPU, %llu samples (every %d ms):\n",
           (unsigned long long)long_frames, (unsigned long long)frames_seen, long_ns_ms,
           (unsigned long long)long_samples, interval_ms);
    for (int i = 0; i < 25 && sorted[i].count; ++i) {
        char where[160], caller[64] = "";
        describe(sorted[i].key, where, sizeof(where));
        if (sorted[i].caller) { caller[0] = ' '; caller[1] = '<'; caller[2] = ' '; describe(sorted[i].caller, caller + 3, sizeof(caller) - 3); }
        printf("Sampler:   %5.1f%%  %s%s\n", long_samples ? 100.0 * sorted[i].count / (double)long_samples : 0.0, where, caller);
    }
    fflush(stdout);
}

static void report_at_exit(void) { if (long_samples) report(); }

void runtime_sampler_init(const void *image, uint64_t size) {
    const char *ms = getenv("BB_SAMPLE_MS");
    if (!ms || atoi(ms) <= 0) return;
    interval_ms = atoi(ms);
    if (getenv("BB_SAMPLE_LONG_MS")) long_ns_ms = atoi(getenv("BB_SAMPLE_LONG_MS"));
    image_lo = (uintptr_t)image;
    image_hi = image_lo + size;
    if (getenv("BB_SAMPLE_DUMP")) dump = fopen(getenv("BB_SAMPLE_DUMP"), "w");
}

/* At each flip, on the flip thread: the main thread's CPU time so far. Starts the sampler at
 * the first flip (the main thread exists by then). */
void runtime_sampler_flip(uint64_t main_cpu_ns) {
    static uint64_t last_cpu;
    static int started;
    if (!interval_ms) return;
    if (!started) {
        if (!runtime_main_pthread(&main_thread)) return;
        pthread_t thread;
        pthread_create(&thread, NULL, sampler, NULL);
        pthread_detach(thread);
        atexit(report_at_exit);
        started = 1;
        last_cpu = main_cpu_ns;
        last_report_ns = now_ns();
        printf("Sampler: main thread every %d ms; frames over %d ms of its CPU are kept\n", interval_ms, long_ns_ms);
        return;
    }
    const uint32_t head = atomic_load_explicit(&ring_head, memory_order_acquire);
    const int is_long = main_cpu_ns - last_cpu > (uint64_t)long_ns_ms * 1000000ull;
    last_cpu = main_cpu_ns;
    ++frames_seen;
    if (head - ring_tail > Ring) ring_tail = head - Ring;
    if (is_long) {
        ++long_frames;
        for (; ring_tail != head; ++ring_tail) {
            const Sample *sample = &ring[ring_tail & (Ring - 1)];
            add(sample);
            ++long_samples;
            if (dump) {
                fprintf(dump, "%llu", (unsigned long long)long_frames);
                for (int i = 0; i < Depth; ++i) {
                    char where[160];
                    describe(sample->pc[i], where, sizeof(where));
                    fprintf(dump, " %s", sample->pc[i] ? where : "-");
                }
                fputc('\n', dump);
            }
        }
        if (dump) fflush(dump);
    }
    ring_tail = head;
    const uint64_t now = now_ns();
    if (now - last_report_ns > 60000000000ull) { last_report_ns = now; if (long_samples) report(); }
}

/* The hang report (probe.c's watchdog): every live guest thread's name, what it is blocked on
 * and since when, and its guest call chain. Each thread is suspended only while its registers
 * and frame chain are read (no signals: CLAUDE.md), and nothing is printed while one is
 * suspended (it may hold the allocator's or stdio's lock). */
void runtime_thread_dump_all(void) {
#if defined(__APPLE__) && defined(__x86_64__)
    enum { Max = 128, Depth = 10 };
    static struct { char name[32]; const char *blocked; uint64_t resource, since_ms;
                    uint64_t pc[Depth]; int frames; } rows[Max];
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now_ms = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
    int n = 0;
    host_lock((HostMutex *)runtime_thread_get_lock());
    for (GuestThread *t = runtime_thread_get_all(); t && n < Max; t = t->next) {
        if (t->finished || pthread_equal(t->host, pthread_self())) continue;
        memcpy(rows[n].name, t->name, sizeof(rows[n].name));
        rows[n].blocked = t->blocked_on;
        rows[n].resource = t->blocked_resource;
        rows[n].since_ms = t->blocked_tick ? now_ms - t->blocked_tick : 0;
        rows[n].frames = 0;
        const mach_port_t port = pthread_mach_thread_np(t->host);
        if (port && thread_suspend(port) == KERN_SUCCESS) {
            x86_thread_state64_t state;
            mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
            if (thread_get_state(port, x86_THREAD_STATE64, (thread_state_t)&state, &count) == KERN_SUCCESS) {
                rows[n].pc[rows[n].frames++] = state.__rip;
                uintptr_t rbp = (uintptr_t)state.__rbp;
                while (rows[n].frames < Depth && rbp && !(rbp & 7)) {
                    uintptr_t frame[2];
                    mach_vm_size_t got = 0;
                    if (mach_vm_read_overwrite(mach_task_self(), rbp, sizeof(frame),
                                               (mach_vm_address_t)(uintptr_t)frame, &got) != KERN_SUCCESS ||
                        got != sizeof(frame) || frame[0] <= rbp) break;
                    rows[n].pc[rows[n].frames++] = frame[1];
                    rbp = frame[0];
                }
            }
            thread_resume(port);
        }
        ++n;
    }
    host_unlock((HostMutex *)runtime_thread_get_lock());
    printf("Hang report: %d guest threads (guest offsets; blocked = the runtime wait it is in)\n", n);
    for (int i = 0; i < n; ++i) {
        char chain[768] = "";
        size_t used = 0;
        for (int f = 0; f < rows[i].frames && used < sizeof(chain) - 96; ++f) {
            const uint64_t pc = rows[i].pc[f];
            Dl_info info;
            if (pc >= 0x800000000ull && pc < 0x900000000ull)
                used += (size_t)snprintf(chain + used, sizeof(chain) - used, "%s%llx", f ? "<" : "",
                                         (unsigned long long)(pc - 0x800000000ull));
            else if (dladdr((void *)(uintptr_t)pc, &info) && info.dli_sname)
                used += (size_t)snprintf(chain + used, sizeof(chain) - used, "%s%s+0x%llx", f ? "<" : "",
                                         info.dli_sname,
                                         (unsigned long long)(pc - (uintptr_t)info.dli_saddr));
            else
                used += (size_t)snprintf(chain + used, sizeof(chain) - used, "%s%llx", f ? "<" : "",
                                         (unsigned long long)pc);
        }
        if (rows[i].blocked)
            printf("  %-24s blocked in %s (0x%llx) for %llu ms; %s\n", rows[i].name, rows[i].blocked,
                   (unsigned long long)rows[i].resource, (unsigned long long)rows[i].since_ms, chain);
        else
            printf("  %-24s running; %s\n", rows[i].name, chain);
    }
    fflush(stdout);
#endif
}
