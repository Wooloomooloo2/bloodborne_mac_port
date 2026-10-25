// SPDX-License-Identifier: GPL-2.0-or-later
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../src/runtime_exit_menu.c"

static void seed(unsigned char *base) {
    memcpy(base+EXIT_CONDITION,exit_condition_original,sizeof(exit_condition_original));
    uint64_t callback=(uintptr_t)(base+EXIT_CALLBACK);
    memcpy(base+EXIT_SLOT,&callback,sizeof(callback));
}

int main(void) {
    const size_t size=EXIT_SLOT+sizeof(uint64_t);
    unsigned char *base=calloc(1,size),*before=malloc(size);
    assert(base && before);
    unsetenv("BB_EXIT_GAME"); unsetenv("BB_EXIT_GAME_READY"); seed(base);
    assert(!runtime_install_exit_menu(base,size));
    setenv("BB_EXIT_GAME_READY","1",1);
    setenv("BB_EXIT_GAME","0",1); assert(!runtime_install_exit_menu(base,size));
    unsetenv("BB_EXIT_GAME");
    assert(!runtime_install_exit_menu(base,size-1));
    for (size_t i=0;i<sizeof(exit_condition_original)+sizeof(uint64_t);++i) {
        seed(base);
        base[i<sizeof(exit_condition_original) ? EXIT_CONDITION+i :
             EXIT_SLOT+i-sizeof(exit_condition_original)]^=1;
        memcpy(before,base,size);
        assert(!runtime_install_exit_menu(base,size));
        assert(!memcmp(before,base,size));
    }
    seed(base); assert(runtime_install_exit_menu(base,size));
    assert(!memcmp(base+EXIT_CONDITION,"\xb8\x01\0\0\0",5));
    uint64_t callback;
    memcpy(&callback,base+EXIT_SLOT,sizeof(callback));
    assert(callback==(uintptr_t)title_exit_game);
    // Exercise the installed callback in a child: a title-menu exit must be a clean quit.
    fflush(NULL);
    pid_t child=fork(); assert(child>=0);
    if (!child) { ((GuestCallback)(uintptr_t)callback)(); _Exit(99); }
    int status=0; assert(waitpid(child,&status,0)==child);
    assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
    free(before); free(base); puts("Exit Game guards and clean callback exit PASS");
}
