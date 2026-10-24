// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: unit test for the scale helper (renderer_metal/mtl_scale.h, Metal-native M5 Task 2):
// linear and nearest scaled colour copies with format conversion (RGBA16F -> RGBA8), into a
// sub-rectangle, from a mip level and in the uint flavour; nearest depth resample (depth only into
// a depth-stencil target, stencil export, stencil per bit); depth clear. Links bbmetal_core alone.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_scale.h"
#include "video_core/renderer_metal/mtl_texture.h"

namespace {

using namespace Metal;

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

id<MTLDevice> Dev(Device& device) {
    return (__bridge id<MTLDevice>)device.NativeDevice();
}

id<MTLBuffer> SharedBuffer(Device& device, NSUInteger length) {
    id<MTLBuffer> buffer = [Dev(device) newBufferWithLength:length
                                                    options:MTLResourceStorageModeShared];
    std::memset(buffer.contents, 0, length);
    return buffer;
}

BufferRef Ref(id<MTLBuffer> buffer) {
    return {(__bridge void*)buffer, 0};
}

Object Texture(Device& device, u32 format, u32 width, u32 height, u32 levels = 1,
               bool format_views = false) {
    Object texture = CreateTexture(device,
                                   TextureDesc{.pixel_format = format,
                                               .width = width,
                                               .height = height,
                                               .levels = levels,
                                               .render_target = true,
                                               .format_views = format_views},
                                   "scale test", nullptr);
    if (!texture) {
        std::printf("FAIL: texture %u %ux%u not created\n", format, width, height);
        std::exit(1);
    }
    return texture;
}

/// Writes `data` (`texel` bytes per texel) into `level` (`aspect`) of `texture`.
void Upload(Device& device, void* texture, u32 level, u32 width, u32 height, u32 texel,
            const void* data, BlitAspect aspect = BlitAspect::All) {
    id<MTLBuffer> staging = SharedBuffer(device, width * height * texel);
    std::memcpy(staging.contents, data, width * height * texel);
    Encoder encoder{device};
    encoder.CopyBufferToTexture(Ref(staging), width * texel, width * height * texel, texture,
                                level, 0, {}, {width, height, 1}, aspect);
    device.Wait(encoder.Submit());
}

template <typename T>
std::vector<T> Read(Device& device, void* texture, u32 width, u32 height,
                    BlitAspect aspect = BlitAspect::All, u32 components = 4) {
    const u32 texel = sizeof(T) * components;
    id<MTLBuffer> readback = SharedBuffer(device, width * height * texel);
    Encoder encoder{device};
    encoder.CopyTextureToBuffer(texture, 0, 0, {}, {width, height, 1}, Ref(readback),
                                width * texel, width * height * texel, aspect);
    device.Wait(encoder.Submit());
    const T* values = static_cast<const T*>(readback.contents);
    return {values, values + width * height * components};
}

/// Clears level 0 of a colour target.
void ClearColor(Device& device, void* texture, double value) {
    Encoder encoder{device};
    RenderPassDesc pass{};
    pass.colors[0].texture = texture;
    pass.colors[0].clear = true;
    pass.colors[0].clear_color = {value, value, value, value};
    encoder.BeginRenderPass(pass);
    encoder.EndRenderPass();
    device.Wait(encoder.Submit());
}

u8 Unorm8(float v) {
    return u8(std::lround(v * 255.0f));
}

bool Near(int a, int b, int tolerance = 1) {
    return std::abs(a - b) <= tolerance;
}

// 2x2 RGBA16F (red 0 / 1 across x, green 0 / 1 across y) -> 4x4 RGBA8, linear: at pixel
// centres the source coordinate is x / 2 - 0.25, clamped at the edges: 0, 0.25, 0.75, 1.
void TestLinearUpscale(Device& device, ScaleHelper& scale) {
    Object src = Texture(device, MTLPixelFormatRGBA16Float, 2, 2);
    const _Float16 texels[2 * 2 * 4] = {0, 0, 0, 1, 1, 0, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1};
    Upload(device, src.Get(), 0, 2, 2, 8, texels);
    Object dst = Texture(device, MTLPixelFormatRGBA8Unorm, 4, 4);
    Encoder encoder{device};
    scale.ScaleColor(encoder, src.Get(), dst.Get(), ScaleFilter::Linear);
    device.Wait(encoder.Submit());
    const auto out = Read<u8>(device, dst.Get(), 4, 4);
    constexpr float ramp[4] = {0.0f, 0.25f, 0.75f, 1.0f};
    bool ok = true;
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const u8* p = &out[(y * 4 + x) * 4];
            ok &= Near(p[0], Unorm8(ramp[x])) && Near(p[1], Unorm8(ramp[y])) && p[2] == 0 &&
                  p[3] == 255;
        }
    }
    Expect(ok, "linear RGBA16F 2x2 -> RGBA8 4x4: bilinear values");
}

// 4x4 RGBA16F -> 2x2 RGBA8, nearest: pixel x reads texel floor((x + 0.5) * 2) = 1, 3.
void TestNearestDownscale(Device& device, ScaleHelper& scale) {
    Object src = Texture(device, MTLPixelFormatRGBA16Float, 4, 4);
    std::vector<_Float16> texels(4 * 4 * 4);
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            _Float16* t = &texels[(y * 4 + x) * 4];
            t[0] = _Float16(float(x) / 4.0f);
            t[1] = _Float16(float(y) / 4.0f);
            t[2] = 0;
            t[3] = 1;
        }
    }
    Upload(device, src.Get(), 0, 4, 4, 8, texels.data());
    Object dst = Texture(device, MTLPixelFormatRGBA8Unorm, 2, 2);
    Encoder encoder{device};
    scale.ScaleColor(encoder, src.Get(), dst.Get(), ScaleFilter::Nearest);
    device.Wait(encoder.Submit());
    const auto out = Read<u8>(device, dst.Get(), 2, 2);
    bool ok = true;
    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 2; ++x) {
            const u8* p = &out[(y * 2 + x) * 4];
            ok &= Near(p[0], Unorm8(float(2 * x + 1) / 4.0f)) &&
                  Near(p[1], Unorm8(float(2 * y + 1) / 4.0f)) && p[3] == 255;
        }
    }
    Expect(ok, "nearest RGBA16F 4x4 -> RGBA8 2x2: texels 1 and 3");
}

// Same size, same filter as the upscaler's output -> UI image copy: exact texels.
void TestSameSizeConvert(Device& device, ScaleHelper& scale) {
    Object src = Texture(device, MTLPixelFormatRGBA16Float, 3, 2);
    std::vector<_Float16> texels(3 * 2 * 4);
    for (u32 i = 0; i < texels.size(); ++i) {
        texels[i] = _Float16(float(i * 10) / 255.0f);
    }
    Upload(device, src.Get(), 0, 3, 2, 8, texels.data());
    Object dst = Texture(device, MTLPixelFormatRGBA8Unorm, 3, 2);
    Encoder encoder{device};
    scale.ScaleColor(encoder, src.Get(), dst.Get(), ScaleFilter::Nearest);
    device.Wait(encoder.Submit());
    const auto out = Read<u8>(device, dst.Get(), 3, 2);
    bool ok = true;
    for (u32 i = 0; i < out.size(); ++i) {
        ok &= Near(out[i], int(i * 10));
    }
    Expect(ok, "nearest RGBA16F -> RGBA8 at the same size: every texel");
}

// Into a rectangle of a larger target: pixels outside keep the clear colour.
void TestRect(Device& device, ScaleHelper& scale) {
    Object src = Texture(device, MTLPixelFormatRGBA8Unorm, 1, 1);
    const u8 white[4] = {255, 255, 255, 255};
    Upload(device, src.Get(), 0, 1, 1, 4, white);
    Object dst = Texture(device, MTLPixelFormatRGBA8Unorm, 4, 4);
    ClearColor(device, dst.Get(), 0.0);
    Encoder encoder{device};
    scale.ScaleColor(encoder, src.Get(), dst.Get(), ScaleFilter::Linear, {1, 2, 2, 1});
    device.Wait(encoder.Submit());
    const auto out = Read<u8>(device, dst.Get(), 4, 4);
    bool ok = true;
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const bool inside = y == 2 && (x == 1 || x == 2);
            ok &= out[(y * 4 + x) * 4] == (inside ? 255 : 0);
        }
    }
    Expect(ok, "rect {1, 2, 2x1}: only those two pixels written");
}

// Level 1 of a mipped source into level 1 of a mipped target, uint flavour.
void TestLevelsUint(Device& device, ScaleHelper& scale) {
    Object src = Texture(device, MTLPixelFormatRGBA8Uint, 4, 4, 2);
    const u8 level1[2 * 2 * 4] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    Upload(device, src.Get(), 1, 2, 2, 4, level1);
    Object dst = Texture(device, MTLPixelFormatRGBA8Uint, 8, 8, 2);
    Encoder encoder{device};
    scale.ScaleColor(encoder, src.Get(), dst.Get(), ScaleFilter::Nearest, {}, 1, 1);
    id<MTLBuffer> readback = SharedBuffer(device, 4 * 4 * 4);
    encoder.CopyTextureToBuffer(dst.Get(), 1, 0, {}, {4, 4, 1}, Ref(readback), 16, 64);
    device.Wait(encoder.Submit());
    const u8* out = static_cast<const u8*>(readback.contents);
    bool ok = true;
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            ok &= std::memcmp(&out[(y * 4 + x) * 4], &level1[((y / 2) * 2 + x / 2) * 4], 4) == 0;
        }
    }
    Expect(ok, "nearest RGBA8Uint level 1 (2x2) -> level 1 (4x4)");
}

// An explicit full-mip copy followed by a full-width partial-height copy.
// Level 0 and the unwritten row of level 1 must retain every channel.
void TestFullMipThenPartial(Device& device, ScaleHelper& scale) {
    Object white = Texture(device, MTLPixelFormatRGBA8Unorm, 1, 1);
    Object blue = Texture(device, MTLPixelFormatRGBA8Unorm, 1, 1);
    const u8 white_pixel[4] = {255, 255, 255, 255}, blue_pixel[4] = {0, 0, 255, 255};
    Upload(device, white.Get(), 0, 1, 1, 4, white_pixel);
    Upload(device, blue.Get(), 0, 1, 1, 4, blue_pixel);
    Object dst = Texture(device, MTLPixelFormatRGBA8Unorm, 8, 4, 2);
    const std::vector<u8> untouched(8 * 4 * 4, 77);
    Upload(device, dst.Get(), 0, 8, 4, 4, untouched.data());
    Encoder encoder{device};
    scale.ScaleColor(encoder, white.Get(), dst.Get(), ScaleFilter::Linear, {0, 0, 4, 2}, 0, 1);
    scale.ScaleColor(encoder, blue.Get(), dst.Get(), ScaleFilter::Nearest, {0, 0, 4, 1}, 0, 1);
    id<MTLBuffer> readback = SharedBuffer(device, 4 * 2 * 4);
    encoder.CopyTextureToBuffer(dst.Get(), 1, 0, {}, {4, 2, 1}, Ref(readback), 16, 32);
    device.Wait(encoder.Submit());
    const u8* out = static_cast<const u8*>(readback.contents);
    bool ok = true;
    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            ok &= std::memcmp(out + (y * 4 + x) * 4, y == 0 ? blue_pixel : white_pixel, 4) == 0;
        }
    }
    Expect(ok, "full-mip overwrite followed by partial-height copy preserves the unwritten row");
    Expect(Read<u8>(device, dst.Get(), 8, 4) == untouched,
           "discarding mip 1's old contents preserves mip 0");
}

std::vector<float> DepthRamp(u32 width, u32 height) {
    std::vector<float> depth(width * height);
    for (u32 i = 0; i < depth.size(); ++i) {
        depth[i] = float(i + 1) / 64.0f;
    }
    return depth;
}

// D32 4x4 -> D32S8 2x2 depth only: texels (1, 1), (3, 1), (1, 3), (3, 3); stencil cleared to 0.
void TestResampleDepthOnly(Device& device, ScaleHelper& scale) {
    Object src = Texture(device, MTLPixelFormatDepth32Float, 4, 4);
    const auto depth = DepthRamp(4, 4);
    Upload(device, src.Get(), 0, 4, 4, 4, depth.data());
    Object dst = Texture(device, MTLPixelFormatDepth32Float_Stencil8, 2, 2);
    {
        Encoder encoder{device};
        scale.ClearDepth(encoder, dst.Get(), 1.0f, 5);
        device.Wait(encoder.Submit());
    }
    Encoder encoder{device};
    scale.ResampleDepth(encoder, src.Get(), dst.Get());
    device.Wait(encoder.Submit());
    const auto out = Read<float>(device, dst.Get(), 2, 2, BlitAspect::Depth, 1);
    const auto stencil = Read<u8>(device, dst.Get(), 2, 2, BlitAspect::Stencil, 1);
    bool ok = true;
    for (u32 y = 0; y < 2; ++y) {
        for (u32 x = 0; x < 2; ++x) {
            ok &= out[y * 2 + x] == depth[(2 * y + 1) * 4 + 2 * x + 1] && stencil[y * 2 + x] == 0;
        }
    }
    Expect(ok, "depth-only resample D32 4x4 -> D32S8 2x2: nearest depth, stencil 0");
}

// D32S8 2x2 -> D32S8 4x4 with stencil (export, then per bit): each source texel covers 2x2.
void TestResampleStencil(Device& device, ScaleHelper& scale, StencilCopy mode, const char* what) {
    // The stencil view needs PixelFormatView usage (the texture cache's depth images have it).
    Object src = Texture(device, MTLPixelFormatDepth32Float_Stencil8, 2, 2, 1, true);
    const float depth[4] = {0.125f, 0.25f, 0.5f, 0.75f};
    const u8 stencil[4] = {0x00, 0x81, 0x5a, 0xff};
    Upload(device, src.Get(), 0, 2, 2, 4, depth, BlitAspect::Depth);
    Upload(device, src.Get(), 0, 2, 2, 1, stencil, BlitAspect::Stencil);
    Object dst = Texture(device, MTLPixelFormatDepth32Float_Stencil8, 4, 4);
    Encoder encoder{device};
    scale.ResampleDepth(encoder, src.Get(), dst.Get(), mode);
    device.Wait(encoder.Submit());
    const auto out_depth = Read<float>(device, dst.Get(), 4, 4, BlitAspect::Depth, 1);
    const auto out_stencil = Read<u8>(device, dst.Get(), 4, 4, BlitAspect::Stencil, 1);
    bool ok = true;
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            const u32 s = (y / 2) * 2 + x / 2;
            ok &= out_depth[y * 4 + x] == depth[s] && out_stencil[y * 4 + x] == stencil[s];
        }
    }
    Expect(ok, what);
}

void TestClearDepth(Device& device, ScaleHelper& scale) {
    Object depth = Texture(device, MTLPixelFormatDepth32Float_Stencil8, 3, 3);
    Object plain = Texture(device, MTLPixelFormatDepth32Float, 3, 3);
    Encoder encoder{device};
    scale.ClearDepth(encoder, depth.Get(), 0.25f, 7);
    scale.ClearDepth(encoder, plain.Get(), 0.5f);
    device.Wait(encoder.Submit());
    bool ok = true;
    for (float v : Read<float>(device, depth.Get(), 3, 3, BlitAspect::Depth, 1)) ok &= v == 0.25f;
    for (u8 v : Read<u8>(device, depth.Get(), 3, 3, BlitAspect::Stencil, 1)) ok &= v == 7;
    for (float v : Read<float>(device, plain.Get(), 3, 3, BlitAspect::All, 1)) ok &= v == 0.5f;
    Expect(ok, "ClearDepth: D32S8 to 0.25 / 7, D32 to 0.5");
}

// Prepare* builds the pipelines a later call uses: the call then builds none (the upscaler builds
// them ahead, not at first use on the encoder thread).
void TestPrepare(Device& device) {
    ScaleHelper fresh{device};
    Expect(fresh.PipelineCount() == 0, "Prepare: a new helper has no pipelines");
    fresh.PrepareColor(MTLPixelFormatRGBA16Float, ScaleFilter::Linear);
    fresh.PrepareColor(MTLPixelFormatRGBA8Unorm_sRGB, ScaleFilter::Nearest);
    fresh.PrepareDepth(MTLPixelFormatDepth32Float_Stencil8, StencilCopy::None);
    fresh.PrepareColor(MTLPixelFormatRGBA16Float, ScaleFilter::Linear); // again: no new one
    Expect(fresh.PipelineCount() == 3, "Prepare: three distinct pipelines");
    Object src = Texture(device, MTLPixelFormatRGBA16Float, 2, 2);
    Object hdr = Texture(device, MTLPixelFormatRGBA16Float, 4, 4);
    Object ldr = Texture(device, MTLPixelFormatRGBA8Unorm_sRGB, 4, 4);
    Object depth_src = Texture(device, MTLPixelFormatDepth32Float_Stencil8, 2, 2);
    Object depth_dst = Texture(device, MTLPixelFormatDepth32Float_Stencil8, 4, 4);
    Encoder encoder{device};
    fresh.ScaleColor(encoder, src.Get(), hdr.Get(), ScaleFilter::Linear);
    fresh.ScaleColor(encoder, src.Get(), ldr.Get(), ScaleFilter::Nearest);
    fresh.ResampleDepth(encoder, depth_src.Get(), depth_dst.Get());
    device.Wait(encoder.Submit());
    Expect(fresh.PipelineCount() == 3, "Prepare: the prepared calls build no pipeline");
}

} // namespace

int main() {
    @autoreleasepool {
        Device device;
        ScaleHelper scale{device};
        TestLinearUpscale(device, scale);
        TestNearestDownscale(device, scale);
        TestSameSizeConvert(device, scale);
        TestRect(device, scale);
        TestLevelsUint(device, scale);
        TestFullMipThenPartial(device, scale);
        TestClearDepth(device, scale);
        TestPrepare(device);
        TestResampleDepthOnly(device, scale);
        TestResampleStencil(device, scale, StencilCopy::Export,
                            "depth+stencil resample 2x2 -> 4x4, stencil export");
        TestResampleStencil(device, scale, StencilCopy::PerBit,
                            "depth+stencil resample 2x2 -> 4x4, stencil per bit");
        Expect(device.ErrorCount() == 0, "no command buffer errors on the device queue");
    }
    if (failures) {
        std::printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    std::printf("PASS: Metal-native scale helper\n");
    return 0;
}
