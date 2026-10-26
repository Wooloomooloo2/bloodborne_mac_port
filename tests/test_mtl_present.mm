// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: unit test for the present helper (renderer_metal/mtl_present.h, Metal-native M4 Task 1):
// aspect fit, the post-process pass (gamma, sRGB input, alpha swizzle) against the CPU formula of
// host_shaders/post_process.frag, the letterboxed present into an offscreen CAMetalLayer's
// drawable, the present queue's wait on the device's shared event, drawable recycling inside
// autorelease pools and the queue's destructor (and its bounded waits). Links bbmetal_core alone; no window.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <ImageIO/ImageIO.h>
#import <QuartzCore/CAMetalLayer.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_present.h"
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

constexpr u32 kBGRA8 = MTLPixelFormatBGRA8Unorm;
constexpr u32 kBGRA8Srgb = MTLPixelFormatBGRA8Unorm_sRGB;
constexpr u32 kRGB10A2 = MTLPixelFormatRGB10A2Unorm;

// post_process.frag, on the CPU.
constexpr float kCutoff = 0.0031308f, kA = 1.055f, kB = 0.055f, kD = 12.92f;
float Gamma(float c, float g) {
    return c < kCutoff ? kD * c / g : kA * std::pow(c, 1.0f / (2.4f + 1.0f - g)) - kB;
}
float Degamma(float c) {
    return c < kD * kCutoff ? c / kD : std::pow(std::max(c + kB, 0.0f) / kA, 2.4f);
}
/// The sRGB decode the GPU applies when sampling an _sRGB texture.
float SrgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
u8 ToUnorm8(float c) {
    return u8(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
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

/// A private texture of `format` filled with `data` (4 bytes per texel).
Object Upload(Device& device, u32 format, u32 width, u32 height, const void* data,
              bool format_views = false) {
    Object texture = CreateTexture(device,
                                   TextureDesc{.pixel_format = format,
                                               .width = width,
                                               .height = height,
                                               .format_views = format_views},
                                   "test source", nullptr);
    id<MTLBuffer> staging = SharedBuffer(device, width * height * 4);
    std::memcpy(staging.contents, data, width * height * 4);
    Encoder encoder{device};
    encoder.CopyBufferToTexture(Ref(staging), width * 4, width * height * 4, texture.Get(), 0, 0,
                                {}, {width, height, 1});
    device.Wait(encoder.Submit());
    return texture;
}

/// Runs the post-process pass from `input` into a new 4x4 BGRA8Unorm target and reads it back.
std::vector<u8> RunPostProcess(Device& device, PresentPasses& passes, void* input,
                               const PostProcessSettings& settings) {
    constexpr u32 kSize = 4;
    Object output = CreateTexture(device,
                                  TextureDesc{.pixel_format = kBGRA8,
                                              .width = kSize,
                                              .height = kSize,
                                              .render_target = true},
                                  "test output", nullptr);
    id<MTLBuffer> readback = SharedBuffer(device, kSize * kSize * 4);
    Encoder encoder{device};
    passes.PostProcess(encoder, input, output.Get(), kSize, kSize, settings);
    encoder.CopyTextureToBuffer(output.Get(), 0, 0, {}, {kSize, kSize, 1}, Ref(readback),
                                kSize * 4, kSize * kSize * 4);
    device.Wait(encoder.Submit());
    const u8* bytes = static_cast<const u8*>(readback.contents);
    return {bytes, bytes + kSize * kSize * 4};
}

bool Near(int a, int b) {
    return std::abs(a - b) <= 1;
}

void TestFitImage() {
    auto same = [](FitRect r, s32 x, s32 y, u32 w, u32 h) {
        return r.x == x && r.y == y && r.width == w && r.height == h;
    };
    Expect(same(FitImage(1920, 1080, 1920, 1200), 0, 60, 1920, 1080), "FitImage 16:9 in 16:10");
    Expect(same(FitImage(1920, 1080, 2560, 1080), 320, 0, 1920, 1080), "FitImage 16:9 in 21:9");
    Expect(same(FitImage(1280, 720, 3456, 2234), 0, 145, 3456, 1944),
           "FitImage 720p in a 16\" MacBook Pro's Retina size");
    Expect(same(FitImage(1280, 720, 2560, 1440), 0, 0, 2560, 1440), "FitImage equal aspect");
    Expect(same(FitImage(16, 9, 32, 32), 0, 7, 32, 18), "FitImage 16x9 in 32x32");
    Expect(same(FitImage(0, 1080, 1920, 1080), 0, 0, 0, 0), "FitImage zero source width");
    Expect(same(FitImage(1920, 0, 1920, 1080), 0, 0, 0, 0), "FitImage zero source height");
    Expect(same(FitImage(1920, 1080, 0, 1080), 0, 0, 0, 0), "FitImage zero target width");
    Expect(same(FitImage(1920, 1080, 1920, 0), 0, 0, 0, 0), "FitImage zero target height");
}

void TestShouldAcquireDrawable() {
    Expect(ShouldAcquireDrawable(true, 1920, 1080, false), "visible window: acquire");
    Expect(!ShouldAcquireDrawable(false, 1920, 1080, false),
           "minimised/hidden/occluded window keeps its size: skip (nextDrawable would block)");
    Expect(ShouldAcquireDrawable(false, 1920, 1080, true),
           "snapshot request on an occluded window: acquire (locked-screen snapshots)");
    Expect(!ShouldAcquireDrawable(true, 0, 1080, false), "zero width: skip");
    Expect(!ShouldAcquireDrawable(true, 1920, 0, true), "zero height: skip, even for a snapshot");
}

/// 4x4 texels whose bytes cycle through {0, 1, 64, 128, 200, 255}.
std::vector<u8> TestBytes() {
    constexpr u8 values[] = {0, 1, 64, 128, 200, 255};
    std::vector<u8> bytes(64);
    for (u32 i = 0; i < bytes.size(); ++i) {
        bytes[i] = values[i % 6];
    }
    return bytes;
}

void TestPostProcessGamma(Device& device, PresentPasses& passes) {
    const std::vector<u8> src = TestBytes();
    Object source = Upload(device, kBGRA8Srgb, 4, 4, src.data());

    const std::vector<u8> unit = RunPostProcess(device, passes, source.Get(), {.gamma = 1.0f});
    bool ok = true;
    for (u32 i = 0; i < src.size(); ++i) {
        if (!Near(unit[i], src[i])) {
            std::printf("  gamma 1.0: byte %u = %u, source %u\n", i, unit[i], src[i]);
            ok = false;
        }
    }
    Expect(ok, "post-process at gamma 1.0 round-trips sRGB bytes");

    const std::vector<u8> bright = RunPostProcess(device, passes, source.Get(), {.gamma = 1.2f});
    ok = true;
    bool differs = false;
    for (u32 i = 0; i < src.size(); ++i) {
        const float c = float(src[i]) / 255.0f;
        const u8 expected = (i % 4 == 3) ? src[i] : ToUnorm8(Gamma(SrgbToLinear(c), 1.2f));
        differs |= std::abs(int(expected) - int(src[i])) > 2;
        if (!Near(bright[i], expected)) {
            std::printf("  gamma 1.2: byte %u = %u, expected %u\n", i, bright[i], expected);
            ok = false;
        }
    }
    Expect(ok, "post-process at gamma 1.2 matches post_process.frag");
    Expect(differs, "gamma 1.2 changes some bytes");
}

void TestPostProcessSrgbInput(Device& device, PresentPasses& passes) {
    // 10-bit channels: R, G, B at bits 0, 10, 20; alpha 2 bits at 30.
    constexpr u32 values[] = {0, 3, 40, 150, 300, 512, 700, 900, 1023};
    std::vector<u32> texels(16);
    std::vector<std::array<u32, 3>> channels(16);
    for (u32 i = 0; i < 16; ++i) {
        const std::array<u32, 3> c{values[i % 9], values[(i + 3) % 9], values[(i + 6) % 9]};
        channels[i] = c;
        texels[i] = c[0] | (c[1] << 10) | (c[2] << 20) | (3u << 30);
    }
    Object source = Upload(device, kRGB10A2, 4, 4, texels.data());
    constexpr float kG = 1.2f;
    const std::vector<u8> out =
        RunPostProcess(device, passes, source.Get(), {.gamma = kG, .srgb_input = 1});
    bool ok = true;
    bool differs = false;
    for (u32 i = 0; i < 16; ++i) {
        // BGRA8 output: byte 0 = B, 1 = G, 2 = R, 3 = A.
        const u32 rgb_of_byte[3] = {2, 1, 0};
        for (u32 b = 0; b < 3; ++b) {
            const float x = float(channels[i][rgb_of_byte[b]]) / 1023.0f;
            const u8 expected = ToUnorm8(Gamma(Degamma(x), kG));
            differs |= std::abs(int(expected) - int(ToUnorm8(x))) > 2;
            if (!Near(out[i * 4 + b], expected)) {
                std::printf("  sRGB input: texel %u byte %u = %u, expected %u\n", i, b,
                            out[i * 4 + b], expected);
                ok = false;
            }
        }
        if (out[i * 4 + 3] != 255) {
            std::printf("  sRGB input: texel %u alpha %u\n", i, out[i * 4 + 3]);
            ok = false;
        }
    }
    Expect(ok, "post-process with srgb_input matches gamma(degamma(x))");
    Expect(differs, "srgb_input test values are not the identity");
}

void TestAlphaSwizzle(Device& device, PresentPasses& passes) {
    std::vector<u8> src(64);
    for (u32 i = 0; i < 16; ++i) {
        src[i * 4 + 0] = 10;
        src[i * 4 + 1] = 20;
        src[i * 4 + 2] = 30;
        src[i * 4 + 3] = 0;
    }
    Object source = Upload(device, kBGRA8Srgb, 4, 4, src.data(), true);
    Object view = CreateTextureView(source.Get(), kBGRA8Srgb, TextureType::Tex2D, 0, 1, 0, 1,
                                    {MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
                                     MTLTextureSwizzleBlue, MTLTextureSwizzleOne});
    Expect(bool(view), "alpha-One view created");
    if (!view) {
        return;
    }
    const std::vector<u8> out = RunPostProcess(device, passes, view.Get(), {.gamma = 1.0f});
    bool ok = true;
    for (u32 i = 0; i < 16; ++i) {
        ok &= out[i * 4 + 3] == 255 && Near(out[i * 4 + 0], 10) && Near(out[i * 4 + 2], 30);
    }
    Expect(ok, "alpha 0 through a swizzle-One view comes out 255");
}

/// The post-process pass after the rasterizer left a depth-stencil state bound: its "no depth
/// state" must not reach Metal as nil (validation: "depthStencilState must not be nil", seen once
/// per frame in game). Under MTL_DEBUG_LAYER=1 a nil state aborts here.
void TestPostProcessAfterDepthState(Device& device, PresentPasses& passes) {
    constexpr u32 kSize = 4;
    const std::vector<u8> src = TestBytes();
    Object input = Upload(device, kBGRA8Srgb, kSize, kSize, src.data());
    Object output = CreateTexture(device,
                                  TextureDesc{.pixel_format = kBGRA8,
                                              .width = kSize,
                                              .height = kSize,
                                              .render_target = true},
                                  "test output", nullptr);
    MTLDepthStencilDescriptor* desc = [MTLDepthStencilDescriptor new];
    desc.depthCompareFunction = MTLCompareFunctionLess;
    desc.depthWriteEnabled = YES;
    id<MTLDepthStencilState> depth_state = [Dev(device) newDepthStencilStateWithDescriptor:desc];
    id<MTLBuffer> readback = SharedBuffer(device, kSize * kSize * 4);
    Encoder encoder{device};
    // Twice: a fresh pass re-applies the state used before it.
    for (int i = 0; i < 2; ++i) {
        encoder.SetDepthStencil((__bridge void*)depth_state);
        passes.PostProcess(encoder, input.Get(), output.Get(), kSize, kSize, {.gamma = 1.0f});
    }
    encoder.CopyTextureToBuffer(output.Get(), 0, 0, {}, {kSize, kSize, 1}, Ref(readback),
                                kSize * 4, kSize * kSize * 4);
    device.Wait(encoder.Submit());
    const u8* out = static_cast<const u8*>(readback.contents);
    bool ok = true;
    for (u32 i = 0; i < kSize * kSize; ++i) {
        ok &= Near(out[i * 4 + 0], src[i * 4 + 0]) && Near(out[i * 4 + 1], src[i * 4 + 1]) &&
              Near(out[i * 4 + 2], src[i * 4 + 2]);
    }
    Expect(ok, "post-process after a depth-stencil state: gamma 1.0 round-trips");
}

CAMetalLayer* OffscreenLayer(Device& device, u32 width, u32 height, u32 max_drawables) {
    CAMetalLayer* layer = [CAMetalLayer layer];
    ConfigureLayer(device, (__bridge void*)layer,
                   LayerConfig{.max_drawables = max_drawables, .framebuffer_only = false});
    layer.drawableSize = CGSizeMake(width, height);
    return layer;
}

/// A 16x9 solid red BGRA8 frame.
Object RedFrame(Device& device) {
    std::vector<u32> red(16 * 9, 0xffff0000u); // BGRA bytes 00 00 ff ff
    return Upload(device, kBGRA8, 16, 9, red.data());
}

void TestLayerConfig(Device& device) {
    CAMetalLayer* layer = [CAMetalLayer layer];
    ConfigureLayer(device, (__bridge void*)layer,
                   LayerConfig{.max_drawables = 3, .display_sync = false});
    Expect(layer.device == Dev(device), "layer device");
    Expect(layer.pixelFormat == MTLPixelFormatBGRA8Unorm, "layer pixel format");
    Expect(layer.maximumDrawableCount == 3, "layer maximumDrawableCount");
    Expect(!layer.displaySyncEnabled, "layer displaySyncEnabled");
    Expect(layer.framebufferOnly, "layer framebufferOnly");
    Expect(layer.colorspace != nil &&
               CFStringCompare(CGColorSpaceGetName(layer.colorspace), kCGColorSpaceSRGB, 0) ==
                   kCFCompareEqualTo,
           "layer sRGB colorspace");
}

void TestPresentLetterbox(Device& device, PresentPasses& passes, PresentQueue& queue) {
    constexpr u32 kW = 32, kH = 32;
    Object frame = RedFrame(device);
    CAMetalLayer* layer = OffscreenLayer(device, kW, kH, 2);
    id<MTLBuffer> readback = SharedBuffer(device, kW * kH * 4);
    std::atomic<int> completed{0};
    WithAutoreleasePool([&] {
        Drawable drawable = NextDrawable((__bridge void*)layer);
        Expect(bool(drawable.drawable) && drawable.texture != nullptr, "offscreen drawable");
        if (!drawable.drawable) {
            return;
        }
        Expect(drawable.width == kW && drawable.height == kH, "drawable size");
        void* cb = queue.Begin();
        passes.Present(cb, frame.Get(), 16, 9, drawable.texture, kW, kH);
        id<MTLCommandBuffer> command_buffer = (__bridge id<MTLCommandBuffer>)cb;
        id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
        [blit copyFromTexture:(__bridge id<MTLTexture>)drawable.texture
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(kW, kH, 1)
                            toBuffer:readback
                   destinationOffset:0
              destinationBytesPerRow:kW * 4
            destinationBytesPerImage:kW * kH * 4];
        [blit endEncoding];
        queue.Commit(cb, nullptr, [&completed] { ++completed; });
    });
    queue.WaitIdle();
    Expect(completed == 1, "present completion callback ran");
    const FitRect fit = FitImage(16, 9, kW, kH);
    const u32* px = static_cast<const u32*>(readback.contents);
    bool ok = true;
    for (u32 y = 0; y < kH; ++y) {
        const bool inside = s32(y) >= fit.y && y < fit.y + fit.height;
        for (u32 x = 0; x < kW; ++x) {
            const u32 expected = inside ? 0xffff0000u : 0xff000000u;
            if (px[y * kW + x] != expected) {
                if (ok) {
                    std::printf("  letterbox: pixel (%u, %u) = %08x, expected %08x\n", x, y,
                                px[y * kW + x], expected);
                }
                ok = false;
            }
        }
    }
    Expect(ok, "letterboxed present: red inside the fit rect, black outside");

    // No frame: the target is only cleared.
    WithAutoreleasePool([&] {
        Drawable drawable = NextDrawable((__bridge void*)layer);
        if (!drawable.drawable) {
            Expect(false, "second offscreen drawable");
            return;
        }
        void* cb = queue.Begin();
        passes.Present(cb, nullptr, 0, 0, drawable.texture, kW, kH);
        id<MTLBlitCommandEncoder> blit = [(__bridge id<MTLCommandBuffer>)cb blitCommandEncoder];
        [blit copyFromTexture:(__bridge id<MTLTexture>)drawable.texture
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(kW, kH, 1)
                            toBuffer:readback
                   destinationOffset:0
              destinationBytesPerRow:kW * 4
            destinationBytesPerImage:kW * kH * 4];
        [blit endEncoding];
        queue.Commit(cb, nullptr, {});
    });
    queue.WaitIdle();
    ok = true;
    for (u32 i = 0; i < kW * kH; ++i) {
        ok &= px[i] == 0xff000000u;
    }
    Expect(ok, "present without a frame clears to black");
}

/// Debug snapshot (BB_SNAPSHOT_TRIGGER): the letterboxed drawable is read back on the present
/// command buffer and written as a PNG once it completed.
void TestSnapshot(Device& device, PresentPasses& passes, PresentQueue& queue) {
    static constexpr u32 kW = 32, kH = 32;
    Object frame = RedFrame(device);
    CAMetalLayer* layer = OffscreenLayer(device, kW, kH, 2);
    char path[] = "/tmp/mtl-present-test-XXXXXX.png";
    const int fd = mkstemps(path, 4);
    Expect(fd >= 0, "snapshot temp file");
    close(fd);
    std::atomic<bool> written{false};
    Object readback; // lives until the command buffer completed
    WithAutoreleasePool([&] {
        Drawable drawable = NextDrawable((__bridge void*)layer);
        if (!drawable.drawable) {
            Expect(false, "snapshot drawable");
            return;
        }
        void* cb = queue.Begin();
        passes.Present(cb, frame.Get(), 16, 9, drawable.texture, kW, kH);
        readback = EncodeSnapshot(device, cb, drawable.texture, kW, kH);
        Expect(bool(readback), "snapshot readback buffer");
        void* const buffer = readback.Get();
        queue.Commit(cb, nullptr, [&written, buffer, &path] {
            written = WriteSnapshotPng(path, buffer, kW, kH);
        });
    });
    queue.WaitIdle();
    Expect(written, "snapshot PNG written");
    // Not BGRA8 (an HDR layer): no snapshot rather than wrong bytes.
    Object half = CreateTexture(device,
                                TextureDesc{.pixel_format = MTLPixelFormatRGBA16Float,
                                            .width = kW,
                                            .height = kH,
                                            .render_target = true},
                                "test half", nullptr);
    WithAutoreleasePool([&] {
        void* cb = queue.Begin();
        Expect(!EncodeSnapshot(device, cb, half.Get(), kW, kH), "no snapshot of RGBA16Float");
        queue.Commit(cb, nullptr, {});
    });
    queue.WaitIdle();
    NSData* data = [NSData dataWithContentsOfFile:@(path)];
    CGImageSourceRef source =
        data ? CGImageSourceCreateWithData((__bridge CFDataRef)data, nullptr) : nullptr;
    CGImageRef image = source ? CGImageSourceCreateImageAtIndex(source, 0, nullptr) : nullptr;
    Expect(image != nullptr, "snapshot PNG decodes");
    if (image != nullptr) {
        Expect(CGImageGetWidth(image) == kW && CGImageGetHeight(image) == kH, "snapshot size");
        // Redraw into known RGBA8 bytes to compare pixels.
        std::vector<u8> rgba(kW * kH * 4);
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef ctx = CGBitmapContextCreate(rgba.data(), kW, kH, 8, kW * 4, srgb,
                                                 kCGImageAlphaPremultipliedLast);
        CGContextDrawImage(ctx, CGRectMake(0, 0, kW, kH), image);
        CGContextRelease(ctx);
        CGColorSpaceRelease(srgb);
        const u8* centre = &rgba[(kH / 2 * kW + kW / 2) * 4];
        const u8* corner = &rgba[0];
        Expect(centre[0] == 255 && centre[1] == 0 && centre[2] == 0 && centre[3] == 255,
               "snapshot: red inside the fit rect");
        Expect(corner[0] == 0 && corner[1] == 0 && corner[2] == 0 && corner[3] == 255,
               "snapshot: black letterbox");
        CGImageRelease(image);
    }
    if (source != nullptr) {
        CFRelease(source);
    }
    unlink(path);
}

void TestFramePair(Device& device, PresentPasses& passes, PresentQueue& queue, bool present_real) {
    CAMetalLayer* layer = OffscreenLayer(device, 64, 36, 2);
    Object source = RedFrame(device);
    auto packet = std::make_shared<int>(1);
    std::weak_ptr<int> lifetime = packet;
    const u64 tick = device.CurrentTick();
    std::atomic<int> done{0};
    WithAutoreleasePool([&] {
        auto generated = NextDrawable((__bridge void*)layer);
        Expect(bool(generated.drawable), "generated drawable acquired independently");
        if (!generated.drawable) return;
        void* first = queue.Begin();
        queue.WaitForDeviceTick(first, tick);
        passes.Present(first, source.Get(), 64, 36, generated.texture, 64, 36);
        queue.Commit(first, generated.drawable.Get(), [packet, &done] { ++done; }, .97 / 120);
    });
    Expect(queue.InFlight() == 1 && done == 0,
           "generated image submitted before acquiring the real drawable");
    if (present_real) {
        WithAutoreleasePool([&] {
            auto real = NextDrawable((__bridge void*)layer);
            Expect(bool(real.drawable), "two-drawable layer admits the next image after submission");
            if (!real.drawable) return;
            void* second = queue.Begin();
            queue.WaitForDeviceTick(second, tick);
            passes.Present(second, source.Get(), 64, 36, real.texture, 64, 36);
            queue.Commit(second, real.drawable.Get(), [packet, &done] { ++done; }, .97 / 120);
        });
    }
    packet.reset();
    Expect(!lifetime.expired() && done == 0,
           "submitted images retain the source even when the real acquisition is skipped");
    Encoder render{device};
    render.CommandBuffer();
    const u64 submitted = render.Submit();
    Expect(submitted == tick, "pair waits for the correct render submission");
    queue.WaitIdle();
    // Completion handlers release captured state just after notifying WaitIdle.
    for (int n = 0; !lifetime.expired() && n < 100; ++n)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Expect(done == (present_real ? 2 : 1) && lifetime.expired(),
           "source retires after every submitted image completes");
}

void TestEventWait(Device& device, PresentQueue& queue) {
    constexpr u32 kPattern = 0x5a5a5a5au;
    id<MTLBuffer> gpu_only = [Dev(device) newBufferWithLength:256
                                                      options:MTLResourceStorageModePrivate];
    id<MTLBuffer> result = SharedBuffer(device, 256);

    // The present-queue command buffer is committed first and waits for the main queue's next
    // tick: without the wait it would copy the buffer before the fill.
    const u64 tick = device.CurrentTick();
    std::atomic<int> done{0};
    std::atomic<int> displayed{0};
    WithAutoreleasePool([&] {
        void* cb = queue.Begin();
        queue.WaitForDeviceTick(cb, tick);
        id<MTLBlitCommandEncoder> blit = [(__bridge id<MTLCommandBuffer>)cb blitCommandEncoder];
        [blit copyFromBuffer:gpu_only sourceOffset:0 toBuffer:result destinationOffset:0 size:256];
        [blit endEncoding];
        queue.Commit(cb, nullptr, [&done] { ++done; }, 0,
                     [&displayed](double) { ++displayed; });
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Expect(queue.InFlight() == 1 && done == 0, "present command buffer waits for the device tick");
    Encoder encoder{device};
    encoder.FillBuffer(Ref(gpu_only), 256, kPattern);
    const u64 committed = encoder.Submit();
    Expect(committed == tick, "fill signalled the predicted tick");
    while (device.CurrentTick() <= tick) { // never hang the queue on a mispredicted tick
        Encoder extra{device};
        extra.CommandBuffer();
        extra.Submit();
    }
    queue.WaitIdle();
    u32 value;
    std::memcpy(&value, result.contents, 4);
    Expect(done == 1 && value == kPattern, "present queue reads what the device tick wrote");
    Expect(displayed == 0, "GPU completion without a displayed drawable is not displayed FPS");

    // Tick 0: no wait.
    std::memset(result.contents, 0, 256);
    WithAutoreleasePool([&] {
        void* cb = queue.Begin();
        queue.WaitForDeviceTick(cb, 0);
        id<MTLBlitCommandEncoder> blit = [(__bridge id<MTLCommandBuffer>)cb blitCommandEncoder];
        [blit fillBuffer:result range:NSMakeRange(0, 4) value:0x11];
        [blit endEncoding];
        queue.Commit(cb, nullptr, [&done] { ++done; });
    });
    queue.WaitIdle();
    std::memcpy(&value, result.contents, 4);
    Expect(done == 2 && value == 0x11111111u, "tick 0 does not wait");
    Expect(queue.InFlight() == 0, "nothing in flight after WaitIdle");
}

void TestAutoreleaseLoop(Device& device, PresentPasses& passes, PresentQueue& queue) {
    Object frame = RedFrame(device);
    CAMetalLayer* layer = OffscreenLayer(device, 64, 36, 2);
    int missing = 0;
    std::atomic<int> completed{0};
    for (int i = 0; i < 200; ++i) {
        WithAutoreleasePool([&] {
            Drawable drawable = NextDrawable((__bridge void*)layer);
            if (!drawable.drawable) {
                ++missing;
                return;
            }
            void* cb = queue.Begin();
            passes.Present(cb, frame.Get(), 16, 9, drawable.texture, drawable.width,
                           drawable.height);
            queue.Commit(cb, nullptr, [&completed] { ++completed; });
        });
    }
    queue.WaitIdle();
    if (missing) {
        std::printf("  %d of 200 drawables were nil\n", missing);
    }
    Expect(missing == 0, "200 presents in autorelease pools all get a drawable");
    Expect(completed == 200, "200 completion callbacks");
    Expect(queue.InFlight() == 0, "InFlight back to 0 after WaitIdle");
}

void TestDestructorWaits(Device& device) {
    std::atomic<int> callbacks{0};
    constexpr int kCount = 5;
    id<MTLBuffer> big = [Dev(device) newBufferWithLength:64u << 20
                                                 options:MTLResourceStorageModePrivate];
    id<MTLBuffer> big2 = [Dev(device) newBufferWithLength:64u << 20
                                                  options:MTLResourceStorageModePrivate];
    bool in_flight_seen = false;
    {
        PresentQueue queue{device};
        for (int i = 0; i < kCount; ++i) {
            WithAutoreleasePool([&] {
                void* cb = queue.Begin();
                id<MTLBlitCommandEncoder> blit =
                    [(__bridge id<MTLCommandBuffer>)cb blitCommandEncoder];
                [blit copyFromBuffer:big sourceOffset:0 toBuffer:big2 destinationOffset:0
                                size:big.length];
                [blit endEncoding];
                queue.Commit(cb, nullptr, [&callbacks] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    ++callbacks;
                });
            });
        }
        in_flight_seen = queue.InFlight() > 0;
    }
    Expect(in_flight_seen, "command buffers in flight before the destructor");
    Expect(callbacks == kCount, "~PresentQueue returns after every completion callback ran");
}

// A present stuck behind a device tick that is not committed yet: WaitIdleFor and the destructor
// give up after their timeout instead of hanging, and the late completion still runs safely.
void TestBoundedWaits(Device& device) {
    auto advance_to = [&device](u64 tick) {
        while (device.CurrentTick() <= tick) {
            Encoder extra{device};
            extra.CommandBuffer();
            extra.Submit();
        }
    };
    auto stuck_present = [](PresentQueue& queue, u64 tick, std::atomic<int>& done) {
        WithAutoreleasePool([&] {
            void* cb = queue.Begin();
            queue.WaitForDeviceTick(cb, tick);
            queue.Commit(cb, nullptr, [&done] { ++done; });
        });
    };
    std::atomic<int> done{0};
    {
        PresentQueue queue{device};
        const u64 tick = device.CurrentTick() + 1;
        stuck_present(queue, tick, done);
        const auto start = std::chrono::steady_clock::now();
        const bool idle = queue.WaitIdleFor(50);
        const auto waited = std::chrono::steady_clock::now() - start;
        Expect(!idle && done == 0, "WaitIdleFor times out while a present waits for a tick");
        Expect(waited >= std::chrono::milliseconds(45) && waited < std::chrono::seconds(2),
               "WaitIdleFor waits about its timeout");
        advance_to(tick);
        Expect(queue.WaitIdleFor(5000) && done == 1, "WaitIdleFor returns once the tick came");
    }
    {
        const u64 tick = device.CurrentTick() + 1;
        const auto start = std::chrono::steady_clock::now();
        {
            PresentQueue queue{device, 100};
            stuck_present(queue, tick, done);
        } // logs an error and leaves the command buffer behind
        const auto waited = std::chrono::steady_clock::now() - start;
        Expect(done == 1 && waited < std::chrono::seconds(2),
               "~PresentQueue gives up after its shutdown timeout");
        advance_to(tick);
        for (int i = 0; i < 500 && done != 2; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        Expect(done == 2, "the abandoned present still completes (its state was kept)");
    }
}

} // namespace

int main() {
    @autoreleasepool {
        TestFitImage();
        TestShouldAcquireDrawable();
        Device device;
        PresentPasses passes{device, kBGRA8, kBGRA8};
        TestPostProcessGamma(device, passes);
        TestPostProcessSrgbInput(device, passes);
        TestAlphaSwizzle(device, passes);
        TestPostProcessAfterDepthState(device, passes);
        TestLayerConfig(device);
        {
            PresentQueue queue{device};
            TestPresentLetterbox(device, passes, queue);
            TestSnapshot(device, passes, queue);
            TestFramePair(device, passes, queue, true);
            TestFramePair(device, passes, queue, false);
            TestEventWait(device, queue);
            TestAutoreleaseLoop(device, passes, queue);
        }
        TestDestructorWaits(device);
        TestBoundedWaits(device);
        Expect(device.ErrorCount() == 0, "no command buffer errors on the device queue");
    }
    if (failures) {
        std::printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    std::printf("PASS: Metal-native present helper\n");
    return 0;
}
