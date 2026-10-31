// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: in-game settings menu (Dear ImGui), drawn by the presenter into the drawable after the
// game frame, at display resolution (imgui_impl_metal; the Metal glue is bbport_overlay_metal.mm).
// Insert (keyboard; Cmd+, on macOS) or L3+R3 (gamepad) opens it; while it is open the game gets
// no pad/keyboard input. Settings live in bbport_settings.h. Plain C++: Metal objects cross as
// void*.

#pragma once

#include <string>

#include "common/types.h"
#include "bbport_frame_rates.h"

union SDL_Event;
struct SDL_Window;

namespace Metal {
class Device;
}

namespace BbOverlay {

/// Once (the presenter's constructor): the ImGui context and its Metal backend for render
/// targets of `pixel_format` (MTLPixelFormat, the drawable's).
void Init(Metal::Device& device, u32 pixel_format);

/// Window thread, for every SDL event: true when the menu consumed it.
bool HandleEvent(const SDL_Event& event);
/// Turns SDL text input on while the menu edits a value (window thread, once per poll).
void UpdateTextInput(SDL_Window* window);

/// Whether anything is drawn this frame (menu open or FPS counter on).
bool Visible();

/// Present thread, once per real/generated pair: advances UI time and saves
/// settings changed in the menu a moment after the last change, also while nothing is drawn.
/// When Visible(), draws the menu or the FPS counter over `target` (id<MTLTexture>, a
/// `width` x `height` render target of Init's pixel format, contents kept) in a render pass on
/// `command_buffer` (id<MTLCommandBuffer>). Null `command_buffer`: a present without a drawable
/// (timing and saving only). Callers hold an autorelease pool.
/// `rates` measures new game frames and actual displayed drawables independently.
void Render(void* command_buffer, void* target, u32 width, u32 height,
            const BbFrameRates::Sample& rates = {});
/// Draws the same overlay over the second image of a generated/real pair. Call immediately
/// after Render on the present thread; no new UI frame, input processing or FPS sample.
void Replay(void* command_buffer, void* target);

/// The menu or the game's text dialog is open: the game's input is held neutral.
bool CapturesInput();

/// Window thread: the game's text dialog (ImeDialog) state, drawn as a box over the frame
/// (the title bar is not visible in fullscreen). `text` is what to show (already masked for a
/// password).
void SetTextPrompt(bool active, const std::string& prompt, const std::string& text);

} // namespace BbOverlay
