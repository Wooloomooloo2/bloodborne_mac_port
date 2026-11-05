// SPDX-License-Identifier: GPL-2.0-or-later
// Title-menu offsets and the unused Opinion Box entry are adapted from
// evill33t/bloodborne_mac scripts/quit_menu.py at bbacb95c3338efcd627c0edaec05ff7534e555da.
// Only the title menu is changed: the in-world save/return-to-title action stays intact.
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { EXIT_CONDITION=0x1b4abf3, EXIT_SLOT=0x533b960, EXIT_CALLBACK=0x1b46030 };
static const unsigned char exit_condition_original[]={0xe8,0xc8,0xee,0xf9,0xff};

static ABI void title_exit_game(void) {
    puts("Runtime: Exit Game (title menu)");
    fflush(NULL);
    // As with a normal window close, the launcher receives status 0. The GPU host's
    // parent watcher follows this process's exit and flushes its pipeline cache.
    _Exit(0);
}

int runtime_install_exit_menu(unsigned char *base,uint64_t size) {
    const char *ready=getenv("BB_EXIT_GAME_READY"),*enabled=getenv("BB_EXIT_GAME");
    if (!ready || strcmp(ready,"1") || (enabled && !strcmp(enabled,"0"))) return 0;
    if (size<EXIT_SLOT+sizeof(uint64_t)) return 0;
    uint64_t callback;
    memcpy(&callback,base+EXIT_SLOT,sizeof(callback));
    if (memcmp(base+EXIT_CONDITION,exit_condition_original,sizeof(exit_condition_original)) ||
        callback!=(uintptr_t)(base+EXIT_CALLBACK)) {
        fputs("Exit Game: unsupported title menu code; hooks disabled\n",stderr);
        return 0;
    }
    // Validate both sites before modifying either. The loader protects these pages later.
    const unsigned char show_entry[]={0xb8,1,0,0,0}; // mov eax, 1 instead of the condition call
    memcpy(base+EXIT_CONDITION,show_entry,sizeof(show_entry));
    callback=(uintptr_t)title_exit_game;
    memcpy(base+EXIT_SLOT,&callback,sizeof(callback));
    fputs("Exit Game: native title menu installed\n",stderr);
    return 1;
}
