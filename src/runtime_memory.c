// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
/* PS4 virtual memory: direct (physical) pool, flexible memory, reservations,
 * protection changes and queries. One sparse memfd backs the whole direct
 * pool, so any mapping of any physical range aliases the same storage.
 * Guest addresses are placed below 1 TiB (PS4 user range): GPU descriptors
 * encode 40-bit addresses, so host-default 0x7f... addresses would not fit. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <pthread/qos.h>
#include "gpu_process.h"
#endif
/* sceKernelGetDirectMemorySize on retail PS4: 5056 MiB. BB_DMEM_MB raises it (the resolution
 * patches above 1080p need about 4 GiB more; run.sh sets it). */
static uint64_t pool_size_bytes(void) {
    static uint64_t size;
    if (!size) {
        const char *env = getenv("BB_DMEM_MB");
        uint64_t mb = env ? strtoull(env, NULL, 10) : 0;
        if (mb < 5056 || mb > 16384) mb = 5056;
        size = mb * 1024 * 1024;
    }
    return size;
}
#define POOL_SIZE pool_size_bytes()
#define FLEXIBLE_SIZE (UINT64_C(448) * 1024 * 1024)
#define PAGE UINT64_C(16384)
#ifdef __APPLE__
/* Apple Silicon keeps [0xfc0000000, 0x7000000000) as a permanent no-access hole in every
 * process (Rosetta too): mmap there fails with EACCES, so the guest range starts above it. */
#define USER_MIN UINT64_C(0x7000000000)
#else
#define USER_MIN UINT64_C(0x1000000000)
#endif
#define USER_MAX UINT64_C(0xfc00000000)
#define LIMIT 4096
#define INVALID ((int32_t)UINT32_C(0x80020016))
#define NO_MEMORY ((int32_t)UINT32_C(0x8002000c))
#define ACCESS ((int32_t)UINT32_C(0x8002000d))
#define NO_SPACE ((int32_t)UINT32_C(0x80020023))
#define MAP_FIXED_FLAG 0x10
#define MAP_NO_OVERWRITE 0x80
enum { KIND_RESERVED=1, KIND_DIRECT, KIND_FLEXIBLE };
typedef struct { uint64_t start, size; int type, used; } Block;
typedef struct { uintptr_t start, end; int kind, prot, type; uint64_t phys; } Vma;
typedef struct {
    uintptr_t start, end; uint64_t offset; int32_t protection, memory_type;
    uint32_t flags; char name[32];
} VirtualQueryInfo; /* flags: flexible, direct, stack, pooled, committed */
typedef struct { void *start; uint64_t offset, length; int8_t prot, type; int16_t reserved; int32_t operation; } BatchEntry;
_Static_assert(sizeof(VirtualQueryInfo)==72,"PS4 virtual query info layout");
_Static_assert(sizeof(BatchEntry)==32,"PS4 batch map entry layout");

/* Region table lock. Nested reads are counted per thread. */
static HostRwlock lock=HOST_RWLOCK_INIT;
static __thread unsigned exclusive_depth, shared_depth;
static uint64_t table_generation;
/* BB_FRAME_STATS: how long the GPU protect relay waits for this lock and who holds it for writing
 * (stutter plan phase 0, performance/experiments/stutter/PLAN.md). */
static int lock_stats=-1;
static int lock_stats_on(void) {
    if (lock_stats<0) lock_stats=getenv("BB_FRAME_STATS")!=NULL;
    return lock_stats;
}
static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static __thread uint64_t write_held_since;
static uint64_t write_held_max_ns;
static char write_held_max_name[32];
static void write_lock(void) {
    if (!exclusive_depth++) {
        host_write_lock(&lock); __atomic_add_fetch(&table_generation,1,__ATOMIC_ACQ_REL);
        if (lock_stats_on()) write_held_since=now_ns();
    }
}
static void write_unlock(void) {
    if (!--exclusive_depth) {
        if (lock_stats_on() && write_held_since) {
            uint64_t held=now_ns()-write_held_since;
            write_held_since=0;
            if (held>__atomic_load_n(&write_held_max_ns,__ATOMIC_RELAXED)) {
                __atomic_store_n(&write_held_max_ns,held,__ATOMIC_RELAXED);
                pthread_getname_np(pthread_self(),write_held_max_name,sizeof write_held_max_name);
            }
        }
        __atomic_add_fetch(&table_generation,1,__ATOMIC_RELEASE); host_write_unlock(&lock);
    }
}
static void read_lock(void) { if (!exclusive_depth && !shared_depth++) host_read_lock(&lock); }
static void read_unlock(void) { if (!exclusive_depth && !--shared_depth) host_read_unlock(&lock); }
static Block blocks[LIMIT];
static Vma *vmas; static size_t vma_count, vma_capacity;
static int pool_fd=-1;
static unsigned char *backing_base; /* second view of the pool: host writes bypass guest/GPU page protection */
#ifdef __APPLE__
/* M1 of docs/DESIGN_NATIVE_GPU_PROCESS.md: the pool is a Mach named memory entry, which a
 * native (arm64) GPU process can map at the same addresses. BB_SHARED_GUEST_MEMORY=0 keeps the
 * earlier private anonymous pool (comparison, fallback). */
static mach_port_t pool_entry=MACH_PORT_NULL;
static int shared_pool(void) {
    static int value=-1;
    if (value<0) { const char *env=getenv("BB_SHARED_GUEST_MEMORY"); value=!(env && env[0]=='0'); }
    return value;
}
/* The pool's memory entry (MACH_PORT_NULL before the first guest map or without sharing). */
mach_port_t runtime_memory_pool_entry(void) { return pool_entry; }
/* M2: guest view changes forwarded to the native GPU process (gpu_process.c), under the lock. */
static RuntimeMemoryMirror mirror;
#define MIRROR(op,address,size,phys,prot) do { if (mirror) mirror(op,address,size,phys,prot); } while (0)
#else
#define MIRROR(op,address,size,phys,prot) ((void)0)
#endif
#define FLEX_SPAN (UINT64_C(1024) * 1024 * 1024)
static uint64_t flex_bitmap[FLEX_SPAN/PAGE/64];

/* Recent memory operations ring buffer for crash diagnostics */
typedef struct {
    uint64_t tick;
    int kind; /* 1: map, 2: unmap, 3: protect, 4: release */
    uintptr_t addr;
    uint64_t size;
    int prot;
    uint32_t tid;
} MemOpLog;
static MemOpLog g_mem_op_ring[64];
static uint32_t g_mem_op_count = 0;
static void log_mem_op(int kind, uintptr_t addr, uint64_t size, int prot) {
    uint32_t idx = __atomic_fetch_add(&g_mem_op_count, 1, __ATOMIC_RELAXED);
    MemOpLog *e = &g_mem_op_ring[idx & 63];
    e->tick = host_monotonic_ns();
    e->kind = kind;
    e->addr = addr;
    e->size = size;
    e->prot = prot;
#ifdef __APPLE__
    e->tid = (uint32_t)gettid(); /* pthread_t is a pointer here */
#else
    e->tid = (uint32_t)host_thread_id();
#endif
}
void runtime_memory_dump_recent_ops(void *file_handle) {
    FILE *f = (FILE *)file_handle;
    if (!f) return;
    fprintf(f, "\n--- LAST 64 MEMORY OPERATIONS ---\n");
    uint32_t total = __atomic_load_n(&g_mem_op_count, __ATOMIC_RELAXED);
    uint32_t count = total < 64 ? total : 64;
    uint32_t start = total > 64 ? (total - 64) : 0;
    for (uint32_t i = 0; i < count; ++i) {
        MemOpLog *e = &g_mem_op_ring[(start + i) & 63];
        const char *k = e->kind == 1 ? "MAP" : e->kind == 2 ? "UNMAP" : e->kind == 3 ? "PROTECT" : "RELEASE";
        fprintf(f, "  #%2u: [%7s] addr=0x%" PRIxPTR ", size=0x%" PRIx64 ", prot=0x%x (tid=%u, tick=%" PRIu64 ")\n",
                i + 1, k, e->addr, e->size, e->prot, e->tid, e->tick);
    }
}

/* GPU hooks (bbgpu): notified outside the lock, in order, after each operation. */
typedef void (*GpuRange)(uintptr_t address, uint64_t size);
static GpuRange hook_map, hook_unmap, hook_invalidate;
enum { HOOK_MAP, HOOK_UNMAP, HOOK_INVALIDATE };
typedef struct { int kind; uintptr_t address; uint64_t size; } PendingHook;
static PendingHook pending[64]; static size_t pending_count;
static size_t allocations, maps, flexible_maps, protects, queries;
static uint64_t live_bytes, flexible_bytes;

static int valid_alignment(uint64_t a) { return a >= PAGE && a <= POOL_SIZE && !(a & (a-1)); }
static uint64_t align_up(uint64_t n, uint64_t a) { return (n+a-1) & ~(a-1); }
static int host_prot(int prot) {
    return ((prot & 0x11) ? PROT_READ : 0) | ((prot & 0x22) ? PROT_WRITE|PROT_READ : 0) | ((prot & 4) ? PROT_EXEC|PROT_READ : 0);
}
#ifdef __APPLE__
/* The guest range is reserved (no access) once: place() maps over it with
 * MAP_FIXED semantics, which on macOS silently replace whatever host library got there
 * first (x86-64 mmap(NULL) under Rosetta hands out low addresses). Unmaps restore it. */
static int space(void) {
    static int state; /* 1 held, -1 failed */
    if (!state) {
        void *p=mmap((void *)USER_MIN,USER_MAX-USER_MIN,PROT_NONE,MAP_PRIVATE|MAP_ANON|MAP_NORESERVE,-1,0);
        state = p==(void *)USER_MIN ? 1 : -1;
        if (state<0) {
            if (p!=MAP_FAILED) munmap(p,USER_MAX-USER_MIN);
            fprintf(stderr,"Runtime: guest range 0x%" PRIx64 "-0x%" PRIx64 " is taken\n",USER_MIN,USER_MAX);
        }
    }
    return state<0 ? -1 : 0;
}
int runtime_memory_reserve_space(void) { return space(); }
#else
int runtime_memory_reserve_space(void) { return 0; }
#endif
static int pool(void) {
    if (pool_fd>=0) return 0;
#ifdef __APPLE__
    /* No memfd/fallocate, and a shm object cannot drop pages: F_PUNCHHOLE is for files
     * (which would write back), MADV_FREE_REUSABLE keeps shm data, MADV_ZERO is ENOTSUP
     * (always, under Rosetta). So the pool is one anonymous region; guest views are
     * mach_vm_remap aliases of it (shared, not copied) and a punch maps fresh zero pages
     * over its range: the old pages go with their last view (callers unmap views first).
     * Placed above 1 TiB, out of the way of the low ranges. */
    if (shared_pool()) {
        /* A named entry: guest views and the backing view map it (shared object), and so can a
         * second process. Pages are created on first touch, as for anonymous memory. */
        memory_object_size_t size=POOL_SIZE+FLEX_SPAN;
        if (mach_make_memory_entry_64(mach_task_self(),&size,0,
                                      MAP_MEM_NAMED_CREATE|VM_PROT_READ|VM_PROT_WRITE|VM_PROT_EXECUTE,
                                      &pool_entry,MACH_PORT_NULL)!=KERN_SUCCESS || size<POOL_SIZE+FLEX_SPAN) {
            fputs("Runtime: cannot create the shared guest memory entry\n",stderr);
            return -1;
        }
        mach_vm_address_t at=UINT64_C(1)<<40;
        if (mach_vm_map(mach_task_self(),&at,POOL_SIZE+FLEX_SPAN,0,VM_FLAGS_ANYWHERE,pool_entry,0,FALSE,
                        VM_PROT_READ|VM_PROT_WRITE,VM_PROT_READ|VM_PROT_WRITE,VM_INHERIT_NONE)!=KERN_SUCCESS)
            return -1;
        backing_base=(unsigned char *)(uintptr_t)at; pool_fd=0;
        return 0;
    }
    void *view=mmap((void *)(UINT64_C(1)<<40),POOL_SIZE+FLEX_SPAN,PROT_READ|PROT_WRITE,
                    MAP_PRIVATE|MAP_ANON|MAP_NORESERVE,-1,0);
    if (view==MAP_FAILED) return -1;
    backing_base=view; pool_fd=0;
    return 0;
#else
    pool_fd=memfd_create("bb-guest-memory", MFD_CLOEXEC);
    if (pool_fd<0 || ftruncate(pool_fd,(off_t)(POOL_SIZE+FLEX_SPAN))) return -1;
    void *view=mmap(NULL,POOL_SIZE+FLEX_SPAN,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_NORESERVE,pool_fd,0);
    if (view==MAP_FAILED) return -1;
    backing_base=view;
    return 0;
#endif
}
static void punch(uint64_t phys, uint64_t size) {
#ifdef __APPLE__
    if (backing_base && shared_pool()) {
        /* Fresh pages mapped over the range would leave other views (another process) on the
         * old object, and MADV_ZERO is ENOTSUP under Rosetta: zero in place, only the pages that
         * may hold data (resident, compressed or paged out), so untouched ones stay uncommitted. */
        const size_t host_page=(size_t)getpagesize(), chunk=UINT64_C(64)<<20;
        static char vec[(UINT64_C(64)<<20)/4096];
        for (uint64_t off=0; off<size; off+=chunk) {
            const size_t n=(size_t)(size-off<chunk ? size-off : chunk);
            unsigned char *at=backing_base+phys+off;
            if (mincore(at,n,vec)) { memset(at,0,n); continue; }
            for (size_t page=0; page*host_page<n; ++page)
                if (vec[page]) memset(at+page*host_page,0,host_page);
        }
    } else if (backing_base)
        mmap(backing_base+phys,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_NORESERVE|MAP_FIXED,-1,0);
#else
    fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)phys,(off_t)size);
#endif
}
static void queue_hook(int kind, uintptr_t address, uint64_t size) {
    GpuRange hook = kind==HOOK_MAP ? hook_map : kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;
    if (!hook) return;
    if (pending_count==sizeof(pending)/sizeof(*pending)) { fputs("STOP: GPU hook queue overflow\n",stderr); exit(21); }
    pending[pending_count++]=(PendingHook){kind,address,size};
}
/* Runs queued GPU notifications; callers invoke it after dropping the lock. */
static void flush_hooks(void) {
    for (;;) {
        write_lock();
        if (!pending_count) { write_unlock(); return; }
        PendingHook h=pending[0];
        memmove(pending,pending+1,--pending_count*sizeof(*pending));
        GpuRange hook = h.kind==HOOK_MAP ? hook_map : h.kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;
        write_unlock();
        if (hook) hook(h.address,h.size);
    }
}
static int flex_test(uint64_t page) { return (int)((flex_bitmap[page/64]>>(page%64))&1); }
static void flex_set(uint64_t first, uint64_t count, int used) {
    for (uint64_t p=first;p<first+count;++p) {
        if (used) flex_bitmap[p/64]|=UINT64_C(1)<<(p%64); else flex_bitmap[p/64]&=~(UINT64_C(1)<<(p%64));
    }
}
static uint64_t flex_alloc(uint64_t size) {
    uint64_t pages=size/PAGE, total=FLEX_SPAN/PAGE, run=0;
    for (uint64_t p=0;p<total;++p) {
        run = flex_test(p) ? 0 : run+1;
        if (run==pages) { flex_set(p+1-pages,pages,1); return POOL_SIZE+(p+1-pages)*PAGE; }
    }
    return UINT64_MAX;
}
static void flex_free(uint64_t phys, uint64_t size) {
    flex_set((phys-POOL_SIZE)/PAGE,size/PAGE,0);
#ifdef __APPLE__
    punch(phys,size);
#else
    fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)phys,(off_t)size);
#endif
}
static size_t vma_index(uintptr_t a) { /* first VMA with end > a */
    size_t lo=0, hi=vma_count;
    while (lo<hi) { size_t mid=(lo+hi)/2; if (vmas[mid].end<=a) lo=mid+1; else hi=mid; }
    return lo;
}
static int vma_insert(size_t at, Vma v) {
    if (vma_count==vma_capacity) {
        size_t capacity=vma_capacity ? vma_capacity*2 : 256;
        Vma *next=realloc(vmas,capacity*sizeof(*vmas));
        if (!next) return -1;
        vmas=next; vma_capacity=capacity;
    }
    memmove(vmas+at+1,vmas+at,(vma_count-at)*sizeof(*vmas));
    vmas[at]=v; ++vma_count; return 0;
}
static void vma_erase(size_t at, size_t n) {
    memmove(vmas+at,vmas+at+n,(vma_count-at-n)*sizeof(*vmas)); vma_count-=n;
}
static int split_at(uintptr_t a) {
    size_t i=vma_index(a);
    if (i==vma_count || vmas[i].start>=a) return 0;
    Vma right=vmas[i];
    right.start=a;
    if (right.kind==KIND_DIRECT) right.phys+=a-vmas[i].start;
    vmas[i].end=a;
    return vma_insert(i+1,right);
}
/* Range [start,end) split on its edges; returns the first index inside it. */
static size_t carve(uintptr_t start, uintptr_t end, int *error) {
    *error = split_at(start) || split_at(end);
    return vma_index(start);
}
static int covered(uintptr_t start, uintptr_t end, int allow_reserved) {
    size_t i=vma_index(start); uintptr_t at=start;
    for (; at<end; ++i) {
        if (i==vma_count || vmas[i].start>at || (!allow_reserved && vmas[i].kind==KIND_RESERVED)) return 0;
        at=vmas[i].end;
    }
    return 1;
}
static int overlaps(uintptr_t start, uintptr_t end, int ignore_reserved) {
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)
        if (!ignore_reserved || vmas[i].kind!=KIND_RESERVED) return 1;
    return 0;
}
/* First-fit search in the PS4 user range, starting from the hint. */
static uintptr_t find_free(uintptr_t hint, uint64_t size, uint64_t alignment) {
    uintptr_t at=align_up(hint<USER_MIN ? USER_MIN : hint, alignment);
    for (size_t i=vma_index(at); at+size<=USER_MAX; ++i) {
        if (i==vma_count || vmas[i].start>=at+size) return at;
        at=align_up(vmas[i].end,alignment);
    }
    return 0;
}
static void drop_range(uintptr_t start, uintptr_t end) {
    int error; size_t i=carve(start,end,&error), n=0;
    while (i+n<vma_count && vmas[i+n].end<=end && vmas[i+n].start>=start) {
        if (vmas[i+n].kind==KIND_FLEXIBLE) {
            flexible_bytes-=vmas[i+n].end-vmas[i+n].start;
            flex_free(vmas[i+n].phys,vmas[i+n].end-vmas[i+n].start);
        }
        ++n;
    }
    vma_erase(i,n);
}
/* Places a host mapping; with MAP_FIXED it replaces what the guest had there. */
static int32_t place(void **inout, uint64_t size, int prot, int flags, uint64_t alignment,
                     int kind, int type, uint64_t phys) {
    uintptr_t address=(uintptr_t)*inout;
    if (flags & MAP_FIXED_FLAG) {
        if (!address || address%PAGE) return INVALID;
        if (address < USER_MIN || address + size > USER_MAX) return INVALID;
        if ((flags & MAP_NO_OVERWRITE) && overlaps(address,address+size,1)) return NO_MEMORY;
    } else {
        address=find_free(address,size,alignment);
        if (!address) return NO_MEMORY;
    }
#ifdef __APPLE__
    if (space()) return NO_MEMORY;
    void *mapped=(void *)address;
    if (kind!=KIND_RESERVED) {
        mach_vm_address_t at=address; vm_prot_t cur, max;
        const kern_return_t kr = shared_pool()
            ? mach_vm_map(mach_task_self(),&at,size,0,VM_FLAGS_FIXED|VM_FLAGS_OVERWRITE,pool_entry,phys,FALSE,
                          VM_PROT_READ|VM_PROT_WRITE,VM_PROT_ALL,VM_INHERIT_SHARE)
            : mach_vm_remap(mach_task_self(),&at,size,0,VM_FLAGS_FIXED|VM_FLAGS_OVERWRITE,mach_task_self(),
                            (mach_vm_address_t)(uintptr_t)(backing_base+phys),FALSE,&cur,&max,VM_INHERIT_SHARE);
        if (kr!=KERN_SUCCESS || mprotect(mapped,size,host_prot(prot))) return NO_MEMORY;
    } else if (mmap(mapped,size,PROT_NONE,MAP_PRIVATE|MAP_ANON|MAP_NORESERVE|MAP_FIXED,-1,0)==MAP_FAILED)
        return NO_MEMORY;
#else
    void *mapped = kind!=KIND_RESERVED
        ? mmap((void *)address,size,host_prot(prot),MAP_SHARED|MAP_FIXED,pool_fd,(off_t)phys)
        : mmap((void *)address,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED,-1,0);
    if (mapped==MAP_FAILED) return NO_MEMORY;
#endif
    drop_range(address,address+size);
    if (vma_insert(vma_index(address),(Vma){address,address+size,kind,prot,type,phys})) return NO_MEMORY;
    if (kind==KIND_FLEXIBLE) flexible_bytes+=size;
    if (kind!=KIND_RESERVED) queue_hook(HOOK_MAP,address,size);
    if (kind!=KIND_RESERVED) MIRROR(RUNTIME_MIRROR_MAP,address,size,phys,prot);
    else MIRROR(RUNTIME_MIRROR_UNMAP,address,size,0,0);
    log_mem_op(1, address, size, prot);
    *inout=mapped;
    return 0;
}
static int block_type(uint64_t phys) {
    for (int i=0;i<LIMIT;++i)
        if (blocks[i].used && phys>=blocks[i].start && phys-blocks[i].start<blocks[i].size) return blocks[i].type;
    return -1;
}
static int direct_range_allocated(uint64_t phys, uint64_t size) {
    for (uint64_t at=phys; at<phys+size;) {
        int found=0;
        for (int i=0;i<LIMIT;++i) if (blocks[i].used && at>=blocks[i].start && at-blocks[i].start<blocks[i].size) {
            at=blocks[i].start+blocks[i].size; found=1; break;
        }
        if (!found) return 0;
    }
    return 1;
}

static ABI uint64_t direct_size(void) { return POOL_SIZE; }
static ABI int32_t direct_allocate(int64_t low, int64_t high, uint64_t size,
                                  uint64_t alignment, int type, int64_t *out) {
    if (!alignment) alignment = PAGE;
    if (!out || low < 0 || high < 0 || !size || size % PAGE || !valid_alignment(alignment) || type < 0 || type > 10)
        return INVALID;
    uint64_t end = (uint64_t)high < POOL_SIZE ? (uint64_t)high : POOL_SIZE;
    if ((uint64_t)low >= end || size > end - (uint64_t)low) return NO_SPACE;
    write_lock();
    if (pool()) { write_unlock(); return NO_MEMORY; }
    uint64_t start = align_up((uint64_t)low, alignment);
    int slot = -1;
    for (int i=0; i<LIMIT; ++i) if (!blocks[i].used) { slot=i; break; }
    for (;;) {
        if (slot<0 || start > end || size > end-start) { write_unlock(); return NO_SPACE; }
        int overlap = 0;
        for (int i=0; i<LIMIT; ++i) {
            Block b = blocks[i];
            if (b.used && start < b.start+b.size && b.start < start+size) {
                start = align_up(b.start+b.size, alignment); overlap = 1; break;
            }
        }
        if (!overlap) break;
    }
    blocks[slot] = (Block){start, size, type, 1}; *out = (int64_t)start;
    ++allocations; live_bytes += size;
    write_unlock();
    printf("Runtime: direct allocation offset=0x%" PRIx64 ", size=%" PRIu64 " bytes, type=%d\n", start, size, type);
    return 0;
}
static int32_t map_direct_locked(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    if (!alignment) alignment=PAGE;
    if (!out || physical < 0 || (uint64_t)physical % PAGE || !size || size % PAGE || !valid_alignment(alignment) ||
        (prot & ~0x37)) return INVALID;
    if (!direct_range_allocated((uint64_t)physical,size)) return INVALID;
    int32_t result=place(out,size,prot,flags,alignment,KIND_DIRECT,block_type((uint64_t)physical),(uint64_t)physical);
    if (!result) ++maps;
    return result;
}
static ABI int32_t direct_map(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    write_lock();
    int32_t result=map_direct_locked(out,size,prot,flags,physical,alignment);
    write_unlock();
    flush_hooks();
    if (!result) printf("Runtime: mapped direct memory %p, size=%" PRIu64 ", prot=0x%x\n", *out, size, prot);
    return result;
}
static ABI int32_t direct_map_named(void **out, uint64_t size, int prot, int flags, int64_t physical,
                                    uint64_t alignment, const char *name) {
    (void)name; return direct_map(out,size,prot,flags,physical,alignment);
}
static int32_t unmap_locked(uintptr_t start, uint64_t size) {
    if (start%PAGE || !size) return INVALID;
    uint64_t end=start+align_up(size,PAGE);
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)
        if (vmas[i].kind!=KIND_RESERVED) {
            uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;
            queue_hook(HOOK_UNMAP,a,b-a);
        }
#ifdef __APPLE__
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i) { /* back to reserved */
        uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;
        if (mmap((void *)a,b-a,PROT_NONE,MAP_PRIVATE|MAP_ANON|MAP_NORESERVE|MAP_FIXED,-1,0)==MAP_FAILED) return INVALID;
    }
#else
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i) {
        uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;
        if (munmap((void *)a,b-a)) return INVALID;
    }
#endif
    MIRROR(RUNTIME_MIRROR_UNMAP,start,end-start,0,0);
    log_mem_op(2, start, size, 0);
    drop_range(start,end);
    return 0;
}
static ABI int32_t direct_unmap(void *address, uint64_t size) {
    /* The GPU forgets the range before it disappears (as shadPS4's early GPU unmap). */
    if (hook_unmap && !((uintptr_t)address%PAGE) && size) hook_unmap((uintptr_t)address,align_up(size,PAGE));
    write_lock();
    size_t before=pending_count;
    int32_t result=unmap_locked((uintptr_t)address,size);
    pending_count=before; /* already notified above */
    write_unlock();
    return result;
}
static ABI int32_t direct_release(uint64_t start, uint64_t size) {
    if (start % PAGE || size % PAGE) return INVALID;
    if (!size) return 0;
    write_lock();
    if (!direct_range_allocated(start,size)) { write_unlock(); return INVALID; }
    /* Mappings of released physical pages disappear with them. */
    for (size_t i=0; i<vma_count;) {
        Vma v=vmas[i];
        uint64_t length=v.end-v.start;
        if (v.kind==KIND_DIRECT && v.phys<start+size && start<v.phys+length) {
            uint64_t a=v.phys>start ? v.phys : start, b=v.phys+length<start+size ? v.phys+length : start+size;
            unmap_locked(v.start+(a-v.phys),b-a);
            i=vma_index(v.start);
            continue;
        }
        ++i;
    }
    for (int i=0; i<LIMIT; ++i) {
        Block *b=&blocks[i];
        if (!b->used || b->start>=start+size || start>=b->start+b->size) continue;
        uint64_t a=b->start>start ? b->start : start, e=b->start+b->size<start+size ? b->start+b->size : start+size;
        Block left=*b, right=*b;
        left.size=a-b->start; right.start=e; right.size=b->start+b->size-e;
        b->used=0;
        if (left.size) *b=left;
        if (right.size) for (int j=0;j<LIMIT;++j) if (!blocks[j].used) { blocks[j]=right; break; }
        live_bytes-=e-a;
    }
    punch(start,size); /* zero on reuse */
    log_mem_op(4, start, size, 0);
    write_unlock();
    flush_hooks();
    return 0;
}
static int32_t map_flexible_locked(void **inout, uint64_t size, int prot, int flags) {
    if (!inout || !size || size%PAGE || (prot & ~0x37)) return INVALID;
    if (flexible_bytes+size>FLEXIBLE_SIZE || pool()) return NO_MEMORY;
    uint64_t phys=flex_alloc(size);
    if (phys==UINT64_MAX) return NO_MEMORY;
    int32_t result=place(inout,size,prot,flags,PAGE,KIND_FLEXIBLE,0,phys);
    if (result) flex_free(phys,size); else ++flexible_maps;
    return result;
}
static ABI int32_t map_flexible(void **inout, uint64_t size, int prot, int flags) {
    write_lock();
    int32_t result=map_flexible_locked(inout,size,prot,flags);
    write_unlock();
    flush_hooks();
    if (!result) printf("Runtime: mapped flexible memory %p, size=%" PRIu64 ", prot=0x%x\n", *inout, size, prot);
    else fprintf(stderr,"Runtime: flexible map of %" PRIu64 " bytes failed (0x%x), in use=%" PRIu64 "\n",
                 size, (unsigned)result, flexible_bytes);
    return result;
}
static ABI int32_t map_flexible_named(void **inout, uint64_t size, int prot, int flags, const char *name) {
    (void)name; return map_flexible(inout,size,prot,flags);
}
static ABI int32_t release_flexible(void *address, uint64_t size) { return direct_unmap(address,size); }
static ABI int32_t reserve_range(void **inout, uint64_t size, int flags, uint64_t alignment) {
    if (!alignment) alignment=PAGE;
    if (!inout || !size || size%PAGE || !valid_alignment(alignment)) return INVALID;
    write_lock();
    if ((flags & MAP_FIXED_FLAG) && overlaps((uintptr_t)*inout,(uintptr_t)*inout+size,0)) {
        write_unlock(); return NO_MEMORY;
    }
    int32_t result=place(inout,size,0,flags,alignment,KIND_RESERVED,0,0);
    write_unlock();
    if (!result) printf("Runtime: reserved virtual range %p, size=%" PRIu64 "\n", *inout, size);
    return result;
}
static int32_t protect_locked(uintptr_t start, uint64_t size, int prot, int type) {
    uintptr_t end=start+align_up(size,PAGE);
    start&=~(uintptr_t)(PAGE-1);
    if ((prot & ~0x37) || !covered(start,end,0)) return INVALID;
    if (mprotect((void *)start,end-start,host_prot(prot))) return INVALID;
    int error; size_t i=carve(start,end,&error);
    if (error) return NO_MEMORY;
    for (; i<vma_count && vmas[i].start<end; ++i) { vmas[i].prot=prot; if (type>=0) vmas[i].type=type; }
    queue_hook(HOOK_INVALIDATE,start,end-start);
    MIRROR(RUNTIME_MIRROR_PROTECT,start,end-start,0,prot);
    log_mem_op(3, start, size, prot);
    ++protects;
    return 0;
}
static ABI int32_t kernel_mprotect(const void *address, uint64_t size, int prot) {
    write_lock();
    int32_t result=protect_locked((uintptr_t)address,size,prot,-1);
    write_unlock();
    flush_hooks();
    return result;
}
static ABI int32_t query_protection(void *address, void **start, void **end, uint32_t *prot) {
    read_lock();
    size_t i=vma_index((uintptr_t)address);
    if (i==vma_count || vmas[i].start>(uintptr_t)address || vmas[i].kind==KIND_RESERVED) {
        read_unlock(); return ACCESS;
    }
    /* Report the whole contiguous run with identical protection. */
    size_t first=i, last=i;
    while (first && vmas[first-1].end==vmas[first].start && vmas[first-1].kind!=KIND_RESERVED && vmas[first-1].prot==vmas[i].prot) --first;
    while (last+1<vma_count && vmas[last+1].start==vmas[last].end && vmas[last+1].kind!=KIND_RESERVED && vmas[last+1].prot==vmas[i].prot) ++last;
    if (start) *start=(void *)vmas[first].start;
    if (end) *end=(void *)vmas[last].end;
    if (prot) *prot=(uint32_t)vmas[i].prot;
    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);
    read_unlock();
    return 0;
}
static ABI int32_t virtual_query(const void *address, int flags, VirtualQueryInfo *info, uint64_t info_size) {
    if (!info || info_size!=sizeof(*info)) return INVALID;
    read_lock();
    size_t i=vma_index((uintptr_t)address);
    if (i==vma_count || (vmas[i].start>(uintptr_t)address && !(flags & 1))) { read_unlock(); return ACCESS; }
    Vma v=vmas[i];
    memset(info,0,sizeof(*info));
    info->start=v.start; info->end=v.end;
    info->offset=v.kind==KIND_DIRECT ? v.phys : 0;
    info->protection=v.prot; info->memory_type=v.type;
    info->flags=(v.kind==KIND_FLEXIBLE ? 1u : 0) | (v.kind==KIND_DIRECT ? 2u : 0) | (v.kind!=KIND_RESERVED ? 16u : 0);
    strcpy(info->name, v.kind==KIND_RESERVED ? "reserved" : v.kind==KIND_DIRECT ? "direct" : "flexible");
    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);
    read_unlock();
    return 0;
}
static ABI int32_t direct_memory_type(uint64_t phys, int *type, void **start, void **end) {
    read_lock();
    for (int i=0;i<LIMIT;++i) if (blocks[i].used && phys>=blocks[i].start && phys-blocks[i].start<blocks[i].size) {
        if (type) *type=blocks[i].type;
        if (start) *start=(void *)(uintptr_t)blocks[i].start;
        if (end) *end=(void *)(uintptr_t)(blocks[i].start+blocks[i].size);
        read_unlock(); return 0;
    }
    read_unlock();
    return ACCESS;
}
static ABI int32_t batch_map2(BatchEntry *entries, int count, int *processed, int flags) {
    if (!entries || count<0) return INVALID;
    int32_t result=0; int done=0;
    write_lock();
    for (; done<count; ++done) {
        BatchEntry *e=&entries[done];
        switch (e->operation) {
        case 0: result=map_direct_locked(&e->start,e->length,(uint8_t)e->prot,flags,(int64_t)e->offset,0); break;
        case 1: result=unmap_locked((uintptr_t)e->start,e->length); break;
        case 2: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,-1); break;
        case 3: result=map_flexible_locked(&e->start,e->length,(uint8_t)e->prot,flags); break;
        case 4: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,e->type); break;
        default: result=INVALID;
        }
        if (result) break;
    }
    write_unlock();
    flush_hooks();
    if (processed) *processed=done;
    return result;
}
static ABI int32_t batch_map(BatchEntry *entries, int count, int *processed) {
    return batch_map2(entries,count,processed,MAP_FIXED_FLAG);
}
static const RuntimeExport exports[]={
    {"sceKernelGetDirectMemorySize",direct_size}, {"sceKernelAllocateDirectMemory",direct_allocate},
    {"sceKernelMapDirectMemory",direct_map}, {"sceKernelMapNamedDirectMemory",direct_map_named},
    {"sceKernelReleaseDirectMemory",direct_release}, {"sceKernelMunmap",direct_unmap}, {"munmap",direct_unmap},
    {"sceKernelMapFlexibleMemory",map_flexible}, {"sceKernelMapNamedFlexibleMemory",map_flexible_named},
    {"sceKernelReleaseFlexibleMemory",release_flexible}, {"sceKernelReserveVirtualRange",reserve_range},
    {"sceKernelMprotect",kernel_mprotect}, {"sceKernelQueryMemoryProtection",query_protection},
    {"sceKernelVirtualQuery",virtual_query}, {"sceKernelGetDirectMemoryType",direct_memory_type},
    {"sceKernelBatchMap",batch_map}, {"sceKernelBatchMap2",batch_map2},
};
uintptr_t runtime_memory_resolve(const char *name) {
    /* Direct-memory NIDs are exercised by test_runtime.c before names are loaded. */
    if (!strcmp(name, "pO96TwzOm5E#p#J")) return (uintptr_t)direct_size;
    if (!strcmp(name, "rTXw65xmLIA#p#J")) return (uintptr_t)direct_allocate;
    if (!strcmp(name, "L-Q3LEjIbgA#p#J")) return (uintptr_t)direct_map;
    if (!strcmp(name, "MBuItvba6z8#p#J")) return (uintptr_t)direct_release;
    if (!strcmp(name, "cQke9UuBQOk#p#J")) return (uintptr_t)direct_unmap;
    return RUNTIME_LOOKUP(exports,name);
}
/* Host-owned memory the guest can see (image, stacks, trampolines) lives in
 * [LOW_MIN, USER_MIN): PS4 code packs pointers into 40-bit fields. */
#ifndef LOW_MIN
#define LOW_MIN UINT64_C(0x0800000000)
#endif
static uintptr_t low_next=LOW_MIN;
void *runtime_low_map(size_t size, int prot) {
    size=align_up(size,PAGE);
    write_lock();
#ifdef __APPLE__
    /* No MAP_FIXED_NOREPLACE: a hint is taken only when free, else the kernel returns the
     * next free range above it, where the search resumes. Stops below the hole at
     * 0xfc0000000 (see USER_MIN). */
    void *p=MAP_FAILED;
    /* At the top, start over from the bottom once: runtime_low_free unmaps large blocks, and
     * their ranges are found again from there. */
    for (int pass=0; pass<2 && p==MAP_FAILED; ++pass) {
        if (pass) low_next=LOW_MIN;
        while (low_next+size<=UINT64_C(0x0fc0000000)) {
            p=mmap((void *)low_next,size,prot,MAP_PRIVATE|MAP_ANON,-1,0);
            if (p==(void *)low_next) { low_next+=size+PAGE; break; } /* unmapped gap catches overruns */
            if (p!=MAP_FAILED) munmap(p,size);
            low_next = p!=MAP_FAILED && (uintptr_t)p>low_next ? align_up((uintptr_t)p,PAGE) : low_next+size+PAGE;
            p=MAP_FAILED;
        }
    }
    write_unlock();
    CHECK_LOW_ADDR(p);
    return p==MAP_FAILED ? NULL : p;
#else
    void *p=MAP_FAILED;
    while (low_next+size<=USER_MIN) {
        p=mmap((void *)low_next,size,prot,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
        low_next+=size+PAGE; /* unmapped gap catches overruns */
        if (p!=MAP_FAILED) break;
    }
    write_unlock();
    CHECK_LOW_ADDR(p);
    return p==MAP_FAILED ? NULL : p;
#endif
}

typedef struct LowHeader {
    uint32_t magic;
    uint32_t size_class;
    uint64_t total_size;
} LowHeader;

typedef struct LowFreeNode {
    struct LowFreeNode *next;
} LowFreeNode;

static const size_t low_classes[] = { 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536 };
#define NUM_LOW_CLASSES (sizeof(low_classes)/sizeof(low_classes[0]))
static LowFreeNode *low_freelists[NUM_LOW_CLASSES];
/* BB_MEM_STATS: live blocks per size class (allocations minus frees) and large blocks. */
static _Atomic int64_t low_live[NUM_LOW_CLASSES + 1];
static _Atomic int64_t low_large_bytes;
static int64_t low_created[NUM_LOW_CLASSES]; /* blocks carved from slabs, under low_alloc_lock */
static HostMutex low_alloc_lock = HOST_MUTEX_INIT;

void *runtime_low_alloc(size_t size) {
    if (!size) size = 1;
    size_t needed = size + sizeof(LowHeader);
    int sc = -1;
    for (size_t i = 0; i < NUM_LOW_CLASSES; ++i) {
        if (low_classes[i] >= needed) {
            sc = (int)i;
            break;
        }
    }
    if (sc != -1) {
        host_mutex_lock(&low_alloc_lock);
        LowFreeNode *node = low_freelists[sc];
        if (node) {
            low_freelists[sc] = node->next;
            host_mutex_unlock(&low_alloc_lock);
        } else {
            size_t block_size = low_classes[sc];
            size_t slab_size = 65536;
            if (slab_size < block_size * 4) slab_size = block_size * 4;
            slab_size = align_up(slab_size, PAGE);
            unsigned char *slab = (unsigned char *)runtime_low_map(slab_size, PROT_READ | PROT_WRITE);
            if (!slab) {
                host_mutex_unlock(&low_alloc_lock);
                return NULL;
            }
            node = (LowFreeNode *)slab;
            size_t count = slab_size / block_size;
            LowFreeNode *head = NULL;
            for (size_t j = 1; j < count; ++j) {
                LowFreeNode *n = (LowFreeNode *)(slab + j * block_size);
                n->next = head;
                head = n;
            }
            low_freelists[sc] = head;
            low_created[sc] += (int64_t)count;
            host_mutex_unlock(&low_alloc_lock);
        }
        LowHeader *hdr = (LowHeader *)node;
        ++low_live[sc];
        hdr->magic = 0x4C4F5741;
        hdr->size_class = (uint32_t)sc;
        hdr->total_size = low_classes[sc];
        void *ret = (void *)(hdr + 1);
        CHECK_LOW_ADDR(ret);
        return ret;
    } else {
        size_t map_size = align_up(needed, PAGE);
        void *p = runtime_low_map(map_size, PROT_READ | PROT_WRITE);
        if (!p) return NULL;
        LowHeader *hdr = (LowHeader *)p;
        hdr->magic = 0x4C4F5741;
        hdr->size_class = 0xFFFFFFFF;
        hdr->total_size = map_size;
        ++low_live[NUM_LOW_CLASSES];
        low_large_bytes += (int64_t)map_size;
        void *ret = (void *)(hdr + 1);
        CHECK_LOW_ADDR(ret);
        return ret;
    }
}

void *runtime_low_malloc(size_t size) { return runtime_low_alloc(size); }

void *runtime_low_calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = runtime_low_alloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void runtime_low_free(void *ptr) {
    if (!ptr) return;
    LowHeader *hdr = (LowHeader *)ptr - 1;
    if (hdr->magic != 0x4C4F5741) return;
    const uint32_t sc = hdr->size_class;
    if (sc < NUM_LOW_CLASSES) {
        --low_live[sc];
        host_mutex_lock(&low_alloc_lock);
        /* The link overlays the header's magic and size class: read the class first (it was
         * read after the store, which filed every freed block under the class named by the
         * upper half of the old list head: the small classes leaked a slab every few
         * allocations (~4.5 GB/h of play) and the 8 KiB class handed out 64-byte blocks). */
        LowFreeNode *node = (LowFreeNode *)hdr;
        node->next = low_freelists[sc];
        low_freelists[sc] = node;
        host_mutex_unlock(&low_alloc_lock);
    } else if (sc == 0xFFFFFFFF) {
        /* A block of its own (over 64 KiB): give it back (it used to stay mapped for good). */
        hdr->magic = 0;
        --low_live[NUM_LOW_CLASSES];
        low_large_bytes -= (int64_t)hdr->total_size;
        munmap(hdr, hdr->total_size);
    }
}

void *runtime_low_alloc_aligned(size_t size, size_t align) {
    if (align < 16) align = 16;
    size_t needed = size + align + sizeof(void *);
    void *raw = runtime_low_alloc(needed);
    if (!raw) return NULL;
    uintptr_t p = (uintptr_t)raw + sizeof(void *);
    p = (p + align - 1) & ~(align - 1);
    ((void **)p)[-1] = raw;
    void *ret = (void *)p;
    CHECK_LOW_ADDR(ret);
    return ret;
}

void runtime_low_free_aligned(void *ptr) {
    if (!ptr) return;
    void *raw = ((void **)ptr)[-1];
    runtime_low_free(raw);
}
/* ---- GPU library interface (gpu/shim/bbgpu.cpp) ---- */
/* Optimizations switched off at run time (diagnostics): the number in the file named by
 * BB_TOGGLE_FILE, re-read every 250 ms. Bits: 1 region cache, 2 fetch shader cache,
 * 4 page tracking early exit, 8 pending-op poll limit, 16 threaded Vulkan recording,
 * 32 texture descriptor cache, 64 lock-free upload check, 128 image lookup cache,
 * 256 buffer uploads on the recording thread, 512 barrier tracker insert memo,
 * 1024 texture binding memo, 2048 page-granular read tracking for barriers,
 * 4096 resource sharps read by the draw-preparation workers,
 * 8192 prepared draws from the draw-preparation workers,
 * 16384 small read-only buffer copies on the recording thread,
 * 32768 hot pages (opt-in with BB_HOT_PAGES=1),
 * 65536 unprotect the 256 KiB window around a guest write fault (BB_FAULT_WINDOW KiB),
 * 131072 large guest memory copies split across copy threads (BB_COPY_THREADS),
 * 262144 with BB_ASYNC_FENCES=1: wait for guest copies at fences again,
 * 524288 small guest copies batched for the copy threads instead of the recording thread,
 * 1073741824 the lock-free UpdateImage path for clean, tracked images. */
/* Bits 32 and up: the draw pipeline and related GPU thread work (gpu/shim/bbport_toggles.h). */
uint64_t runtime_disabled_optimizations;
__thread RuntimeRecoverBuf *runtime_fault_recover;
static void *toggle_watcher(void *path) {
    for (unsigned long long last=ULLONG_MAX;;) {
        FILE *f=fopen(path,"r");
        unsigned long long value=0;
        if (f) { if (fscanf(f,"%llu",&value)!=1) value=0; fclose(f); }
        if (value!=last) {
            __atomic_store_n(&runtime_disabled_optimizations,(uint64_t)value,__ATOMIC_RELEASE);
            printf("Runtime: disabled optimizations mask=%llu\n",value);
            last=value;
        }
        struct timespec t={0,250000000}; nanosleep(&t,NULL);
    }
    return NULL;
}
void runtime_memory_set_gpu_hooks(GpuRange map, GpuRange unmap, GpuRange invalidate) {
    const char *toggles=getenv("BB_TOGGLE_FILE");
    static pthread_t watcher;
    if (toggles && !watcher) pthread_create(&watcher,NULL,toggle_watcher,(void *)toggles);
    write_lock();
    hook_map=map; hook_unmap=unmap; hook_invalidate=invalidate;
    for (size_t i=0;i<vma_count;++i) if (vmas[i].kind!=KIND_RESERVED) queue_hook(HOOK_MAP,vmas[i].start,vmas[i].end-vmas[i].start);
    write_unlock();
    flush_hooks();
}
/* Guest-visible writes from the GPU side (fence labels, WriteData, downloads): every byte is
 * stored once. glibc's memcpy copies 4-7 bytes as two overlapping 4-byte stores (the same
 * 4 bytes twice for a 4-byte label): a thread preempted between them stored the label again
 * after the guest had seen it and freed the block that held it (a free-list link ending in
 * 0x00000004: guest fault 0x263b8e7). Aligned 2, 4 and 8-byte pieces are single stores. */
static void store_once(unsigned char *dst, const unsigned char *src, uint64_t n) {
    if (n > 64) { memcpy(dst,src,n); return; }
    while (n) {
        const uintptr_t at=(uintptr_t)dst;
        if (n>=8 && !(at&7)) { uint64_t v; memcpy(&v,src,8); __atomic_store_n((uint64_t *)dst,v,__ATOMIC_RELEASE); dst+=8; src+=8; n-=8; }
        else if (n>=4 && !(at&3)) { uint32_t v; memcpy(&v,src,4); __atomic_store_n((uint32_t *)dst,v,__ATOMIC_RELEASE); dst+=4; src+=4; n-=4; }
        else if (n>=2 && !(at&1)) { uint16_t v; memcpy(&v,src,2); __atomic_store_n((uint16_t *)dst,v,__ATOMIC_RELEASE); dst+=2; src+=2; n-=2; }
        else { __atomic_store_n(dst,*src,__ATOMIC_RELEASE); ++dst; ++src; --n; }
    }
}

/* Writes through the backing view; 0 when part of the range has no backing. */
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size) {
    read_lock();
    int ok=1;
    for (uintptr_t at=address, end=address+size; at<end && ok;) {
        size_t i=vma_index(at);
        if (i==vma_count || vmas[i].start>at || vmas[i].kind==KIND_RESERVED) { ok=0; break; }
        uint64_t n=(vmas[i].end<end ? vmas[i].end : end)-at;
        store_once(backing_base+vmas[i].phys+(at-vmas[i].start),(const unsigned char *)data+(at-address),n);
        at+=n;
    }
    read_unlock();
    return ok;
}
/* Reads through the backing view (diagnostics: compare with the guest's mapping); 0 when part
 * of the range has no backing. */
int runtime_memory_read_backing(uintptr_t address, void *out, uint64_t size) {
    read_lock();
    int ok=1;
    for (uintptr_t at=address, end=address+size; at<end && ok;) {
        size_t i=vma_index(at);
        if (i==vma_count || vmas[i].start>at || vmas[i].kind==KIND_RESERVED) { ok=0; break; }
        uint64_t n=(vmas[i].end<end ? vmas[i].end : end)-at;
        memcpy((unsigned char *)out+(at-address),backing_base+vmas[i].phys+(at-vmas[i].start),n);
        at+=n;
    }
    read_unlock();
    return ok;
}
/* Per-thread cache of recently found regions for the GPU's per-draw queries:
 * entries hold for as long as the table generation they were read at. */
typedef struct { uint64_t generation; uintptr_t start, end; int kind; } CachedRegion;
static __thread CachedRegion region_cache[8];
static __thread unsigned region_cache_next;
static const CachedRegion *cached_region(uintptr_t address) {
    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    if (generation & 1) return NULL;
    for (unsigned i=0;i<8;++i) {
        const CachedRegion *c=&region_cache[i];
        if (c->generation==generation && c->end && c->start<=address && address<c->end) return c;
    }
    return NULL;
}
/* Called under the read lock after a lookup of address. */
static void cache_region(size_t i) {
    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    if (i==vma_count || (generation & 1)) return;
    region_cache[region_cache_next++ % 8]=(CachedRegion){generation,vmas[i].start,vmas[i].end,vmas[i].kind};
}
/* Mapped length from address, up to size, across adjacent mappings. */
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c && c->kind!=KIND_RESERVED && size<=c->end-address) return size;
    read_lock();
    { size_t i=vma_index(address); if (i<vma_count && vmas[i].start<=address) cache_region(i); }
    uint64_t length=0;
    for (size_t i=vma_index(address); i<vma_count && length<size; ++i) {
        if (vmas[i].start>address+length || vmas[i].kind==KIND_RESERVED) break;
        length=vmas[i].end-address;
    }
    read_unlock();
    return length<size ? length : size;
}
/* bbport: protection and direct memory type of the mapping at address (type -1: not direct
 * memory); 0 when nothing is mapped there. */
int runtime_memory_vma_info(uintptr_t address, int *prot, int *type, uintptr_t *end) {
    read_lock();
    const size_t i = vma_index(address);
    const int ok = i < vma_count && vmas[i].start <= address && vmas[i].kind != KIND_RESERVED;
    if (ok) {
        if (prot) *prot = vmas[i].prot;
        if (type) *type = vmas[i].kind == KIND_DIRECT ? vmas[i].type : -1;
        if (end) *end = vmas[i].end;
    }
    read_unlock();
    return ok;
}
/* Region around address: [start,end) mapped or a gap; 0 past the last mapping. */
int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c) { *start=c->start; *end=c->end; *mapped=c->kind!=KIND_RESERVED; return 1; }
    read_lock();
    size_t i=vma_index(address);
    int found=i<vma_count;
    if (found && vmas[i].start<=address) {
        cache_region(i);
        *start=vmas[i].start; *end=vmas[i].end; *mapped=vmas[i].kind!=KIND_RESERVED;
    } else if (found) {
        *start=i ? vmas[i-1].end : 0; *end=vmas[i].start; *mapped=0;
    }
    read_unlock();
    return found;
}
/* GPU page tracking: restrict host access, never beyond the guest's own protection. */
/* BB_FRAME_STATS: the relay's work in the game (bb:GpuProtect), printed every 5 s. */
static struct { uint64_t calls, pages, lock_ns, lock_max_ns, slow_waits, mprotect_ns, mprotect_max_ns,
                         window_start; } protect_stats;
static void protect_stats_print(uint64_t now) {
    if (!protect_stats.window_start) { protect_stats.window_start=now; return; }
    if (now-protect_stats.window_start<5000000000ull) return;
    struct timespec wall; clock_gettime(CLOCK_REALTIME,&wall);
    uint64_t held=__atomic_exchange_n(&write_held_max_ns,0,__ATOMIC_RELAXED);
    printf("Game protect (%.1f s): %llu calls, %llu pages; waiting for the memory table lock %.1f ms "
           "(max %.2f, %llu waits over 2 ms), mprotect %.1f ms (max %.2f); table write lock held "
           "at most %.2f ms (%s) | t=%.3f\n",
           (now-protect_stats.window_start)/1e9,(unsigned long long)protect_stats.calls,
           (unsigned long long)protect_stats.pages,protect_stats.lock_ns/1e6,protect_stats.lock_max_ns/1e6,
           (unsigned long long)protect_stats.slow_waits,protect_stats.mprotect_ns/1e6,
           protect_stats.mprotect_max_ns/1e6,held/1e6,held ? write_held_max_name : "-",
           wall.tv_sec+wall.tv_nsec/1e9);
    fflush(stdout);
    memset(&protect_stats,0,sizeof protect_stats);
    protect_stats.window_start=now;
}
/* Stutter plan phase 2: the GPU protect relay's consumer (bb:GpuProtect) runs at user-interactive
 * QoS unless bit 16 of BB_TOGGLE_FILE's second number is set. The game's own toggle watcher runs
 * only in single-process mode, so this one starts with the first relay. */
static uint64_t toggles2_game;
static void *toggles2_watcher(void *path) {
    for (;;) {
        FILE *f=fopen(path,"r");
        unsigned long long first=0, second=0;
        if (f) { if (fscanf(f,"%llu %llu",&first,&second)!=2) second=0; fclose(f); }
        __atomic_store_n(&toggles2_game,(uint64_t)second,__ATOMIC_RELAXED);
        struct timespec t={0,250000000}; nanosleep(&t,NULL);
    }
    return NULL;
}
static void protect_thread_qos(void) {
    static int watcher_started;
    static __thread int applied=-1;
    if (!__atomic_exchange_n(&watcher_started,1,__ATOMIC_ACQ_REL)) {
        const char *toggles=getenv("BB_TOGGLE_FILE");
        pthread_t watcher;
        if (toggles && pthread_create(&watcher,NULL,toggles2_watcher,(void *)toggles)==0) pthread_detach(watcher);
    }
    const int want=(__atomic_load_n(&toggles2_game,__ATOMIC_RELAXED) & 16) ? 0 : 1;
    if (want!=applied) {
#ifdef __APPLE__
        pthread_set_qos_class_self_np(want ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_DEFAULT,0);
#endif
        applied=want;
    }
}
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write) {
    protect_thread_qos();
    const int stats=lock_stats_on();
    const uint64_t start=stats ? now_ns() : 0;
    read_lock();
    const uint64_t locked=stats ? now_ns() : 0;
    for (size_t i=vma_index(address); i<vma_count && vmas[i].start<address+size; ++i) {
        if (vmas[i].kind==KIND_RESERVED) continue;
        uintptr_t a=vmas[i].start>address ? vmas[i].start : address;
        uintptr_t b=vmas[i].end<address+size ? vmas[i].end : address+size;
        int guest=host_prot(vmas[i].prot);
        int want=(read ? PROT_READ : 0)|(write ? PROT_WRITE|PROT_READ : 0)|(guest & PROT_EXEC);
        mprotect((void *)a,b-a,guest & want);
    }
    read_unlock();
    if (stats) {
        /* Single consumer (bb:GpuProtect in split mode); single-process callers may race on these
         * diagnostics counters, which is harmless. */
        const uint64_t end=now_ns(), wait=locked-start, work=end-locked;
        protect_stats.calls++;
        protect_stats.pages+=(size+4095)/4096;
        protect_stats.lock_ns+=wait;
        if (wait>protect_stats.lock_max_ns) protect_stats.lock_max_ns=wait;
        if (wait>2000000) protect_stats.slow_waits++;
        protect_stats.mprotect_ns+=work;
        if (work>protect_stats.mprotect_max_ns) protect_stats.mprotect_max_ns=work;
        protect_stats_print(end);
    }
}
/* Host view of a guest range, for the GPU backend: the range must be mapped. */
int runtime_memory_is_mapped(uintptr_t address, uint64_t size) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c && c->kind!=KIND_RESERVED && size<=c->end-address) return 1;
    read_lock();
    int result=covered(address,address+size,0);
    read_unlock();
    return result;
}
#ifdef __APPLE__
uint64_t runtime_memory_pool_entry_size(void) { return POOL_SIZE+FLEX_SPAN; }
int runtime_memory_create_pool(void) {
    write_lock();
    int result=pool();
    write_unlock();
    return result;
}
void runtime_memory_set_mirror(RuntimeMemoryMirror fn) {
    write_lock();
    mirror=fn;
    if (fn) for (size_t i=0;i<vma_count;++i)
        if (vmas[i].kind!=KIND_RESERVED) fn(RUNTIME_MIRROR_MAP,vmas[i].start,vmas[i].end-vmas[i].start,vmas[i].phys,vmas[i].prot);
    write_unlock();
}
/* Checksum through the backing view (never protected), streamed across adjacent views. */
static int backing_checksum(uintptr_t address, uint64_t size, uint64_t *out) {
    uint64_t h=BBGPU_CHECKSUM_SEED;
    for (uintptr_t at=address, end=address+size; at<end;) {
        size_t i=vma_index(at);
        if (i==vma_count || vmas[i].start>at || vmas[i].kind==KIND_RESERVED) return 0;
        uint64_t n=(vmas[i].end<end ? vmas[i].end : end)-at;
        h=bbgpu_checksum(h,backing_base+vmas[i].phys+(at-vmas[i].start),n);
        at+=n;
    }
    *out=h;
    return 1;
}
int runtime_memory_mirror_checksum(uintptr_t address, uint64_t size, uint64_t *out) {
    read_lock();
    int ok=backing_base && backing_checksum(address,size,out);
    read_unlock();
    return ok;
}
void runtime_memory_mirror_snapshot(unsigned rotate, uint64_t max_sample, void (*emit)(const RuntimeMirrorSnapshot *)) {
    RuntimeMirrorSnapshot s={0};
    read_lock();
    size_t mapped=0;
    for (size_t i=0;i<vma_count;++i) if (vmas[i].kind!=KIND_RESERVED) { ++mapped; s.bytes+=vmas[i].end-vmas[i].start; }
    s.views=mapped;
    /* Samples: windows of views spread over the table, starting at a rotating index, that hold
     * resident pages (untouched pages would be faulted in for nothing and only compare zeros). */
    const unsigned max_count=sizeof(s.samples)/sizeof(*s.samples);
    const uint64_t host_page=(uint64_t)getpagesize();
    char resident[UINT64_C(1)<<20>>12];
    max_sample=max_sample/PAGE*PAGE;
    if (max_sample>sizeof(resident)*host_page) max_sample=sizeof(resident)*host_page;
    if (backing_base && max_sample) for (size_t n=0, i=vma_count ? (size_t)rotate*7919%vma_count : 0;
                                         n<vma_count && n<512 && s.count<max_count; ++n, i=(i+1)%vma_count) {
        Vma v=vmas[i];
        if (v.kind==KIND_RESERVED) continue;
        uint64_t length=v.end-v.start, window=length<max_sample ? length : max_sample;
        uint64_t offset=(uint64_t)rotate*(UINT64_C(1)<<20)%(length-window+PAGE)/PAGE*PAGE;
        unsigned char *at=backing_base+v.phys+offset;
        if (mincore(at,window,resident)) continue;
        int any=0;
        for (uint64_t p=0;p<window/host_page && !any;++p) any=resident[p]&1;
        if (!any) continue;
        RuntimeMirrorSample *sample=&s.samples[s.count++];
        sample->address=v.start+offset; sample->size=window;
        sample->checksum=bbgpu_checksum(BBGPU_CHECKSUM_SEED,at,window);
    }
    emit(&s);
    read_unlock();
}
#endif
void runtime_memory_report(void) {
    printf("Runtime: direct memory allocations=%zu, maps=%zu, live=%" PRIu64 ", budget=%" PRIu64 "\n",
           allocations, maps, live_bytes, POOL_SIZE);
    printf("Runtime: flexible maps=%zu, in use=%" PRIu64 ", protects=%zu, queries=%zu, regions=%zu\n",
           flexible_maps, flexible_bytes, protects, queries, vma_count);
}

#ifdef __APPLE__
#include <malloc/malloc.h>
#include <pthread.h>
/* BB_MEM_STATS=1: every 60 s, where the game process's memory is (diagnostics for its growth
 * during play): the physical footprint, the host malloc zones in use, the resident pages of the
 * guest pool's backing view (direct + flexible), the low range used by runtime_low_map, and the
 * direct memory the game holds. */
static uint64_t resident_bytes(const unsigned char *base, uint64_t size) {
    static unsigned char vec[(UINT64_C(64) << 20) / 4096];
    const uint64_t page = (uint64_t)getpagesize(), chunk = UINT64_C(64) << 20;
    uint64_t resident = 0;
    for (uint64_t off = 0; off < size; off += chunk) {
        const size_t n = (size_t)(size - off < chunk ? size - off : chunk);
        if (mincore(base + off, n, (char *)vec)) continue;
        for (size_t i = 0; i * page < n; ++i) resident += (vec[i] & 1) ? page : 0;
    }
    return resident;
}
static void *mem_stats_thread(void *unused) {
    (void)unused;
    for (;;) {
        sleep(60);
        task_vm_info_data_t vm; mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        const uint64_t footprint = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &count)
                                       == KERN_SUCCESS ? vm.phys_footprint : 0;
        malloc_statistics_t ms = {0};
        malloc_zone_statistics(NULL, &ms);
        const uint64_t pool_resident = backing_base ? resident_bytes(backing_base, POOL_SIZE + FLEX_SPAN) : 0;
        printf("Game memory: footprint %" PRIu64 " MB; malloc in use %" PRIu64 " MB; guest pool "
               "resident %" PRIu64 " MB; low range %" PRIu64 " MB; direct live %" PRIu64 " MB, "
               "flexible %" PRIu64 " MB\n",
               footprint >> 20, (uint64_t)ms.size_in_use >> 20, pool_resident >> 20,
               (uint64_t)(low_next - LOW_MIN) >> 20, live_bytes >> 20, flexible_bytes >> 20);
        printf("Game memory: low blocks per class created/live/free-listed (large: %lld live, %lld MB):",
               (long long)low_live[NUM_LOW_CLASSES], (long long)(low_large_bytes >> 20));
        host_mutex_lock(&low_alloc_lock);
        for (size_t i = 0; i < NUM_LOW_CLASSES; ++i) {
            int64_t listed = 0;
            for (LowFreeNode *n = low_freelists[i]; n && listed < 100000000; n = n->next) ++listed;
            if (low_created[i]) printf(" %zu:%lld/%lld/%lld", low_classes[i], (long long)low_created[i],
                                       (long long)low_live[i], (long long)listed);
        }
        host_mutex_unlock(&low_alloc_lock);
        printf("\n");
        fflush(stdout);
    }
    return NULL;
}
void runtime_memory_start_stats(void) {
    const char *env = getenv("BB_MEM_STATS");
    if (!env || env[0] != '1') return;
    pthread_t thread;
    if (!pthread_create(&thread, NULL, mem_stats_thread, NULL)) pthread_detach(thread);
}
#else
void runtime_memory_start_stats(void) {}
#endif
