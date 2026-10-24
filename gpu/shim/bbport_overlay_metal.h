// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the settings overlay's Metal side (imgui_impl_metal), used by bbport_overlay.cpp only.
// The shape follows Huax-1839/bloodborne-macos's renderer_metal/mtl_overlay.mm
// (GPL-2.0-or-later). Plain C++: Metal objects cross as void*. Both run with the ImGui context
// current, under the overlay's lock.

#pragma once

#include <functional>
#include "common/types.h"

namespace Metal {
class Device;
}

namespace BbOverlay::MetalBackend {

/// ImGui_ImplMetal_Init for `device`; targets are expected in `pixel_format` (MTLPixelFormat).
bool Init(Metal::Device& device, u32 pixel_format);
// Uses the user's unencrypted trophy PNG when present. False: draw the grade cup instead.
bool TrophyImage(int id, float size);

/// One ImGui frame drawn over `target` (id<MTLTexture>, contents loaded and stored) on
/// `command_buffer` (id<MTLCommandBuffer>): ImGui_ImplMetal_NewFrame for the pass, then `build`
/// (ImGui::NewFrame .. ImGui::Render), then the draw data in a render encoder of its own. Font
/// atlas uploads happen in there (the backend has RendererHasTextures).
void Frame(void* command_buffer, void* target, const std::function<void()>& build);

} // namespace BbOverlay::MetalBackend
