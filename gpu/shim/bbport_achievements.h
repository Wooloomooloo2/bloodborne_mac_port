// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "gpu/bbgpu_trophy.h"
#include <string>

namespace BbAchievements {
const RuntimeTrophy* Catalog();
RuntimeTrophySnapshot Snapshot();
uint64_t Notifications();
std::string ArtworkPath();
bool BannerVisible();
// ImGui calls, under the overlay's context lock. Returns true when a setting changed.
bool DrawTab(float scale);
void DrawBanner(float scale);
} // namespace BbAchievements
