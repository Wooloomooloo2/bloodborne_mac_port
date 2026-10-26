// SPDX-FileCopyrightText: Copyright 2026 Ronny Stiftel
// SPDX-License-Identifier: GPL-2.0-or-later
// Offscreen GPU regression: snapshot ownership, HUD restoration, video-out
// sRGB, warmup/cuts/gaps, and bounded pool fallback. No game files or window
// required.
#include "bbport_framegen.h"
#include "bbport_settings.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_texture.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#include <tuple>
#include <vector>

namespace {
int failures = 0;
void Expect(bool ok, const char* message) {
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}
BbFrameGen::Description Description() {
    return {.width = 64,
            .height = 36,
            .out_width = 128,
            .out_height = 72,
            .ui_format = MTLPixelFormatRGBA8Unorm,
            .sequence = 1,
            .interval = 1.f / 30.f,
            .near_plane = .1f,
            .far_plane = 3000,
            .vertical_fov = 1.f};
}
void Policy() {
    auto& settings = BbSettings::Get();
    Expect(!BbFrameGen::Requested(), "frame generation starts disabled");
    settings.framegen = true;
    Expect(!BbFrameGen::Requested(), "menu changes cannot enable an unprepared presenter");
    settings.startup_framegen = true;
    Expect(BbFrameGen::Requested(), "saved launch setting enables generation");
    settings.framegen = false;
    Expect(BbFrameGen::Requested(), "menu changes keep in-flight frame ownership until restart");
    settings.startup_framegen = false;
    for (const auto [hz, cap, supported] : {
             std::tuple{60.0, 30u, true}, {60.0, 60u, false}, {120.0, 60u, true},
             {120.0, 120u, false}, {144.0, 60u, false}, {160.0, 60u, false},
             {240.0, 60u, true}, {360.0, 60u, true}, {240.0, 120u, true},
             {80.0, 40u, true}, {120.0, 40u, false}, {180.0, 90u, true},
             {59.94, 30u, true}, {119.88, 60u, true}, {239.76, 60u, true},
             {119.0, 60u, false}, {121.0, 60u, false}, {0.0, 30u, false}, {120.0, 0u, false},
             {-1.0, 30u, false}, {std::numeric_limits<double>::quiet_NaN(), 30u, false}}) {
        Expect(BbFrameGen::DisplaySupported(hz, cap) == supported,
               "fixed refresh must be a whole multiple of twice the cap within 0.2 percent");
        if (supported) {
            const double hold = BbFrameGen::PresentDuration(1.f / cap, cap);
            const double refreshes = std::ceil(hold * hz);
            const double paced_real_rate = hz / (2.0 * refreshes);
            Expect(std::abs(paced_real_rate / cap - 1) <= .002,
                   "accepted fixed modes do not add refreshes that reduce the real rate beyond tolerance");
        }
    }
    for (const auto [maximum, minimum, cap, supported] : {
             std::tuple{144.0, 48.0, 60u, true}, {160.0, 48.0, 60u, true},
             {119.88, 23.976, 60u, true},
             {160.0, 48.0, 40u, true}, {160.0, 100.0, 40u, false},
             {160.0, 48.0, 90u, false}, {160.0, 0.0, 60u, false},
             {160.0, 160.0, 60u, false}, {160.0, 200.0, 60u, false},
             {160.0, std::numeric_limits<double>::quiet_NaN(), 60u, false}}) {
        Expect(BbFrameGen::DisplaySupported(maximum, cap, minimum) == supported,
               "active variable refresh allows only paired rates inside its range");
    }
    settings.startup_framegen = settings.framegen_supported = true;
    settings.upscaler = BbSettings::UpscalerMetalFx;
    settings.framegen_real_cap = 60;
    settings.display_timing = {120, 0};
    Expect(BbFrameGen::Active(), "supported launch activates generation at 120 Hz / 60 FPS");
    settings.display_timing = {60, 0};
    Expect(!BbFrameGen::Active(), "moving the window to 60 Hz suspends generation");
    settings.display_timing = {144, 0};
    Expect(!BbFrameGen::Active(), "moving to fixed 144 Hz suspends capture and paired presentation");
    settings.display_timing = {160, 0};
    Expect(!BbFrameGen::Active(), "fixed 160 Hz cannot activate paired presentation");
    settings.display_timing = {160, 48};
    Expect(BbFrameGen::Active(), "active 48-160 Hz variable refresh resumes generation");
    settings.display_timing = {160, 0};
    Expect(!BbFrameGen::Active(), "leaving Adaptive-Sync suspends generation on an incompatible fixed mode");
    settings.display_timing = {120, 0};
    Expect(BbFrameGen::Active(), "moving back to a suitable display resumes generation");
    settings.upscaler = BbSettings::UpscalerOff;
    Expect(!BbFrameGen::Active(), "native output without a temporal upscaler cannot activate generation");
    settings.upscaler = BbSettings::UpscalerMetalFx;
    settings.startup_framegen = false;
    const auto previous = Description();
    auto next = previous;
    ++next.sequence;
    Expect(BbFrameGen::Continuous(previous, next), "adjacent real frames share history");
    next.reset = true;
    Expect(!BbFrameGen::Continuous(previous, next), "camera cut discards history");
    next = previous;
    next.sequence += 2;
    Expect(!BbFrameGen::Continuous(previous, next), "missing scene/flip discards history");
    next = previous;
    Expect(!BbFrameGen::Continuous(previous, next), "reused real frame is not interpolated");
    next.sequence++;
    next.out_width++;
    Expect(!BbFrameGen::Continuous(previous, next), "resize discards history");
    next = previous;
    next.sequence++;
    next.vertical_fov *= 1.1f;
    Expect(!BbFrameGen::Continuous(previous, next), "projection change discards history");
    for (float interval : {0.f, -1.f, .2f, std::numeric_limits<float>::quiet_NaN()}) {
        next = previous;
        next.sequence++;
        next.interval = interval;
        Expect(!BbFrameGen::Continuous(previous, next), "invalid timing discards history");
        Expect(BbFrameGen::PresentDuration(interval, 60) == 0, "invalid timing cannot pace a pair");
    }
    for (float interval : {1.f / 120.f, 1.f / 60.f, 1.f / 30.f, 1.f / 20.f, 1.f / 12.f}) {
        Expect(std::abs(BbFrameGen::PresentDuration(interval, 120) - .97 / 240) < 1e-8,
               "drawable backpressure cannot lengthen the next pair's display hold");
    }
    Expect(std::abs(BbFrameGen::PresentDuration(1.f / 60.f, 30) - .97 / 60) < 1e-8,
           "a queued real frame cannot outrun the game frame cap");
    Expect(BbFrameGen::PresentDuration(1.f / 30.f, 0) == 0,
           "uncapped output adds no measured-cadence display hold");
}

id<MTLDevice> Dev(Metal::Device& device) { return (__bridge id<MTLDevice>)device.NativeDevice(); }
Metal::Object Texture(Metal::Device& device, uint32_t format, uint32_t w, uint32_t h) {
    return Metal::CreateTexture(
        device, {.pixel_format = format, .width = w, .height = h, .render_target = true},
        "framegen test", nullptr);
}
void Upload(Metal::Device& device, Metal::Encoder& e, void* texture, const void* data, uint32_t w,
            uint32_t h, uint32_t bytes) {
    auto buffer = [Dev(device) newBufferWithBytes:data
                                           length:w * h * bytes
                                          options:MTLResourceStorageModeShared];
    e.CopyBufferToTexture({(__bridge void*)buffer, 0}, w * bytes, w * h * bytes, texture, 0, 0, {},
                          {w, h, 1});
    e.KeepAlive(Metal::Object::Adopt((__bridge_retained void*)buffer));
}

void SceneDisplay(Metal::Device& device, Metal::Encoder& encoder, BbFrameGen::Generator& generator,
                  const BbFrameGen::FramePtr& frame, void* ui, void* displayed) {
    auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
        width:256 height:1 mipmapped:NO];
    d.textureType = MTLTextureType2DArray; d.arrayLength = 1;
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    auto lut = [Dev(device) newTextureWithDescriptor:d];
    std::vector<float> values(256);
    for (unsigned i=0; i<256; ++i) {
        const float x = float(i)/255;
        values[i] = x <= .04045f ? x/12.92f : std::pow((x+.055f)/1.055f,2.4f);
    }
    [lut replaceRegion:MTLRegionMake2D(0,0,256,1) mipmapLevel:0 slice:0
        withBytes:values.data() bytesPerRow:256*4 bytesPerImage:256*4];
    auto sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    auto sampler = [Dev(device) newSamplerStateWithDescriptor:sd];
    const Metal::Draw::Bound bindings[] = {{.texture=ui}, {.texture=(__bridge void*)lut},
                                           {.sampler=(__bridge void*)sampler}};
    Expect(generator.CaptureDisplay(encoder, frame, displayed, bindings),
           "retail gamma pass admits the captured scene view and LUT");
    encoder.KeepAlive(Metal::Object::Adopt((__bridge_retained void*)lut));
    encoder.KeepAlive(Metal::Object::Adopt((__bridge_retained void*)sampler));
}

void Gpu(Metal::Device& device) {
    using namespace BbFrameGen;
    Generator generator{device};
    auto description = Description();
    auto depth = Texture(device, MTLPixelFormatDepth32Float, 64, 36);
    auto motion = Texture(device, MTLPixelFormatRG16Float, 64, 36);
    auto ui = Texture(device, MTLPixelFormatRGBA8Unorm, 128, 72);
    auto displayed = Texture(device, MTLPixelFormatBGRA8Unorm_sRGB, 128, 72);
    Expect(depth && motion && ui && displayed, "source textures allocated");
    if (!depth || !motion || !ui || !displayed)
        return;
    std::vector<float> depth_data(64 * 36, .5f);
    std::vector<_Float16> motion_data(64 * 36 * 2, 0);
    motion_data[0] = std::numeric_limits<float>::quiet_NaN();
    std::vector<uint8_t> scene(128 * 72 * 4, 128), hud = scene, display = scene;
    for (uint32_t p = 0; p < 128 * 72; p++)
        scene[p * 4 + 3] = hud[p * 4 + 3] = display[p * 4 + 3] = 255;
    for (uint32_t y = 3; y < 10; y++)
        for (uint32_t x = 3; x < 18; x++) {
            const auto p = (y * 128 + x) * 4;
            hud[p] = 255;
            hud[p + 1] = hud[p + 2] = 0;
            display[p] = 0;
            display[p + 1] = 32;
            display[p + 2] = 255; // BGRA: red HUD
        }
    FramePtr last;
    for (uint32_t n = 0; n < 6; n++) {
        if (n == 2)
            ++description.sequence; // a skipped/missing frame
        description.reset = n == 4;
        if (n == 5) generator.Invalidate(); // menu/loading frame without scene inputs
        auto frame = generator.Prepare(description);
        Expect(bool(frame), "frame pool provides a free snapshot");
        if (!frame)
            return;
        Metal::Encoder encoder{device};
        Upload(device, encoder, depth.Get(), depth_data.data(), 64, 36, 4);
        Upload(device, encoder, motion.Get(), motion_data.data(), 64, 36, 4);
        Upload(device, encoder, ui.Get(), scene.data(), 128, 72, 4);
        generator.Capture(encoder, frame, depth.Get(), motion.Get(), ui.Get());
        // Inputs can be overwritten after Capture; their private snapshots must
        // survive.
        std::vector<_Float16> different_motion(64 * 36 * 2, 2);
        Upload(device, encoder, motion.Get(), different_motion.data(), 64, 36, 4);
        Upload(device, encoder, ui.Get(), hud.data(), 128, 72, 4);
        generator.FinishUi(encoder, frame, ui.Get());
        Upload(device, encoder, displayed.Get(), display.data(), 128, 72, 4);
        SceneDisplay(device, encoder, generator, frame, ui.Get(), displayed.Get());
        generator.Interpolate(encoder, frame, displayed.Get());
        auto readback = [Dev(device) newBufferWithLength:128 * 72 * 8
                                                 options:MTLResourceStorageModeShared];
        auto real = [Dev(device) newBufferWithLength:128 * 72 * 8
                                             options:MTLResourceStorageModeShared];
        auto scene_input = [Dev(device) newBufferWithLength:128 * 72 * 8 options:MTLResourceStorageModeShared];
        auto vectors = [Dev(device) newBufferWithLength:64 * 36 * 4
                                                options:MTLResourceStorageModeShared];
        encoder.CopyTextureToBuffer(frame->generated.Get(), 0, 0, {}, {128, 72, 1},
                                    {(__bridge void*)readback, 0}, 128 * 8, 128 * 72 * 8);
        encoder.CopyTextureToBuffer(frame->real.Get(), 0, 0, {}, {128, 72, 1},
                                    {(__bridge void*)real, 0}, 128 * 8, 128 * 72 * 8);
        encoder.CopyTextureToBuffer(frame->color.Get(), 0, 0, {}, {128,72,1},
            {(__bridge void*)scene_input,0},128*8,128*72*8);
        encoder.CopyTextureToBuffer(frame->motion.Get(), 0, 0, {}, {64, 36, 1},
                                    {(__bridge void*)vectors, 0}, 64 * 4, 64 * 36 * 4);
        device.Wait(encoder.Submit());
        const bool expected = n == 1 || n == 3;
        Expect(frame->interpolated.load() == expected,
               "GPU history gates warmup, gaps and camera cuts");
        const auto* a = static_cast<const _Float16*>(readback.contents);
        const auto* b = static_cast<const _Float16*>(real.contents);
        const auto* clean = static_cast<const _Float16*>(scene_input.contents);
        const auto hud_pixel = (5 * 128 + 5) * 4;
        Expect(std::abs(float(clean[hud_pixel])-.21586f)<.01f &&
               std::abs(float(clean[hud_pixel+2])-.21586f)<.01f,
               "interpolator color excludes the colored HUD and keeps display gamma");
        const auto centre = (36 * 128 + 64) * 4;
        Expect(std::abs(float(b[centre]) - .21586f) < .005f,
               "video-out view preserves sRGB decoding");
        Expect(float(b[(5 * 128 + 5) * 4]) > .99f && float(b[(5 * 128 + 5) * 4 + 2]) < .001f,
               "video-out view preserves BGRA channel mapping");
        if (expected) {
            Expect(std::isfinite(float(a[centre])) &&
                       std::abs(float(a[centre]) - float(b[centre])) < .03f,
                   "interpolated static scene stays finite and stable");
            Expect(std::memcmp(a + (5 * 128 + 5) * 4, b + (5 * 128 + 5) * 4, 8) == 0,
                   "current HUD is restored exactly after interpolation");
        }
        Expect(float(static_cast<const _Float16*>(vectors.contents)[0]) == 0,
               "motion snapshot sanitizes invalid vectors and survives later source writes");
        last = frame;
        ++description.sequence;
    }
    generator.Invalidate();
    auto after_reset = generator.Prepare(description);
    Expect(bool(after_reset), "pool remains usable after invalidation");

    Generator bounded{device};
    std::vector<FramePtr> held;
    for (size_t i = 0; i < MaxSnapshotSets; i++)
        held.push_back(bounded.Prepare(description));
    Expect(held[0] && held[1] && held[2] && !bounded.Prepare(description),
           "three pinned slots fall back without allocating or blocking");
    std::weak_ptr<Frame> unpresented = held.front();
    const auto* old_slot = held.front().get();
    held.erase(held.begin());
    Expect(unpresented.expired(), "an unpresented display reference cannot pin its snapshot");
    auto reused = bounded.Prepare(description);
    Expect(reused && reused.get() == old_slot, "released snapshot allocation is reused");
    Expect(!unpresented.lock(), "an old display reference cannot acquire the reused slot");
    reused.reset();

    // A frame without CPU owners still pins its slot while GPU commands use it.
    auto pending = bounded.Prepare(description);
    auto gate = [Dev(device) newSharedEvent];
    Metal::Encoder pending_encoder{device};
    [(__bridge id<MTLCommandBuffer>)pending_encoder.CommandBuffer() encodeWaitForEvent:gate value:1];
    bounded.Capture(pending_encoder, pending, depth.Get(), motion.Get(), ui.Get());
    pending.reset();
    Expect(!bounded.Prepare(description), "GPU completion pins an otherwise unowned snapshot slot");
    gate.signaledValue = 1;
    device.Wait(pending_encoder.Submit());
    FramePtr retired;
    for (int n = 0; !retired && n < 100; ++n) {
        retired = bounded.Prepare(description);
        if (!retired) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Expect(bool(retired), "GPU completion releases its snapshot slot for reuse");

    // A translating bright rectangle must land between the two real positions,
    // rather than merely returning the current real frame under a generated label.
    Generator moving{device};
    description = Description();
    for (uint32_t n = 0; n < 5; ++n) {
        const uint32_t left = 20 + n * 4;
        std::vector<uint8_t> image(128 * 72 * 4, 0);
        for (uint32_t p = 0; p < 128 * 72; ++p)
            image[p * 4 + 3] = 255;
        for (uint32_t y = 16; y < 56; ++y) {
            for (uint32_t x = left; x < left + 32; ++x) {
                const auto p = (y * 128 + x) * 4;
                image[p] = image[p + 1] = image[p + 2] = 255;
            }
        }
        // Four output pixels = two render pixels, current-to-previous.
        for (uint32_t p = 0; p < 64 * 36; ++p) {
            motion_data[p * 2] = -2;
            motion_data[p * 2 + 1] = 0;
        }
        auto frame = moving.Prepare(description);
        Expect(bool(frame), "moving-scene snapshot available");
        if (!frame)
            return;
        Metal::Encoder encoder{device};
        Upload(device, encoder, depth.Get(), depth_data.data(), 64, 36, 4);
        Upload(device, encoder, motion.Get(), motion_data.data(), 64, 36, 4);
        Upload(device, encoder, ui.Get(), image.data(), 128, 72, 4);
        moving.Capture(encoder, frame, depth.Get(), motion.Get(), ui.Get());
        moving.FinishUi(encoder, frame, ui.Get()); // no HUD, so no restoration should run
        Upload(device, encoder, displayed.Get(), image.data(), 128, 72, 4);
        SceneDisplay(device, encoder, moving, frame, ui.Get(), displayed.Get());
        moving.Interpolate(encoder, frame, displayed.Get());
        auto readback = [Dev(device) newBufferWithLength:128 * 72 * 8
                                                 options:MTLResourceStorageModeShared];
        encoder.CopyTextureToBuffer(frame->generated.Get(), 0, 0, {}, {128, 72, 1},
                                    {(__bridge void*)readback, 0}, 128 * 8, 128 * 72 * 8);
        device.Wait(encoder.Submit());
        if (n == 4) {
            const auto* pixels = static_cast<const _Float16*>(readback.contents);
            double sum = 0, weighted = 0;
            for (uint32_t y = 16; y < 56; ++y)
                for (uint32_t x = 0; x < 128; ++x) {
                    const float value = float(pixels[(y * 128 + x) * 4]);
                    if (std::isfinite(value)) {
                        sum += value;
                        weighted += x * value;
                    }
                }
            const double centre = sum > 0 ? weighted / sum : -1;
            std::printf("Moving rectangle: real centre %.1f, generated %.2f, midpoint %.1f\n",
                        left + 15.5, centre, left + 13.5);
            Expect(frame->interpolated.load() && std::abs(centre - (left + 13.5)) < 1.5,
                   "moving object is interpolated near the temporal midpoint");
        }
        ++description.sequence;
    }
}

void CroppedDisplay(Metal::Device& device) {
    // Native aspect patches can leave video-out smaller than the scene output.
    // Give the right/bottom edge a distinct color; every target pixel must use
    // an in-bounds source coordinate, including the expanded margins.
    for (const auto [w, h] : {std::pair{128u, 64u}, {96u, 72u}, {96u, 60u}}) {
        BbFrameGen::Generator generator{device};
        const auto description = Description();
        auto frame = generator.Prepare(description);
        Expect(bool(frame), "cropped display snapshot allocated");
        if (!frame) return;
        auto source = Texture(device, MTLPixelFormatRGBA8Unorm, w, h);
        std::vector<uint8_t> image(w * h * 4, 0);
        for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
            const auto p = (y * w + x) * 4;
            image[p] = x == w - 1 ? 255 : 0;
            image[p + 1] = y == h - 1 ? 255 : 0;
            image[p + 3] = 255;
        }
        Metal::Encoder encoder{device};
        Upload(device, encoder, source.Get(), image.data(), w, h, 4);
        // Warmup only: observe the color snapshot, independently of interpolation.
        generator.Interpolate(encoder, frame, source.Get());
        auto readback = [Dev(device) newBufferWithLength:128 * 72 * 8
                                                 options:MTLResourceStorageModeShared];
        encoder.CopyTextureToBuffer(frame->color.Get(), 0, 0, {}, {128, 72, 1},
                                    {(__bridge void*)readback, 0}, 128 * 8, 128 * 72 * 8);
        device.Wait(encoder.Submit());
        const auto* pixels = static_cast<const _Float16*>(readback.contents);
        bool matches = true;
        for (uint32_t y = 0; y < 72; ++y) for (uint32_t x = 0; x < 128; ++x) {
            const auto p = (y * 128 + x) * 4;
            matches &= float(pixels[p]) == (x >= w - 1 ? 1.f : 0.f) &&
                       float(pixels[p + 1]) == (y >= h - 1 ? 1.f : 0.f) &&
                       float(pixels[p + 3]) == 1.f;
        }
        Expect(matches, "display color clamps every pixel to the source width and height");
    }
}
} // namespace
int main() {
    Policy();
    @autoreleasepool {
        Metal::Device device;
        if (BbFrameGen::Supported(device)) {
            Gpu(device);
            CroppedDisplay(device);
        } else
            std::printf("SKIP: MetalFX frame interpolation unsupported\n");
    }
    std::printf("%s: frame generation\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
