// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#include "mutex_fixture.h"

static void *mutex, *cond;
static atomic_int waiting;
static int stop;
static void *waiter(void *unused) {
    (void)unused;
    assert(LOCK(&mutex) == 0);
    atomic_store(&waiting, 1);
    while (!stop) assert(TIMEDWAIT(&cond, &mutex, 3000000) == 0);
    assert(UNLOCK(&mutex) == 0);
    return NULL;
}
int main(void) {
    alarm(10);
    void *unused = NULL;
    FixtureOp destroy_cond = FN(FixtureOp, "g+PZd2hiacg#p#J");
    assert(destroy_cond(&unused) == 0 && unused == NULL);
    assert(destroy_cond(NULL) == SCE_ERROR(22));
    assert(SIGNAL(&cond) == 0 && cond);
    assert(destroy_cond(&cond) == 0 && cond == (void *)(uintptr_t)1);
    assert(destroy_cond(&cond) == SCE_ERROR(22));
    assert(SIGNAL(&cond) == SCE_ERROR(22));
    assert(FN(FixtureOp, "JGgj7Uvrl+A#p#J")(&cond) == SCE_ERROR(22));
    assert(FN(FixtureOp, "2MOy+rUfuhQ#I#J")(&cond) == 22);
    assert(FN(FixtureOp, "RXXqi4CtF8w#I#J")(&cond) == 22);
    fixture_mutex(&mutex, 1);
    assert(LOCK(&mutex) == 0);
    assert(WAIT(&cond, &mutex) == SCE_ERROR(22));
    assert(TIMEDWAIT(&cond, &mutex, 0) == SCE_ERROR(22));
    assert(UNLOCK(&mutex) == 0);
    /* Explicit initialization is allowed to start a fresh lifetime. */
    assert(FN(FixtureInit, "2Tb92quprl0#p#J")(&cond, NULL, "reinit") == 0);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, waiter, NULL) == 0);
    while (!atomic_load(&waiting)) host_yield();
    assert(LOCK(&mutex) == 0); /* waiter released it inside the production wait */
    assert(destroy_cond(&cond) == SCE_ERROR(16));
    stop = 1;
    assert(SIGNAL(&cond) == 0 && UNLOCK(&mutex) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(destroy_cond(&cond) == 0 && cond == (void *)(uintptr_t)1);
    assert(DESTROY(&mutex) == 0);
    puts("PASS: condition destroyed handles, explicit reinit and busy waiters");
}
