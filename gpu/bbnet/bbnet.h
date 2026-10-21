/* bbport online module (libbbnet.so, gpu/bbnet): the PS4 network and PSN libraries with the
 * shadNet client. A separate library the runtime loads only for online play (BB_ONLINE=1,
 * src/runtime_net.c); offline the port never touches it, and a build without it runs offline.
 * Its C++ inside is private (hidden symbols, -Bsymbolic): these few calls are all it exports. */
#ifndef BBNET_H
#define BBNET_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BBNET_ABI_VERSION 2

/* What the runtime gives the module: the game's identity (param.sfo, its folder for
 * sce_sys/npbind.dat), guest threads for the NP libraries' callbacks into the game, and what
 * is mapped in guest memory (NpSignaling's diagnostics dump memory around a bad activation). */
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
    /* Ends the calling thread (one started with thread_spawn) with this result; never returns. */
    void (*thread_exit)(void *result);
    /* 1 when [address, address + size) is mapped guest memory. */
    int (*memory_is_mapped)(uintptr_t address, uint64_t size);
} BbNetHost;

/* Registers the libraries; 0 on success, else the module cannot run (the game stays offline). */
typedef int (*BbNetInitFn)(const BbNetHost *host);
int bbnet_init(const BbNetHost *host);

/* The module's function for an imported NID ("NID#lib#mod"), or 0. */
typedef uintptr_t (*BbNetResolveFn)(const char *scoped_nid);
uintptr_t bbnet_resolve(const char *scoped_nid);

/* The game's loaded image before the patches change it (The Hunter's Dream backend reads the
 * 1.09 executable's code and a few of its variables), and the moment the game starts (its
 * online session begins then). Both optional: a module without them plays on shadNet. */
typedef void (*BbNetSetImageFn)(unsigned char *image, uint64_t size);
void bbnet_set_image(unsigned char *image, uint64_t size);
typedef void (*BbNetStartFn)(void);
void bbnet_start(void);

#ifdef __cplusplus
}
#endif
#endif
