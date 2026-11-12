// SPDX-FileCopyrightText: Copyright 2026 Ronny Stiftel
// SPDX-License-Identifier: GPL-2.0-or-later
// Offscreen GPU regression: snapshot ownership, HUD restoration, video-out
// sRGB, warmup/cuts/gaps, and bounded pool fallback. No game files or window
// required.
#include "bbport_framegen.h"
#include "bbport_framegen_budget.h"
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
#include <span>
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
             {120.0, 120u, false}, {144.0, 60u, true}, {160.0, 60u, true},
             {240.0, 60u, true}, {360.0, 60u, true}, {240.0, 120u, true},
             {80.0, 40u, true}, {120.0, 40u, true}, {180.0, 90u, true},
             {59.94, 30u, true}, {119.88, 60u, true}, {239.76, 60u, true},
             {119.0, 60u, false}, {121.0, 60u, true}, {0.0, 30u, false}, {120.0, 0u, false},
             {-1.0, 30u, false}, {std::numeric_limits<double>::quiet_NaN(), 30u, false}}) {
        Expect(BbFrameGen::DisplaySupported(hz, cap) == supported,
               "absolute pair deadlines require at least twice the cap within tolerance");
    }
    for (const auto [maximum, minimum, cap, supported] : {
             std::tuple{144.0, 48.0, 60u, true}, {160.0, 48.0, 60u, true},
             {119.88, 23.976, 60u, true},
             {160.0, 48.0, 40u, true}, {160.0, 100.0, 40u, false},
             {160.0, 48.0, 90u, false}, {160.0, 0.0, 60u, true},
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
    Expect(BbFrameGen::Active(), "absolute deadlines support fixed 144 Hz without a mode change");
    settings.display_timing = {160, 0};
    Expect(BbFrameGen::Active(), "absolute deadlines support fixed 160 Hz");
    settings.display_timing = {160, 48};
    Expect(BbFrameGen::Active(), "active 48-160 Hz variable refresh resumes generation");
    settings.display_timing = {160, 0};
    Expect(BbFrameGen::Active(), "leaving Adaptive-Sync keeps absolute pacing on fixed 160 Hz");
    settings.display_timing = {120, 0};
    Expect(BbFrameGen::Active(), "moving back to a suitable display resumes generation");
    settings.upscaler = BbSettings::UpscalerOff;
    Expect(BbFrameGen::Active(), "native output captures frame generation inputs without a temporal upscaler");
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
        Expect(BbFrameGen::PresentInterval(interval, 60) == 0, "invalid timing cannot pace a pair");
    }
    for (float interval : {1.f / 120.f, 1.f / 60.f, 1.f / 30.f, 1.f / 20.f, 1.f / 12.f}) {
        Expect(std::abs(BbFrameGen::PresentInterval(interval, 120) - 1. / 240) < 1e-8,
               "drawable backpressure cannot lengthen the next pair's display hold");
    }
    // Model refresh quantisation over all phases. Absolute deadlines are based on each
    // source, so a hold rounded to two refreshes cannot feed back into the next pair.
    // The old relative 8 ms holds needed four refreshes per pair at 144/160 Hz.
    for (double hz : {120., 144., 160., 240.}) {
        for (int phase = 0; phase < 100; ++phase) {
            const double offset = phase / (100. * hz);
            double previous_real = -1;
            double first_real = 0, last_real = 0;
            for (int frame = 0; frame < 600; ++frame) {
                // A single late source must not shift every following deadline.
                const double ready = offset + frame / 60. + (frame == 300 ? .025 : 0);
                const double real_deadline = ready + BbFrameGen::PresentInterval(1.f / 60.f, 60);
                const double shown = std::ceil(real_deadline * hz - 1e-9) / hz;
                if (frame == 0) first_real = shown;
                if (frame == 599) last_real = shown;
                if (frame > 301) {
                    Expect(shown - previous_real <= 1. / 60. + 1. / hz + 1e-9,
                           "late source does not accumulate a recurring pacing stall");
                }
                previous_real = shown;
            }
            Expect(std::abs(599. / (last_real - first_real) - 60.) < .1,
                   "non-integer refresh ratio preserves the real source rate");
        }
    }
    Expect(std::abs(BbFrameGen::PresentInterval(1.f / 60.f, 30) - 1. / 60) < 1e-8,
           "a queued real frame cannot outrun the game frame cap");
    Expect(BbFrameGen::PresentInterval(1.f / 30.f, 0) == 0,
           "uncapped output adds no measured-cadence display hold");
}

void BudgetPolicy() {
    BbFrameGen::GpuBudget budget;
    double now = 1;
    uint64_t busy = 0;
    uint64_t sequence = 0;
    const auto sample = [&](double gpu_ms, double frame_ms = 1000. / 60,
                            uint64_t draw_count = 1100, uint64_t config = 1) {
        now += frame_ms / 1000;
        busy += uint64_t(gpu_ms * 1e6);
        return budget.Observe(now, busy, draw_count > 100, 60, config, .004, ++sequence);
    };
    for (int i = 0; i < 120; ++i)
        Expect(!sample(1, 1000. / 60, 10), "loading frames cannot admit interpolation");
    for (int i = 0; i < 120; ++i) sample(7);
    Expect(sample(7), "spare GPU time admits generation after measuring real-only gameplay");
    sample(8, 100); // CPU/snapshot hitch, with little GPU work
    for (int i = 0; i < 60; ++i)
        Expect(sample(7), "an isolated CPU hitch does not disable generation");
    for (int i = 0; i < 90; ++i) sample(20, 1000. / 45);
    Expect(!sample(16), "sustained GPU overload returns to real-only presentation");
    for (int i = 0; i < 600; ++i)
        Expect(!sample(16), "saturated real-only output never periodically probes interpolation");
    for (int i = 0; i < 120; ++i) sample(6);
    Expect(sample(6), "lighter gameplay resumes generation with headroom for its measured cost");
    Expect(!sample(6, 1000. / 60, 1100, 2), "resolution/preset changes remeasure real-only cost");
    for (int i = 0; i < 120; ++i)
        Expect(!sample(16, 1000. / 60, 1100, 2), "GPU-bound baseline never enables generation");
    for (int i = 0; i < 120; ++i) sample(6, 1000. / 60, 1100, 2);
    Expect(sample(6, 1000. / 60, 1100, 2), "headroom recovers without changing the saved switch");
    Expect(!sample(6, 500, 1100, 2), "occlusion/pause discards stale cost estimates");
    BbFrameGen::GpuBudget unknown;
    for (int i = 0; i < 120; ++i)
        Expect(!unknown.Observe(1. + i / 60., 0, true, 60, 1),
               "missing GPU timestamps cannot claim spare time");

    // Real gameplay can exceed the old 70%-of-period startup limit while still leaving
    // enough room for interpolation. Model actual optional work only after admission.
    for (const uint32_t cap : {30u, 60u}) {
        BbFrameGen::GpuBudget moderate;
        now = 1; busy = 0; sequence = 0;
        bool active = false;
        const double source_ms = cap == 30 ? 25 : 13;
        double optional_ms = cap == 30 ? 3 : 1;
        const auto step = [&] {
            const double gpu_ms = source_ms + (active ? optional_ms : 0);
            now += std::max(1. / cap, gpu_ms / 1000);
            busy += uint64_t(gpu_ms * 1e6);
            if (active) ++sequence;
            active = moderate.Observe(now, busy, true, cap, 1, optional_ms / 1000, sequence);
            return active;
        };
        for (int i = 0; i < 120; ++i) step();
        Expect(active, "usable moderate GPU load is admitted at both 30 and 60 FPS");
        for (int i = 0; i < 120; ++i)
            Expect(step(), "measured inexpensive interpolation sustains the real-frame cap");
        optional_ms = 1000. / cap; // genuinely too expensive; learn and reject it
        for (int i = 0; i < 120; ++i) step();
        Expect(!active, "expensive interpolation still falls back under sustained pressure");
        for (int i = 0; i < 120; ++i)
            Expect(!step(), "measured expensive work is not periodically retried");
    }

    // The first MetalFX encode can include cold work. If a transition pauses generation
    // before more samples arrive, that single duration must not lock out later gameplay.
    BbFrameGen::GpuBudget cold;
    now = 1; busy = 0;
    auto observe = [&](double gpu_ms, bool scene, double optional_ms, uint64_t sample) {
        now += 1. / 60;
        busy += uint64_t(gpu_ms * 1e6);
        return cold.Observe(now, busy, scene, 60, 1, optional_ms / 1000, sample);
    };
    for (int i = 0; i < 120; ++i) observe(5, true, 0, 0);
    Expect(observe(5, true, 80, 1), "one cold interpolation sample does not suspend a light scene");
    observe(1, false, 80, 1);
    for (int i = 0; i < 120; ++i) observe(5, true, 80, 1);
    Expect(observe(5, true, 80, 1), "a repeated cold timestamp cannot permanently block resumption");
    for (uint64_t sample = 2; sample < 35; ++sample) observe(7, true, sample == 20 ? 90 : 4, sample);
    observe(1, false, 4, 34);
    for (int i = 0; i < 120; ++i) observe(7, true, 4, 34);
    Expect(observe(7, true, 4, 34), "one later GPU spike does not poison the measured resume cost");
    // Sustained expensive interpolation remains a real limit; do not keep probing it.
    for (uint64_t sample = 35; sample < 125; ++sample) observe(20, true, 16, sample);
    for (int i = 0; i < 120; ++i)
        Expect(!observe(7, true, 16, 124), "sustained optional cost still prevents unsafe resumption");
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
                  const BbFrameGen::FramePtr& frame, void* ui, void* displayed,
                  std::span<const float> curves = {}) {
    // The retail LUT: one 1-row curve per array slice (red, green, blue inputs: slices 2, 1, 0).
    const uint32_t slices = curves.empty() ? 3 : uint32_t(curves.size() / 256);
    auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
        width:256 height:1 mipmapped:NO];
    d.textureType = MTLTextureType2DArray; d.arrayLength = slices;
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    auto lut = [Dev(device) newTextureWithDescriptor:d];
    std::vector<float> values(256 * slices);
    for (unsigned i=0; i<256; ++i) {
        const float x = float(i)/255;
        for (uint32_t slice = 0; slice < slices; ++slice)
            values[slice * 256 + i] = x <= .04045f ? x/12.92f : std::pow((x+.055f)/1.055f,2.4f);
    }
    if (!curves.empty()) values.assign(curves.begin(), curves.end());
    for (uint32_t slice = 0; slice < slices; ++slice)
        [lut replaceRegion:MTLRegionMake2D(0,0,256,1) mipmapLevel:0 slice:slice
            withBytes:values.data() + slice * 256 bytesPerRow:256*4 bytesPerImage:256*4];
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

void ForegroundMotion(Metal::Device& device) {
    // One-pixel railings over a deep background must retain their own motion
    // across the surrounding interpolation footprint. Read the production
    // snapshots, including distant perspective depths and texture boundaries.
    using namespace BbFrameGen;
    Generator generator{device};
    const auto d = Description();
    const auto frame = generator.Prepare(d);
    Expect(bool(frame), "foreground motion snapshot allocated");
    if (!frame) return;
    auto depth = Texture(device, MTLPixelFormatDepth32Float, d.width, d.height);
    auto motion = Texture(device, MTLPixelFormatRG16Float, d.width, d.height);
    auto ui = Texture(device, d.ui_format, d.out_width, d.out_height);
    std::vector<float> depths(d.width * d.height, 1.f);
    std::vector<_Float16> vectors(d.width * d.height * 2, 0);
    const auto put = [&](uint32_t x, uint32_t y, float z, float mx, float my) {
        const auto p = y * d.width + x;
        depths[p] = z; vectors[p * 2] = mx; vectors[p * 2 + 1] = my;
    };
    put(8, 8, .25f, -6, 2);
    put(20, 8, .99999f, 3, -4); // distant foreground still differs from clear depth
    put(32, 8, .5f, 0, 0);     // zero is valid foreground motion
    put(31, 8, .75f, 8, 8);
    put(44, 8, .5f, 1, 2);
    put(43, 8, .5f, 9, 9);    // equal depth must not replace the centre's vector
    put(0, 0, .2f, -2, -3);
    put(d.width - 1, d.height - 1, .3f, 2, 3);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    put(8, 24, nan, 10, 10);
    put(20, 24, .4f, inf, 2);
    put(32, 24, -1, 10, 10);
    put(44, 24, 2, 10, 10);
    put(56, 16, 1, 5, -7); // sky rotation remains valid motion
    auto z_read = [Dev(device) newBufferWithLength:depths.size() * 4
        options:MTLResourceStorageModeShared];
    auto mv_read = [Dev(device) newBufferWithLength:vectors.size() * 2
        options:MTLResourceStorageModeShared];
    Metal::Encoder encoder{device};
    Upload(device, encoder, depth.Get(), depths.data(), d.width, d.height, 4);
    Upload(device, encoder, motion.Get(), vectors.data(), d.width, d.height, 4);
    generator.Capture(encoder, frame, depth.Get(), motion.Get(), ui.Get());
    encoder.CopyTextureToBuffer(frame->depth.Get(), 0, 0, {}, {d.width, d.height, 1},
        {(__bridge void*)z_read, 0}, d.width * 4, d.width * d.height * 4);
    encoder.CopyTextureToBuffer(frame->motion.Get(), 0, 0, {}, {d.width, d.height, 1},
        {(__bridge void*)mv_read, 0}, d.width * 4, d.width * d.height * 4);
    device.Wait(encoder.Submit());
    const auto check = [&](uint32_t x, uint32_t y, float z, float mx, float my) {
        const auto p = y * d.width + x;
        return static_cast<const float*>(z_read.contents)[p] == z &&
            float(static_cast<const _Float16*>(mv_read.contents)[p * 2]) == mx &&
            float(static_cast<const _Float16*>(mv_read.contents)[p * 2 + 1]) == my;
    };
    bool footprint = true;
    for (uint32_t y = 7; y <= 9; ++y) for (uint32_t x = 7; x <= 9; ++x)
        footprint &= check(x, y, .25f, -6, 2);
    Expect(footprint, "thin foreground supplies paired depth and motion to its 3x3 footprint");
    Expect(check(19, 9, .99999f, 3, -4), "distant foreground is preserved without a depth epsilon");
    Expect(check(32, 8, .5f, 0, 0), "stationary foreground does not borrow background motion");
    Expect(check(44, 8, .5f, 1, 2), "centre wins equal-depth motion ties");
    Expect(check(1, 1, .2f, -2, -3) && check(d.width - 2, d.height - 2, .3f, 2, 3),
           "foreground selection clamps at both texture boundaries");
    Expect(check(8, 24, 1, 0, 0) && check(32, 24, 1, 0, 0) && check(44, 24, 1, 0, 0),
           "invalid depth cannot become foreground");
    Expect(check(20, 24, .4f, 0, 0), "invalid foreground vector is sanitized without borrowing depth");
    Expect(check(56, 16, 1, 5, -7), "clear sky retains its motion and depth");
}

void PeriodicMotion(Metal::Device& device) {
    // Identical endpoints after one full pattern period make optical flow
    // ambiguous. The game vectors must choose the half-period midpoint,
    // both at native resolution and with lower-resolution depth/motion.
    for (uint32_t divisor : {1u, 2u}) {
        using namespace BbFrameGen;
        Generator generator{device};
        auto d = Description();
        d.out_width = 256; d.out_height = 144;
        d.width = d.out_width / divisor; d.height = d.out_height / divisor;
        auto depth = Texture(device, MTLPixelFormatDepth32Float, d.width, d.height);
        auto motion = Texture(device, MTLPixelFormatRG16Float, d.width, d.height);
        auto ui = Texture(device, d.ui_format, d.out_width, d.out_height);
        auto displayed = Texture(device, MTLPixelFormatRGBA16Float, d.out_width, d.out_height);
        std::vector<float> depths(d.width * d.height, .5f);
        std::vector<_Float16> vectors(d.width * d.height * 2, 0);
        for (size_t p = 0; p < depths.size(); ++p) vectors[p * 2] = -16.f / divisor;
        std::vector<uint8_t> image(d.out_width * d.out_height * 4, 255);
        for (uint32_t y = 0; y < d.out_height; ++y)
            for (uint32_t x = 0; x < d.out_width; ++x)
                for (uint32_t c = 0; c < 3; ++c)
                    image[(y * d.out_width + x) * 4 + c] = x % 16 < 8 ? 255 : 0;
        for (unsigned n = 0; n < 5; ++n, ++d.sequence) {
            auto frame = generator.Prepare(d);
            Expect(bool(frame), "periodic motion snapshot allocated");
            if (!frame) return;
            Metal::Encoder encoder{device};
            Upload(device, encoder, depth.Get(), depths.data(), d.width, d.height, 4);
            Upload(device, encoder, motion.Get(), vectors.data(), d.width, d.height, 4);
            Upload(device, encoder, ui.Get(), image.data(), d.out_width, d.out_height, 4);
            generator.Capture(encoder, frame, depth.Get(), motion.Get(), ui.Get());
            generator.FinishUi(encoder, frame, ui.Get());
            SceneDisplay(device, encoder, generator, frame, ui.Get(), displayed.Get());
            generator.Interpolate(encoder, frame, displayed.Get());
            auto readback = [Dev(device) newBufferWithLength:d.out_width * d.out_height * 8
                options:MTLResourceStorageModeShared];
            encoder.CopyTextureToBuffer(frame->generated.Get(), 0, 0, {},
                {d.out_width, d.out_height, 1}, {(__bridge void*)readback, 0},
                d.out_width * 8, d.out_width * d.out_height * 8);
            device.Wait(encoder.Submit());
            if (n == 4) {
                const auto* values = static_cast<const _Float16*>(readback.contents);
                double error = 0;
                for (uint32_t x = 64; x < 192; ++x)
                    error += std::abs(float(values[(72 * d.out_width + x) * 4]) -
                                      (x % 16 < 8 ? 0.f : 1.f));
                error /= 128;
                std::printf("Periodic motion: %ux upscale midpoint error %.4f\n", divisor, error);
                Expect(frame->interpolated.load() && error < .1,
                       "game vectors resolve repeated geometry at the temporal midpoint");
            }
        }
    }
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

void GammaSlices(Metal::Device& device) {
    // The retail LUT stores one channel curve per array slice, in reverse channel order.
    // Distinct curves and a fourth sentinel slice expose a slice or channel mix-up.
    for (const uint32_t slices : {3u, 4u}) {
        BbFrameGen::Generator generator{device};
        const auto d = Description();
        auto frame = generator.Prepare(d);
        Expect(bool(frame), "gamma slice snapshot allocated");
        if (!frame) return;
        auto depth = Texture(device, MTLPixelFormatDepth32Float, d.width, d.height);
        auto motion = Texture(device, MTLPixelFormatRG16Float, d.width, d.height);
        auto ui = Texture(device, d.ui_format, d.out_width, d.out_height);
        auto display = Texture(device, MTLPixelFormatRGBA16Float, d.out_width, d.out_height);
        std::vector<float> depths(d.width * d.height, 1.f);
        std::vector<_Float16> vectors(d.width * d.height * 2, 0);
        std::vector<uint8_t> image(d.out_width * d.out_height * 4);
        for (size_t p = 0; p < image.size(); p += 4) {
            image[p] = 51; image[p + 1] = 102; image[p + 2] = 153; image[p + 3] = 255;
        }
        std::vector<float> curves(256 * slices, .95f);
        for (uint32_t x = 0; x < 256; ++x) {
            const float coordinate = (x + .5f) / 256.f;
            curves[x] = .1f + .6f * coordinate;
            curves[256 + x] = .2f + .3f * coordinate;
            curves[512 + x] = .05f + .5f * coordinate;
        }
        Metal::Encoder encoder{device};
        Upload(device, encoder, depth.Get(), depths.data(), d.width, d.height, 4);
        Upload(device, encoder, motion.Get(), vectors.data(), d.width, d.height, 4);
        Upload(device, encoder, ui.Get(), image.data(), d.out_width, d.out_height, 4);
        generator.Capture(encoder, frame, depth.Get(), motion.Get(), ui.Get());
        SceneDisplay(device, encoder, generator, frame, ui.Get(), display.Get(), curves);
        auto pixels = [Dev(device) newBufferWithLength:d.out_width * d.out_height * 8
                                             options:MTLResourceStorageModeShared];
        encoder.CopyTextureToBuffer(frame->scene_display.Get(), 0, 0, {},
            {d.out_width, d.out_height, 1}, {(__bridge void*)pixels, 0},
            d.out_width * 8, d.out_width * d.out_height * 8);
        device.Wait(encoder.Submit());
        const auto* rgb = static_cast<const _Float16*>(pixels.contents);
        // Input (.2, .4, .6): red = slice 2 (blue), green = slice 1, blue = slice 0 (red).
        Expect(std::abs(float(rgb[0]) - .35f) < .002f,
               "red output maps blue input through gamma LUT slice two");
        Expect(std::abs(float(rgb[1]) - .32f) < .002f,
               "green output maps green input through gamma LUT slice one");
        Expect(std::abs(float(rgb[2]) - .22f) < .002f,
               "blue output maps red input through gamma LUT slice zero");
        Expect(float(rgb[3]) == 1.f, "gamma slice sampling preserves alpha");
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
    BudgetPolicy();
    @autoreleasepool {
        Metal::Device device;
        if (BbFrameGen::Supported(device)) {
            ForegroundMotion(device);
            PeriodicMotion(device);
            Gpu(device);
            GammaSlices(device);
            CroppedDisplay(device);
        } else
            std::printf("SKIP: MetalFX frame interpolation unsupported\n");
    }
    std::printf("%s: frame generation\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
