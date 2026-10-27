// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_mouse.h"

#include <array>
#include <cmath>
#include "bbport_overlay.h"
#include "bbport_settings.h"
#include "bbport_split.h"

namespace BbMouse {
namespace {

// The speed is the motion of the last SpeedWindow, so it does not depend on how often the game
// samples its pad, and falls to zero once the mouse stops.
constexpr u64 SpeedWindow = 33'000'000; // ns

struct Motion {
    u64 ns;
    float dx, dy;
};
std::array<Motion, 256> motions{};
size_t next_motion = 0;
bool captured = false;
u32 buttons = 0;
float wheel = 0.0f; // scrolled since the last whole step (trackpads scroll in fractions)

void Publish(BbGpuShared* s, float vx, float vy) {
    __atomic_store_n(&s->mouse_vx, s32(std::lround(vx)), __ATOMIC_RELAXED);
    __atomic_store_n(&s->mouse_vy, s32(std::lround(vy)), __ATOMIC_RELAXED);
    __atomic_store_n(&s->mouse_buttons, buttons, __ATOMIC_RELAXED);
    __atomic_store_n(&s->mouse_captured, captured ? 1u : 0u, __ATOMIC_RELEASE);
}

void SetCaptured(SDL_Window* window, BbGpuShared* s, bool on) {
    if (captured == on) {
        return;
    }
    SDL_SetWindowRelativeMouseMode(window, on);
    captured = on;
    buttons = 0;
    wheel = 0.0f;
    motions.fill({});
    Publish(s, 0.0f, 0.0f);
}

} // namespace

bool HandleEvent(SDL_Window* window, const SDL_Event& event) {
    BbGpuShared* s = BbSplit::Host() ? BbSplit::Shared() : nullptr;
    if (!s) {
        return false;
    }
    switch (event.type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (!captured) {
            // The click that captures the cursor is not also an attack.
            if (event.button.button == SDL_BUTTON_LEFT && BbSettings::Get().mouse_look &&
                !BbOverlay::CapturesInput()) {
                SetCaptured(window, s, true);
                return true;
            }
            return false;
        }
        buttons |= SDL_BUTTON_MASK(event.button.button);
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!captured) {
            return false;
        }
        buttons &= ~SDL_BUTTON_MASK(event.button.button);
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        if (!captured) {
            return false;
        }
        motions[next_motion++ % motions.size()] = {event.motion.timestamp, event.motion.xrel,
                                                   event.motion.yrel};
        return true;
    case SDL_EVENT_MOUSE_WHEEL: {
        if (!captured) {
            return false;
        }
        // Whole steps in the direction the wheel turned, whatever "natural scrolling" says.
        wheel += event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
        for (; wheel >= 1.0f; wheel -= 1.0f) {
            __atomic_add_fetch(&s->mouse_wheel_up, 1u, __ATOMIC_RELAXED);
        }
        for (; wheel <= -1.0f; wheel += 1.0f) {
            __atomic_add_fetch(&s->mouse_wheel_down, 1u, __ATOMIC_RELAXED);
        }
        return true;
    }
    case SDL_EVENT_KEY_DOWN:
        // Cmd gives the cursor back (it also starts Cmd+, and Cmd+Tab); the key still reaches the
        // game's key state.
        if (captured && (event.key.key == SDLK_LGUI || event.key.key == SDLK_RGUI)) {
            SetCaptured(window, s, false);
        }
        return false;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        SetCaptured(window, s, false);
        return false;
    default:
        return false;
    }
}

void Update(SDL_Window* window) {
    BbGpuShared* s = BbSplit::Host() ? BbSplit::Shared() : nullptr;
    if (!s) {
        return;
    }
    const auto& settings = BbSettings::Get();
    if (captured && (!settings.mouse_look || BbOverlay::CapturesInput() ||
                     !(SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS))) {
        SetCaptured(window, s, false);
    }
    if (!captured) {
        return;
    }
    const u64 now = SDL_GetTicksNS();
    float dx = 0.0f, dy = 0.0f;
    for (const Motion& m : motions) {
        if (m.ns && m.ns + SpeedWindow >= now) {
            dx += m.dx;
            dy += m.dy;
        }
    }
    const float scale = settings.mouse_sensitivity * (1e9f / float(SpeedWindow));
    Publish(s, dx * scale,
            dy * scale * settings.mouse_vertical * (settings.mouse_invert_y ? -1.0f : 1.0f));
}

} // namespace BbMouse
