// SPDX-License-Identifier: GPL-2.0-or-later
// Cross-process publication, including an interrupted writer, on arm64 and Rosetta.
#include "gpu/bbgpu_trophy.h"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

_Static_assert(sizeof(BbGpuTrophies) == 600, "C/x86-64/arm64 wire size");
_Static_assert(offsetof(BbGpuTrophies, state) == 16, "wire snapshot offset");

int main(void) {
    BbGpuTrophies *shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
                                MAP_ANON | MAP_SHARED, -1, 0);
    assert(shared != MAP_FAILED);
    RuntimeTrophySnapshot state = {0}, out = {0};
    uint64_t notifications = 99;
    assert(bbgpu_trophies_read(shared, &out, &notifications) && !notifications && !out.revision);
    __atomic_store_n(&shared->sequence, 1, __ATOMIC_SEQ_CST);
    out.revision = 777;
    assert(!bbgpu_trophies_read(shared, &out, &notifications) && out.revision == 777);
    __atomic_store_n(&shared->sequence, 0, __ATOMIC_SEQ_CST);
    const pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        unsigned samples = 0;
        for (;;) {
            if (!bbgpu_trophies_read(shared, &out, &notifications)) continue;
            for (int i = 0; i < BB_TROPHY_COUNT; ++i) assert(out.unlocked[i] == (int64_t)out.revision);
            for (unsigned i = 0; i < sizeof(out.error)-1; ++i) assert(out.error[i] == (char)(out.revision % 100));
            assert(notifications == out.revision);
            ++samples;
            if (out.revision == 10000) break;
        }
        _exit(samples ? 0 : 1);
    }
    for (uint64_t revision = 1; revision <= 10000; ++revision) {
        state.revision = revision;
        for (int i = 0; i < BB_TROPHY_COUNT; ++i) state.unlocked[i] = (int64_t)revision;
        for (unsigned i = 0; i < sizeof(state.error)-1; ++i) state.error[i] = (char)(revision % 100);
        bbgpu_trophies_write(shared, &state, revision);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    munmap(shared, sizeof(*shared));
    puts("PASS: trophy snapshots across processes, coherent payloads, bounded busy-writer reads");
}
