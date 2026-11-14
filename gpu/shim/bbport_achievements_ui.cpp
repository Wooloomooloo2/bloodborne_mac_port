// SPDX-License-Identifier: GPL-2.0-or-later
// ImGui adaptation of our native Mac achievement browser/banners. Sources: docs/ACHIEVEMENTS.md.
#include "bbport_achievements.h"
#include "bbport_settings.h"
#include "bbport_overlay_metal.h"
#include "imgui.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>

namespace BbAchievements {
namespace {
std::atomic<uint64_t> shown{0};
std::atomic<bool> banner_active{false};
int banner_id = -1;
std::chrono::steady_clock::time_point banner_start;
constexpr float BannerSeconds = 6.5f;

const char* Grade(int grade) {
    static const char* names[] = {"", "Platinum", "Gold", "Silver", "Bronze"};
    return names[grade];
}
ImVec4 GradeColor(int grade) {
    static const ImVec4 colors[] = {{}, {0.67f, 0.86f, 0.94f, 1}, {0.92f, 0.73f, 0.35f, 1},
                                   {0.77f, 0.81f, 0.86f, 1}, {0.80f, 0.54f, 0.35f, 1}};
    return colors[grade];
}
void TrophyIcon(int id, int grade, float size, bool earned) {
    if (BbOverlay::MetalBackend::TrophyImage(id, size)) return;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    ImVec4 color = GradeColor(grade);
    color.w = earned ? 1.0f : 0.4f;
    const ImU32 ink = ImGui::GetColorU32(color);
    const auto at = [&](float x, float y) { return ImVec2(p.x + size*x, p.y + size*y); };
    dl->AddCircleFilled(at(.5f,.5f), size*.49f, IM_COL32(255,255,255,12));
    dl->AddQuadFilled(at(.28f,.22f), at(.72f,.22f), at(.65f,.58f), at(.35f,.58f), ink);
    dl->AddCircle(at(.27f,.36f), size*.13f, ink, 20, size*.04f);
    dl->AddCircle(at(.73f,.36f), size*.13f, ink, 20, size*.04f);
    dl->AddLine(at(.5f,.55f), at(.5f,.77f), ink, size*.07f);
    dl->AddLine(at(.34f,.78f), at(.66f,.78f), ink, size*.07f);
    ImGui::Dummy(ImVec2(size,size));
}
void Date(int64_t stamp) {
    const time_t time = static_cast<time_t>(stamp);
    tm local{};
    char text[80]{};
    if (localtime_r(&time, &local) && std::strftime(text, sizeof text, "%Y-%m-%d %H:%M", &local)) {
        ImGui::TextDisabled("Earned %s", text);
    }
}
}

bool BannerVisible() {
    return banner_active || ((Notifications() & ~shown.load()) != 0);
}

bool DrawTab(float scale) {
    const auto state = Snapshot();
    int earned[2]{};
    for (int i = 0; i < BB_TROPHY_COUNT; ++i) earned[Catalog()[i].group] += state.unlocked[i] != 0;
    ImGui::Text("%d / 40 achievements", earned[0] + earned[1]);
    ImGui::ProgressBar(float(earned[0] + earned[1]) / BB_TROPHY_COUNT, ImVec2(-1, 6*scale), "");
    ImGui::TextDisabled("Bloodborne %d / 34     The Old Hunters %d / 6", earned[0], earned[1]);
    ImGui::Spacing();
    bool enabled = BbSettings::Get().achievement_banners;
    const bool changed = ImGui::Checkbox("Show in-game unlock banners", &enabled);
    if (changed) BbSettings::Get().achievement_banners = enabled;
    ImGui::TextDisabled("Platinum requires the 33 base-game trophies. DLC is optional.");
    if (state.error[0]) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,.55f,.4f,1));
        ImGui::TextWrapped("%s", state.error);
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
    static char search[128]{};
    static int status = 0, group = 0;
    static bool descriptions = false;
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##achievement-search", "Search achievements", search, sizeof search);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * .48f);
    ImGui::Combo("##achievement-status", &status, "All achievements\0Earned\0Locked\0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::Combo("##achievement-group", &group, "Base game and DLC\0Bloodborne\0The Old Hunters\0");
    ImGui::Checkbox("Reveal locked descriptions", &descriptions);
    int matches = 0;
    for (int i = 0; i < BB_TROPHY_COUNT; ++i) {
        const auto& trophy = Catalog()[i];
        const bool unlocked = state.unlocked[i] != 0;
        if ((status == 1 && !unlocked) || (status == 2 && unlocked) ||
            (group && trophy.group != group - 1) ||
            (search[0] && !strcasestr(trophy.name, search) &&
             (!(unlocked || descriptions) || !strcasestr(trophy.description, search)))) continue;
        ++matches;
        ImGui::PushID(i);
        ImGui::Separator();
        const auto row_start = ImGui::GetCursorScreenPos();
        TrophyIcon(i, trophy.grade, 42*scale, unlocked);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextWrapped("%s", trophy.name);
        ImGui::TextColored(GradeColor(trophy.grade), "%s  ·  %s  ·  %s", Grade(trophy.grade),
                            trophy.group ? "The Old Hunters" : "Bloodborne", unlocked ? "Earned" : "Locked");
        if (unlocked || descriptions) ImGui::TextWrapped("%s", trophy.description);
        if (unlocked) Date(state.unlocked[i]);
        ImGui::EndGroup();
        const float row_height = std::max(42*scale, ImGui::GetItemRectMax().y - row_start.y);
        // Every row is a navigation target, so D-pad/keyboard browsing scrolls the full list.
        ImGui::SetCursorScreenPos(row_start);
        ImGui::InvisibleButton("##trophy-row", ImVec2(ImGui::GetContentRegionAvail().x, row_height),
                               ImGuiButtonFlags_EnableNav);
        if (ImGui::IsItemFocused()) {
            ImGui::GetWindowDrawList()->AddRect(row_start, ImGui::GetItemRectMax(),
                ImGui::GetColorU32(ImGuiCol_NavCursor), 5*scale);
        }
        ImGui::PopID();
    }
    if (!matches) ImGui::TextDisabled("No achievements match these filters.");
    return changed;
}

void DrawBanner(float scale) {
    const auto now = std::chrono::steady_clock::now();
    if (banner_id >= 0 && std::chrono::duration<float>(now - banner_start).count() >= BannerSeconds) {
        banner_id = -1;
        banner_active = false;
    }
    const auto state = Snapshot();
    const uint64_t notifications = Notifications();
    if (!BbSettings::Get().achievement_banners) {
        shown = notifications;
        banner_id = -1;
        banner_active = false;
        return;
    }
    if (banner_id < 0) {
        const uint64_t pending = notifications & ~shown.load();
        // Numeric order keeps the platinum after the final ordinary trophy in a burst.
        for (int i = 1; i <= BB_TROPHY_COUNT; ++i) {
            const int id = i % BB_TROPHY_COUNT;
            if ((pending & (UINT64_C(1) << id)) && state.unlocked[id]) {
                banner_id = id;
                shown.fetch_or(UINT64_C(1) << id);
                banner_start = now;
                banner_active = true;
                break;
            }
        }
    }
    if (banner_id < 0) return;
    const auto& trophy = Catalog()[banner_id];
    const auto* view = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(view->WorkPos.x + view->WorkSize.x*.5f,
                                   view->WorkPos.y + 20*scale), ImGuiCond_Always, ImVec2(.5f,0));
    ImGui::SetNextWindowSize(ImVec2(std::min(520*scale, view->WorkSize.x - 24*scale),0));
    // Stationary for all users, including Reduce Motion. No input, focus, sound or navigation.
    ImGui::Begin("##achievement-banner", nullptr, ImGuiWindowFlags_NoDecoration |
                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoFocusOnAppearing);
    TrophyIcon(banner_id, trophy.grade, 48*scale, true);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextColored(GradeColor(trophy.grade), "%s trophy earned", Grade(trophy.grade));
    ImGui::TextWrapped("%s", trophy.name);
    ImGui::TextWrapped("%s", trophy.description);
    ImGui::EndGroup();
    ImGui::End();
}
} // namespace BbAchievements
