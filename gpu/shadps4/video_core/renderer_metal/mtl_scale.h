// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: scaled and format-converting texture copies, depth resample and depth clear as
// fullscreen-triangle render passes (Metal-native renderer). Metal blit encoders copy only between
// equal formats and sizes; Vulkan's blitImage (scaled, nearest or linear, colour or depth) and
// clearDepthStencilImage become these. Used by the scene targets (proxy fills and resolves) and
// the upscaler (UI background, UI depth, LDR output copies). Plain C++; textures cross as void*.

#pragma once

#include <cstddef>
#include <memory>
#include "common/types.h"

namespace Metal {

class Device;
class Encoder;

enum class ScaleFilter : u8 {
    Nearest, ///< the source texel nearest to the target pixel's centre; any colour format
    Linear,  ///< bilinear (clamp to edge); float-sampleable formats only
};

/// A destination rectangle in texels of the destination level; width 0: the whole level.
struct ScaleRect {
    u32 x{}, y{}, width{}, height{};
};

/// What ResampleDepth does with stencil.
enum class StencilCopy : u8 {
    None,   ///< depth only; a destination with stencil gets stencil 0
    Export, ///< stencil copied by the fragment shader's stencil export (Apple GPUs)
    PerBit, ///< stencil rebuilt one bit per draw by fixed-function replace (portable path)
};

/// Every method records one render pass on the Encoder (ending the open pass, if any) and leaves
/// the Encoder's render state changed: pipeline, depth-stencil state, viewport, scissor, cull,
/// depth bias, fill and clip mode (PerBit also the stencil reference), fragment texture 0/1,
/// sampler 0 and buffer 0. A caller that also draws with the rasterizer invalidates its dynamic
/// state afterwards. Pipelines are made at first use, per destination format (thread-safe), so
/// the methods may run inside Scheduler::Record closures; PrepareColor/PrepareDepth make them
/// ahead (a first use on the encoder thread would stall the frame). Textures are id<MTLTexture>, unretained;
/// views the helper makes are kept alive by the Encoder.
class ScaleHelper {
public:
    explicit ScaleHelper(Device& device);
    ~ScaleHelper();

    ScaleHelper(const ScaleHelper&) = delete;
    ScaleHelper& operator=(const ScaleHelper&) = delete;

    /// Copies the whole of `src` (level `src_level`, slice 0) into `rect` of `dst` (level
    /// `dst_level`, a render target), scaling with `filter` and converting the format (the
    /// shader reads src as float/uint/int by dst's format: both must share that numeric type).
    /// Pixels of dst outside `rect` keep their content. Linear on an integer format asserts.
    void ScaleColor(Encoder& encoder, void* src, void* dst, ScaleFilter filter,
                    ScaleRect rect = {}, u32 src_level = 0, u32 dst_level = 0);

    /// Nearest resample of the depth (level 0) of `src_depth` (a depth format) over the whole of
    /// `dst_depth` (level 0, a depth render target, any size). Export/PerBit need stencil in
    /// both textures, and src_depth made with PixelFormatView usage (its stencil is read
    /// through a stencil view).
    void ResampleDepth(Encoder& encoder, void* src_depth, void* dst_depth,
                       StencilCopy stencil = StencilCopy::None);

    /// Clears level 0 of `depth` (a depth or depth-stencil render target) to `value` and its
    /// stencil, if any, to `stencil`: Vulkan's clearDepthStencilImage.
    void ClearDepth(Encoder& encoder, void* depth, float value, u32 stencil = 0);

    /// Makes the pipeline ScaleColor(..., `filter`) into a `dst_format` (MTLPixelFormat) target
    /// uses, now instead of at the first call.
    void PrepareColor(u32 dst_format, ScaleFilter filter);
    /// The same for ResampleDepth(..., `stencil`) into a `dst_format` depth target.
    void PrepareDepth(u32 dst_format, StencilCopy stencil = StencilCopy::None);
    /// Pipelines made so far (tests).
    [[nodiscard]] size_t PipelineCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Metal
