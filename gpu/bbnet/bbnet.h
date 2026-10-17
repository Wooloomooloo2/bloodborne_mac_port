/* bbport online module (libbbnet.so, gpu/bbnet): the PS4 network and PSN libraries with the
 * shadNet client. A separate library the runtime loads only for online play (BB_ONLINE=1,
 * src/runtime_net.c); offline the port never touches it, and a build without it runs offline.
 * Its C++ inside is private (hidden symbols, -Bsymbolic): these two calls are all it exports. */
#ifndef BBNET_H
#define BBNET_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BBNET_ABI_VERSION 1

/* What the runtime gives the module: the game's identity (param.sfo, its folder for
 * sce_sys/npbind.dat) and guest threads for the NP libraries' callbacks into the game. */
typedef struct BbNetHost {
    uint32_t abi;           /* BBNET_ABI_VERSION */
    const char *app0;       /* the game's folder */
    const char *serial;     /* TITLE_ID */
    const char *title;      /* TITLE */
    const char *app_ver;    /* APP_VER */
    /* 0 or a positive errno value; entry has the System V ABI (the game's code calls it). */
    int32_t (*thread_spawn)(void **thread, void *(__attribute__((sysv_abi)) *entry)(void *),
                            void *argument, uint64_t stack_size, const char *name);
    int32_t (*thread_join)(void *thread, void **result);
} BbNetHost;

/* Registers the libraries; 0 on success, else the module cannot run (the game stays offline). */
typedef int (*BbNetInitFn)(const BbNetHost *host);
int bbnet_init(const BbNetHost *host);

/* The module's function for an imported NID ("NID#lib#mod"), or 0. */
typedef uintptr_t (*BbNetResolveFn)(const char *scoped_nid);
uintptr_t bbnet_resolve(const char *scoped_nid);

#ifdef __cplusplus
}
#endif
#endif
