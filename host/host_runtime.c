/* bbgpu-host: the runtime_* functions the GPU library (gpu/, built arm64) imports from bb-probe
 * in a single process, provided here from the mirrored guest view table (M3, design "M3 plan").
 * Guest memory is mapped at the runtime's addresses and always read/write here: the game's page
 * protection only matters in bb-probe (design 3.4; the protection relay is M4).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#include "host.h"
#include "gpu/bbgpu.h"
#include <errno.h>
#include <inttypes.h>
#include <mach/mach_vm.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

/* ---- view table: guest views at the runtime's addresses (writer: the ring thread) ---------- */
typedef struct { uint64_t start, end, phys; uint32_t prot; } View;
static View *views;
static size_t view_count, view_capacity;
static uint64_t events_applied, failures, view_bytes;
static pthread_rwlock_t table_lock = PTHREAD_RWLOCK_INITIALIZER;
static mach_port_t entry = MACH_PORT_NULL;
static uint64_t entry_size;

typedef void (*GpuRange)(uintptr_t address, uint64_t size);
static GpuRange hook_map, hook_unmap, hook_invalidate;

void host_runtime_init(mach_port_t guest_entry, uint64_t guest_entry_size) {
    entry = guest_entry;
    entry_size = guest_entry_size;
}

static size_t view_index(uint64_t a) { /* first view with end > a */
    size_t lo = 0, hi = view_count;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (views[mid].end <= a) lo = mid + 1; else hi = mid; }
    return lo;
}
static void view_insert(size_t at, View v) {
    if (view_count == view_capacity) {
        view_capacity = view_capacity ? view_capacity * 2 : 256;
        views = realloc(views, view_capacity * sizeof *views);
        if (!views) { say("out of memory"); _exit(1); }
    }
    memmove(views + at + 1, views + at, (view_count - at) * sizeof *views);
    views[at] = v;
    ++view_count;
    view_bytes += v.end - v.start;
}
static void split_at(uint64_t a) {
    size_t i = view_index(a);
    if (i == view_count || views[i].start >= a) return;
    View right = views[i];
    right.phys += a - right.start;
    right.start = a;
    views[i].end = a;
    view_bytes -= right.end - right.start; /* view_insert adds it back */
    view_insert(i + 1, right);
}
static void drop(uint64_t start, uint64_t end) {
    split_at(start);
    split_at(end);
    size_t i = view_index(start), n = 0;
    while (i + n < view_count && views[i + n].start < end) view_bytes -= views[i + n].end - views[i + n].start, ++n;
    memmove(views + i, views + i + n, (view_count - i - n) * sizeof *views);
    view_count -= n;
}
static int covered_locked(uint64_t start, uint64_t end) {
    uint64_t at = start;
    for (size_t i = view_index(start); at < end; ++i) {
        if (i == view_count || views[i].start > at) return 0;
        at = views[i].end;
    }
    return 1;
}
int host_view_covered(uint64_t start, uint64_t end) {
    pthread_rwlock_rdlock(&table_lock);
    const int result = covered_locked(start, end);
    pthread_rwlock_unlock(&table_lock);
    return result;
}
void host_view_stats(uint64_t *count, uint64_t *bytes, uint64_t *applied, uint64_t *failed) {
    pthread_rwlock_rdlock(&table_lock);
    *count = view_count;
    *bytes = view_bytes;
    *applied = events_applied;
    *failed = failures;
    pthread_rwlock_unlock(&table_lock);
}

static void fail_view(uint32_t op, uint64_t address, uint64_t size, uint64_t phys, const char *what, int code) {
    if (failures++ < 10)
        say("event %u at 0x%" PRIx64 "+0x%" PRIx64 " (phys 0x%" PRIx64 "): %s failed (%d)", op, address, size, phys,
            what, code);
}

/* Applies one view event; the renderer's hooks (when started) run after the table is updated,
 * outside the lock: they query the table. */
void host_view_apply(uint32_t op, uint32_t prot, uint64_t address, uint64_t size, uint64_t phys) {
    const uint64_t start = address, end = address + size;
    GpuRange hook = NULL;
    pthread_rwlock_wrlock(&table_lock);
    ++events_applied;
    if (!size || (start | size) % BBGPU_GUEST_PAGE || start < BBGPU_GUEST_MIN || end > BBGPU_GUEST_MAX || end < start) {
        fail_view(op, address, size, phys, "range check", EINVAL);
        pthread_rwlock_unlock(&table_lock);
        return;
    }
    switch (op) {
    case BBGPU_EV_MAP: {
        if (phys % BBGPU_GUEST_PAGE || phys > entry_size || size > entry_size - phys) {
            fail_view(op, address, size, phys, "entry range check", EINVAL);
            break;
        }
        mach_vm_address_t at = start;
        kern_return_t kr = mach_vm_map(mach_task_self(), &at, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, entry, phys,
                                       FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS || at != start) { fail_view(op, address, size, phys, "mach_vm_map", kr); break; }
        drop(start, end);
        view_insert(view_index(start), (View){start, end, phys, prot});
        hook = hook_map;
        break;
    }
    case BBGPU_EV_UNMAP:
        /* back to the reservation, as the runtime does */
        if (mmap((void *)start, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0) == MAP_FAILED) {
            fail_view(op, address, size, phys, "mmap(PROT_NONE)", errno);
            break;
        }
        drop(start, end);
        hook = hook_unmap;
        break;
    case BBGPU_EV_PROTECT:
        split_at(start);
        split_at(end);
        for (size_t i = view_index(start); i < view_count && views[i].start < end; ++i) views[i].prot = prot;
        hook = hook_invalidate; /* as the runtime: a guest mprotect invalidates GPU copies */
        break;
    default:
        fail_view(op, address, size, phys, "unknown op", EINVAL);
    }
    pthread_rwlock_unlock(&table_lock);
    if (hook) hook(start, size);
}

/* ---- runtime_* for the GPU library ------------------------------------------------------- */
uint64_t runtime_disabled_optimizations;
__thread sigjmp_buf *runtime_fault_recover;
static __thread int32_t thread_errno;

int32_t *runtime_errno(void) { return &thread_errno; }

/* BB_TOGGLE_FILE: the renderer's optimization/debug toggles, re-read every 250 ms (as bb-probe
 * does for its own side; the renderer runs here in split mode). */
static void *toggle_watcher(void *path) {
    for (unsigned long long last = ~0ULL;;) {
        FILE *f = fopen(path, "r");
        unsigned long long value = 0;
        if (f) {
            if (fscanf(f, "%llu", &value) != 1) value = 0;
            fclose(f);
        }
        if (value != last) {
            __atomic_store_n(&runtime_disabled_optimizations, (uint64_t)value, __ATOMIC_RELEASE);
            say("disabled optimizations mask=%llu", value);
            last = value;
        }
        struct timespec t = {0, 250000000};
        nanosleep(&t, NULL);
    }
    return NULL;
}

void runtime_memory_set_gpu_hooks(GpuRange map, GpuRange unmap, GpuRange invalidate) {
    const char *toggles = getenv("BB_TOGGLE_FILE");
    static pthread_t watcher;
    if (toggles && !watcher) pthread_create(&watcher, NULL, toggle_watcher, (void *)toggles);
    pthread_rwlock_wrlock(&table_lock);
    hook_map = map;
    hook_unmap = unmap;
    hook_invalidate = invalidate;
    size_t n = view_count;
    View *copy = n ? malloc(n * sizeof *copy) : NULL;
    if (copy) memcpy(copy, views, n * sizeof *copy);
    pthread_rwlock_unlock(&table_lock);
    for (size_t i = 0; map && i < n && copy; ++i) map(copy[i].start, copy[i].end - copy[i].start);
    free(copy);
}

/* Guest-visible writes from the GPU side (fence labels, WriteData, downloads): each piece is one
 * release store, as in the runtime (store_once): the game reads labels without locks, and on
 * arm64 plain stores may become visible out of order. */
static void store_once(unsigned char *dst, const unsigned char *src, uint64_t n) {
    if (n > 64) {
        memcpy(dst, src, n);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        return;
    }
    while (n) {
        const uintptr_t at = (uintptr_t)dst;
        if (n >= 8 && !(at & 7)) { uint64_t v; memcpy(&v, src, 8); __atomic_store_n((uint64_t *)dst, v, __ATOMIC_RELEASE); dst += 8; src += 8; n -= 8; }
        else if (n >= 4 && !(at & 3)) { uint32_t v; memcpy(&v, src, 4); __atomic_store_n((uint32_t *)dst, v, __ATOMIC_RELEASE); dst += 4; src += 4; n -= 4; }
        else if (n >= 2 && !(at & 1)) { uint16_t v; memcpy(&v, src, 2); __atomic_store_n((uint16_t *)dst, v, __ATOMIC_RELEASE); dst += 2; src += 2; n -= 2; }
        else { __atomic_store_n(dst, *src, __ATOMIC_RELEASE); ++dst; ++src; --n; }
    }
}

int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size) {
    if (!host_view_covered(address, address + size)) return 0;
    store_once((unsigned char *)address, data, size);
    return 1;
}
/* Game threads live in bb-probe: the spike log's CPU times come through the control block. */
uint64_t runtime_thread_cpu_ns(int which) {
    (void)which;
    return 0;
}
void runtime_game_cpu_since_last(uint64_t *total_ns, uint64_t *busiest_ns, char busiest_name[32]) {
    *total_ns = *busiest_ns = 0;
    busiest_name[0] = 0;
}

int runtime_memory_read_backing(uintptr_t address, void *out, uint64_t size) {
    if (!host_view_covered(address, address + size)) return 0;
    memcpy(out, (const void *)address, size);
    return 1;
}
int runtime_memory_is_mapped(uintptr_t address, uint64_t size) {
    return host_view_covered(address, address + size);
}
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size) {
    pthread_rwlock_rdlock(&table_lock);
    uint64_t length = 0;
    for (size_t i = view_index(address); i < view_count && length < size; ++i) {
        if (views[i].start > address + length) break;
        length = views[i].end - address;
    }
    pthread_rwlock_unlock(&table_lock);
    return length < size ? length : size;
}
int runtime_memory_vma_info(uintptr_t address, int *prot, int *type, uintptr_t *end) {
    pthread_rwlock_rdlock(&table_lock);
    const size_t i = view_index(address);
    const int ok = i < view_count && views[i].start <= address;
    if (ok) {
        if (prot) *prot = (int)views[i].prot;
        if (type) *type = -1; /* the mirror does not carry the direct memory type */
        if (end) *end = views[i].end;
    }
    pthread_rwlock_unlock(&table_lock);
    return ok;
}
int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped) {
    pthread_rwlock_rdlock(&table_lock);
    const size_t i = view_index(address);
    const int found = i < view_count;
    if (found && views[i].start <= address) {
        *start = views[i].start; *end = views[i].end; *mapped = 1;
    } else if (found) {
        *start = i ? views[i - 1].end : 0; *end = views[i].start; *mapped = 0;
    }
    pthread_rwlock_unlock(&table_lock);
    return found;
}

/* GPU page tracking (M4): protection only matters where the game runs, so each request goes to
 * bb-probe (protect ring) and returns once applied there, as an mprotect would in one process:
 * a game write after this returns faults, and the fault comes back here (main.c). */
static pthread_mutex_t protect_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t protect_seq;
static void game_alive_or_exit(void) {
    if (g_shared->game_pid > 0 && kill(g_shared->game_pid, 0) != 0) {
        say("bb-probe is gone (protect relay); exiting");
        _exit(0);
    }
}
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write) {
    pthread_mutex_lock(&protect_lock);
    const uint32_t seq = ++protect_seq;
    const BbGpuProtect record = {{BBGPU_REC_PROTECT, sizeof record}, address, size, (uint32_t)read, (uint32_t)write, seq, 0};
    while (bbgpu_ring_push(g_shared, &g_shared->protect, &record, UINT64_C(1000000000))) game_alive_or_exit();
    pthread_mutex_unlock(&protect_lock);
    for (;;) {
        const uint32_t done = __atomic_load_n(&g_shared->protect_done, __ATOMIC_ACQUIRE);
        if ((int32_t)(done - seq) >= 0) return;
        if (bbgpu_wait_word(&g_shared->protect_done, done, UINT64_C(1000000000))) game_alive_or_exit();
    }
}

/* Timebase: the runtime's process clock (CLOCK_MONOTONIC is system-wide) and guest TSC rate. */
uint64_t runtime_process_time_counter(void) {
    return clock_gettime_nsec_np(CLOCK_MONOTONIC) - g_shared->process_start_ns;
}
uint64_t runtime_process_time_us(void) { return runtime_process_time_counter() / 1000; }
uint64_t runtime_tsc_frequency(void) { return g_shared->tsc_hz; }

/* bb-probe-only services: AvPlayer, guest thread bookkeeping, restart. */
int runtime_file_translate(const char *guest, char *out, size_t size) {
    (void)guest; (void)out; (void)size;
    return -1;
}
void runtime_thread_attach_host(const char *name) { (void)name; }
void runtime_thread_set_blocked(const char *what, uint64_t resource) { (void)what; (void)resource; }
void runtime_thread_clear_blocked(void) {}
/* The settings menu's Restart: bb-probe restarts through run.sh (exec); this process ends when
 * it sees the exec (main.c parent_watch) and the new bb-probe starts a new host. */
void runtime_restart(void) {
    say("restart requested: asking bb-probe to restart");
    bbgpu_split_post_restart();
}
