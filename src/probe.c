// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "runtime.h"
#include "gpu/bbgpu.h"
#include "gpu_process.h"
#if !defined(__x86_64__) || !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang.
#endif
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/uio.h>
#ifdef __APPLE__
#include <sys/ucontext.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_vm.h>
#undef round_page /* <mach/vm_param.h> macros; probe.c has its own */
#undef trunc_page
#define UC_REG(uc, r) ((uc)->uc_mcontext->__ss.__##r)
#else
#include <malloc.h>
#include <ucontext.h>
#include <sys/syscall.h>
#define UC_REG(uc, r) ((uc)->uc_mcontext.gregs[UC_##r])
#define UC_rip REG_RIP
#define UC_rbp REG_RBP
#define UC_rdi REG_RDI
#define UC_rax REG_RAX
#define UC_rbx REG_RBX
#define UC_rsi REG_RSI
#define UC_rsp REG_RSP
#endif
/* Mach-O prefixes C symbols with '_': top-level asm must name them through SYM(). */
#ifdef __APPLE__
#define SYM(name) "_" #name
#else
#define SYM(name) #name
#endif

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static size_t page_size;
typedef struct { uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module; } LinkedModule;
static LinkedModule modules[16];
static uint64_t module_count;
static int entered_game;
static int gpu_enabled;
static uint64_t image_size;
static void **resolved_targets;
static uint64_t *import_call_counts;
static uint8_t *is_unbound;
static FILE *g_import_file = NULL;
static volatile uint64_t g_total_import_calls = 0;
static volatile uint64_t g_import_file_lines = 0;
static int g_bb_trace = 0;

typedef struct {
    uint32_t index;
    uint64_t args[6];
} MOMainCallRecord;

#define MO_RING_CAP 64
static MOMainCallRecord mo_recent_calls[MO_RING_CAP];
static uint32_t mo_recent_count = 0;
static uint64_t mo_total_import_count = 0;
static uint64_t *mo_call_counts = NULL;

static volatile uint64_t g_stat_total_seconds = 0;
static volatile uint64_t g_stat_zero_flip_seconds = 0;
static volatile uint64_t g_stat_longest_freeze_s = 0;
static volatile uint64_t g_stat_current_freeze_s = 0;

static void print_exit_summary(void) {
    static int summary_printed = 0;
    if (summary_printed) return;
    summary_printed = 1;
    uint64_t total_s = g_stat_total_seconds;
    if (total_s == 0) return;
    uint64_t total_flips = gpu_enabled ? bbgpu_get_flip_count() : 0;
    double avg_flips_per_s = (double)total_flips / (double)total_s;
    double zero_flip_pct = (double)g_stat_zero_flip_seconds * 100.0 / (double)total_s;
    uint64_t max_freeze = g_stat_longest_freeze_s;
    if (g_stat_current_freeze_s > max_freeze) max_freeze = g_stat_current_freeze_s;

    printf("\n================================================================================\n");
    printf("=== BLOODBORNE RUN SUMMARY ===\n");
    printf("  Total Runtime:       %llu s\n", (unsigned long long)total_s);
    printf("  Total Flips:         %llu\n", (unsigned long long)total_flips);
    printf("  Average Flips/s:     %.2f\n", avg_flips_per_s);
    printf("  Zero-Flip Seconds:   %llu of %llu (%.1f%%)\n",
           (unsigned long long)g_stat_zero_flip_seconds, (unsigned long long)total_s, zero_flip_pct);
    printf("  Longest Freeze:      %llu s\n", (unsigned long long)max_freeze);
    printf("================================================================================\n\n");
    fflush(stdout);

    FILE *f = fopen("out/summary.log", "w");
    if (f) {
        fprintf(f, "=== BLOODBORNE RUN SUMMARY ===\n");
        fprintf(f, "Total Runtime:       %llu s\n", (unsigned long long)total_s);
        fprintf(f, "Total Flips:         %llu\n", (unsigned long long)total_flips);
        fprintf(f, "Average Flips/s:     %.2f\n", avg_flips_per_s);
        fprintf(f, "Zero-Flip Seconds:   %llu of %llu (%.1f%%)\n",
                (unsigned long long)g_stat_zero_flip_seconds, (unsigned long long)total_s, zero_flip_pct);
        fprintf(f, "Longest Freeze:      %llu s\n", (unsigned long long)max_freeze);
        fclose(f);
    }
}


static void fail(const char *message) {
    fprintf(stderr, "ERROR: %s\n", message);
    fflush(stderr);
    exit(1);
}
#ifdef __APPLE__
/* Ends the game with a "Crash:" line (the launcher's crash report leads with it), taking the GPU
 * process down first so its window closes. */
static void crash_exit(const char *why, int status) {
    printf("Crash: %s\n", why);
    fflush(stdout);
    gpu_process_stop();
    _exit(status);
}
/* Hang watchdog: the game flips a frame many times a second, loading screens included. No new
 * flip for BB_HANG_SECONDS (default 30, 0 = off) after the first one means the game is stuck,
 * for example waiting for GPU work that never arrives (#18). A late wake-up (system sleep,
 * the process stopped) restarts the count. */
static void *hang_watchdog(void *arg) {
    const unsigned limit = (unsigned)(uintptr_t)arg;
    uint64_t last = 0;
    struct timespec since, before;
    clock_gettime(CLOCK_MONOTONIC, &since);
    before = since;
    for (;;) {
        sleep(1);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const uint64_t flips = bbgpu_get_flip_count();
        const int woke_late = now.tv_sec - before.tv_sec > 5;
        before = now;
        if (flips != last || flips == 0 || woke_late) {
            last = flips;
            since = now;
            continue;
        }
        if ((unsigned)(now.tv_sec - since.tv_sec) >= limit) {
            char why[160];
            snprintf(why, sizeof why, "the game stopped responding (no new frame for %u seconds, %" PRIu64
                     " frames before; BB_HANG_SECONDS)", limit, flips);
            runtime_thread_dump_all(); /* where each guest thread is stuck (#46) */
            crash_exit(why, GPU_PROCESS_EXIT_HANG);
        }
    }
    return NULL;
}
static void start_hang_watchdog(void) {
    const char *env = getenv("BB_HANG_SECONDS");
    const unsigned limit = env ? (unsigned)strtoul(env, NULL, 10) : 30;
    if (!limit) return;
    pthread_t thread;
    if (!pthread_create(&thread, NULL, hang_watchdog, (void *)(uintptr_t)limit)) pthread_detach(thread);
}
#endif
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
static void *allocate(size_t size) {
    void *low=runtime_low_map(size,PROT_READ|PROT_WRITE);
    if (low) {
        CHECK_LOW_ADDR(low);
        return low;
    }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fail("mmap failed");
    CHECK_LOW_ADDR(p);
    return p;
}
static void protect(void *p, size_t size, unsigned flags) {
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (mprotect(p, size, mode)) fail("mprotect failed");
}

void runtime_notify_progress(void) {
}

static inline int check_noisy_import(const char *sym) {
    if (!sym) return 0;
    return (!strcmp(sym, "memcpy") || !strcmp(sym, "memset") || !strcmp(sym, "memcmp") ||
            !strcmp(sym, "memmove") || !strcmp(sym, "memchr") || !strcmp(sym, "strlen") ||
            !strcmp(sym, "strcmp") || !strcmp(sym, "strncmp") || !strcmp(sym, "strcpy") ||
            !strcmp(sym, "wmemcmp") || !strcmp(sym, "wmemcpy") || !strcmp(sym, "wmemset") ||
            !strcmp(sym, "wcslen") || !strcmp(sym, "wmemchr") || !strcmp(sym, "wcschr") ||
            !strcmp(sym, "pthread_mutex_lock") || !strcmp(sym, "pthread_mutex_unlock") ||
            !strcmp(sym, "scePthreadMutexLock") || !strcmp(sym, "scePthreadMutexUnlock") ||
            !strcmp(sym, "sceKernelReadTsc") || !strcmp(sym, "sceKernelGetProcessTimeCounter") ||
            !strcmp(sym, "scePthreadSelf") || !strcmp(sym, "_Getpctype"));
}

ABI void *pre_import_hook(uint32_t index, uint64_t *args, void *caller) {
    runtime_notify_progress();
    if (index >= import_count) return NULL;
    uint64_t total = ++g_total_import_calls;
    uint64_t count = ++import_call_counts[index];
    const char *name = names[index];
    const char *sym = runtime_import_name(name);
    uintptr_t caller_off = (uintptr_t)caller >= (uintptr_t)image && (uintptr_t)caller < (uintptr_t)image + 0x20000000
                           ? (uintptr_t)caller - (uintptr_t)image : (uintptr_t)caller;
    int is_noisy = check_noisy_import(sym);

    GuestThread *cur = runtime_thread_current();
    if (cur) {
        uint32_t ring_slot = cur->recent_import_count++;
        cur->recent_imports[ring_slot & 31] = index;
    }
    if (cur && cur->name[0] == 'M' && !strncmp(cur->name, "MOMainThread", 12)) {
        uint32_t slot = (mo_recent_count++) % MO_RING_CAP;
        mo_recent_calls[slot].index = index;
        for (int a = 0; a < 6; ++a) mo_recent_calls[slot].args[a] = args[a];
        mo_total_import_count++;
        if (mo_call_counts && index < import_count) {
            mo_call_counts[index]++;
        }
    }

    if (g_import_file && g_import_file_lines < 100000) {
        if (!is_noisy || count <= 5) {
            fprintf(g_import_file, "[#%" PRIu64 "] [idx %u] %s (%s) caller=0x%" PRIxPTR " args=(0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ")\n",
                    total, index, name, sym ? sym : "unknown", caller_off,
                    args[0], args[1], args[2], args[3], args[4], args[5]);
            g_import_file_lines++;
            if (g_import_file_lines == 100000) {
                fprintf(g_import_file, "[IMPORT LOG CAPPED AT 100,000 ENTRIES TO PREVENT OOM]\n");
            }
        }
    }

    if (is_unbound[index]) {
        if (g_bb_trace && count <= 3) {
            printf("[UNBOUND IMPORT CALLED #%" PRIu64 "] index %u: %s (%s) from 0x%" PRIxPTR "\n"
                   "  args: (0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ") -> returning 0\n",
                   count, index, name, sym ? sym : "unknown", caller_off,
                   args[0], args[1], args[2], args[3], args[4], args[5]);
            fflush(stdout);
        }
        return NULL;
    }

    if (g_bb_trace && ((is_noisy && count == 1) || (!is_noisy && count <= 5))) {
        printf("[IMPORT CALL #%" PRIu64 "] %s (%s) from 0x%" PRIxPTR "\n"
               "  args: (0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ", 0x%" PRIx64 ")\n",
               count, name, sym ? sym : "unknown", caller_off,
               args[0], args[1], args[2], args[3], args[4], args[5]);
        fflush(stdout);
    }
    return resolved_targets[index];
}

ABI void post_import_hook(uint32_t index, uint64_t ret_val) {
    if (index >= import_count) return;
    uint64_t count = import_call_counts[index];
    const char *name = names[index];
    const char *sym = runtime_import_name(name);
    int is_noisy = check_noisy_import(sym);

    if (g_import_file && g_import_file_lines < 100000) {
        if (!is_noisy || count <= 5) {
            fprintf(g_import_file, "  -> [#%" PRIu64 " RET] %s (%s) ret=0x%" PRIx64 "\n",
                    count, name, sym ? sym : "unknown", ret_val);
            g_import_file_lines++;
        }
    }

    if ((is_noisy && count == 1) || (!is_noisy && count <= 5)) {
        printf("[IMPORT RET #%" PRIu64 "] %s (%s) -> 0x%" PRIx64 "\n",
               count, name, sym ? sym : "unknown", ret_val);
        fflush(stdout);
    }
}

void common_dispatch(void);
__asm__(
".text\n"
".globl " SYM(common_dispatch) "\n"
SYM(common_dispatch) ":\n"
"   push %rbp\n"
"   mov %rsp, %rbp\n"
"   sub $208, %rsp\n"
"   mov %r11, 0(%rsp)\n"
"   mov %rdi, 8(%rsp)\n"
"   mov %rsi, 16(%rsp)\n"
"   mov %rdx, 24(%rsp)\n"
"   mov %rcx, 32(%rsp)\n"
"   mov %r8,  40(%rsp)\n"
"   mov %r9,  48(%rsp)\n"
"   mov %rax, 56(%rsp)\n"
"   mov %r10, 64(%rsp)\n"
"   movdqu %xmm0, 80(%rsp)\n"
"   movdqu %xmm1, 96(%rsp)\n"
"   movdqu %xmm2, 112(%rsp)\n"
"   movdqu %xmm3, 128(%rsp)\n"
"   movdqu %xmm4, 144(%rsp)\n"
"   movdqu %xmm5, 160(%rsp)\n"
"   movdqu %xmm6, 176(%rsp)\n"
"   movdqu %xmm7, 192(%rsp)\n"
"   mov 0(%rsp), %rdi\n"
"   lea 8(%rsp), %rsi\n"
"   mov 8(%rbp), %rdx\n"
"   call " SYM(pre_import_hook) "\n"
"   test %rax, %rax\n"
"   jnz 1f\n"
"   xor %rax, %rax\n"
"   xor %rdx, %rdx\n"
"   pxor %xmm0, %xmm0\n"
"   add $208, %rsp\n"
"   pop %rbp\n"
"   ret\n"
"1:\n"
"   mov %rax, %r11\n"
"   sub $64, %rsp\n"
"   mov 16(%rbp), %rax\n"
"   mov %rax, 0(%rsp)\n"
"   mov 24(%rbp), %rax\n"
"   mov %rax, 8(%rsp)\n"
"   mov 32(%rbp), %rax\n"
"   mov %rax, 16(%rsp)\n"
"   mov 40(%rbp), %rax\n"
"   mov %rax, 24(%rsp)\n"
"   mov 48(%rbp), %rax\n"
"   mov %rax, 32(%rsp)\n"
"   mov 56(%rbp), %rax\n"
"   mov %rax, 40(%rsp)\n"
"   mov 64(%rbp), %rax\n"
"   mov %rax, 48(%rsp)\n"
"   mov 72(%rbp), %rax\n"
"   mov %rax, 56(%rsp)\n"
"   movdqu 80+64(%rsp), %xmm0\n"
"   movdqu 96+64(%rsp), %xmm1\n"
"   movdqu 112+64(%rsp), %xmm2\n"
"   movdqu 128+64(%rsp), %xmm3\n"
"   movdqu 144+64(%rsp), %xmm4\n"
"   movdqu 160+64(%rsp), %xmm5\n"
"   movdqu 176+64(%rsp), %xmm6\n"
"   movdqu 192+64(%rsp), %xmm7\n"
"   mov 8+64(%rsp),  %rdi\n"
"   mov 16+64(%rsp), %rsi\n"
"   mov 24+64(%rsp), %rdx\n"
"   mov 32+64(%rsp), %rcx\n"
"   mov 40+64(%rsp), %r8\n"
"   mov 48+64(%rsp), %r9\n"
"   mov 56+64(%rsp), %rax\n"
"   mov 64+64(%rsp), %r10\n"
"   call *%r11\n"
"   add $64, %rsp\n"
"   mov %rax, 56(%rsp)\n"
"   mov %rdx, 24(%rsp)\n"
"   movdqu %xmm0, 80(%rsp)\n"
"   mov 0(%rsp), %rdi\n"
"   mov %rax, %rsi\n"
"   call " SYM(post_import_hook) "\n"
"   mov 56(%rsp), %rax\n"
"   mov 24(%rsp), %rdx\n"
"   movdqu 80(%rsp), %xmm0\n"
"   add $208, %rsp\n"
"   pop %rbp\n"
"   ret\n"
);

static char g_run_timestamp[32];

static void start_timeout(unsigned seconds) {
    alarm(seconds);
}
/* enter_on_stack(entry, arg0, arg1, stack_top): call entry(arg0,arg1) on a new stack. */
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
__asm__(".text\n.globl " SYM(enter_on_stack) "\n" SYM(enter_on_stack) ":\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n");
static ABI void guest_exit(void) {
    puts("Runtime: process finalizer callback reached");
    print_exit_summary();
}
static void fault(int sig, siginfo_t *info, void *context) {
    /* GPU page tracking (write-protected guest pages) is resolved first. macOS reports a
     * write to a mapped read-only page as SIGBUS, not SIGSEGV. */
#ifdef __APPLE__
    if (gpu_enabled && (sig == SIGSEGV || sig == SIGBUS) && bbgpu_handle_fault(context, info->si_addr)) return;
#else
    if (gpu_enabled && sig == SIGSEGV && bbgpu_handle_fault(context, info->si_addr)) return;
#endif
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    if ((sig == SIGSEGV || sig == SIGBUS) && runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    /* The process is terminating: dladdr/snprintf are acceptable here. */
    ucontext_t *uc = context;
    uintptr_t rip = (uintptr_t)UC_REG(uc, rip);
    char line[512];
    Dl_info where;
    /* One plain line first: the launcher's crash report leads with it. */
    if (rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Crash: the game crashed (signal %d at guest offset 0x%lx)\n", sig,
                 (unsigned long)(rip - (uintptr_t)image));
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(line, sizeof(line), "Crash: the port crashed (signal %d in %s, %s)\n", sig,
                 strrchr(where.dli_fname, '/') ? strrchr(where.dli_fname, '/') + 1 : where.dli_fname,
                 where.dli_sname ? where.dli_sname : "?");
    else
        snprintf(line, sizeof(line), "Crash: the port crashed (signal %d at %p)\n", sig, (void *)rip);
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    if (rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Guest fault (signal %d) at guest offset 0x%lx, address %p\n",
                 sig, (unsigned long)(rip - (uintptr_t)image), info->si_addr);
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(line, sizeof(line), "Host fault (signal %d) in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                 (unsigned long)(rip - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?", info->si_addr);
    else
        snprintf(line, sizeof(line), "Fault (signal %d) at RIP %p, address %p\n", sig, (void *)rip, info->si_addr);
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    snprintf(line, sizeof(line), "  rax=%#llx rbx=%#llx rdi=%#llx rsi=%#llx rsp=%#llx\n",
             (unsigned long long)UC_REG(uc, rax), (unsigned long long)UC_REG(uc, rbx),
             (unsigned long long)UC_REG(uc, rdi), (unsigned long long)UC_REG(uc, rsi),
             (unsigned long long)UC_REG(uc, rsp));
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    if (gpu_enabled) bbgpu_dump_guest_writes(context);
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    int depth = backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        if (dladdr(frames[i], &where) && where.dli_fname)
            snprintf(line, sizeof(line), "  #%d %s+0x%lx (%s)\n", i, where.dli_fname,
                     (unsigned long)((uintptr_t)frames[i] - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(line, sizeof(line), "  #%d %p\n", i, frames[i]);
        ssize_t written_=write(2, line, strlen(line)); (void)written_;
    }
    _exit(128 + sig);
}
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets
 * when inside the image). Reads use process_vm_readv so bad frames cannot fault. */
static uintptr_t exe_base;
static void write_hex(char *out, uint64_t v) {
    const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(ucontext_t *uc) {
    char line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    uintptr_t rip=(uintptr_t)UC_REG(uc, rip), rbp=(uintptr_t)UC_REG(uc, rbp);
    uint64_t tid=(uint64_t)gettid();
    /* First argument register: the lock address when a thread waits on a futex. */
    char arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg+6,tid); write_hex(arg+27,(uint64_t)UC_REG(uc, rdi));
    { ssize_t written_=write(2,arg,sizeof(arg)-1); (void)written_; }
    for (int depth=0; depth<24; ++depth) {
        write_hex(line+6,tid); write_hex(line+27,rip); write_hex(line+59,rip-(uintptr_t)image); write_hex(line+90,rip-exe_base);
        { ssize_t written_=write(2,line,sizeof(line)-1); (void)written_; }
        uintptr_t frame[2];
#ifdef __APPLE__
        mach_vm_size_t got=0;
        if (!rbp || mach_vm_read_overwrite(mach_task_self(),rbp,sizeof(frame),(mach_vm_address_t)(uintptr_t)frame,&got)!=KERN_SUCCESS ||
            got!=sizeof(frame)) break;
#else
        struct iovec local={frame,sizeof(frame)}, remote={(void *)rbp,sizeof(frame)};
        if (!rbp || process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof(frame)) break;
#endif
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
    }
}
static void thread_dump(int sig, siginfo_t *info, void *context) { (void)sig; (void)info; dump_frames(context); }
static void watchdog(int sig, siginfo_t *info, void *context) {
    (void)info;
    const char head[]="STOP: watchdog timeout; thread stacks:\n";
    { ssize_t written_=write(2,head,sizeof(head)-1); (void)written_; }
    dump_frames(context);
#ifdef __APPLE__
    /* No /proc: signal every other pthread of the task (Mach threads without a pthread,
     * e.g. kernel workqueue or Rosetta helpers, are skipped). */
    thread_act_array_t list;
    mach_msg_type_number_t count=0;
    mach_port_t self=pthread_mach_thread_np(pthread_self());
    if (task_threads(mach_task_self(),&list,&count)==KERN_SUCCESS)
        for (mach_msg_type_number_t i=0;i<count;++i) {
            pthread_t thread=list[i]!=self ? pthread_from_mach_thread_np(list[i]) : NULL;
            if (thread) { pthread_kill(thread,SIGUSR2); usleep(20000); }
        }
#else
    int dir=open("/proc/self/task",O_RDONLY|O_DIRECTORY);
    char buffer[4096];
    long n;
    pid_t self=gettid();
    while (dir>=0 && (n=syscall(SYS_getdents64,dir,buffer,sizeof(buffer)))>0)
        for (long at=0; at<n;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *d=(void *)(buffer+at);
            pid_t tid=(pid_t)strtol(d->name,NULL,10);
            if (tid>0 && tid!=self) { syscall(SYS_tgkill,getpid(),tid,SIGUSR2); usleep(20000); }
            at+=d->reclen;
        }
#endif
    usleep(100000);
    _exit(128 + sig);
}
/* param.sfo lookup: string or integer value of key, 0 when absent. */
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char data[65536];
    size_t n=fread(data,1,sizeof(data),f); fclose(f);
    if (n<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset,e,2); memcpy(&format,e+2,2); memcpy(&length,e+4,4); memcpy(&offset,e+12,4);
        if (keys+key_offset>=n || values+offset+length>n || strcmp((const char *)data+keys+key_offset,key)) continue;
        if (format==0x0404 && number && length>=4) { memcpy(number,data+values+offset,4); return 1; }
        if (text && text_size) {
            size_t copy=length<text_size-1 ? length : text_size-1;
            memcpy(text,data+values+offset,copy); text[copy]=0;
        }
        return 1;
    }
    return 0;
}
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}
/* BBPATCH2 (patches.py): the patches' image base, then byte writes at image offsets, applied
 * after relocation. A write may replace a whole base-relative pointer slot (60/90 FPS++ swap
 * function pointers): the patch holds the address at the patches' base, rebased here. */
static void apply_patches(const char *path, Segment *segments, uint64_t ns, const Reloc *relocs, uint64_t nr) {
    FILE *f=fopen(path,"rb");
    char magic[8];
    if (!f || fread(magic,1,8,f)!=8 || memcmp(magic,"BBPATCH2",8)) fail("invalid patch file");
    uint64_t base=read64(f), count=read64(f), bytes=0, rebased=0;
    unsigned char data[4096];
    for (uint64_t i=0;i<count;++i) {
        uint64_t offset=read64(f), length=read64(f);
        if (!length || length>sizeof(data) || !mapped(segments,ns,offset,length) || fread(data,1,length,f)!=length)
            fail("bad patch entry");
        uint64_t slots[sizeof(data)/8]; size_t nslots=0;
        for (uint64_t r=0;r<nr;++r) {
            if (!(relocs[r].target<offset+length && offset<relocs[r].target+8)) continue;
            uint64_t target=relocs[r].target, value;
            if (relocs[r].kind || target<offset || target+8>offset+length) fail("patch overlaps a relocation");
            memcpy(&value,data+(target-offset),8);
            if (value<base || !mapped(segments,ns,value-base,1)) fail("patch writes a pointer outside the image");
            slots[nslots++]=target;
        }
        memcpy(image+offset,data,length);
        for (size_t s=0;s<nslots;++s) {
            uint64_t value;
            memcpy(&value,image+slots[s],8);
            value=(uint64_t)(uintptr_t)image+(value-base);
            memcpy(image+slots[s],&value,8);
        }
        rebased+=nslots;
        bytes+=length;
    }
    if (fgetc(f)!=EOF) fail("trailing data in patch file");
    fclose(f);
    printf("Patches: %" PRIu64 " writes, %" PRIu64 " bytes applied, %" PRIu64 " pointers rebased\n",count,bytes,rebased);
}
/* Restarts the game through run.sh (the settings menu: a new render resolution is a patch
 * applied at start). Descriptors are closed first so the old GPU device and its memory are
 * released before the new process opens its own. */
void runtime_restart(void) {
    fflush(NULL);
    puts("Runtime: restarting through run.sh");
#ifdef __APPLE__
    for (int fd=3, end=getdtablesize(); fd<end; ++fd) close(fd);
#else
    syscall(SYS_close_range, 3u, ~0u, 0u);
#endif
    execlp("bash", "bash", "run.sh", (char *)NULL);
    perror("runtime_restart: exec");
    _exit(1);
}

#ifdef __APPLE__
/* Cocoa (the SDL window and its events) only works on the main thread: the loader and the
 * game run on this thread instead, and main() runs the main run loop, which serves the main
 * dispatch queue (bbgpu.cpp). dispatch_main() would not do: it exits the main thread, and Cocoa
 * then finds no main thread (SDL: "No available video device"). */
static int probe_main(int argc, char **argv);
static int main_argc;
static char **main_argv;
static void *game_thread(void *unused) {
    (void)unused;
    exit(probe_main(main_argc, main_argv));
}
int main(int argc, char **argv) {
    main_argc = argc; main_argv = argv;
    pthread_attr_t attr;
    pthread_t thread;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20); /* the main thread's stack is 8 MiB; be generous */
    if (pthread_create(&thread, &attr, game_thread, NULL)) { perror("pthread_create"); return 1; }
    pthread_attr_destroy(&attr);
    /* SDL stops the main run loop at times (entering full screen): run it again. */
    for (;;) {
        if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0e10, false) == kCFRunLoopRunFinished)
            usleep(1000); /* no sources yet */
    }
}
static int probe_main(int argc, char **argv) {
#else
int main(int argc, char **argv) {
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    /* stdout is the launcher's pipe: if the launcher goes away, a print fails with EPIPE (a
     * lost log line) instead of SIGPIPE ending the game. (Also no SIGPIPE from closed sockets.) */
    signal(SIGPIPE, SIG_IGN);
#ifdef __APPLE__
    /* Before SDL maps anything: on macOS large host mmaps land in the guest range. */
    if (runtime_memory_reserve_space()) fail("cannot reserve guest address range");
    /* The native arm64 GPU process renders; the game stops if it cannot start. */
    gpu_process_start();
#endif
    runtime_memory_start_stats();
    time_t t_now = time(NULL);
    struct tm tm_buf;
    localtime_r(&t_now, &tm_buf);
    strftime(g_run_timestamp, sizeof(g_run_timestamp), "%Y%m%d_%H%M%S", &tm_buf);

    const char *env_trace = getenv("BB_TRACE");
    if (env_trace && (!strcmp(env_trace, "1") || !strcmp(env_trace, "true") || !strcmp(env_trace, "yes"))) {
        g_bb_trace = 1;
    }

    int cpu_only = 0, strict_imports = 0;
    unsigned timeout_seconds = 10;
    const char *content_profile=NULL, *app0=NULL, *user_dir=NULL, *patch_file=NULL;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--trace")) g_bb_trace = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) user_dir=argv[++i];
        else if (!strcmp(argv[i], "--patches") && i+1<argc) patch_file=argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }

    if (g_bb_trace) {
        char imp_ts_path[128];
        snprintf(imp_ts_path, sizeof(imp_ts_path), "out/imports_%s.log", g_run_timestamp);
        g_import_file = fopen(imp_ts_path, "w");
        if (!g_import_file) g_import_file = fopen("out/imports.log", "w");
        if (!g_import_file) g_import_file = fopen("imports.log", "w");
        if (g_import_file) setvbuf(g_import_file, NULL, _IOFBF, 64 * 1024);
        printf("[BB_TRACE] Tracing enabled: per-second heartbeat, hang snapshots, and import logging active.\n");
    } else {
        printf("[FAST PATH] Tracing disabled (BB_TRACE=0): direct import dispatch active.\n");
    }
    atexit(print_exit_summary);
#ifdef __linux__
    /* Keep host heap objects handed to the guest (thread handles, TLS) in the
       non-PIE brk heap, i.e. below 1 TiB: the guest packs pointers into 40 bits.
       macOS malloc lives far above 1 TiB: such objects come from runtime_low_alloc. */
    mallopt(M_ARENA_MAX,1);
    mallopt(M_MMAP_THRESHOLD,32*1024*1024);
#endif
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot.bin [--cpu-only] [--strict-imports] [--content-profile file] [--app0 dir] [--user dir] [--patches file] [--timeout seconds]\n", argv[0]);
        return 1;
    }
    if (content_profile) {
        FILE *profile=fopen(content_profile,"rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data,1,sizeof(data),profile)!=sizeof(data) || fgetc(profile)!=EOF || memcmp(data,"BBCONT01",8)) fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i=0;i<5;++i) {
            unsigned char *p=data+8+i*4;
            values[i]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        }
        runtime_content_configure(values);
    }
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        char sfo[4096], id[16]="";
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            runtime_savedata_configure(id);
        char serial[16]="", title[128]="", version[16]="";
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"APP_VER",version,sizeof(version),NULL);
        runtime_net_configure(app0,serial,title,version);
    }
    bbgpu_register_kernel();
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    struct sigaction sa = {0}; sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    Dl_info self_info;
    if (dladdr((void *)main,&self_info)) exe_base=(uintptr_t)self_info.dli_fbase;
    struct sigaction dump = {0}; dump.sa_sigaction = thread_dump; dump.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&dump.sa_mask); sigaction(SIGUSR2, &dump, NULL);
    struct sigaction alarm_action = {0}; alarm_action.sa_sigaction = watchdog; alarm_action.sa_flags = SA_SIGINFO;
    sigemptyset(&alarm_action.sa_mask); sigaction(SIGALRM, &alarm_action, NULL);
    start_timeout(timeout_seconds);
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file; run prepare.py first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "BBPROBE1", 8) && memcmp(magic, "BBPROBE2", 8) && memcmp(magic,"BBPROBE3",8) && memcmp(magic,"BBPROBE4",8) && memcmp(magic,"BBPROBE5",8))) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = memcmp(magic, "BBPROBE1", 8) ? read64(f) : 0;
    if (capabilities & ~UINT64_C(3)) fail("unknown runtime capabilities");
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int multi=!memcmp(magic,"BBPROBE5",8);
    int linked=!memcmp(magic,"BBPROBE3",8) || !memcmp(magic,"BBPROBE4",8) || multi;
    /* Bit 1 adds one GS-displacement word before the existing linked metadata.
     * Older images cannot prove which slot was selected on macOS. Refuse them
     * before any initializer/entry executes, even with --strict-imports. run.sh
     * always regenerates linked images, invalidating stale prepared files. */
    if ((capabilities & 2) && !linked) fail("TLS slot metadata requires a linked image");
#ifdef __APPLE__
    if (linked && !(capabilities & 2)) fail("boot image lacks TLS slot metadata; prepare the game again");
#endif
    if (linked && (capabilities & 2)) {
        uint64_t displacement=read64(f);
        if (displacement>=512*8 || displacement%8) fail("invalid boot TLS displacement");
        if (displacement!=runtime_thread_gs_displacement())
            fail("boot TLS slot does not match this runtime; prepare the game again");
    }
    runtime_start(strict_imports ? 0 : capabilities & 1);
    int native_libc=linked && !strict_imports && (capabilities&1);
    uint64_t main_tls[4]={0};
    uint64_t nb=0,procparam=0;
    uint64_t *bindings=calloc(import_count ? import_count : 1,sizeof(*bindings));
    uint64_t *binding_kinds=calloc(import_count ? import_count : 1,sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    if (multi) {
        /* BBPROBE5: procparam, eboot TLS, module table, bindings (link_modules.py). */
        procparam=read64(f);
        for (int i=0;i<4;++i) main_tls[i]=read64(f);
        module_count=read64(f);
        if (!module_count || module_count>sizeof(modules)/sizeof(*modules)) fail("invalid module count");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            x->base=read64(f); x->size=read64(f); x->init=read64(f); x->tls_address=read64(f);
            x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=read64(f);
        }
        nb=read64(f);
    } else if (linked) {
        LinkedModule *x=&modules[0];
        module_count=1;
        x->base=read64(f); x->size=read64(f); x->init=read64(f);
        x->tls_address=read64(f); x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=2; nb=read64(f);
        procparam=read64(f);
        if (!memcmp(magic,"BBPROBE4",8)) for (int i=0;i<4;++i) main_tls[i]=read64(f);
    }
    if (linked) {
        if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || main_tls[0]>size ||
            main_tls[1]>size-main_tls[0] || (main_tls[3] & (main_tls[3]-1)) || main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            if (x->base>=size || !x->size || x->size>size-x->base || x->init<x->base || x->init-x->base>=x->size ||
                (x->tls_module && (x->tls_address>=size || x->tls_memsz>size-x->tls_address || x->tls_memsz>1024*1024 ||
                                   x->tls_filesz>x->tls_memsz || x->tls_module<2 || x->tls_module>7)))
                fail("invalid linked module metadata");
        }
        if (nb>import_count) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t index=read64(f),address=read64(f),kind=read64(f);
            int inside=0;
            for (uint64_t m=0;m<module_count;++m)
                if (address>=modules[m].base && address-modules[m].base<modules[m].size) inside=1;
            if (index>=import_count || !inside || (kind!=1 && kind!=2) || bindings[index]) fail("invalid native binding");
            bindings[index]=address; binding_kinds[index]=kind;
        }
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m=0;m<module_count;++m)
        if (!mapped(segments,ns,modules[m].init,1) || (modules[m].tls_module && !mapped(segments,ns,modules[m].tls_address,modules[m].tls_memsz)))
            fail("unmapped module metadata");
    if (module_count && !mapped(segments,ns,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<import_count;++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments,ns,bindings[i],1)) fail("unmapped native export");
        if (binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                    bindings[i]-segments[s].address<segments[s].size) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
    image = allocate(round_page(size));
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    runtime_sampler_init(image, size);
    if (!cpu_only) {
        char title[128]="Bloodborne", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
        BbGpuConfig gpu={title,serial,user_dir ? user_dir : "user",(uint32_t)sdk,attributes,1920,1080};
        gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
#ifdef __APPLE__
        void *split=gpu_process_shared();
        if (!split) crash_exit("the GPU process is not running", GPU_PROCESS_EXIT_START);
        if (bbgpu_init_remote(&gpu,split)) crash_exit("the renderer could not start in the GPU process", 1);
        gpu_process_rendering();
        start_hang_watchdog();
#else
        if (bbgpu_init(&gpu)) fail("GPU initialization failed");
#endif
        /* BB_SYNC_TRACE=1: lock calls in the Gnm check's report (load-crash hunt). */
        if (getenv("BB_SYNC_TRACE") && getenv("BB_SYNC_TRACE")[0]=='1') runtime_sync_hook = bbgpu_record_sync;
        printf("GPU: window and presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,bbgpu_symbol_count());
    }
    image_size = size;
    resolved_targets = calloc(import_count + 1, sizeof(void *));
    import_call_counts = calloc(import_count + 1, sizeof(uint64_t));
    is_unbound = calloc(import_count + 1, sizeof(uint8_t));
    if (g_bb_trace) {
        mo_call_counts = calloc(import_count + 1, sizeof(uint64_t));
    }
    for (uint64_t i = 0; i < import_count; ++i) {
        uintptr_t resolved = runtime_resolve(names[i], 0);
        if (resolved) {
            resolved_targets[i] = (void *)resolved;
            is_unbound[i] = 0;
        } else if (native_libc && bindings[i]) {
            resolved_targets[i] = (void *)(image + bindings[i]);
            is_unbound[i] = 0;
        } else {
            resolved_targets[i] = NULL;
            is_unbound[i] = 1;
        }
    }
    unsigned char *thunks = allocate(round_page((import_count + 1) * 32));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = thunks + i * 32;
        uint32_t index = (uint32_t)i;
        uintptr_t dispatch = (uintptr_t)common_dispatch;
        t[0] = 0x41; t[1] = 0xbb; memcpy(t + 2, &index, 4);          /* mov $index, %r11d */
        t[6] = 0x48; t[7] = 0xb8; memcpy(t + 8, &dispatch, 8);       /* movabs $dispatch, %rax */
        t[16] = 0xff; t[17] = 0xe0;                                   /* jmp *%rax */
    }
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = 0;
        if (relocs[i].kind == 2) {
            uintptr_t resolved = runtime_resolve(names[relocs[i].value], 1);
            if (resolved) value = resolved + relocs[i].addend;
            else if (native_libc && bindings[relocs[i].value]) {
                value = (uintptr_t)image + bindings[relocs[i].value] + relocs[i].addend;
            } else {
                value = (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend);
            }
        } else if (relocs[i].kind == 1) {
            uint64_t imp_idx = relocs[i].value;
            uintptr_t resolved = runtime_resolve(names[imp_idx], 0);
            if (resolved) {
                if (g_bb_trace) {
                    value = (uintptr_t)(thunks + 32 * imp_idx);
                } else {
                    value = resolved + relocs[i].addend;
                }
            } else if (native_libc && bindings[imp_idx]) {
                uint64_t address = bindings[imp_idx];
                if (binding_kinds[imp_idx] != relocs[i].kind || !mapped(segments, ns, address, relocs[i].addend + 1))
                    fail("native export kind/range mismatch");
                value = (uintptr_t)image + address + relocs[i].addend;
            } else {
                value = (uintptr_t)(thunks + 32 * imp_idx);
            }
        } else {
            value = (uintptr_t)image + relocs[i].value;
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    runtime_net_set_image(image, size); /* the online module reads the game's code before the patches change it */
    if (patch_file) apply_patches(patch_file, segments, ns, relocs, nr);
    runtime_install_context_lock(image, size);
    protect(thunks, round_page((import_count + 1) * 32), 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);
    if (native_libc) {
        for (uint64_t m=0;m<module_count;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<ns;++i)
                if ((segments[i].flags&1) && modules[m].init>=segments[i].address && modules[m].init-segments[i].address<segments[i].size) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module,image+modules[m].tls_address,modules[m].tls_filesz,modules[m].tls_memsz);
        }
        runtime_set_main_tls(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]);
        runtime_thread_attach_main();
        runtime_set_procparam(image+procparam);
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. */
        for (uint64_t m=0;m<module_count;++m) {
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            typedef int (ABI *ModuleInit)(uint64_t,void *,void *);
            int result=((ModuleInit)(image+modules[m].init))(0,NULL,NULL);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
        }
    }
    runtime_net_start(); /* the online session starts with the game */
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
    struct { uint64_t argc; const char *argv[2]; } params = {1, {"/app0/eboot.bin", NULL}};
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks. */
    enum { MAIN_STACK=8*1024*1024 };
    unsigned char *stack=runtime_low_map(MAIN_STACK,PROT_READ|PROT_WRITE);
    if (!stack) fail("cannot allocate guest main stack");
    enter_on_stack(image+entry,&params,(void *)guest_exit,stack+MAIN_STACK-64);
    fail("entry unexpectedly returned");
    return 1;
}
