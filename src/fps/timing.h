// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef BB_FPS_TIMING_H
#define BB_FPS_TIMING_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Install while the loader's image is writable, before patches/guest execution.
// cap: 0 means at most 240 FPS; 30 preserves the unmodified game.
// The caller reserves tail_size() extra bytes immediately after round_page(size).
// Returns 1 installed, 0 at 30, -1 on refusal (no game may execute after refusal).
size_t bb_fps_tail_size(void);
int bb_fps_install(unsigned char *image, uint64_t size, int cap);
int bb_fps_conflicts(uint64_t offset, uint64_t size);
// Reapply writable timing pages after the loader restores segment protections.
int bb_fps_protect(void);
#ifdef __cplusplus
}
#endif
#endif
