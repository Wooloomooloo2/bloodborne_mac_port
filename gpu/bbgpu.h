// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
/* C interface of the GPU library (gpu/): shadPS4's Liverpool/Vulkan video core,
 * GnmDriver, VideoOut and kernel event queues, adapted to the native loader. */
#ifndef BBGPU_H
#define BBGPU_H
#include <stdbool.h>
#include <stdint.h>
#include "bbmac_settings.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const char *title;          /* window title */
    const char *serial;         /* CUSA id, names the pipeline cache */
    const char *user_dir;       /* pipeline cache/logs directory */
    uint32_t sdk_version;       /* from the eboot's procparam */
    uint32_t psf_attributes;    /* param.sfo ATTRIBUTE */
    int32_t width, height;      /* initial window size */
} BbGpuConfig;
/* ---- Two-process split (docs/DESIGN_NATIVE_GPU_PROCESS.md, M3; shared: host/bbgpu_shared.h) ----
 * Game side (bb-probe): instead of bbgpu_init. Writes the configuration into the shared control
 * block, starts the renderer in the GPU host (waits for it), then registers GnmDriver/VideoOut
 * with the work forwarded to the host. 0 on success. */
int bbgpu_init_remote(const BbGpuConfig *config, void *shared);
/* GPU host (bbgpu-host): creates the window, Vulkan device, presenter and command processor
 * here, publishes host facts in the shared block and forwards GPU interrupts. 0 on success. */
int bbgpu_host_start(const BbGpuConfig *config, void *shared);
/* GPU host: a fault the game took on a GPU-protected page (M4): 1 when the renderer handled it
 * (the game retries the access), 0 when the address is not GPU memory. */
int bbgpu_host_fault(uint64_t address, int write);
/* GPU host: asks the game to restart (the settings menu's Restart, through runtime_restart). */
void bbgpu_split_post_restart(void);
/* Before an _exit (no destructors run): writes the Metal pipeline archive if pipelines are
 * pending, waiting at most timeout_ms (logged). Any thread, any number of times: every call waits
 * for the same single flush. 0 written or nothing pending, -1 the write failed, 1 timed out. */
int bbgpu_flush_caches(unsigned timeout_ms);
#define BBGPU_EXIT_FLUSH_MS 5000u
/* Metal back end check (arm64 host): compiles every .spv in spv_dir to Metal; 0 when all did. */
int bbgpu_msl_check(const char *spv_dir, const char *msl_cache_dir);
/* GPU host: runs one GnmDriver/VideoOut record from the submit ring (answers requests itself).
 * 0 when the record type is not the GPU library's. */
int bbgpu_host_dispatch(const void *record);
/* Game side: attaches the shared control block (before any push) and pushes one record onto
 * its submit ring. The runtime's view mirror and the GPU library share this one ordered
 * producer, so memory is mapped in the host before work that uses it. */
void bbgpu_split_attach(void *shared, int host_pid);
void bbgpu_split_push(const void *record);
/* Game side: the host window's keyboard state (SDL scancode-indexed, 512 entries) while the
 * renderer runs in the GPU host, else NULL. */
const bool *bbgpu_split_keyboard(void);
/* Benchmark (BB_BENCHMARK, game side): the host's draws in its last presented frame and its
 * presented frames so far (0 without a GPU host); the phase the host records by (0 waiting,
 * 1 measuring, 2 done, 3 failed with `error`). */
uint32_t bbgpu_bench_frame_draws(void);
uint64_t bbgpu_bench_host_flips(void);
void bbgpu_bench_set_phase(uint32_t phase, const char *error);
/* Registers kernel event queues (needed with or without graphics). */
void bbgpu_register_kernel(void);
/* Creates window, Vulkan device, presenter and GPU command processor. */
int bbgpu_init(const BbGpuConfig *config);
/* Function for an imported NID ("NID#lib#mod"), or 0 when the GPU library does not provide it. */
uintptr_t bbgpu_resolve(const char *scoped_nid);
/* Called first by the loader's SIGSEGV handler: 1 when a GPU page-tracking fault was handled. */
int bbgpu_handle_fault(void *ucontext, void *address);
/* BB_WRITE_LOG=1: prints the logged GPU-side writes to guest memory near the fault. */
void bbgpu_dump_guest_writes(void *ucontext);
/* Keyboard text entry through the game window (IME dialog). begin returns 0 when
 * no window exists; poll returns 0 typing, 1 confirmed, 2 cancelled (UTF-8 text). */
/* password: the dialog asked for masked entry (OrbisImeOption PASSWORD); the typed text is shown as dots. */
int bbgpu_text_input_begin(const char *initial_utf8, const char *prompt_utf8, int password);
int bbgpu_text_input_poll(char *out_utf8, uint64_t size);
/* 1 while the in-game settings menu is open: the game's pad input is held neutral. */
int bbgpu_overlay_captures_input(void);
/* 1 when bbport.ini mouse_look is on (the launcher's Mouse camera): the arrow keys are the d-pad. */
int bbgpu_mouse_look_enabled(void);
/* Number of symbols registered by the vendored libraries (diagnostics). */
unsigned bbgpu_symbol_count(void);
/* Records a guest synchronization call in the Gnm check's call ring (runtime_sync_hook). */
void bbgpu_record_sync(const char *op, uintptr_t object, int64_t result);
/* Flips and submissions query for heartbeat / diagnostics. */
uint64_t bbgpu_get_flip_count(void);
uint64_t bbgpu_get_submit_count(void);
/* Dumps host GPU / presenter thread states and counters to a log file. */
void bbgpu_dump_host_threads_hang(void *file_handle);
/* Computes average FPS and p95/p99 frame times in milliseconds over recent flips. */
void bbgpu_get_frametime_percentiles(double *avg_fps, double *p95_ms, double *p99_ms);
/* Dumps GPU breadcrumbs state to log/crash files. */
void bbgpu_dump_breadcrumbs(void *file_handle);
#ifdef __cplusplus
}
#endif
#endif
