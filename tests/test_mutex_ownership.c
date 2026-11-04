// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE
#include "mutex_fixture.h"

static void *mutex, *cond;
static void *foreign(void *unused) {
    (void)unused;
    assert(UNLOCK(&mutex) == SCE_ERROR(1));
    assert(FN(FixtureOp, "2Z+PpY6CaJg#I#J")(&mutex) == 1);
    assert(TIMEDWAIT(&cond, &mutex, 1000) == SCE_ERROR(1));
    assert(TRYLOCK(&mutex) == SCE_ERROR(16));
    return NULL;
}
static void *claim_and_signal(void *unused) {
    (void)unused;
    assert(LOCK(&mutex) == 0);
    assert(SIGNAL(&cond) == 0);
    assert(UNLOCK(&mutex) == 0);
    return NULL;
}
int main(void) {
    alarm(10);
    for (int type = 1; type <= 4; ++type) {
        mutex = cond = NULL;
        fixture_mutex(&mutex, type);
        assert(SIGNAL(&cond) == 0);
        assert(LOCK(&mutex) == 0);
        pthread_t worker;
        assert(pthread_create(&worker, NULL, foreign, NULL) == 0);
        assert(pthread_join(worker, NULL) == 0);
        assert(TIMEDWAIT(&cond, &mutex, 1000) == SCE_ERROR(60));
        /* A normal mutex's owner must also be restored after a signaled wait. */
        assert(pthread_create(&worker, NULL, claim_and_signal, NULL) == 0);
        assert(TIMEDWAIT(&cond, &mutex, 3000000) == 0);
        assert(pthread_join(worker, NULL) == 0);
        assert(UNLOCK(&mutex) == 0 && UNLOCK(&mutex) == SCE_ERROR(1));
        assert(DESTROY(&mutex) == 0);
        assert(FN(FixtureOp, "g+PZd2hiacg#p#J")(&cond) == 0);
    }
    puts("PASS: mutex ownership across all guest types, POSIX errors and condition waits");
}
