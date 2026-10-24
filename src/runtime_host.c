// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
/* host_sync.h: the runtime's threads, clocks and sleeps on each host. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

int host_thread_start(HostThread *thread, size_t stack, void *(*entry)(void *), void *argument) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
#ifdef __APPLE__
    /* macOS secondary threads default to 512 KiB (Linux: 8 MiB) and setstacksize rejects sizes
     * that are not page multiples. */
    if (!stack) stack = 8u << 20;
    stack = (stack + 16383) & ~(size_t)16383;
#endif
    if (stack) pthread_attr_setstacksize(&attr, stack);
    int e = pthread_create(thread, &attr, entry, argument);
    pthread_attr_destroy(&attr);
    return e;
}
int host_thread_join(HostThread thread) { return pthread_join(thread, NULL); }
void host_thread_detach(HostThread thread) { pthread_detach(thread); }
static uint64_t read_clock(clockid_t id) {
    struct timespec t;
    clock_gettime(id, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
uint64_t host_monotonic_ns(void) { return read_clock(CLOCK_MONOTONIC); }
uint64_t host_realtime_ns(void) { return read_clock(CLOCK_REALTIME); }
void host_sleep_ns(uint64_t ns) {
    struct timespec t = {.tv_sec = (time_t)(ns / 1000000000), .tv_nsec = (long)(ns % 1000000000)};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
void host_sleep_until_ns(uint64_t deadline) {
    struct timespec t = {(time_t)(deadline / 1000000000u), (long)(deadline % 1000000000u)};
#ifdef __APPLE__
    /* No clock_nanosleep: sleep the remaining monotonic time until the deadline passes. */
    (void)t;
    for (uint64_t now; (now = host_monotonic_ns()) < deadline;) {
        uint64_t ns = deadline - now;
        struct timespec r = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};
        nanosleep(&r, NULL);
    }
#else
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL)) {}
#endif
}
