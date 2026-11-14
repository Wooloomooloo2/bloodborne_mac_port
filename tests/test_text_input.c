// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
/* The text box and the keyboard pad (src/runtime_text_input.h): no key typed into the box, nor the
 * key that closed it, reaches the game; spaces around an entry are dropped. */
#include "runtime_text_input.h"

#include <assert.h>
#include <string.h>

int runtime_text_input_open(void) { return 0; }

int main(void) {
    bool keys[512] = {0};
    int latch = 0;
    /* No box: the keyboard is the pad. */
    keys[40] = true; /* Return */
    assert(!text_input_blocks_keys(0, keys, 512, &latch));
    /* Box open: blocked whatever is held. */
    assert(text_input_blocks_keys(1, keys, 512, &latch));
    keys[40] = false;
    assert(text_input_blocks_keys(1, keys, 512, &latch));
    /* Closed by Enter, still held: blocked until released (the game saw Cross and reopened). */
    keys[40] = true;
    assert(text_input_blocks_keys(0, keys, 512, &latch));
    assert(text_input_blocks_keys(0, keys, 512, &latch));
    keys[40] = false;
    assert(!text_input_blocks_keys(0, keys, 512, &latch));
    /* Released once: the next press is game input again. */
    keys[44] = true; /* Space */
    assert(!text_input_blocks_keys(0, keys, 512, &latch));
    /* No keyboard array: nothing to wait for. */
    latch = 1;
    assert(!text_input_blocks_keys(0, NULL, 512, &latch) && latch == 0);

    char a[32] = "secret ";
    assert(text_input_trim(a) == 6 && strcmp(a, "secret") == 0);
    char b[32] = "  two words  ";
    assert(text_input_trim(b) == 9 && strcmp(b, "two words") == 0);
    char c[32] = "   ";
    assert(text_input_trim(c) == 0 && c[0] == 0);
    char d[32] = "";
    assert(text_input_trim(d) == 0);
    return 0;
}
