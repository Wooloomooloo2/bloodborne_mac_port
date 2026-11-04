// SPDX-License-Identifier: GPL-2.0-or-later
// Actual public entry points must refuse invalid work before any GPU state is used.
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>
#include "../gpu/bbgpu.h"
#include "../host/bbgpu_shared.h"
#include "core/libraries/gnmdriver/gnmdriver.h"

int main() {
    using namespace Libraries::GnmDriver;
    alarm(15);
    setenv("BB_GNM_TRACE", "0", 1);
    auto* shared = static_cast<BbGpuShared*>(mmap(nullptr, BBGPU_SHARED_SIZE,
        PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0));
    assert(shared != MAP_FAILED);
    bbgpu_shared_init(shared);
    shared->host_pid = getpid();
    // Only the startup handshake is synthetic. Register the production remote
    // imports through the public interface; no renderer or guest is started.
    std::thread handshake([&] {
        const auto* record = bbgpu_ring_peek(shared, &shared->submit, UINT64_C(5000000000));
        assert(record && record->type == BBGPU_REC_START);
        const auto* start = reinterpret_cast<const BbGpuStart*>(record);
        bbgpu_reply_send(shared, start->slot, 0);
        bbgpu_ring_pop(&shared->submit, record);
    });
    const BbGpuConfig config{"GNM rejection fixture", "GNMTEST", "out/gnm-api-user", 0x05050031, 0, 640, 360};
    assert(bbgpu_init_remote(&config, shared) == 0);
    handshake.join();
    const auto submit = reinterpret_cast<decltype(&sceGnmSubmitCommandBuffers)>(bbgpu_resolve("zwY0YV91TTI"));
    const auto flip = reinterpret_cast<decltype(&sceGnmSubmitAndFlipCommandBuffers)>(bbgpu_resolve("xbxNatawohc"));
    assert(submit && flip);
    const uint64_t published = __atomic_load_n(&shared->submit.head, __ATOMIC_ACQUIRE);
    constexpr auto invalid = static_cast<s32>(0x80d11000u);
    assert(flip(0, nullptr, nullptr, nullptr, nullptr, 0, 0, 0, 0) == invalid);
    std::array<u32, 64> commands{};
    u32* dcbs[] = {commands.data()};
    u32 sizes[] = {252};
    assert(flip(1, dcbs, sizes, nullptr, nullptr, 0, 0, 0, 0) == invalid);
    sizes[0] = sizeof(commands);
    assert(flip(1, dcbs, sizes, nullptr, nullptr, 0, 0, 0, 0) == invalid);
    const u32* const_dcbs[] = {nullptr};
    assert(submit(1, const_dcbs, sizes, nullptr, nullptr) == invalid);
    const_dcbs[0] = commands.data(); sizes[0] = 3;
    assert(submit(1, const_dcbs, sizes, nullptr, nullptr) == invalid);
    assert(__atomic_load_n(&shared->submit.head, __ATOMIC_ACQUIRE) == published);
    puts("PASS: public GNM APIs reject invalid batches without initializing a GPU");
    fflush(stdout);
    _exit(0); // terminate only this fixture's background protocol threads
}
