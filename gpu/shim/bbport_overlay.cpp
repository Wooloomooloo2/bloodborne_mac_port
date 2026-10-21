// SPDX-License-Identifier: GPL-2.0-or-later
#define IMGUI_DEFINE_MATH_OPERATORS // ImVec2 arithmetic for the custom-drawn controls
#include "bbport_overlay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include "bbport_settings.h"
#include "bbport_overlay_metal.h"
#include "imgui.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"

// DejaVu Sans, embedded (third_party/fonts, Bitstream Vera license): Latin, Cyrillic and
// symbols such as U+2318 (Cmd) and U+2192 (arrow). ImGui 1.92 rasterizes glyphs on demand.
#ifdef __APPLE__
// Mach-O: C symbols carry a leading underscore; read-only data lives in __TEXT,__const.
asm(".section __TEXT,__const\n"
    ".balign 16\n"
    ".private_extern _bb_font_ttf\n"
    ".globl _bb_font_ttf\n"
    "_bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".private_extern _bb_font_ttf_end\n"
    ".globl _bb_font_ttf_end\n"
    "_bb_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];

extern "C" void runtime_restart(void); // bb-probe (probe.c)

// Shortcut shown in the menu header, and the modifier ImGui uses for click-to-type on sliders
// (with io.ConfigMacOSXBehaviors ImGui swaps Cmd and Ctrl, so Cmd+click on macOS).
#ifdef __APPLE__
#define BB_MENU_KEYS_UI "\xE2\x8C\x98," // "⌘,"
#define BB_CTRL_CLICK "\xE2\x8C\x98-click"     // "⌘-click"
#else
#define BB_MENU_KEYS_UI "Insert"
#define BB_CTRL_CLICK "Ctrl+click"
#endif

namespace BbOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
std::atomic<bool> initialized{false}; // read without the lock (Visible, Render)
std::atomic<bool> menu_open{false};
bool l3_down = false, r3_down = false;
// A key or pad combo that toggled/closed the menu is still held: the game polls the keyboard
// and pad state (runtime_pad.c), so it would see it right after the menu closes (Escape is
// Circle, L3/R3 are stick clicks). CapturesInput() holds the game's input
// neutral until it is released.
std::atomic<int> held_scancode{SDL_SCANCODE_UNKNOWN};
std::atomic<bool> held_sticks{false};
// The game's text dialog (ImeDialog): set from the window thread, drawn by the present thread.
std::atomic<bool> prompt_active{false};
std::mutex prompt_mutex;
std::string prompt_title, prompt_text;

bool IsMenuToggle(const SDL_KeyboardEvent& key) {
    if (key.key == SDLK_INSERT) {
        return true;
    }
#ifdef __APPLE__
    // Mac keyboards have no Insert key: Cmd+, (the macOS Settings shortcut). Not F1: the
    // game's keyboard map (runtime_pad.c) uses it for Options.
    if (key.key == SDLK_COMMA && (key.mod & SDL_KMOD_GUI) &&
        !(key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_SHIFT))) {
        return true;
    }
#endif
    return false;
}
bool dirty = false; // settings changed: saved shortly after the last change, and on close
std::chrono::steady_clock::time_point last_change{};
float base_scale = 1.0f;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;
// "Capture Frame": FrameCapture::Written() when it was pressed (~0: no request), and when.
u64 capture_base = ~u64{0};
std::chrono::steady_clock::time_point capture_time;

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    ImGui::GetIO().MouseDrawCursor = value;
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    case SDLK_LGUI: return ImGuiKey_LeftSuper;
    case SDLK_RGUI: return ImGuiKey_RightSuper;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// BB_MENU_TRACE=1: clicks, widget changes and any outside change of key settings (diagnostics).
const bool menu_trace = [] {
    const char* env = std::getenv("BB_MENU_TRACE");
    return env && env[0] == '1';
}();
bool widget_changed_this_frame = false;

// A widget changed the settings: saved shortly after the last change (Render), and on close.
void MarkChanged() {
    dirty = true;
    last_change = std::chrono::steady_clock::now();
    widget_changed_this_frame = true;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        MarkChanged();
    }
}

// ---------------------------------------------------------------------------------------------
// Look: a macOS dark-mode settings panel. Grouped rows on lighter cards, system-blue accent for
// whatever is selected or on, orange only for "needs a restart". Sizes below are at 1080p and
// multiplied by base_scale (style sizes are scaled by ScaleAllSizes in Render).
// ---------------------------------------------------------------------------------------------

constexpr ImVec4 kWindowBg{0.110f, 0.110f, 0.118f, 0.94f};  // #1C1C1E
constexpr ImVec4 kCard{0.173f, 0.173f, 0.180f, 1.0f};       // #2C2C2E
constexpr ImVec4 kControl{0.227f, 0.227f, 0.235f, 1.0f};    // #3A3A3C
constexpr ImVec4 kControlHover{0.282f, 0.282f, 0.290f, 1.0f}; // #48484A
constexpr ImVec4 kControlActive{0.329f, 0.329f, 0.337f, 1.0f}; // #545456
constexpr ImVec4 kTrack{0.110f, 0.110f, 0.118f, 1.0f};      // segmented-control trough
constexpr ImVec4 kSwitchOff{0.471f, 0.471f, 0.502f, 0.36f}; // macOS "off" switch track
constexpr ImVec4 kText{0.949f, 0.949f, 0.969f, 1.0f};       // #F2F2F7
constexpr ImVec4 kSecondary{0.557f, 0.557f, 0.576f, 1.0f};  // #8E8E93
constexpr ImVec4 kHairline{1.0f, 1.0f, 1.0f, 0.08f};
constexpr ImVec4 kAccent{0.039f, 0.518f, 1.0f, 1.0f};       // #0A84FF
constexpr ImVec4 kAccentHover{0.251f, 0.612f, 1.0f, 1.0f};  // #409CFF
constexpr ImVec4 kAccentActive{0.0f, 0.443f, 0.890f, 1.0f}; // #0071E3
constexpr ImVec4 kOrange{1.0f, 0.624f, 0.039f, 1.0f};       // #FF9F0A
constexpr ImVec4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};

constexpr float kBaseFontSize = 17.0f; // px at 1080p
constexpr float kWindowWidth = 720.0f;

ImVec4 WithAlpha(ImVec4 c, float a) {
    c.w *= a;
    return c;
}

// The current card: right edge of its content area (screen space), for full-width rows.
struct CardState {
    ImVec2 start{};
    float width = 0.0f;
    float inner_right = 0.0f;
    int rows = 0; // rows so far, for the hairline between rows
    bool open = false;
};
CardState card;

float RightEdge() {
    return card.open ? card.inner_right
                     : ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
}
float AvailWidth() {
    return std::max(RightEdge() - ImGui::GetCursorScreenPos().x, 1.0f);
}

/// Secondary-colour text in a slightly smaller size, wrapped to the card.
void Note(const char* text, const ImVec4& color = kSecondary) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.88f);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(RightEdge() - ImGui::GetWindowPos().x);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

/// A note that pulls up against the control above it.
void HintBelow(const char* text) {
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y * 0.5f);
    Note(text);
}

/// Rounded label ("pill"); returns its size. Drawn at `pos`, no layout.
ImVec2 DrawPill(ImDrawList* dl, ImVec2 pos, const char* text, const ImVec4& color, bool filled) {
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetStyle().FontSizeBase * 0.76f * ImGui::GetStyle().FontScaleMain;
    const ImVec2 pad(7.0f * base_scale, 2.0f * base_scale);
    const ImVec2 text_size = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 box = text_size + pad * 2.0f;
    dl->AddRectFilled(pos, pos + box, ImGui::GetColorU32(WithAlpha(color, filled ? 0.22f : 0.12f)),
                      box.y * 0.5f);
    dl->AddText(font, size, pos + pad, ImGui::GetColorU32(filled ? color : kSecondary), text);
    return box;
}

/// Restart badge after a label on the same line: orange when a change is waiting for a
/// restart, grey when the setting only ever applies at launch.
void RestartPill(bool pending) {
    ImGui::SameLine(0.0f, 8.0f * base_scale);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float line = ImGui::GetTextLineHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* text = pending ? "Restart to apply" : "Applies after restart";
    // Centred on the text line.
    const float size = ImGui::GetStyle().FontSizeBase * 0.76f * ImGui::GetStyle().FontScaleMain;
    const float box_h = size + 4.0f * base_scale;
    const ImVec2 box = DrawPill(dl, ImVec2(pos.x, pos.y + (line - box_h) * 0.5f), text,
                                pending ? kOrange : kSecondary, pending);
    ImGui::Dummy(ImVec2(box.x, line));
}

/// Small circled "i" after a label; the text appears as a tooltip on hover or nav focus.
void InfoTip(const char* text, float y_offset = 0.0f) {
    ImGui::SameLine(0.0f, 6.0f * base_scale);
    const float line = ImGui::GetTextLineHeight();
    const float d = line * 0.82f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    pos.y += y_offset; // after AlignTextToFramePadding the text sits lower than the cursor
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushID(text);
    ImGui::InvisibleButton("##info", ImVec2(d, line), ImGuiButtonFlags_EnableNav);
    ImGui::PopID();
    const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 center(pos.x + d * 0.5f, pos.y + line * 0.5f);
    const ImVec4 color = hot ? kAccentHover : kSecondary;
    dl->AddCircle(center, d * 0.5f - 0.5f, ImGui::GetColorU32(color), 0, 1.25f * base_scale);
    ImFont* font = ImGui::GetFont();
    const float size = d * 0.78f;
    const ImVec2 glyph = font->CalcTextSizeA(size, FLT_MAX, 0.0f, "i");
    dl->AddText(font, size, center - glyph * 0.5f, ImGui::GetColorU32(color), "i");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

/// Hairline between rows of a card (macOS grouped list).
void RowStart() {
    if (card.open && card.rows++ > 0) {
        const float y = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(ImGui::GetCursorScreenPos().x, y),
                                            ImVec2(card.inner_right, y),
                                            ImGui::GetColorU32(kHairline), 1.0f);
    }
}

/// Grouped section: optional title (and restart badge / info) above a rounded card.
void BeginCard(const char* title = nullptr, int restart = -1, const char* info = nullptr) {
    if (title) {
        ImGui::Indent(4.0f * base_scale);
        ImGui::PushStyleColor(ImGuiCol_Text, kSecondary);
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        if (restart >= 0) {
            RestartPill(restart > 0);
        }
        if (info) {
            InfoTip(info);
        }
        ImGui::Unindent(4.0f * base_scale);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y * 0.4f);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    const ImVec2 pad(16.0f * base_scale, 12.0f * base_scale);
    card.start = ImGui::GetCursorScreenPos();
    // Keep clear of the content's scrollbar when it shows.
    card.width = ImGui::GetContentRegionAvail().x -
                 (ImGui::GetScrollMaxY() > 0.0f ? 6.0f * base_scale : 0.0f);
    card.inner_right = card.start.x + card.width - pad.x;
    card.rows = 0;
    card.open = true;
    ImGui::SetCursorScreenPos(card.start + pad);
    ImGui::BeginGroup();
}

void EndCard() {
    ImGui::EndGroup();
    const float bottom = ImGui::GetItemRectMax().y + 12.0f * base_scale;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSetCurrent(0);
    const ImVec2 end(card.start.x + card.width, bottom);
    const float rounding = 10.0f * base_scale;
    dl->AddRectFilled(card.start, end, ImGui::GetColorU32(kCard), rounding);
    dl->AddRect(card.start, end, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.045f)), rounding);
    dl->ChannelsMerge();
    card.open = false;
    ImGui::SetCursorScreenPos(ImVec2(card.start.x, bottom));
    ImGui::Dummy(ImVec2(0.0f, 2.0f * base_scale));
}

/// Inline explanation shown above controls that are disabled, so the dimmed state reads as
/// intentional ("why can't I change this?") rather than broken.
void DisabledReason(const char* text) {
    const float line = ImGui::GetTextLineHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = line * 0.36f;
    const ImVec2 center(pos.x + r, pos.y + line * 0.5f);
    dl->AddCircleFilled(center, r, ImGui::GetColorU32(WithAlpha(kAccent, 0.9f)));
    ImFont* font = ImGui::GetFont();
    const float size = r * 1.6f;
    const ImVec2 glyph = font->CalcTextSizeA(size, FLT_MAX, 0.0f, "i");
    dl->AddText(font, size, center - glyph * 0.5f, ImGui::GetColorU32(kWhite), "i");
    ImGui::Indent(r * 2.0f + 8.0f * base_scale);
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::PushTextWrapPos(RightEdge() - ImGui::GetWindowPos().x);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Unindent(r * 2.0f + 8.0f * base_scale);
}

/// Label on its own line above a full-width control.
void Label(const char* text) {
    RowStart();
    ImGui::TextUnformatted(text);
}

/// Segmented control (a row of mutually exclusive buttons). Picks need one click on the
/// visible option: nothing pops up over other options, so a stray second click lands on the
/// same control instead of a list item. Falls back to a dropdown when the options don't fit.
/// Returns the index picked this frame, or -1. A click on the selected option counts only with
/// `reselect` (it shows what runs, which the user may want to choose explicitly).
int Segmented(const char* id, const char* const* labels, int count, int current,
              const bool* enabled = nullptr, const char* const* tips = nullptr,
              bool accent = true, float width = 0.0f, bool reselect = false) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float pad_x = 12.0f * base_scale;
    const float inset = 2.0f * base_scale;
    const float height = ImGui::GetFrameHeight() + 2.0f * base_scale;
    float natural[16];
    float total = 0.0f, widest = 0.0f;
    count = std::min(count, 16);
    for (int i = 0; i < count; ++i) {
        natural[i] = ImGui::CalcTextSize(labels[i]).x + pad_x * 2.0f;
        widest = std::max(widest, natural[i]);
        total += natural[i];
    }
    const float full = width > 0.0f ? width : AvailWidth();
    int picked = -1;
    ImGui::PushID(id);
    if (total + inset * 2.0f > full) {
        // Too narrow: a dropdown that still looks like one.
        ImGui::SetNextItemWidth(full);
        if (ImGui::BeginCombo("##combo", labels[std::clamp(current, 0, count - 1)])) {
            for (int i = 0; i < count; ++i) {
                ImGui::BeginDisabled(enabled && !enabled[i]);
                if (ImGui::Selectable(labels[i], i == current)) {
                    picked = i;
                }
                if (tips && tips[i]) {
                    ImGui::SetItemTooltip("%s", tips[i]);
                }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
        return picked;
    }
    const float extra = (full - inset * 2.0f - total) / float(count);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = style.FrameRounding + inset;
    dl->AddRectFilled(pos, pos + ImVec2(full, height), ImGui::GetColorU32(kTrack), rounding);
    float x = pos.x + inset;
    for (int i = 0; i < count; ++i) {
        const float w = (width > 0.0f ? widest : natural[i]) +
                        (width > 0.0f ? (full - inset * 2.0f - widest * count) / count : extra);
        const bool on = !enabled || enabled[i];
        const bool selected = i == current;
        ImGui::SetCursorScreenPos(ImVec2(x, pos.y + inset));
        ImGui::BeginDisabled(!on);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton(labels[i], ImVec2(w, height - inset * 2.0f),
                                   ImGuiButtonFlags_EnableNav) &&
            (!selected || reselect)) {
            picked = i;
        }
        ImGui::PopID();
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (selected) {
            const ImVec4 fill = accent ? (held ? kAccentActive : kAccent) : kControlHover;
            dl->AddRectFilled(a, b, ImGui::GetColorU32(fill), style.FrameRounding);
        } else if (held) {
            dl->AddRectFilled(a, b, ImGui::GetColorU32(kControlActive), style.FrameRounding);
        } else if (hovered) {
            dl->AddRectFilled(a, b, ImGui::GetColorU32(kControl), style.FrameRounding);
        } else if (i > 0 && i - 1 != current) {
            // Divider between two unselected segments.
            const float inset_y = (b.y - a.y) * 0.25f;
            dl->AddLine(ImVec2(a.x, a.y + inset_y), ImVec2(a.x, b.y - inset_y),
                        ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.10f)), 1.0f);
        }
        const ImVec2 text = ImGui::CalcTextSize(labels[i]);
        const ImVec4 text_color = selected && accent ? kWhite : on ? kText : kSecondary;
        dl->AddText(ImVec2(std::floor(a.x + (b.x - a.x - text.x) * 0.5f),
                           std::floor(a.y + (b.y - a.y - text.y) * 0.5f)),
                    ImGui::GetColorU32(text_color), labels[i]);
        if (tips && tips[i] && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip |
                                                    ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", tips[i]);
        }
        ImGui::EndDisabled();
        x += w;
    }
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(ImVec2(full, height));
    ImGui::PopID();
    return picked;
}

/// Full-width settings row with a label and a macOS-style switch; the whole row is the hit
/// target and the nav stop. Returns true when toggled.
bool ToggleRow(const char* label, bool* value, int restart = -1) {
    RowStart();
    const float height = ImGui::GetFrameHeight() + 2.0f * base_scale;
    const float width = AvailWidth();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    const bool pressed = ImGui::InvisibleButton("##toggle", ImVec2(width, height),
                                                ImGuiButtonFlags_EnableNav);
    const ImGuiID id = ImGui::GetItemID();
    ImGui::PopID();
    if (pressed) {
        *value = !*value;
    }
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float line = ImGui::GetTextLineHeight();
    const float text_y = std::floor(pos.y + (height - line) * 0.5f);
    dl->AddText(ImVec2(pos.x, text_y), ImGui::GetColorU32(ImGuiCol_Text), label);
    if (restart >= 0) {
        const float size = ImGui::GetStyle().FontSizeBase * 0.76f * ImGui::GetStyle().FontScaleMain;
        const float box_h = size + 4.0f * base_scale;
        const float label_w = ImGui::CalcTextSize(label).x;
        DrawPill(dl, ImVec2(pos.x + label_w + 8.0f * base_scale, pos.y + (height - box_h) * 0.5f),
                 restart > 0 ? "Restart to apply" : "Applies after restart",
                 restart > 0 ? kOrange : kSecondary, restart > 0);
    }
    // Switch, animated towards its new state.
    float& t = *ImGui::GetStateStorage()->GetFloatRef(id, *value ? 1.0f : 0.0f);
    const float target = *value ? 1.0f : 0.0f;
    t += (target - t) * std::min(1.0f, ImGui::GetIO().DeltaTime * 18.0f);
    if (std::abs(target - t) < 0.01f) {
        t = target;
    }
    const float sh = line * 1.3f;
    const float sw = sh * 1.72f;
    const ImVec2 s0(pos.x + width - sw, pos.y + (height - sh) * 0.5f);
    const ImVec4 off = hovered ? WithAlpha(kSwitchOff, 1.35f) : kSwitchOff;
    const ImVec4 track(off.x + (kAccent.x - off.x) * t, off.y + (kAccent.y - off.y) * t,
                       off.z + (kAccent.z - off.z) * t, off.w + (kAccent.w - off.w) * t);
    dl->AddRectFilled(s0, s0 + ImVec2(sw, sh), ImGui::GetColorU32(track), sh * 0.5f);
    const float r = sh * 0.5f - 2.0f * base_scale;
    const ImVec2 knob(s0.x + sh * 0.5f + (sw - sh) * t, s0.y + sh * 0.5f);
    dl->AddCircleFilled(knob + ImVec2(0.0f, 0.75f * base_scale), r,
                        ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.25f)));
    dl->AddCircleFilled(knob, r, ImGui::GetColorU32(kWhite));
    return pressed;
}

/// Toggle bound to a setting (marks it dirty; BB_MENU_TRACE logs it).
void Toggle(const char* label, std::atomic<bool>& value, int restart = -1) {
    bool v = value;
    const bool changed = ToggleRow(label, &v, restart);
    if (changed && menu_trace) {
        std::printf("menu: checkbox '%s' -> %d\n", label, int(v));
    }
    Store(value, v, changed);
}

/// Label on the left, slider filling the right side of the row.
void SliderRow(const char* label, std::atomic<float>& value, float lo, float hi,
               const char* info = nullptr) {
    RowStart();
    const float x0 = ImGui::GetCursorScreenPos().x;
    const float row_y = ImGui::GetCursorScreenPos().y;
    const float column = (card.inner_right - x0) * 0.42f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (info) {
        InfoTip(info, ImGui::GetStyle().FramePadding.y);
    }
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorScreenPos(ImVec2(x0 + column, row_y));
    ImGui::SetNextItemWidth(AvailWidth());
    float v = value;
    char id[64];
    std::snprintf(id, sizeof(id), "##%s", label);
    bool changed = ImGui::SliderFloat(id, &v, lo, hi, "%.2f");
    // D-pad left/right step the focused slider: ImGui's own gamepad tweak needs Cross first and
    // moves 0.01 a press (200 presses across the sharpness range).
    if (ImGui::IsItemFocused() && !ImGui::IsItemActive()) {
        const float step = (hi - lo) / 40.0f;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft)) {
            v = std::max(lo, v - step);
            changed = true;
        } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)) {
            v = std::min(hi, v + step);
            changed = true;
        }
    }
    Store(value, v, changed);
}

/// Label on the left, read-only value on the right.
void ValueRow(const char* label, const char* value) {
    RowStart();
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    const float w = ImGui::CalcTextSize(value).x;
    ImGui::SetCursorScreenPos(ImVec2(card.inner_right - w, ImGui::GetCursorScreenPos().y));
    ImGui::PushStyleColor(ImGuiCol_Text, kSecondary);
    ImGui::TextUnformatted(value);
    ImGui::PopStyleColor();
}

/// Rounded key-cap, e.g. "⌘," or "L1". Returns its size; `measure` only measures.
ImVec2 Keycap(ImDrawList* dl, ImVec2 pos, const char* text, bool measure = false) {
    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetStyle().FontSizeBase * 0.8f * ImGui::GetStyle().FontScaleMain;
    const ImVec2 pad(6.0f * base_scale, 2.0f * base_scale);
    const ImVec2 box = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text) + pad * 2.0f;
    if (!measure) {
        const float rounding = 5.0f * base_scale;
        dl->AddRectFilled(pos, pos + box, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.06f)),
                          rounding);
        dl->AddRect(pos, pos + box, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.16f)),
                    rounding);
        dl->AddText(font, size, pos + pad, ImGui::GetColorU32(kSecondary), text);
    }
    return box;
}

/// Accent-filled button (the default action), as on macOS.
bool PrimaryButton(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentActive);
    ImGui::PushStyleColor(ImGuiCol_Text, kWhite);
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    return pressed;
}

float ButtonWidth(const char* label) {
    return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
}

// Effects that are about how the game starts rather than how it looks (Advanced tab).
bool IsStartupOption(int e) {
    const char* key = BbSettings::Effects[e].key;
    return !std::strcmp(key, "skip_intro") || !std::strncmp(key, "debug_", 6);
}

enum Tab : int { TabDisplay = 0, TabGraphics, TabAdvanced, TabCount };
int current_tab = TabDisplay;
std::atomic<bool> gamepad_in_use{false}; // last menu input came from a pad: show L1/R1 hints

void DisplayTab(BbSettings::Values& s) {
    // The menu's choices (FSR 4 is removed from the Mac build; an ini naming it still loads).
    static constexpr int choices[] = {BbSettings::UpscalerOff, BbSettings::UpscalerFsr3,
                                      BbSettings::UpscalerTaa, BbSettings::UpscalerMetalFx};
    static constexpr int choice_count = int(sizeof(choices) / sizeof(choices[0]));
    static const char* labels[choice_count] = {"Off", "FSR 3.1", "TAA", "MetalFX"};
    static const char* about[choice_count] = {
        "No temporal upscaling: the game's own image.",
        "AMD FSR 3.1. Light on the GPU.",
        "Anti-aliasing at output resolution, without upscaling.",
        "Apple MetalFX temporal upscaling, on the Mac's GPU.",
    };
    bool available[choice_count];
    const char* tips[choice_count] = {};
    for (int i = 0; i < choice_count; ++i) {
        available[i] = BbSettings::UpscalerAvailable(choices[i]);
        if (!available[i]) {
            tips[i] = "Not supported by this GPU";
        }
    }
    const int upscaler = s.upscaler;
    const int requested = s.requested_upscaler;
    const bool upscaler_on = upscaler != BbSettings::UpscalerOff;
    const bool taa = upscaler == BbSettings::UpscalerTaa;
    int current = 0;
    for (int i = 0; i < choice_count; ++i) {
        if (choices[i] == upscaler) {
            current = i;
        }
    }

    BeginCard("Upscaling");
    Label("Upscaler");
    InfoTip("Temporal upscalers build each frame from several previous ones: sharper edges, and "
            "with a lower preset, a faster game. DLSS and XeSS are planned for later.");
    // The selected segment is what runs; while that is a fallback for the saved choice, a click
    // on it saves it as the choice.
    if (const int picked = Segmented("upscaler", labels, choice_count, current, available, tips,
                                     true, 0.0f, requested != upscaler);
        picked >= 0 && choices[picked] != requested) {
        BbSettings::ChooseUpscaler(choices[picked]);
        MarkChanged();
    }
    HintBelow(about[current]);
    for (int i = 0; i < choice_count; ++i) {
        if (!available[i]) {
            char line[96];
            std::snprintf(line, sizeof(line), "Not available on this GPU: %s", labels[i]);
            Note(line);
        }
    }
    if (requested != upscaler) {
        // The saved choice cannot run here (FSR 3.1 below Apple7, the removed FSR 4, MetalFX
        // without a scaler): say what runs instead; the ini keeps the choice.
        char text[256];
        std::snprintf(text, sizeof(text),
                      "%s from your settings is not available%s; %s runs instead. Your choice "
                      "stays saved until you pick another.",
                      requested == BbSettings::UpscalerFsr3 ? "FSR 3.1"
                      : BbSettings::IsFsr4(requested)       ? "FSR 4"
                      : requested == BbSettings::UpscalerMetalFx ? "MetalFX"
                                                                 : "The upscaler",
                      BbSettings::IsFsr4(requested) ? " on the Mac" : " on this GPU",
                      labels[current]);
        Note(text, kOrange);
    }
    EndCard();

    // Everything below depends on an upscaler: say so instead of only dimming it.
    BeginCard("Quality");
    if (!upscaler_on) {
        DisabledReason("Choose an upscaler above to adjust these options.");
        ImGui::Spacing();
    }
    ImGui::BeginDisabled(!upscaler_on);
    Label("Preset");
    if (BbSettings::FixedRenderSession()) {
        if (const char* automatic = std::getenv("BB_AUTO_RENDER_RES");
            automatic && automatic[0] == '1') {
            InfoTip("With an output other than 1080p, the whole game renders at the preset "
                    "resolution (patched at launch): fastest on the Steam Deck and weaker GPUs. "
                    "Preset or output resolution changes need a restart. \"Live resolution "
                    "changes\" (Advanced) allows changes without a restart (post-processing then "
                    "stays at 1080p, which is slower).");
        } else {
            InfoTip("BB_RENDER_RES fixes the scene size at launch. Unset this variable to change "
                    "resolution and presets without restarting the game.");
        }
        RestartPill(BbSettings::ResolutionNeedsRestart());
    } else {
        InfoTip("Native AA: the upscaler works as anti-aliasing only. The other presets lower the "
                "scene's render resolution relative to the output. The HUD is drawn at output "
                "resolution. A new preset applies from the next frame, without restarting the "
                "game.");
    }
    {
        const char* names[BbSettings::PresetCount];
        char tip_text[BbSettings::PresetCount][96];
        const char* preset_tips[BbSettings::PresetCount];
        const int output = s.output_res;
        for (int i = 0; i < BbSettings::PresetCount; ++i) {
            const float scale = BbSettings::PresetScale(i);
            names[i] = BbSettings::PresetName(i);
            std::snprintf(tip_text[i], sizeof(tip_text[i]), "x%.1f: renders %d \xC3\x97 %d", scale,
                          int(std::lround(BbSettings::OutputWidths[output] / scale / 2) * 2),
                          int(std::lround(BbSettings::OutputHeights[output] / scale / 2) * 2));
            preset_tips[i] = tip_text[i];
        }
        const int preset = taa ? BbSettings::NativeAA : s.preset.load();
        ImGui::BeginDisabled(taa);
        if (const int picked = Segmented("preset", names, BbSettings::PresetCount, preset,
                                         nullptr, preset_tips);
            picked >= 0) {
            Store(s.preset, picked, true);
        }
        ImGui::EndDisabled();
    }
    if (taa) {
        HintBelow("TAA anti-aliases the scene at output resolution, without upscaling. The saved "
                  "preset returns when an upscaler is selected.");
    }
    char resolution[64];
    std::snprintf(resolution, sizeof(resolution), "%d \xC3\x97 %d", s.active_render_width.load(),
                  s.active_render_height.load());
    ValueRow("Scene resolution", resolution);
    if (BbSettings::FixedRenderSession()) {
        ValueRow("Preset at launch", BbSettings::PresetName(s.startup_preset));
    }
    Toggle("Sharpening (RCAS)", s.sharpen);
    ImGui::BeginDisabled(!s.sharpen);
    SliderRow("Sharpness", s.sharpness, 0.0f, 2.0f,
              "Up to 1: the upscaler's own sharpening (RCAS). Above 1, an extra RCAS pass is "
              "added. " BB_CTRL_CLICK " the slider to type an exact value.");
    ImGui::EndDisabled();
    Toggle("Subpixel jitter", s.jitter);
    HintBelow("Offsets each frame by a fraction of a pixel so the upscaler gathers more detail "
              "over time. Off: history-based anti-aliasing only.");
    ImGui::EndDisabled(); // upscaler off
    EndCard();

    BeginCard("Output", BbSettings::FixedRenderSession() ? int(BbSettings::ResolutionNeedsRestart())
                                                          : -1);
    Label("Output resolution");
    InfoTip(BbSettings::FixedRenderSession()
                ? "Size of the final frame and HUD. The preset sets the scene size relative to "
                  "the output: 4K Performance = 1920x1080. Takes effect after restarting the game."
                : "Size of the final frame and HUD; changes at the next frame boundary. The preset "
                  "sets the scene size relative to the output: 4K Performance = 1920x1080. "
                  "Changing it resets the upscaler history and may cause a brief pause.");
    static const char* outputs[] = {"1280 \xC3\x97 720", "1920 \xC3\x97 1080",
                                    "2560 \xC3\x97 1440", "3456 \xC3\x97 1944",
                                    "3840 \xC3\x97 2160"};
    static_assert(sizeof(outputs) / sizeof(outputs[0]) == BbSettings::OutputCount);
    if (const int picked = Segmented("output", outputs, BbSettings::OutputCount, s.output_res);
        picked >= 0) {
        Store(s.output_res, picked, true);
    }
    HintBelow("Size of the final frame and HUD.");
    EndCard();

    BeginCard("Overlay");
    Toggle("FPS counter in corner", s.show_fps);
    EndCard();

    // Started by the launcher: its "show every time" switch, which is hard to find again once off.
    if (const int shown = BbSettings::ShowLauncher(); shown >= 0) {
        BeginCard("Launcher");
        bool v = shown != 0;
        if (ToggleRow("Show the launcher when Bloodborne opens", &v)) {
            BbSettings::SetShowLauncher(v);
        }
        HintBelow("Holding \xE2\x8C\xA5 Option while opening Bloodborne shows it once.");
        EndCard();
    }

    BeginCard("Mouse");
    Toggle("Mouse camera", s.mouse_look);
    SliderRow("Sensitivity", s.mouse_sensitivity, 0.2f, 4.0f);
    Toggle("Invert vertical", s.mouse_invert_y);
    HintBelow("Click in the game window to use the mouse; Cmd gives the cursor back. Left click "
              "R1, right click L2, middle click R3 (lock on), side buttons R2 and L1.");
    EndCard();
}

void GraphicsTab(BbSettings::Values& s) {
    BeginCard("Model detail", int(s.model_lod != s.startup_model_lod));
    static const char* lods[] = {"Highest", "Game default", "Lower", "Lowest"};
    static const char* lod_tips[] = {"Level of detail bias -2", "Level of detail bias 0",
                                     "Level of detail bias 1", "Level of detail bias 2"};
    static constexpr int lod_values[] = {-2, 0, 1, 2};
    int lod_index = 1;
    for (int i = 0; i < 4; ++i) {
        if (lod_values[i] == s.model_lod) lod_index = i;
    }
    if (const int picked = Segmented("lod", lods, 4, lod_index, nullptr, lod_tips); picked >= 0) {
        Store(s.model_lod, lod_values[picked], true);
    }
    HintBelow("Distance at which characters and objects switch to simpler models.");
    EndCard();

    bool pending = false;
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        pending |= !IsStartupOption(e) && s.effects[e] != s.startup_effects[e];
    }
    BeginCard("Effects", int(pending),
              "Effects are switched on and off by game patches at launch "
              "(patches/Bloodborne.xml).");
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        if (!IsStartupOption(e)) {
            Toggle(BbSettings::Effects[e].label, s.effects[e],
                   s.effects[e] != s.startup_effects[e] ? 1 : -1);
        }
    }
    HintBelow("Motion blur and shadows from dynamic lights are noticeably heavy on the GPU.");
    EndCard();
}

void AdvancedTab(BbSettings::Values& s) {
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;

    BeginCard("Reactive mask");
    if (!upscaler_on || taa) {
        DisabledReason(!upscaler_on ? "Choose an upscaler on the Display tab to adjust these "
                                      "options."
                                    : "TAA doesn't use a reactive mask. Choose FSR or MetalFX on "
                                      "the Display tab to adjust it.");
        ImGui::Spacing();
    }
    ImGui::BeginDisabled(!upscaler_on || taa);
    Toggle("Enable mask", s.reactive);
    HintBelow("Marks transparent effects (particles, haze) so the upscaler relies less on previous "
              "frames. Less trailing behind effects, but shimmering returns beneath them.");
    ImGui::BeginDisabled(!s.reactive);
    SliderRow("Scale", s.reactive_scale, 0.0f, 4.0f);
    SliderRow("Threshold", s.reactive_threshold, 0.0f, 1.0f);
    SliderRow("Maximum", s.reactive_max, 0.0f, 1.0f);
    bool show_mask = s.debug_view == BbSettings::DebugReactive;
    if (ToggleRow("Show mask (debug)", &show_mask)) {
        s.debug_view = show_mask ? BbSettings::DebugReactive : BbSettings::DebugNone;
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    EndCard();

    BeginCard("Motion vectors");
    if (!upscaler_on) {
        DisabledReason("Choose an upscaler on the Display tab to adjust these options.");
        ImGui::Spacing();
    }
    ImGui::BeginDisabled(!upscaler_on);
    Toggle("Character motion vectors", s.object_motion,
           int(s.object_motion != s.startup_object_motion));
    HintBelow("Precise vectors for animated objects: clothing and weapons break up less in "
              "motion. The static scene gets no extra pass.");
    bool show_motion = s.debug_view == BbSettings::DebugMotion;
    if (ToggleRow("Show motion vectors (debug)", &show_motion)) {
        s.debug_view = show_motion ? BbSettings::DebugMotion : BbSettings::DebugNone;
    }
    if (show_motion) {
        HintBelow("Red/green: horizontal/vertical motion (8 pixels = full brightness). Blue: the "
                  "pixel got a precise object vector, not just camera motion. A moving object "
                  "with neither blue nor red/green looks static to the upscaler, which causes "
                  "trailing.");
    }
    ImGui::EndDisabled();
    EndCard();

    BeginCard("Resolution", int(s.live_resolution != s.startup_live_resolution));
    Label("Live resolution changes");
    InfoTip("On: output resolution and preset change without a restart, but the game's "
            "post-processing stays at 1080p, which is noticeably slower on the Steam Deck and "
            "older GPUs. Off: everything renders at the preset resolution; changes need a "
            "restart. Auto turns it on for powerful discrete GPUs.");
    static const char* live_modes[] = {"Auto", "Off", "On"};
    static const char* live_tips[] = {"On for powerful discrete GPUs, otherwise off",
                                      "Faster: changes to resolution need a restart",
                                      "Change resolution and preset without restarting"};
    if (const int picked = Segmented("live", live_modes, 3, s.live_resolution + 1, nullptr,
                                     live_tips);
        picked >= 0) {
        Store(s.live_resolution, picked - 1, true);
    }
    EndCard();

    BeginCard("Shader compilation");
    Label("New shaders");
    InfoTip("Shaders the game has not used on this Mac before compile in the background. "
            "Wait: a draw waits for its shader (a short hitch, the image is always complete). "
            "Skip: the draw is left out until its shader is ready (no hitch, an object can be "
            "missing for a frame or two). Balanced: waits up to a few milliseconds per frame, "
            "then skips (short hitches only). Shaders seen before load at start either way.");
    static const char* compile_modes[] = {"Wait", "Balanced", "Skip"};
    static const char* compile_tips[] = {"Brief hitch, complete image",
                                         "Waits a few ms per frame, then skips",
                                         "No hitch, objects may pop in"};
    // Menu order differs from the stored values (Wait, Skip, Balanced).
    static constexpr int menu_to_mode[] = {BbSettings::ShaderWait, BbSettings::ShaderBalanced,
                                           BbSettings::ShaderSkip};
    int current = 0;
    for (int m = 0; m < 3; ++m) {
        current = menu_to_mode[m] == s.shader_compile ? m : current;
    }
    if (const int picked = Segmented("compile", compile_modes, 3, current, nullptr, compile_tips);
        picked >= 0) {
        Store(s.shader_compile, menu_to_mode[picked], true);
    }
    EndCard();

    bool pending = false;
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        pending |= IsStartupOption(e) && s.effects[e] != s.startup_effects[e];
    }
    BeginCard("Startup and debugging", int(pending));
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        if (IsStartupOption(e)) {
            Toggle(BbSettings::Effects[e].label, s.effects[e],
                   s.effects[e] != s.startup_effects[e] ? 1 : -1);
        }
    }
    HintBelow("Free camera: hold Cross and press L3 (keyboard: Space + Z). Debug menu: left "
              "touchpad / Tab; needs DbgFont14h.ccm and DbgFont14h.tpf in dvdroot_ps4/font from "
              "Nexus mod #253. Right touchpad: Backspace.");
    EndCard();
}

void Menu() {
    auto& s = BbSettings::Get();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float margin = 16.0f * base_scale;
    const float width = std::min(kWindowWidth * base_scale, viewport->WorkSize.x - margin * 2.0f);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + std::max(viewport->WorkSize.y * 0.05f,
                                                                  margin)),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always); // height fits the content
    bool keep_open = true;
    if (!ImGui::Begin("Bloodborne Settings", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                          ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::End();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 win_pos = ImGui::GetWindowPos();
    const float content_right = win_pos.x + ImGui::GetWindowWidth() - style.WindowPadding.x;

    // Header: title, shortcut key-caps, close button.
    {
        ImGui::PushFont(nullptr, style.FontSizeBase * 1.4f);
        const ImVec2 title_pos = ImGui::GetCursorScreenPos();
        ImGui::TextUnformatted("Bloodborne");
        // Semibold by double-striking: the system font is loaded at its regular weight.
        dl->AddText(title_pos + ImVec2(std::max(1.0f, std::round(base_scale * 0.8f)), 0.0f),
                    ImGui::GetColorU32(ImGuiCol_Text), "Bloodborne");
        ImGui::SameLine(0.0f, 9.0f * base_scale);
        ImGui::PushStyleColor(ImGuiCol_Text, kSecondary);
        ImGui::TextUnformatted("Settings");
        ImGui::PopStyleColor();
        const float title_h = ImGui::GetTextLineHeight();
        ImGui::PopFont();

        const float close_d = title_h * 0.95f;
        const ImVec2 close_pos(content_right - close_d, title_pos.y + (title_h - close_d) * 0.5f);
        ImGui::SetCursorScreenPos(close_pos);
        if (ImGui::InvisibleButton("##close", ImVec2(close_d, close_d),
                                   ImGuiButtonFlags_EnableNav)) {
            keep_open = false;
        }
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        ImGui::SetItemTooltip("Close (Esc)");
        const ImVec2 c = close_pos + ImVec2(close_d, close_d) * 0.5f;
        dl->AddCircleFilled(c, close_d * 0.5f, ImGui::GetColorU32(hot ? kControlHover : kControl));
        const float k = close_d * 0.18f;
        const ImU32 x_col = ImGui::GetColorU32(hot ? kText : kSecondary);
        dl->AddLine(c - ImVec2(k, k), c + ImVec2(k, k), x_col, 1.6f * base_scale);
        dl->AddLine(c + ImVec2(-k, k), c + ImVec2(k, -k), x_col, 1.6f * base_scale);

        const char* keys[] = {BB_MENU_KEYS_UI, "L3+R3"};
        float x = close_pos.x - 12.0f * base_scale;
        for (int i = 1; i >= 0; --i) {
            const ImVec2 size = Keycap(dl, ImVec2(), keys[i], true);
            x -= size.x;
            Keycap(dl, ImVec2(x, title_pos.y + (title_h - size.y) * 0.5f), keys[i]);
            x -= 5.0f * base_scale;
        }
        ImGui::SetCursorScreenPos(ImVec2(title_pos.x, title_pos.y + title_h));
        ImGui::Dummy(ImVec2(0.0f, 6.0f * base_scale));
    }

    // Tabs: a centred segmented control. Gamepad L1/R1 switch tabs as in console menus.
    {
        static const char* tabs[TabCount] = {"Display", "Graphics", "Advanced"};
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            !ImGui::IsAnyItemActive()) {
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) {
                current_tab = (current_tab + TabCount - 1) % TabCount;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) {
                current_tab = (current_tab + 1) % TabCount;
            }
        }
        float widest = 0.0f;
        for (const char* t : tabs) {
            widest = std::max(widest, ImGui::CalcTextSize(t).x);
        }
        const float tabs_w = (widest + 36.0f * base_scale) * float(TabCount);
        const float x0 = win_pos.x + (ImGui::GetWindowWidth() - tabs_w) * 0.5f;
        const ImVec2 row = ImGui::GetCursorScreenPos();
        if (gamepad_in_use) {
            const float h = ImGui::GetFrameHeight() + 2.0f * base_scale;
            const ImVec2 l1 = Keycap(dl, ImVec2(), "L1", true);
            Keycap(dl, ImVec2(x0 - l1.x - 10.0f * base_scale, row.y + (h - l1.y) * 0.5f), "L1");
            Keycap(dl, ImVec2(x0 + tabs_w + 10.0f * base_scale, row.y + (h - l1.y) * 0.5f), "R1");
        }
        ImGui::SetCursorScreenPos(ImVec2(x0, row.y));
        if (const int picked = Segmented("tabs", tabs, TabCount, current_tab, nullptr, nullptr,
                                         false, tabs_w);
            picked >= 0) {
            current_tab = picked;
        }
        ImGui::SetCursorScreenPos(ImVec2(row.x, ImGui::GetCursorScreenPos().y));
        ImGui::Dummy(ImVec2(0.0f, 4.0f * base_scale));
    }

    // Content: scrolls when taller than the screen allows; header and footer stay put.
    {
        const float footer_h = ImGui::GetFrameHeight() + style.ItemSpacing.y * 3.0f +
                               style.WindowPadding.y + 8.0f * base_scale;
        const float max_h = std::max(viewport->WorkPos.y + viewport->WorkSize.y - margin -
                                         ImGui::GetCursorScreenPos().y - footer_h,
                                     160.0f * base_scale);
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, max_h));
        ImGui::BeginChild("##content", ImVec2(0.0f, 0.0f),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened);
        switch (current_tab) {
        case TabDisplay: DisplayTab(s); break;
        case TabGraphics: GraphicsTab(s); break;
        default: AdvancedTab(s); break;
        }
        ImGui::EndChild();
    }

    // Footer: status on the left, actions on the right.
    {
        bool restart = s.object_motion != s.startup_object_motion ||
                       s.model_lod != s.startup_model_lod ||
                       s.live_resolution != s.startup_live_resolution ||
                       BbSettings::ResolutionNeedsRestart();
        for (int e = 0; e < BbSettings::EffectCount; ++e) {
            restart |= s.effects[e] != s.startup_effects[e];
        }
        ImGui::Dummy(ImVec2(0.0f, 2.0f * base_scale));
        const float y = ImGui::GetCursorScreenPos().y;
        dl->AddLine(ImVec2(win_pos.x, y), ImVec2(win_pos.x + ImGui::GetWindowWidth(), y),
                    ImGui::GetColorU32(kHairline), 1.0f);
        ImGui::Dummy(ImVec2(0.0f, 6.0f * base_scale));

        ImGui::AlignTextToFramePadding();
        const bool capture_status =
            capture_base != ~u64{0} &&
            std::chrono::steady_clock::now() - capture_time < std::chrono::seconds(15);
        if (capture_status) {
            ImGui::PushStyleColor(ImGuiCol_Text, kSecondary);
            if (Vulkan::FrameCapture::Written() > capture_base) {
                ImGui::TextUnformatted(
                    "Frame saved to ~/Library/Logs/Bloodborne/captures: take a screenshot too");
            } else {
                ImGui::TextUnformatted("Capturing the next frame...");
            }
            ImGui::PopStyleColor();
        } else if (restart) {
            const float line = ImGui::GetTextLineHeight();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float r = 4.0f * base_scale;
            dl->AddCircleFilled(ImVec2(p.x + r, p.y + style.FramePadding.y + line * 0.5f), r,
                                ImGui::GetColorU32(kOrange));
            ImGui::Indent(r * 2.0f + 8.0f * base_scale);
            ImGui::TextUnformatted("Some changes apply after a restart");
            ImGui::Unindent(r * 2.0f + 8.0f * base_scale);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, kSecondary);
            ImGui::Text("%.0f FPS  (%.1f ms)   Saved to bbport.ini",
                        frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg);
            ImGui::PopStyleColor();
        }
        const char* apply = "Apply and Restart Game";
        const char* close = "Close";
        // Records the frame's passes for a bug report (vk_frame_capture.h).
        const char* capture = "Capture Frame";
        float buttons = ButtonWidth(close) + ButtonWidth(capture) + style.ItemSpacing.x;
        if (restart) {
            buttons += ButtonWidth(apply) + style.ItemSpacing.x;
        }
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(
            ImVec2(content_right - buttons, ImGui::GetCursorScreenPos().y));
        if (ImGui::Button(capture)) {
            capture_base = Vulkan::FrameCapture::Written();
            capture_time = std::chrono::steady_clock::now();
            Vulkan::FrameCapture::Arm();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Records what the GPU draws in the next frame, for a bug report");
        }
        ImGui::SameLine();
        if (ImGui::Button(close)) {
            keep_open = false;
        }
        if (restart) {
            ImGui::SameLine();
            if (PrimaryButton(apply)) {
                BbSettings::Save();
                runtime_restart();
            }
        }
    }
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

void FpsCounter() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 14.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                                   viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.62f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * base_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(11.0f, 7.0f) * base_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 1.0f) * base_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    const auto& s = BbSettings::Get();
    ImGui::Text("%.0f FPS", frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f);
    ImGui::SameLine();
    ImGui::TextColored(kSecondary, "%.1f ms", frame_ms_avg);
    // Scene (render) size as reported by the upscaler, and the output size in effect: a
    // startup-patched session keeps its launch output until restart.
    const char* upscaler = s.upscaler == BbSettings::UpscalerFsr3      ? "FSR 3.1"
                           : s.upscaler == BbSettings::UpscalerTaa     ? "TAA"
                           : s.upscaler == BbSettings::UpscalerMetalFx ? "MetalFX"
                                                                       : "";
    const int output = BbSettings::FixedRenderSession() ? s.startup_output_res : s.output_res.load();
    const int out_w = BbSettings::OutputWidths[output], out_h = BbSettings::OutputHeights[output];
    const int render_w = s.active_render_width, render_h = s.active_render_height;
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.82f);
    if (render_w == out_w && render_h == out_h) {
        ImGui::TextColored(kSecondary, "%dx%d  %s", out_w, out_h, upscaler);
    } else {
        ImGui::TextColored(kSecondary, "%dx%d \xE2\x86\x92 %dx%d  %s", render_w, render_h, out_w,
                           out_h, upscaler);
    }
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar(4);
}

/// The game's text dialog: prompt, the text typed so far and the keys, centred over the frame.
void TextPrompt() {
    std::string title, text;
    {
        std::scoped_lock lock{prompt_mutex};
        title = prompt_title;
        text = prompt_text;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f * base_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f, 16.0f) * base_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f) * base_scale);
    ImGui::Begin("##textprompt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted(title.c_str());
    ImGui::Separator();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.25f);
    ImGui::Text("%s_", text.c_str());
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::TextColored(kSecondary, "Type on the keyboard \xC2\xB7 Backspace deletes \xC2\xB7 "
                                   "Enter confirms \xC2\xB7 Esc cancels");
    ImGui::End();
    ImGui::PopStyleVar(3);
}

/// Colours and metrics at 1080p (Render scales the sizes to the display).
void SetupStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    style.FontSizeBase = kBaseFontSize;
    style.Alpha = 1.0f;
    style.DisabledAlpha = 0.32f; // clearly dimmed, not just slightly grey
    style.WindowPadding = ImVec2(20.0f, 16.0f);
    style.WindowRounding = 12.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildRounding = 10.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupRounding = 8.0f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = ImVec2(10.0f, 5.0f);
    style.FrameRounding = 7.0f;
    style.FrameBorderSize = 0.0f;
    style.ItemSpacing = ImVec2(10.0f, 9.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    style.IndentSpacing = 18.0f;
    style.ScrollbarSize = 9.0f;
    style.ScrollbarRounding = 12.0f;
    style.ScrollbarPadding = 2.0f;
    style.GrabMinSize = 18.0f;
    style.GrabRounding = 12.0f;
    style.TabRounding = 7.0f;
    style.SelectableRounding = 5.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.SelectableTextAlign = ImVec2(0.0f, 0.5f);

    ImGui::StyleColorsDark(&style); // anything not set below keeps a sane default
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kSecondary;
    c[ImGuiCol_WindowBg] = kWindowBg;
    c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.173f, 0.173f, 0.180f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(1.0f, 1.0f, 1.0f, 0.10f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg] = kTrack;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.16f, 0.17f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.19f, 0.19f, 0.20f, 1.0f);
    c[ImGuiCol_TitleBg] = kWindowBg;
    c[ImGuiCol_TitleBgActive] = kWindowBg;
    c[ImGuiCol_TitleBgCollapsed] = kWindowBg;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1.0f, 1.0f, 1.0f, 0.20f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.32f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.42f);
    c[ImGuiCol_CheckMark] = kWhite;
    c[ImGuiCol_CheckboxSelectedBg] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccentHover;
    c[ImGuiCol_Button] = kControl;
    c[ImGuiCol_ButtonHovered] = kControlHover;
    c[ImGuiCol_ButtonActive] = kControlActive;
    c[ImGuiCol_Header] = WithAlpha(kAccent, 0.45f);
    c[ImGuiCol_HeaderHovered] = kAccent;
    c[ImGuiCol_HeaderActive] = kAccentActive;
    c[ImGuiCol_Separator] = kHairline;
    c[ImGuiCol_SeparatorHovered] = kHairline;
    c[ImGuiCol_SeparatorActive] = kHairline;
    c[ImGuiCol_ResizeGrip] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ResizeGripActive] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_InputTextCursor] = kAccent;
    c[ImGuiCol_Tab] = kTrack;
    c[ImGuiCol_TabHovered] = kControl;
    c[ImGuiCol_TabSelected] = kControlHover;
    c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_TabDimmed] = kTrack;
    c[ImGuiCol_TabDimmedSelected] = kControl;
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_TextSelectedBg] = WithAlpha(kAccent, 0.40f);
    c[ImGuiCol_NavCursor] = kAccentHover; // the keyboard focus ring
    c[ImGuiCol_NavWindowingHighlight] = kAccent;
}

/// macOS: Apple's system font (SF Pro) read from the user's own system at run time -- never
/// shipped with the port. DejaVu (embedded) fills glyphs it lacks, e.g. the Cmd sign.
void SetupFonts(ImGuiIO& io) {
    ImFont* font = nullptr;
#ifdef __APPLE__
    for (const char* path : {"/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/SFNSText.ttf",
                             "/Library/Fonts/SF-Pro-Text-Regular.otf"}) {
        if (FILE* probe = std::fopen(path, "rb")) {
            std::fclose(probe);
            ImFontConfig config;
            config.Flags |= ImFontFlags_NoLoadError;
            std::snprintf(config.Name, sizeof(config.Name), "System");
            font = io.Fonts->AddFontFromFileTTF(path, kBaseFontSize, &config);
            if (font) {
                std::printf("Overlay: font %s\n", path);
                break;
            }
        }
    }
#endif
    ImFontConfig fallback;
    fallback.FontDataOwnedByAtlas = false;
    fallback.MergeMode = font != nullptr; // only the glyphs the system font lacks
    std::snprintf(fallback.Name, sizeof(fallback.Name), "DejaVu Sans");
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), kBaseFontSize, &fallback);
}

/// UI scale follows the display height (1080p = 1).
void ApplyScale(float display_height) {
    const float scale = std::max(display_height / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }
}

} // namespace

void Init(Metal::Device& device, u32 pixel_format) {
    if (const char* env = std::getenv("BB_OVERLAY")) {
        if (!std::strcmp(env, "0") || !std::strcmp(env, "off") || !std::strcmp(env, "false")) {
            std::printf("Overlay: disabled (BB_OVERLAY=0)\n");
            return;
        }
    }
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "bbport";

    SetupStyle();
    SetupFonts(io);

    if (!MetalBackend::Init(device, pixel_format)) {
        std::printf("Overlay: ImGui Metal backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    std::printf("Overlay: menu ready (" BBPORT_MENU_KEYS " or L3+R3)\n");
    // BB_MENU_OPEN=1..3: start with the menu open on that tab (automated menu screenshots).
    if (const char* open = std::getenv("BB_MENU_OPEN"); open && open[0] >= '1' && open[0] <= '3') {
        current_tab = open[0] - '1';
        SetOpen(true);
    }
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (!down && event.key.scancode == held_scancode) {
            held_scancode = SDL_SCANCODE_UNKNOWN;
            return true;
        }
        const bool toggle = IsMenuToggle(event.key);
        if (toggle && event.key.repeat) {
            return true; // auto-repeat of the shortcut: never reaches the game
        }
        if (down && (toggle || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(toggle ? !is_open : false);
            held_scancode = event.key.scancode;
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (down) {
            gamepad_in_use = false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        // Cmd: ImGui's macOS behaviours treat it as the shortcut modifier (Cmd+click, Cmd+A).
        io.AddKeyEvent(ImGuiMod_Super, (event.key.mod & SDL_KMOD_GUI) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (down && l3_down && r3_down) {
            SetOpen(!is_open);
            held_sticks = true;
            return true;
        }
        if (held_sticks && !l3_down && !r3_down) {
            held_sticks = false;
        }
        if (held_sticks && (button == SDL_GAMEPAD_BUTTON_LEFT_STICK ||
                            button == SDL_GAMEPAD_BUTTON_RIGHT_STICK)) {
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (down) {
            gamepad_in_use = true;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_REMOVED:
        // No button-up events follow a disconnect: forget held sticks (the game sees it too).
        l3_down = r3_down = false;
        held_sticks = false;
        return false;
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            gamepad_in_use = false;
            // The click's own position first: motion events alone may leave ImGui's cursor
            // stale (e.g. no movement since the menu opened).
            const float density = PixelDensity(event.button.windowID);
            io.AddMousePosEvent(event.button.x * density, event.button.y * density);
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            if (menu_trace) {
                std::printf("menu: mouse %d %s at %.0f,%.0f (window %u, display %.0fx%.0f, "
                            "density %.2f)\n",
                            button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? "down" : "up",
                            event.button.x, event.button.y, unsigned(event.button.windowID),
                            io.DisplaySize.x, io.DisplaySize.y,
                            PixelDensity(event.button.windowID));
            }
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

bool Visible() {
    return initialized && (menu_open || prompt_active || BbSettings::Get().show_fps);
}

void SetTextPrompt(bool active, const std::string& prompt, const std::string& text) {
    {
        std::scoped_lock lock{prompt_mutex};
        prompt_title = prompt;
        prompt_text = text;
    }
    prompt_active = active;
}

bool CapturesInput() {
    // The text dialog too: keys typed into it (Backspace is the touchpad) stay out of the game.
    if (menu_open || prompt_active) {
        return true;
    }
    // Shortcut still held after the menu closed. Checked against the live keyboard state too,
    // so a key-up lost to a focus change cannot hold the game's input forever.
    const int held = held_scancode;
    if (held != SDL_SCANCODE_UNKNOWN) {
        const bool* keys = SDL_GetKeyboardState(nullptr);
        if (keys && keys[held]) {
            return true;
        }
        held_scancode = SDL_SCANCODE_UNKNOWN;
    }
    return held_sticks;
}

namespace {

/// BB_MENU_INPUT=<file>: scripted menu input for automated runs (the window gets no keyboard or
/// mouse events while the screen is locked). Checked about twice a second; the file is read,
/// removed and each whitespace-separated token fed to HandleEvent as the window thread would:
/// cmd-comma, esc, tab, space, enter, up/down/left/right, cmd-hold/cmd-release, text:<chars>,
/// click:<x>,<y> (drawable pixels). Write the file elsewhere and mv it into place: a file read
/// while it is still being written loses the tokens not yet written.
void ScriptedInput() {
    static const char* path = std::getenv("BB_MENU_INPUT");
    static int presents = 0;
    if (!path || !path[0] || ++presents % 30 != 0) {
        return;
    }
    FILE* file = std::fopen(path, "r");
    if (!file) {
        return;
    }
    char token[256];
    std::vector<std::string> tokens;
    while (std::fscanf(file, "%255s", token) == 1) {
        tokens.emplace_back(token);
    }
    std::fclose(file);
    std::remove(path);
    const auto key = [](SDL_Keycode code, SDL_Keymod mod, SDL_Scancode scancode) {
        for (const bool down : {true, false}) {
            SDL_Event event{};
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.key = code;
            event.key.mod = mod;
            event.key.scancode = scancode;
            event.key.down = down;
            HandleEvent(event);
        }
    };
    for (const std::string& t : tokens) {
        std::printf("menu: scripted %s\n", t.c_str());
        if (t == "cmd-comma") {
            key(SDLK_COMMA, SDL_KMOD_GUI, SDL_SCANCODE_COMMA);
        } else if (t == "esc") {
            key(SDLK_ESCAPE, SDL_KMOD_NONE, SDL_SCANCODE_ESCAPE);
        } else if (t == "tab") {
            key(SDLK_TAB, SDL_KMOD_NONE, SDL_SCANCODE_TAB);
        } else if (t == "space") {
            key(SDLK_SPACE, SDL_KMOD_NONE, SDL_SCANCODE_SPACE);
        } else if (t == "enter") {
            key(SDLK_RETURN, SDL_KMOD_NONE, SDL_SCANCODE_RETURN);
        } else if (t == "up" || t == "down" || t == "left" || t == "right") {
            key(t == "up" ? SDLK_UP : t == "down" ? SDLK_DOWN : t == "left" ? SDLK_LEFT : SDLK_RIGHT,
                SDL_KMOD_NONE,
                t == "up" ? SDL_SCANCODE_UP : t == "down" ? SDL_SCANCODE_DOWN
                          : t == "left" ? SDL_SCANCODE_LEFT : SDL_SCANCODE_RIGHT);
        } else if (t == "cmd-hold" || t == "cmd-release") {
            // Cmd held across the next tokens (Cmd+click on a slider types a value).
            SDL_Event event{};
            event.type = t == "cmd-hold" ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.key = SDLK_LGUI;
            event.key.scancode = SDL_SCANCODE_LGUI;
            event.key.mod = t == "cmd-hold" ? SDL_KMOD_LGUI : SDL_KMOD_NONE;
            event.key.down = t == "cmd-hold";
            HandleEvent(event);
        } else if (t.rfind("text:", 0) == 0) {
            SDL_Event event{};
            event.type = SDL_EVENT_TEXT_INPUT;
            const std::string chars = t.substr(5);
            event.text.text = chars.c_str();
            HandleEvent(event);
        } else if (float x = 0, y = 0; std::sscanf(t.c_str(), "click:%f,%f", &x, &y) == 2) {
            // Window id 0: no window, density 1, so the coordinates are drawable pixels.
            SDL_Event event{};
            event.type = SDL_EVENT_MOUSE_MOTION;
            event.motion.x = x;
            event.motion.y = y;
            HandleEvent(event);
            for (const bool down : {true, false}) {
                event = {};
                event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
                event.button.button = SDL_BUTTON_LEFT;
                event.button.down = down;
                event.button.x = x;
                event.button.y = y;
                HandleEvent(event);
            }
        }
    }
}

} // namespace

void Render(void* command_buffer, void* target, u32 width, u32 height) {
    ScriptedInput();
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    if (!initialized) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    // Save 0.3 s after the last change (a slider drag writes once), not only when the menu
    // closes: closing the window or a crash with the menu open used to lose the changes. Under
    // the lock: the window thread's SetOpen(false) saves and clears `dirty` too.
    if (dirty && now - last_change > std::chrono::milliseconds(300)) {
        dirty = false;
        BbSettings::Save();
    }
    if (!Visible() || !command_buffer || !target || width == 0 || height == 0) {
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(width), float(height));
    if (menu_trace) {
        // Changes to these not made by a widget in the previous menu frame come from elsewhere.
        // Sharpness in hundredths (typed values: Cmd+click on the slider).
        static int last[5] = {-1, -1, -1, -1, -1};
        const auto& s = BbSettings::Get();
        const int now_values[5] = {int(s.jitter.load()), s.upscaler.load(), s.preset.load(),
                                   s.requested_upscaler.load(),
                                   int(std::lround(s.sharpness.load() * 100.0f))};
        static const char* names[5] = {"jitter", "upscaler", "preset", "requested upscaler",
                                       "sharpness x100"};
        for (int i = 0; i < 5; ++i) {
            if (last[i] != -1 && last[i] != now_values[i]) {
                std::printf("menu: %s %d -> %d (%s)\n", names[i], last[i], now_values[i],
                            widget_changed_this_frame ? "by a widget" : "NOT by a widget");
            }
            last[i] = now_values[i];
        }
        widget_changed_this_frame = false;
    }
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    ApplyScale(float(height));

    MetalBackend::Frame(command_buffer, target, [&] {
        ImGui::NewFrame();
        if (menu_open) {
            Menu();
        }
        if (BbSettings::Get().show_fps && !menu_open) {
            FpsCounter();
        }
        if (prompt_active && !menu_open) {
            TextPrompt();
        }
        if (menu_trace && (io.MouseClicked[0] || io.MouseReleased[0])) {
            std::printf("menu: frame sees %s at %.0f,%.0f, over a control: %s, control "
                        "active: %s\n",
                        io.MouseClicked[0] ? "press" : "release", io.MousePos.x, io.MousePos.y,
                        ImGui::IsAnyItemHovered() ? "yes" : "no",
                        ImGui::IsAnyItemActive() ? "yes" : "no");
        }
        ImGui::Render();
    });
}

} // namespace BbOverlay
