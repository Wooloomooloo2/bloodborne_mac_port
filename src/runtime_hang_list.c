// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime_hang_list.h"
#include <string.h>

static uint64_t u64(const void *p) { uint64_t v; memcpy(&v, p, sizeof(v)); return v; }
static uint32_t u32(const void *p) { uint32_t v; memcpy(&v, p, sizeof(v)); return v; }

void runtime_hang_list_capture(RuntimeHangList *out, uint64_t image, uint64_t image_size,
                              uint64_t pc, uint64_t owner, uint64_t head, uint64_t current,
                              RuntimeHangRead read, void *context) {
    /* mov r15,[r15+0x10]; cmp r15,r12; jne 0x24a4160. This is a diagnostic
     * signature, not a patch. The worker may be anywhere in that loop's body. */
    static const unsigned char signature[] = {0x4d,0x8b,0x7f,0x10,0x4d,0x39,0xe7,0x75,0xa5};
    enum { Start = 0x24a4160, End = 0x24a41bb, Signature = 0x24a41b2 };
    unsigned char code[sizeof(signature)], header[0x30];
    if (!image || image > UINT64_MAX - image_size || image_size < End ||
        pc < image || pc - image < Start || pc - image >= End ||
        !read(context, image + Signature, code, sizeof(code)) ||
        memcmp(code, signature, sizeof(code))) return;
    out->owner = owner; out->head = head; out->current = current;
    if (owner < 0x10000 || (owner & 7) || owner > UINT64_MAX - 0x98 || head != owner + 0x58) {
        out->status = HANG_LIST_BAD_REGISTERS;
        return;
    }
    if (!read(context, owner + 0x68, header, sizeof(header))) {
        out->status = HANG_LIST_UNREADABLE;
        out->stopped_at = owner + 0x68;
        return;
    }
    out->first = u64(header);
    out->cursor = u64(header + 0x18);
    out->generation = u32(header + 0x28);
    out->flags = header[0x2c];
    uint64_t node = current;
    for (;;) {
        out->stopped_at = node;
        if (node == head) { out->status = HANG_LIST_HEAD; return; }
        for (unsigned i = 0; i < out->count; ++i)
            if (out->nodes[i].address == node) {
                out->repeated = i;
                out->status = HANG_LIST_CYCLE;
                return;
            }
        if (out->count == RUNTIME_HANG_LIST_NODES) { out->status = HANG_LIST_LIMIT; return; }
        unsigned char data[0x24];
        if (node < 0x10000 || (node & 7) || node > UINT64_MAX - sizeof(data) ||
            !read(context, node, data, sizeof(data))) {
            out->status = HANG_LIST_UNREADABLE;
            return;
        }
        RuntimeHangNode *row = &out->nodes[out->count++];
        row->address = node; row->next = u64(data + 0x10);
        row->previous = u64(data + 0x18); row->flags = u32(data + 0x20);
        node = row->next;
    }
}
