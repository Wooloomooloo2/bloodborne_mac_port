// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask, so `taskset` can emulate a Steam Deck), and speculative helpers run as
// SCHED_IDLE: they use cores the game leaves idle and never take time from its threads.

#pragma once

#include <algorithm>
#include <thread>

#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#include <setjmp.h>
typedef sigjmp_buf BbRecoverBuf;
extern "C" __thread BbRecoverBuf* runtime_fault_recover;
#define BB_RECOVER_SET(buf) sigsetjmp(buf, 0)

#ifdef __APPLE__
/// macOS has no gettid(): the Mach thread id (unique per thread, small enough for u32 in practice).
static inline pid_t gettid() {
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return static_cast<pid_t>(id);
}
#endif

namespace BbThreads {

/// Hardware threads available to the process.
inline unsigned Available() {
#ifdef __APPLE__
    // No affinity masks on macOS.
    return std::max(1u, std::thread::hardware_concurrency());
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        return std::max(1, CPU_COUNT(&set));
    }
    return std::max(1u, std::thread::hardware_concurrency());
#endif
}

/// The calling thread only runs on otherwise idle cores (falls back to the lowest nice level).
inline void MakeBackground() {
#ifdef __APPLE__
    // Background QoS: efficiency cores, lowest priority (the closest match to SCHED_IDLE).
    pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
#else
    sched_param param{};
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
        setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
    }
#endif
}

} // namespace BbThreads
