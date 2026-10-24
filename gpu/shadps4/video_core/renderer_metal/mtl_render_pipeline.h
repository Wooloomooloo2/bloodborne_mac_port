// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: render pipelines for the renderer's own hand-written MSL programs (Metal-native
// renderer): blit helper, scene resolution. Plain C++; objects cross as Metal::Object.

#pragma once

#include <string_view>
#include "common/types.h"
#include "video_core/renderer_metal/mtl_object.h"

namespace Metal {

class Device;

/// Compiles MSL source into an id<MTLLibrary>. Asserts when it does not compile (the source is
/// part of the renderer).
Object CompileLibrary(Device& device, std::string_view source, std::string_view name);

struct RenderPipelineDesc {
    void* library{};                 ///< id<MTLLibrary> (CompileLibrary)
    const char* vertex_function{};   ///< function names in the library
    const char* fragment_function{};
    u32 color_format{};   ///< MTLPixelFormat of colour attachment 0 (blending off); 0: none
    u32 depth_format{};   ///< MTLPixelFormat of the depth attachment; 0: none
    u32 stencil_format{}; ///< MTLPixelFormat of the stencil attachment; 0: none
    u32 samples{1};       ///< rasterSampleCount
};

/// An id<MTLRenderPipelineState>. Asserts when Metal refuses it.
Object CreateRenderPipeline(Device& device, const RenderPipelineDesc& desc,
                            std::string_view label);

} // namespace Metal
