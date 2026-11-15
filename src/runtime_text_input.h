/* SPDX-FileCopyrightText: Copyright 2026 Mark Hall
 * SPDX-License-Identifier: GPL-2.0-or-later */
/* The text entry box (ImeDialog) and the keyboard pad share the keyboard. Pure helpers, tested in
 * tests/test_text_input.c. */
#ifndef RUNTIME_TEXT_INPUT_H
#define RUNTIME_TEXT_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Whether a text entry box is open (runtime_services.c). */
int runtime_text_input_open(void);

/* Whether the keyboard must stay out of the game's pad. While a box is open the keys belong to
 * it; when it closes, the key that closed it (Enter and Space are Cross, Escape is Circle) is
 * still down and would press that button in the menu: the Enter that confirmed the matchmaking
 * password opened the field again, over and over. So the keyboard stays out until no key is
 * held. *latch carries that state between samples (starts at 0). */
static inline int text_input_blocks_keys(int box_open, const bool *keys, size_t count, int *latch) {
    if (box_open) { *latch=1; return 1; }
    if (!*latch) return 0;
    for (size_t i=0; keys && i<count; ++i) if (keys[i]) return 1;
    *latch=0;
    return 0;
}

/* Spaces around a confirmed entry are dropped: they come from a held key, not from the player
 * (a password typed as "abc" came back as "abc " and never matched its confirmation). */
static inline size_t text_input_trim(char *text) {
    size_t start=0, end=strlen(text);
    while (start<end && text[start]==' ') ++start;
    while (end>start && text[end-1]==' ') --end;
    memmove(text,text+start,end-start);
    text[end-start]=0;
    return end-start;
}

#endif
