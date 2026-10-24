// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
/* Native arm64 GPU process (docs/DESIGN_NATIVE_GPU_PROCESS.md), runtime side: spawns
 * out/bbgpu-host, hands it the guest memory entry and the shared control block, mirrors every
 * guest view into it at the same addresses; probe.c then starts the renderer there. macOS only;
 * the game cannot run without it. */
#pragma once
#include <stdint.h>

#ifdef __APPLE__
#include "host/bbgpu_protocol.h" /* bbgpu_checksum */
/* Exit statuses (the launcher's crash report reads the "Crash:" line; these tell scripts). */
enum { GPU_PROCESS_EXIT_START = 70, GPU_PROCESS_EXIT_HANG = 76 };
/* Starts the host: spawn, handshake, then mirror guest views. Any failure ends the game with a
 * "Crash:" line (GPU_PROCESS_EXIT_START). Call once, early, before guest maps. */
void gpu_process_start(void);
/* Stops the host (SIGTERM, then waits for it): before the game exits on its own. */
void gpu_process_stop(void);
/* The shared control block while the host is up (for bbgpu_init_remote), else NULL. */
void *gpu_process_shared(void);
/* The renderer now runs in the host: from here on, losing the host ends the game. */
void gpu_process_rendering(void);

/* ---- runtime_memory.c side of the mirror ---- */
enum { RUNTIME_MIRROR_MAP = 1, RUNTIME_MIRROR_UNMAP = 2, RUNTIME_MIRROR_PROTECT = 3 };
/* Called in operation order with the region table's write lock held; must not call back into
 * runtime_memory. phys is the pool offset (map); prot the PS4 protection (map, protect). */
typedef void (*RuntimeMemoryMirror)(int op, uintptr_t address, uint64_t size, uint64_t phys, int prot);
/* The pool's memory entry (MACH_PORT_NULL without the shared pool) and its size in bytes. */
mach_port_t runtime_memory_pool_entry(void);
uint64_t runtime_memory_pool_entry_size(void);
/* Creates the guest memory pool now (normally on the first guest map). 0 on success. */
int runtime_memory_create_pool(void);
/* Installs (or removes, NULL) the mirror; every current view is replayed to it as a map. */
void runtime_memory_set_mirror(RuntimeMemoryMirror mirror);

typedef struct { uint64_t address, size, checksum; } RuntimeMirrorSample;
typedef struct {
    uint64_t views, bytes; /* non-reserved views */
    unsigned count;
    RuntimeMirrorSample samples[8];
} RuntimeMirrorSnapshot;
/* Counts the views and checksums up to 8 sample windows (at most max_sample bytes each, resident
 * data preferred; rotate varies the choice), then calls emit with the region table still locked,
 * so nothing changes between the snapshot and emit. */
void runtime_memory_mirror_snapshot(unsigned rotate, uint64_t max_sample,
                                    void (*emit)(const RuntimeMirrorSnapshot *snapshot));
/* Checksum of a guest range now (through the backing view); 0 if not fully mapped. */
int runtime_memory_mirror_checksum(uintptr_t address, uint64_t size, uint64_t *checksum);
#endif
