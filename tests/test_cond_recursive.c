// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#include "mutex_fixture.h"
#include <string.h>

typedef int32_t (ABI *FixtureMutexTimedLock)(void **, uint32_t);
static void *mutex, *cond;
static int ready;
static atomic_int worker_acquired;
static void *signaler(void *unused) {
    (void)unused;
    const uint64_t end = host_monotonic_ns() + UINT64_C(2000000000);
    do {
        int32_t e = TRYLOCK(&mutex);
        if (!e) {
            ready = 1;
            atomic_store(&worker_acquired, 1);
            assert(SIGNAL(&cond) == 0);
            assert(UNLOCK(&mutex) == 0);
            return NULL;
        }
        assert(e == SCE_ERROR(16));
        host_sleep_ns(1000000);
    } while (host_monotonic_ns() < end);
    return NULL;
}
static void *try_foreign(void *result) {
    *(int32_t *)result = TRYLOCK(&mutex);
    if (*(int32_t *)result == 0) assert(UNLOCK(&mutex) == 0);
    return NULL;
}
int main(int argc, char **argv) {
    alarm(10);
    const int untimed = argc > 1 && !strcmp(argv[1], "--untimed");
    const int posix = argc > 1 && !strcmp(argv[1], "--posix");
    fixture_mutex(&mutex, 2);
    assert(LOCK(&mutex) == 0 && TRYLOCK(&mutex) == 0);
    assert(FN(FixtureMutexTimedLock, "IafI2PxcPnQ#p#J")(&mutex, 1000) == 0);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, signaler, NULL) == 0);
    while (!ready) {
        if (posix) {
            typedef int32_t (ABI *AbsWait)(void **, void **, const int64_t *);
            struct timespec now;
            assert(clock_gettime(CLOCK_REALTIME, &now) == 0);
            const int64_t end[] = {now.tv_sec + 3, now.tv_nsec};
            assert(FN(AbsWait, "27bAgiJmOh0#I#J")(&cond, &mutex, end) == 0);
        } else if (untimed) assert(WAIT(&cond, &mutex) == 0);
        else assert(TIMEDWAIT(&cond, &mutex, 3000000) == 0);
    }
    assert(pthread_join(worker, NULL) == 0 && atomic_load(&worker_acquired));
    /* A timeout must restore the same three levels too. */
    assert(TIMEDWAIT(&cond, &mutex, 1000) == SCE_ERROR(60));
    assert(UNLOCK(&mutex) == 0 && UNLOCK(&mutex) == 0);
    int32_t result = 0;
    assert(pthread_create(&worker, NULL, try_foreign, &result) == 0);
    assert(pthread_join(worker, NULL) == 0 && result == SCE_ERROR(16));
    assert(UNLOCK(&mutex) == 0);
    assert(UNLOCK(&mutex) == SCE_ERROR(1));
    assert(DESTROY(&mutex) == 0);
    assert(FN(FixtureOp, "g+PZd2hiacg#p#J")(&cond) == 0);
    puts("PASS: recursive condition waits release and restore all three levels");
}
