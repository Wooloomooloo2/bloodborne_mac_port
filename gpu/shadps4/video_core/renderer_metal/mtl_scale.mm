// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: scaled colour copies, depth resample and depth clear as render passes (Metal-native
// renderer).

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <fmt/format.h>
#include "common/assert.h"
#include "video_core/renderer_metal/mtl_blit_shaders.h"
#include "video_core/renderer_metal/mtl_depth_stencil.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_render_pipeline.h"
#include "video_core/renderer_metal/mtl_scale.h"
#include "video_core/renderer_metal/mtl_texture.h"

namespace Metal {

namespace {

constexpr std::array<u8, 4> IdentitySwizzle{MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
                                            MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha};

constexpr u8 StencilOpReplace = 2; // VK_STENCIL_OP_REPLACE
constexpr u8 CompareAlways = 7;    // VK_COMPARE_OP_ALWAYS

/// Depth always passes and is written; with `stencil`, the reference (or the exported value)
/// replaces the stencil bits of `write_mask`.
DepthStencilKey ResampleState(bool stencil, u8 write_mask) {
    DepthStencilKey key{};
    key.depth_test = true;
    key.depth_write = true;
    key.depth_compare = CompareAlways;
    key.stencil_test = stencil;
    if (stencil) {
        const StencilFace face{.fail = StencilOpReplace,
                               .pass = StencilOpReplace,
                               .depth_fail = StencilOpReplace,
                               .compare = CompareAlways,
                               .read_mask = 0xff,
                               .write_mask = write_mask};
        key.front = face;
        key.back = face;
    }
    return key;
}

bool IsSintPixelFormat(u32 pixel_format) {
    switch (MTLPixelFormat(pixel_format)) {
    case MTLPixelFormatR8Sint: case MTLPixelFormatR16Sint: case MTLPixelFormatRG8Sint:
    case MTLPixelFormatR32Sint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRGBA8Sint:
    case MTLPixelFormatRG32Sint: case MTLPixelFormatRGBA16Sint: case MTLPixelFormatRGBA32Sint:
        return true;
    default:
        return false;
    }
}

/// Which flavour of scale_nearest writes the format: 0 float, 1 uint, 2 int.
u32 NumericFlavour(u32 pixel_format) {
    if (IsSintPixelFormat(pixel_format)) {
        return 2;
    }
    return IsIntegerPixelFormat(pixel_format) ? 1 : 0;
}

/// Fullscreen-triangle render state over `rect`: what the rasterizer may have left is reset.
void SetFullscreenState(Encoder& e, void* pipeline, const ScaleRect& rect) {
    e.SetRenderPipeline(pipeline, Access::None); // textures only
    e.SetCullMode(CullMode::None);
    e.SetDepthBias(0.0f, 0.0f, 0.0f);
    e.SetFillMode(FillMode::Fill);
    e.SetDepthClip(DepthClip::Clip);
    e.SetViewport({float(rect.x), float(rect.y), float(rect.width), float(rect.height), 0.0f,
                   1.0f});
    e.SetScissor({rect.x, rect.y, rect.width, rect.height});
}

struct PipelineKey {
    const char* fragment{};
    u32 color_format{}, depth_format{}, stencil_format{};
    bool operator==(const PipelineKey&) const = default;
};

PipelineKey ColorKey(u32 format, ScaleFilter filter) {
    const u32 flavour = NumericFlavour(format);
    ASSERT_MSG(filter == ScaleFilter::Nearest || flavour == 0,
               "Scale helper: linear copy into integer format {}", format);
    return {.fragment = filter == ScaleFilter::Linear ? BlitShaders::Present
                                                      : BlitShaders::ScaleNearest[flavour],
            .color_format = format};
}

PipelineKey DepthKey(u32 format, StencilCopy stencil) {
    const bool dst_stencil = StencilViewPixelFormat(format) != 0;
    return {.fragment = stencil == StencilCopy::None     ? BlitShaders::DepthResample
                        : stencil == StencilCopy::Export ? BlitShaders::DepthResampleStencil
                                                         : BlitShaders::DepthResampleStencilBits,
            .depth_format = format,
            .stencil_format = dst_stencil ? format : 0};
}

} // namespace

struct ScaleHelper::Impl {
    explicit Impl(Device& d) : device{d}, states{d} {}

    void* Pipeline(const PipelineKey& key) {
        std::scoped_lock lock{mutex};
        for (const auto& [k, pipeline] : pipelines) {
            if (k == key) {
                return pipeline.Get();
            }
        }
        const RenderPipelineDesc desc{
            .library = library.Get(),
            .vertex_function = BlitShaders::FullscreenTriangle,
            .fragment_function = key.fragment,
            .color_format = key.color_format,
            .depth_format = key.depth_format,
            .stencil_format = key.stencil_format,
        };
        const std::string label = fmt::format("scale {} {}/{}/{}", key.fragment, key.color_format,
                                              key.depth_format, key.stencil_format);
        pipelines.emplace_back(key, CreateRenderPipeline(device, desc, label));
        return pipelines.back().second.Get();
    }

    Device& device;
    DepthStencilCache states;
    Object library;
    Object linear_sampler; ///< linear, clamp to edge
    void* depth_write_state{};   ///< depth always/write, no stencil
    void* depth_stencil_state{}; ///< plus stencil replace (exported value), write mask 0xff
    std::array<void*, 9> stencil_bit_states{}; ///< write masks 0, 1, 2, 4, ..., 128
    std::mutex mutex;
    std::vector<std::pair<PipelineKey, Object>> pipelines;
};

ScaleHelper::ScaleHelper(Device& device) : impl{std::make_unique<Impl>(device)} {
    impl->library = CompileLibrary(device, BlitShaders::Source, "Scale helper");
    impl->linear_sampler =
        CreateSampler(device, SamplerDesc{.min_filter = MTLSamplerMinMagFilterLinear,
                                          .mag_filter = MTLSamplerMinMagFilterLinear,
                                          .mip_filter = MTLSamplerMipFilterNotMipmapped,
                                          .address_u = MTLSamplerAddressModeClampToEdge,
                                          .address_v = MTLSamplerAddressModeClampToEdge,
                                          .address_w = MTLSamplerAddressModeClampToEdge});
    ASSERT_MSG(impl->linear_sampler, "Scale helper: no sampler");
    impl->depth_write_state = impl->states.Get(ResampleState(false, 0));
    impl->depth_stencil_state = impl->states.Get(ResampleState(true, 0xff));
    impl->stencil_bit_states[0] = impl->states.Get(ResampleState(true, 0));
    for (u32 i = 1; i < impl->stencil_bit_states.size(); ++i) {
        impl->stencil_bit_states[i] = impl->states.Get(ResampleState(true, u8(1u << (i - 1))));
    }
}

ScaleHelper::~ScaleHelper() = default;

void ScaleHelper::ScaleColor(Encoder& encoder, void* src, void* dst, ScaleFilter filter,
                             ScaleRect rect, u32 src_level, u32 dst_level) {
    id<MTLTexture> source = (__bridge id<MTLTexture>)src;
    id<MTLTexture> target = (__bridge id<MTLTexture>)dst;
    const u32 format = u32(target.pixelFormat);
    void* const pipeline = impl->Pipeline(ColorKey(format, filter));
    const u32 width = std::max<u32>(1, u32(target.width) >> dst_level);
    const u32 height = std::max<u32>(1, u32(target.height) >> dst_level);
    if (rect.width == 0) {
        rect = {0, 0, width, height};
    }
    // The shaders read level 0 of a 2D texture: other levels and types go through a view.
    void* texture = src;
    if (src_level != 0 || source.textureType != MTLTextureType2D) {
        Object view = CreateTextureView(src, u32(source.pixelFormat), TextureType::Tex2D,
                                        src_level, 1, 0, 1, IdentitySwizzle);
        ASSERT_MSG(view, "Scale helper: no 2D view of level {}", src_level);
        texture = view.Get();
        encoder.KeepAlive(std::move(view));
    }
    RenderPassDesc pass{};
    pass.colors[0].texture = dst;
    pass.colors[0].level = dst_level;
    // A full, unblended color copy overwrites the whole attached mip. Partial
    // copies still Load: pixels outside `rect` must retain their content.
    pass.colors[0].discard_load = rect.x == 0 && rect.y == 0 &&
                                  rect.width == width && rect.height == height;
    encoder.BeginRenderPass(pass);
    SetFullscreenState(encoder, pipeline, rect);
    encoder.SetDepthStencil(nullptr);
    encoder.SetTexture(Stage::Fragment, 0, texture);
    if (filter == ScaleFilter::Linear) {
        encoder.SetSampler(Stage::Fragment, 0, impl->linear_sampler.Get());
    }
    encoder.Draw(Primitive::Triangle, 3);
    encoder.EndRenderPass();
}

void ScaleHelper::ResampleDepth(Encoder& encoder, void* src_depth, void* dst_depth,
                                StencilCopy stencil) {
    id<MTLTexture> source = (__bridge id<MTLTexture>)src_depth;
    id<MTLTexture> target = (__bridge id<MTLTexture>)dst_depth;
    const u32 format = u32(target.pixelFormat);
    const bool dst_stencil = StencilViewPixelFormat(format) != 0;
    Object stencil_view;
    if (stencil != StencilCopy::None) {
        const u32 view_format = StencilViewPixelFormat(u32(source.pixelFormat));
        ASSERT_MSG(dst_stencil && view_format != 0,
                   "Scale helper: stencil resample between formats {} and {}",
                   u32(source.pixelFormat), format);
        stencil_view = CreateTextureView(src_depth, view_format, TextureType::Tex2D, 0, 1, 0, 1,
                                         IdentitySwizzle);
        ASSERT_MSG(stencil_view, "Scale helper: no stencil view of format {}",
                   u32(source.pixelFormat));
    }
    void* const pipeline = impl->Pipeline(DepthKey(format, stencil));
    RenderPassDesc pass{};
    pass.depth.texture = dst_depth;
    pass.depth.has_stencil = dst_stencil;
    pass.depth.clear_depth = true;           // every pixel is written (compare always)
    pass.depth.clear_stencil = dst_stencil; // None leaves 0; PerBit ORs bits into zero
    pass.depth.stencil = 0;
    void* const stencil_texture = stencil_view.Get();
    if (stencil_view) {
        encoder.KeepAlive(std::move(stencil_view));
    }
    encoder.BeginRenderPass(pass);
    SetFullscreenState(encoder, pipeline, {0, 0, u32(target.width), u32(target.height)});
    encoder.SetTexture(Stage::Fragment, 0, src_depth);
    if (stencil_texture) {
        encoder.SetTexture(Stage::Fragment, 1, stencil_texture);
    }
    if (stencil == StencilCopy::PerBit) {
        // Copy depth first (bit 0, write mask 0), then reconstruct all eight stencil bits with
        // fixed-function replacement; discard keeps unset bits clear.
        for (u32 i = 0; i < impl->stencil_bit_states.size(); ++i) {
            const u32 bit = i ? 1u << (i - 1) : 0u;
            encoder.SetBytes(Stage::Fragment, 0, &bit, sizeof(bit));
            encoder.SetDepthStencil(impl->stencil_bit_states[i]);
            encoder.SetStencilReference(bit, bit);
            encoder.Draw(Primitive::Triangle, 3);
        }
    } else {
        encoder.SetDepthStencil(stencil == StencilCopy::None ? impl->depth_write_state
                                                             : impl->depth_stencil_state);
        encoder.Draw(Primitive::Triangle, 3);
    }
    encoder.EndRenderPass();
}

void ScaleHelper::PrepareColor(u32 dst_format, ScaleFilter filter) {
    impl->Pipeline(ColorKey(dst_format, filter));
}

void ScaleHelper::PrepareDepth(u32 dst_format, StencilCopy stencil) {
    impl->Pipeline(DepthKey(dst_format, stencil));
}

size_t ScaleHelper::PipelineCount() const {
    std::scoped_lock lock{impl->mutex};
    return impl->pipelines.size();
}

void ScaleHelper::ClearDepth(Encoder& encoder, void* depth, float value, u32 stencil) {
    id<MTLTexture> target = (__bridge id<MTLTexture>)depth;
    const bool has_stencil = StencilViewPixelFormat(u32(target.pixelFormat)) != 0;
    RenderPassDesc pass{};
    pass.depth.texture = depth;
    pass.depth.has_stencil = has_stencil;
    pass.depth.clear_depth = true;
    pass.depth.clear_stencil = has_stencil;
    pass.depth.depth = value;
    pass.depth.stencil = stencil;
    encoder.BeginRenderPass(pass);
    encoder.EndRenderPass();
}

} // namespace Metal
