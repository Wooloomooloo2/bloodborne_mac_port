/* Focused fixture support. No guest code, GPU library or game assets are loaded. */
#include "runtime.h"
#include <assert.h>
#include <limits.h>
#include <time.h>

RuntimeSyncHook runtime_sync_hook;
void runtime_thread_set_blocked(const char *what,uint64_t resource) {
    (void)what; (void)resource;
}
void runtime_thread_clear_blocked(void) {}
uint64_t host_monotonic_ns(void) {
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC,&t)==0);
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}

/* Include the production implementation to reach its ID exhaustion boundary
   without allocating billions of semaphores or adding a production test hook. */
#include "../src/runtime_sema.c"

void semaphore_id_limit_test(void) {
    next_id=INT32_MAX;
    uint64_t id=UINT64_MAX,untouched=UINT64_MAX;
    assert(sem_create(&id,"last ID",1,1,1,NULL)==0 && id==INT32_MAX);
    assert((uint32_t)sem_create(&untouched,"exhausted",1,0,1,NULL)==0x8002001c);
    assert(untouched==UINT64_MAX);
    assert(sem_poll(id,1)==0 && sem_delete(id)==0);
    /* Deleting the last object must not wrap or reuse the ID space. */
    assert((uint32_t)sem_create(&untouched,"still exhausted",1,0,1,NULL)==0x8002001c);
    assert(untouched==UINT64_MAX);
}
