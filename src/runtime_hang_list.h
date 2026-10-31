// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef BB_RUNTIME_HANG_LIST_H
#define BB_RUNTIME_HANG_LIST_H
#include <stddef.h>
#include <stdint.h>

/* Hang-time evidence only. The reader must neither fault nor allocate/take user locks:
 * the sampled thread is suspended and may hold them. No guest memory is written. */
typedef int (*RuntimeHangRead)(void *context, uint64_t address, void *out, size_t size);
enum { RUNTIME_HANG_LIST_NODES = 64 };
typedef enum {
    HANG_LIST_NONE, HANG_LIST_HEAD, HANG_LIST_CYCLE, HANG_LIST_LIMIT,
    HANG_LIST_UNREADABLE, HANG_LIST_BAD_REGISTERS
} RuntimeHangListStatus;
typedef struct {
    uint64_t address, next, previous;
    uint32_t flags;
} RuntimeHangNode;
typedef struct {
    RuntimeHangListStatus status;
    uint64_t owner, head, current, first, cursor, stopped_at;
    uint32_t generation;
    uint8_t flags;
    unsigned count, repeated;
    RuntimeHangNode nodes[RUNTIME_HANG_LIST_NODES];
} RuntimeHangList;

/* Recognize the checked 1.09 loop before interpreting its registers or object layout.
 * Call with a cleared output. NONE means an unrelated PC/image; LIMIT is inconclusive. */
void runtime_hang_list_capture(RuntimeHangList *out, uint64_t image, uint64_t image_size,
                              uint64_t pc, uint64_t owner, uint64_t head, uint64_t current,
                              RuntimeHangRead read, void *context);
#endif
