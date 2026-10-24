// SPDX-License-Identifier: GPL-2.0-or-later
// Achievement state shared by the C runtime and both GPU-library architectures.
#pragma once
#include "../src/runtime_trophy.h"

#ifdef __cplusplus
extern "C" {
#endif
// Called with the runtime trophy lock held. notifications contains only this session's
// unlocks. No Cocoa/ImGui calls or file access on the rendering side.
void bbgpu_trophy_update(const RuntimeTrophySnapshot *state, uint64_t notifications);
// Launch-time only, before rendering starts; reads artwork from the user's dump.
void bbgpu_trophy_set_artwork(const char *app0);
#ifdef __cplusplus
}
#endif

// Single game-side writer (serialized by runtime_trophy); bounded, nonblocking readers.
// Every field is accessed atomically, including payload bytes: no seqlock data races.
typedef struct {
    uint64_t sequence, notifications;
    RuntimeTrophySnapshot state;
} BbGpuTrophies;

static inline void bbgpu_trophies_write(BbGpuTrophies *shared,
                                       const RuntimeTrophySnapshot *state, uint64_t notifications) {
    __atomic_fetch_add(&shared->sequence, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < BB_TROPHY_COUNT; ++i)
        __atomic_store_n(&shared->state.unlocked[i], state->unlocked[i], __ATOMIC_SEQ_CST);
    for (size_t i = 0; i < sizeof(state->error); ++i)
        __atomic_store_n(&shared->state.error[i], state->error[i], __ATOMIC_SEQ_CST);
    __atomic_store_n(&shared->state.revision, state->revision, __ATOMIC_SEQ_CST);
    __atomic_store_n(&shared->notifications, notifications, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&shared->sequence, 1, __ATOMIC_SEQ_CST);
}

static inline int bbgpu_trophies_read(const BbGpuTrophies *shared,
                                     RuntimeTrophySnapshot *state, uint64_t *notifications) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint64_t before = __atomic_load_n(&shared->sequence, __ATOMIC_SEQ_CST);
        if (before & 1) continue;
        RuntimeTrophySnapshot copy;
        for (int i = 0; i < BB_TROPHY_COUNT; ++i)
            copy.unlocked[i] = __atomic_load_n(&shared->state.unlocked[i], __ATOMIC_SEQ_CST);
        for (size_t i = 0; i < sizeof(copy.error); ++i)
            copy.error[i] = __atomic_load_n(&shared->state.error[i], __ATOMIC_SEQ_CST);
        copy.revision = __atomic_load_n(&shared->state.revision, __ATOMIC_SEQ_CST);
        const uint64_t pending = __atomic_load_n(&shared->notifications, __ATOMIC_SEQ_CST);
        if (before == __atomic_load_n(&shared->sequence, __ATOMIC_SEQ_CST)) {
            copy.error[sizeof(copy.error) - 1] = 0;
            *state = copy;
            *notifications = pending & ((UINT64_C(1) << BB_TROPHY_COUNT) - 1);
            return 1;
        }
    }
    return 0; // writer busy: retain the last complete snapshot until the next frame
}
