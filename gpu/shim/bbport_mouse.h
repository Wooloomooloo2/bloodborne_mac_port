// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <SDL3/SDL.h>

/// Mouse look in the GPU host's window (split mode): when it is on (mouse_look, off by default),
/// a click captures the cursor; its speed and buttons go to the game's pad emulation through
/// the shared block (runtime_pad.c).
namespace BbMouse {

/// Window thread, for an event the settings overlay passed on. True if it was consumed.
bool HandleEvent(SDL_Window* window, const SDL_Event& event);

/// Window thread, after each event pump: releases the cursor when it must, publishes the speed.
void Update(SDL_Window* window);

} // namespace BbMouse
