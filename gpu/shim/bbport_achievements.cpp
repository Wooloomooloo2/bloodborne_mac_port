// SPDX-License-Identifier: GPL-2.0-or-later
// Runtime/catalog ported from evill33t/bloodborne_mac; original catalog/log design by
// pyL1nx/bloodborne_pc. See docs/ACHIEVEMENTS.md for immutable source links.
#include "bbport_achievements.h"
#include "bbport_split.h"
#include <mutex>
#include <cstdio>

namespace {
std::mutex state_mutex;
RuntimeTrophySnapshot local_state{};
uint64_t local_notifications{};
char artwork_path[1200]{};
constexpr RuntimeTrophy catalog[] = {
#include "src/bloodborne_trophies.inc"
};
static_assert(sizeof(catalog) / sizeof(*catalog) == BB_TROPHY_COUNT);
static_assert(sizeof(BbGpuTrophies) == 600, "arm64/x86-64 trophy wire ABI");
}

extern "C" void bbgpu_trophy_update(const RuntimeTrophySnapshot* state, uint64_t notifications) {
    std::scoped_lock lock{state_mutex};
    local_state = *state;
    local_notifications = notifications;
    // Attach happens before trophy configuration, even before InitRemote starts rendering.
    if (auto* shared = BbSplit::Shared()) {
        bbgpu_trophies_write(&shared->trophies, state, notifications);
    }
}

extern "C" void bbgpu_trophy_set_artwork(const char* app0) {
    if (!app0 || std::snprintf(artwork_path, sizeof artwork_path,
                              "%s/sce_sys/trophy/trophy00.trp", app0) >= int(sizeof artwork_path)) {
        artwork_path[0] = 0;
    }
    if (auto* shared = BbSplit::Shared()) {
        std::snprintf(shared->trophy_archive, sizeof shared->trophy_archive, "%s", artwork_path);
    }
}

namespace BbAchievements {
const RuntimeTrophy* Catalog() { return catalog; }
std::string ArtworkPath() {
    return BbSplit::Host() ? BbSplit::Shared()->trophy_archive : artwork_path;
}

RuntimeTrophySnapshot Snapshot() {
    std::scoped_lock lock{state_mutex};
    if (BbSplit::Host()) {
        bbgpu_trophies_read(&BbSplit::Shared()->trophies, &local_state, &local_notifications);
    }
    return local_state;
}

uint64_t Notifications() {
    if (BbSplit::Host()) {
        // Only a visibility hint; Snapshot reads a coherent payload before displaying it.
        return __atomic_load_n(&BbSplit::Shared()->trophies.notifications, __ATOMIC_SEQ_CST);
    }
    std::scoped_lock lock{state_mutex};
    return local_notifications;
}
} // namespace BbAchievements
