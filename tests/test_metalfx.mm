// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: smoke test for MetalFX on native textures (gpu/shim/bbport_metalfx.h, Metal-native M5
// Task 8): the temporal scaler is made with the upscaler's formats (colour RGBA16F or the
// tonemapped RGBA8 sRGB/Unorm frame, depth D32S8 or D32, motion RG16F or RGBA16F, output RGBA16F
// or RGBA8) and one frame is encoded into a Metal::Encoder's command buffer between our own
// commands; the jitter signs (BB_METALFX_JITTER, BB_METALFX_JITTER_SIGN) are parsed per axis.
// Links bbmetal_core and the shim alone; run plain and with MTL_DEBUG_LAYER=1.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "bbport_metalfx.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
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

Object Texture(Device& device, u32 format, u32 width, u32 height, bool render_target,
               bool shader_write) {
    Object texture = CreateTexture(device,
                                   TextureDesc{.pixel_format = format,
                                               .width = width,
                                               .height = height,
                                               .render_target = render_target,
                                               .shader_write = shader_write},
                                   "metalfx test", nullptr);
    if (!texture) {
        std::printf("FAIL: texture %u %ux%u not created\n", format, width, height);
        std::exit(1);
    }
    return texture;
}

void Clear(Device& device, void* texture, double value) {
    Encoder encoder{device};
    RenderPassDesc pass{};
    pass.colors[0].texture = texture;
    pass.colors[0].clear = true;
    pass.colors[0].clear_color = {value, value, value, 1.0};
    encoder.BeginRenderPass(pass);
    encoder.EndRenderPass();
    device.Wait(encoder.Submit());
}

void UploadDepth(Device& device, void* texture, u32 width, u32 height, bool stencil) {
    std::vector<float> values(width * height, 0.5f);
    id<MTLBuffer> staging = SharedBuffer(device, values.size() * 4);
    std::memcpy(staging.contents, values.data(), values.size() * 4);
    Encoder encoder{device};
    encoder.CopyBufferToTexture(Ref(staging), width * 4, width * height * 4, texture, 0, 0, {},
                                {width, height, 1},
                                stencil ? BlitAspect::Depth : BlitAspect::All);
    device.Wait(encoder.Submit());
}

/// Writes `value` into every half-float component of `texture` (`components` per texel, iw x ih)
/// outside its top-left w x h, keeping zeros inside.
void FillBorder(Device& device, void* texture, u32 components, u32 iw, u32 ih, u32 w, u32 h,
                uint16_t value) {
    std::vector<uint16_t> texels(iw * ih * components, 0);
    for (u32 y = 0; y < ih; ++y) {
        for (u32 x = 0; x < iw; ++x) {
            if (x >= w || y >= h) {
                for (u32 i = 0; i < components; ++i) {
                    texels[(y * iw + x) * components + i] = value;
                }
            }
        }
    }
    const u32 texel = components * 2;
    id<MTLBuffer> staging = SharedBuffer(device, texels.size() * 2);
    std::memcpy(staging.contents, texels.data(), texels.size() * 2);
    Encoder encoder{device};
    encoder.CopyBufferToTexture(Ref(staging), iw * texel, iw * ih * texel, texture, 0, 0, {},
                                {iw, ih, 1});
    device.Wait(encoder.Submit());
}

/// An RGBA8 colour: the scene grey (128) in the top-left w x h, white outside.
void FillBorder8(Device& device, void* texture, u32 iw, u32 ih, u32 w, u32 h) {
    std::vector<uint8_t> texels(iw * ih * 4);
    for (u32 y = 0; y < ih; ++y) {
        for (u32 x = 0; x < iw; ++x) {
            const uint8_t v = x >= w || y >= h ? 255 : 128;
            for (u32 i = 0; i < 4; ++i) {
                texels[(y * iw + x) * 4 + i] = i == 3 ? 255 : v;
            }
        }
    }
    id<MTLBuffer> staging = SharedBuffer(device, texels.size());
    std::memcpy(staging.contents, texels.data(), texels.size());
    Encoder encoder{device};
    encoder.CopyBufferToTexture(Ref(staging), iw * 4, iw * ih * 4, texture, 0, 0, {}, {iw, ih, 1});
    device.Wait(encoder.Submit());
}

void TestJitterSigns() {
    using BbMetalFx::JitterSigns;
    const auto is = [](std::array<float, 2> s, float x, float y) {
        return s[0] == x && s[1] == y;
    };
    Expect(is(JitterSigns(nullptr, nullptr), 1, 1), "jitter signs: default 1,1");
    Expect(is(JitterSigns("-1,1", nullptr), -1, 1), "jitter signs: per axis -1,1");
    Expect(is(JitterSigns("1,-1", nullptr), 1, -1), "jitter signs: per axis 1,-1");
    Expect(is(JitterSigns(nullptr, "-1"), -1, -1), "jitter signs: BB_METALFX_JITTER_SIGN -1");
    Expect(is(JitterSigns("-1,1", "-1"), 1, -1), "jitter signs: both variables multiply");
    Expect(is(JitterSigns("0,1", nullptr), 0, 1), "jitter signs: 0 switches an axis off");
    Expect(is(JitterSigns("bogus", nullptr), 1, 1), "jitter signs: malformed per-axis ignored");
    Expect(is(JitterSigns("-1", nullptr), 1, 1), "jitter signs: one number is not per-axis");
}

struct Case {
    const char* name;
    u32 color, depth, motion, output;
    bool hdr;
    u32 iw, ih; ///< the colour/depth texture size (>= the 64x36 input: BB_RENDER_RES targets)
};

void RunCase(Device& device, const Case& c) {
    constexpr u32 w = 64, h = 36, ow = 128, oh = 72;
    const BbMetalFx::Config config{
        .color_format = c.color,
        .depth_format = c.depth,
        .motion_format = c.motion,
        .output_format = c.output,
        .width = w,
        .height = h,
        .out_width = ow,
        .out_height = oh,
        .hdr = c.hdr,
    };
    Object scaler = BbMetalFx::CreateScaler(device, config);
    if (!scaler) {
        std::printf("FAIL: %s: no scaler\n", c.name);
        ++failures;
        return;
    }
    // The upscaler's usages: the guest colour is a render target (sampled), the depth a depth
    // attachment, motion and output written by kernels, the output also a render target. The
    // scaler's input size is the scene (w x h, = the content size); the textures may be larger
    // (BB_RENDER_RES targets), but MetalFX's validation asserts that the motion texture has the
    // colour texture's size, so the upscaler makes it that size.
    Object color = Texture(device, c.color, c.iw, c.ih, true, false);
    Object depth = Texture(device, c.depth, c.iw, c.ih, false, false);
    Object motion = Texture(device, c.motion, c.iw, c.ih, true, true);
    Object output = Texture(device, c.output, ow, oh, true, true);
    Clear(device, color.Get(), 0.5);
    Clear(device, motion.Get(), 0.0);
    if (c.iw > w || c.ih > h) {
        // BB_RENDER_RES: nothing writes outside the scene; there the targets hold garbage (NaN
        // motion, a white colour border), which must not reach the output.
        FillBorder(device, motion.Get(), c.motion == MTLPixelFormatRG16Float ? 2 : 4, c.iw, c.ih,
                   w, h, 0x7e00); // half NaN
        if (c.color == MTLPixelFormatRGBA8Unorm_sRGB || c.color == MTLPixelFormatRGBA8Unorm) {
            FillBorder8(device, color.Get(), c.iw, c.ih, w, h);
        }
    }
    Clear(device, output.Get(), 0.0);
    UploadDepth(device, depth.Get(), c.iw, c.ih, c.depth == MTLPixelFormatDepth32Float_Stencil8);

    const bool rgba8 = c.output != MTLPixelFormatRGBA16Float;
    const u32 texel = rgba8 ? 4 : 8;
    id<MTLBuffer> readback = SharedBuffer(device, ow * oh * texel);
    {
        Encoder encoder{device};
        // Our own commands before and after the scaler's, in one command buffer: a copy that
        // leaves a blit encoder open, MetalFX, then the readback copy.
        encoder.CopyTexture(motion.Get(), 0, 0, {}, motion.Get(), 0, 0, {}, {1, 1, 1});
        {
            auto recorded = [owned = Object::Retain(scaler.Get()), &encoder, color = color.Get(),
                             depth = depth.Get(), motion = motion.Get(), output = output.Get()] {
                BbMetalFx::Encode(encoder, owned.Get(),
                                  {.color = color,
                                   .depth = depth,
                                   .motion = motion,
                                   .output = output,
                                   .content_width = w,
                                   .content_height = h,
                                   .jitter_x = 0.25f,
                                   .jitter_y = -0.25f,
                                   .reset = true});
            };
            // As with a deferred Scheduler::Record: replacing the owner's scaler
            // before the closure runs must not require finishing the GPU first.
            scaler.Reset();
            recorded();
        } // the recorded closure is gone; the command buffer now keeps the scaler
        encoder.CopyTextureToBuffer(output.Get(), 0, 0, {}, {ow, oh, 1}, Ref(readback),
                                    ow * texel, ow * oh * texel);
        device.Wait(encoder.Submit());
    }
    // A flat 0.5 grey stays about 0.5 grey (bytes: the sRGB or Unorm encoding of it).
    const u32 centre = (oh / 2) * ow + ow / 2;
    float red = 0.0f;
    if (rgba8) {
        red = static_cast<const u8*>(readback.contents)[centre * 4] / 255.0f;
    } else {
        red = float(static_cast<const _Float16*>(readback.contents)[centre * 4]);
    }
    const float expected =
        c.output == MTLPixelFormatRGBA8Unorm_sRGB || c.color == MTLPixelFormatRGBA8Unorm_sRGB
            ? -1.0f // colour-space conversions: only "not black"
            : 0.5f;
    char what[160];
    std::snprintf(what, sizeof(what), "%s: output centre %.3f (expected %s)", c.name, red,
                  expected < 0 ? "> 0.1" : "0.5 +- 0.1");
    Expect(expected < 0 ? red > 0.1f : std::fabs(red - expected) <= 0.1f, what);
    if (c.iw > w || c.ih > h) {
        // The output's corner pixel comes from the scene's corner, next to the border.
        const u32 corner = (oh - 1) * ow + ow - 1;
        const float v = rgba8 ? static_cast<const u8*>(readback.contents)[corner * 4] / 255.0f
                              : float(static_cast<const _Float16*>(readback.contents)[corner * 4]);
        std::snprintf(what, sizeof(what), "%s: output corner %.3f (no border garbage)", c.name, v);
        Expect(std::isfinite(v) && std::fabs(v - 0.5f) <= 0.1f, what);
    }
    std::printf("%s: %ux%u -> %ux%u, centre %.3f\n", c.name, w, h, ow, oh, red);
}

} // namespace

int main() {
    TestJitterSigns();
    @autoreleasepool {
        Device device;
        if (!BbMetalFx::Supported(device)) {
            std::printf("PASS: MetalFX (not supported on this device: scaler cases skipped)\n");
            return failures ? 1 : 0;
        }
        const Case cases[] = {
            {"HDR RGBA16F, D32S8, RG16F -> RGBA16F", MTLPixelFormatRGBA16Float,
             MTLPixelFormatDepth32Float_Stencil8, MTLPixelFormatRG16Float,
             MTLPixelFormatRGBA16Float, true, 64, 36},
            {"tonemapped RGBA8 sRGB, D32S8, RG16F -> RGBA16F", MTLPixelFormatRGBA8Unorm_sRGB,
             MTLPixelFormatDepth32Float_Stencil8, MTLPixelFormatRG16Float,
             MTLPixelFormatRGBA16Float, false, 64, 36},
            {"tonemapped RGBA8, D32, RGBA16F motion -> RGBA16F", MTLPixelFormatRGBA8Unorm,
             MTLPixelFormatDepth32Float, MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA16Float,
             false, 64, 36},
            {"tonemapped RGBA8, D32S8, RG16F -> RGBA8", MTLPixelFormatRGBA8Unorm,
             MTLPixelFormatDepth32Float_Stencil8, MTLPixelFormatRG16Float,
             MTLPixelFormatRGBA8Unorm, false, 64, 36},
            {"BB_RENDER_RES: 72x40 targets (garbage border), 64x36 scene", MTLPixelFormatRGBA8Unorm,
             MTLPixelFormatDepth32Float_Stencil8, MTLPixelFormatRG16Float,
             MTLPixelFormatRGBA16Float, false, 72, 40},
        };
        for (const Case& c : cases) {
            RunCase(device, c);
        }
        Expect(device.ErrorCount() == 0, "no command buffer errors on the device queue");
    }
    if (failures) {
        std::printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    std::printf("PASS: MetalFX on native textures\n");
    return 0;
}
