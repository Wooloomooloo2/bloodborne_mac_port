// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
/* The runtime's own locks, condition variables, threads and clocks.
 * Linux: pthreads and clock_gettime. macOS: the same pthreads path (see __APPLE__ below). */
#ifndef BB_HOST_SYNC_H
#define BB_HOST_SYNC_H
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#ifdef __APPLE__
#include <sys/types.h>
/* macOS has no gettid(): the Mach thread id, as in gpu/shim/bbport_threads.h, so thread ids
 * reported by the runtime and by libbbgpu agree. */
static inline pid_t gettid(void) {
    uint64_t id = 0;
    pthread_threadid_np(NULL, &id);
    return (pid_t)id;
}
#endif

typedef pthread_mutex_t HostMutex;
#define HOST_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
static inline void host_mutex_init(HostMutex *m) { pthread_mutex_init(m, NULL); }
static inline void host_lock(HostMutex *m) { pthread_mutex_lock(m); }
static inline int host_trylock(HostMutex *m) { return pthread_mutex_trylock(m) == 0; }
static inline void host_unlock(HostMutex *m) { pthread_mutex_unlock(m); }
static inline void host_mutex_lock(HostMutex *m) { host_lock(m); }
static inline void host_mutex_unlock(HostMutex *m) { host_unlock(m); }

typedef pthread_mutex_t HostRecursiveMutex;
#ifdef __APPLE__
#define HOST_RECURSIVE_MUTEX_INIT PTHREAD_RECURSIVE_MUTEX_INITIALIZER
#else
#define HOST_RECURSIVE_MUTEX_INIT PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#endif
static inline void host_recursive_lock(HostRecursiveMutex *m) { pthread_mutex_lock(m); }
static inline void host_recursive_unlock(HostRecursiveMutex *m) { pthread_mutex_unlock(m); }

typedef pthread_rwlock_t HostRwlock;
#define HOST_RWLOCK_INIT PTHREAD_RWLOCK_INITIALIZER
static inline void host_rwlock_init(HostRwlock *l) { pthread_rwlock_init(l, NULL); }
static inline void host_read_lock(HostRwlock *l) { pthread_rwlock_rdlock(l); }
static inline int host_try_read_lock(HostRwlock *l) { return pthread_rwlock_tryrdlock(l) == 0; }
static inline void host_read_unlock(HostRwlock *l) { pthread_rwlock_unlock(l); }
static inline void host_write_lock(HostRwlock *l) { pthread_rwlock_wrlock(l); }
static inline int host_try_write_lock(HostRwlock *l) { return pthread_rwlock_trywrlock(l) == 0; }
static inline void host_write_unlock(HostRwlock *l) { pthread_rwlock_unlock(l); }

typedef pthread_cond_t HostCond;
#define HOST_COND_INIT PTHREAD_COND_INITIALIZER
static inline void host_cond_init(HostCond *c) { pthread_cond_init(c, NULL); }
static inline void host_cond_wait(HostCond *c, HostMutex *m) { pthread_cond_wait(c, m); }
static inline void host_cond_signal(HostCond *c) { pthread_cond_signal(c); }
static inline void host_cond_broadcast(HostCond *c) { pthread_cond_broadcast(c); }

typedef pthread_t HostThreadId;
static inline HostThreadId host_thread_id(void) { return pthread_self(); }
static inline int host_thread_equal(HostThreadId a, HostThreadId b) { return pthread_equal(a, b); }
static inline void host_yield(void) { sched_yield(); }
typedef pthread_t HostThread;

/* Runtime helper threads. */
int host_thread_start(HostThread *thread, size_t stack, void *(*entry)(void *), void *argument);
int host_thread_join(HostThread thread);
void host_thread_detach(HostThread thread);

/* Clocks: monotonic and realtime, in nanoseconds. */
uint64_t host_monotonic_ns(void);
uint64_t host_realtime_ns(void);
void host_sleep_ns(uint64_t ns);
void host_sleep_until_ns(uint64_t monotonic_deadline);
#endif
