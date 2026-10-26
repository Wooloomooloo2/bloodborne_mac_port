// SPDX-FileCopyrightText: Copyright 2026 Ronny Stiftel
// SPDX-License-Identifier: GPL-2.0-or-later
// Experimental MetalFX interpolation. Scene/HUD snapshots and their lifetime
// follow the direct-Metal port's frame_generation.mm at b395cf4; adapted to
// this port's two queues.
#include "bbport_framegen.h"
#include "bbport_settings.h"

#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace BbFrameGen {
namespace {
constexpr const char* Source = R"msl(
#include <metal_stdlib>
using namespace metal;
kernel void capture_inputs(depth2d<float> source [[texture(0)]],
                           texture2d<float, access::read> vectors [[texture(1)]],
                           texture2d<float, access::write> target [[texture(2)]],
                           texture2d<float, access::write> motion [[texture(3)]],
                           uint2 p [[thread_position_in_grid]]) {
    uint2 size = uint2(target.get_width(), target.get_height());
    if (any(p >= size)) return;
    float z = source.read(p);
    target.write(float4(isfinite(z) ? clamp(z, 0.f, 1.f) : 1.f), p);
    float2 mv = vectors.read(p).rg;
    if (!all(isfinite(mv))) mv = 0;
    motion.write(float4(clamp(mv, -float2(size)*2, float2(size)*2), 0, 0), p);
}
kernel void hud_change(texture2d<float, access::read> before [[texture(0)]],
                     texture2d<float, access::read> after [[texture(1)]],
                     texture2d<float, access::write> mask [[texture(2)]],
                     uint2 p [[thread_position_in_grid]]) {
    if (p.x >= mask.get_width() || p.y >= mask.get_height()) return;
    bool changed = any(abs(before.read(p).rgb - after.read(p).rgb) > 1.f/1024.f);
    mask.write(float4(changed ? 1.f : 0.f), p);
}
kernel void hud_mask(texture2d<float, access::read> change [[texture(0)]],
                     texture2d<float, access::write> mask [[texture(1)]],
                     uint2 p [[thread_position_in_grid]]) {
    int2 size = int2(mask.get_width(), mask.get_height());
    if (any(int2(p) >= size)) return;
    bool hud = false;
    // Restore current HUD pixels, including a two-pixel border for antialiasing.
    for (int y = -2; y <= 2; ++y) for (int x = -2; x <= 2; ++x) {
        uint2 q = uint2(clamp(int2(p) + int2(x,y), int2(0), size - 1));
        hud = hud || change.read(q).r > 0.f;
    }
    mask.write(float4(hud ? 1.f : 0.f), p);
}
kernel void displayed_color(texture2d<float, access::read> source [[texture(0)]],
                            texture2d<float, access::write> target [[texture(1)]],
                            uint2 p [[thread_position_in_grid]]) {
    if (p.x >= target.get_width() || p.y >= target.get_height()) return;
    // Read through the presenter's video-out view: preserve its sRGB decode,
    // component swizzle and alpha One, after the game's own gamma LUT.
    uint2 size = uint2(source.get_width(), source.get_height());
    target.write(source.read(min(p, size - 1)), p);
}
kernel void restore_hud(texture2d<float, access::read> real [[texture(0)]],
                        texture2d<float, access::read> mask [[texture(1)]],
                        texture2d<float, access::read_write> generated [[texture(2)]],
                        uint2 p [[thread_position_in_grid]]) {
    if (p.x >= generated.get_width() || p.y >= generated.get_height()) return;
    if (mask.read(p).r > 0.f) generated.write(real.read(p), p);
}
)msl";

id<MTLTexture> Texture(Metal::Device& device, MTLPixelFormat format, uint32_t width,
                       uint32_t height, MTLTextureUsage usage) {
    auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                width:width
                                                               height:height
                                                            mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = usage;
    return [(__bridge id<MTLDevice>)device.NativeDevice() newTextureWithDescriptor:d];
}
Metal::Object Own(id object) { return Metal::Object::Adopt((__bridge_retained void*)object); }
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
id<MTLTexture> Tex(const Metal::Object& object) { return (__bridge id<MTLTexture>)object.Get(); }
#endif
bool SameSize(const Description& a, const Description& b) {
    return a.width == b.width && a.height == b.height && a.out_width == b.out_width &&
           a.out_height == b.out_height && a.ui_format == b.ui_format;
}
bool Valid(const Description& d) {
    return d.width && d.height && d.out_width >= d.width && d.out_height >= d.height &&
           std::isfinite(d.interval) && d.interval >= 1.f / 240.f && d.interval <= .1f &&
           std::isfinite(d.near_plane) && std::isfinite(d.far_plane) && d.near_plane > 0 &&
           d.far_plane > d.near_plane && std::isfinite(d.vertical_fov) && d.vertical_fov > 0 &&
           d.vertical_fov < 3.141593f;
}
// A completion handler retains the snapshots even if the producer
// skips/replaces a frame before encoding finishes. Metal command-buffer
// resource retention alone does not prevent the CPU from reusing a pool slot
// and writing the same textures.
void Pin(Metal::Encoder& e, const FramePtr& frame) {
    const FramePtr owned = frame;
    [(__bridge id<MTLCommandBuffer>)e.CommandBuffer()
        addCompletedHandler:^(id<MTLCommandBuffer> completed) {
          if (completed.status != MTLCommandBufferStatusCompleted) {
              owned->interpolated.store(false, std::memory_order_release);
          }
        }];
}
} // namespace

bool Requested() {
    return BbSettings::Get().startup_framegen;
}
bool DisplaySupported(double refresh_hz, uint32_t real_cap, double min_refresh_hz) {
    if (!real_cap || !std::isfinite(refresh_hz) || refresh_hz <= 0) return false;
    const double paired_rate = 2.0 * real_cap;
    if (min_refresh_hz != 0) {
        return std::isfinite(min_refresh_hz) && min_refresh_hz > 0 &&
               min_refresh_hz < refresh_hz && paired_rate >= min_refresh_hz * .998 &&
               paired_rate <= refresh_hz * 1.002;
    }
    const double ratio = refresh_hz / paired_rate;
    const double multiple = std::round(ratio);
    // Includes 59.94/119.88 Hz modes without admitting 144/160 Hz at a 60 FPS cap.
    return multiple >= 1 && std::abs(ratio - multiple) <= .002 * multiple;
}
bool DisplaySupported() {
    const auto& s = BbSettings::Get();
    const auto timing = s.display_timing.load();
    return DisplaySupported(timing.refresh_hz, s.framegen_real_cap, timing.min_refresh_hz);
}
void UpdateDisplayTiming(void* cocoa_window, bool fullscreen) {
    NSScreen* screen = [(__bridge NSWindow*)cocoa_window screen];
    BbSettings::DisplayTiming timing;
    if (screen) {
        const auto display = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
        const double shortest = screen.minimumRefreshInterval;
        const double longest = screen.maximumRefreshInterval;
        const bool valid_interval = std::isfinite(shortest) && shortest > 0;
        const bool variable = valid_interval && std::isfinite(longest) && longest > shortest;
        // External Adaptive-Sync scheduling requires full screen. Built-in ProMotion can
        // also service composited windows; fixed modes have equal interval limits.
        const bool adaptive = variable && (fullscreen || CGDisplayIsBuiltin(display));
        CGDisplayModeRef mode = display ? CGDisplayCopyDisplayMode(display) : nullptr;
        const double fixed_rate = mode ? CGDisplayModeGetRefreshRate(mode) : 0;
        if (mode) CGDisplayModeRelease(mode);
        timing.refresh_hz = variable ? 1.0 / shortest
            : (std::isfinite(fixed_rate) && fixed_rate > 0 ? fixed_rate
               : (valid_interval ? 1.0 / shortest : 0));
        timing.min_refresh_hz = adaptive ? 1.0 / longest : 0;
    }
    BbSettings::Get().display_timing = timing;
}
bool Active() {
    const auto& s = BbSettings::Get();
    return Requested() && s.framegen_supported && s.upscaler != BbSettings::UpscalerOff &&
           DisplaySupported();
}
bool Supported(Metal::Device& device) {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
    if (@available(macOS 26.0, *)) {
        return [MTLFXFrameInterpolatorDescriptor
            supportsDevice:(__bridge id<MTLDevice>)device.NativeDevice()];
    }
#endif
    (void)device;
    return false;
}
bool Continuous(const Description& previous, const Description& current) {
    return Valid(previous) && Valid(current) && !current.reset && previous.sequence != UINT64_MAX &&
           current.sequence == previous.sequence + 1 && SameSize(previous, current) &&
           previous.near_plane == current.near_plane && previous.far_plane == current.far_plane &&
           previous.vertical_fov == current.vertical_fov;
}
double PresentDuration(float interval, uint32_t game_limit) {
    if (!std::isfinite(interval) || interval < 1.f / 240.f || interval > .1f)
        return 0;
    // Measured cadence includes nextDrawable backpressure. Feeding that delay
    // back into the minimum display time lengthens later waits and can collapse
    // the real-frame rate. Only the configured cap may request an extra hold.
    const double period = game_limit ? 1.0 / game_limit : 0;
    return period * .5 * .97;
}

struct Generator::Impl {
    Metal::Device& device;
    std::vector<FramePtr> pool; // producer thread only; shared reference counts are atomic
    Metal::Object depth, change, mask, color, hud;
    Metal::Object effect;
    Description config{};
    FramePtr previous; // recorder thread only
    bool ready = false;
    uint64_t generated = 0;

    explicit Impl(Metal::Device& d) : device{d} {
        if (!Supported(device))
            return;
        @autoreleasepool {
            NSError* error = nil;
            auto library =
                [(__bridge id<MTLDevice>)device.NativeDevice() newLibraryWithSource:@(Source)
                                                                            options:nil
                                                                              error:&error];
            const auto kernel = [&](NSString* name) {
                return Own([(__bridge id<MTLDevice>)device.NativeDevice()
                    newComputePipelineStateWithFunction:[library newFunctionWithName:name]
                                                  error:&error]);
            };
            if (library) {
                depth = kernel(@"capture_inputs");
                change = kernel(@"hud_change");
                mask = kernel(@"hud_mask");
                color = kernel(@"displayed_color");
                hud = kernel(@"restore_hud");
            }
            ready = depth && change && mask && color && hud;
            if (!ready)
                std::fprintf(stderr, "Frame generation: kernels unavailable: %s\n",
                             error.localizedDescription.UTF8String ?: "no library");
        }
    }
    void Dispatch(Metal::Encoder& e, const Metal::Object& kernel,
                  std::initializer_list<void*> textures, uint32_t width, uint32_t height) {
        e.EndEncoding();
        auto ce = [(__bridge id<MTLCommandBuffer>)e.CommandBuffer() computeCommandEncoder];
        [ce setComputePipelineState:(__bridge id<MTLComputePipelineState>)kernel.Get()];
        NSUInteger slot = 0;
        for (auto texture : textures)
            [ce setTexture:(__bridge id<MTLTexture>)texture atIndex:slot++];
        [ce dispatchThreads:MTLSizeMake(width, height, 1)
            threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        [ce endEncoding];
        e.KeepAlive(Metal::Object::Retain(kernel.Get()));
    }
    bool EnsureEffect(const Description& d) {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
        if (@available(macOS 26.0, *)) {
            if (SameSize(d, config))
                return bool(effect);
            previous.reset();
            auto descriptor = [MTLFXFrameInterpolatorDescriptor new];
            descriptor.inputWidth = d.width;
            descriptor.inputHeight = d.height;
            descriptor.outputWidth = d.out_width;
            descriptor.outputHeight = d.out_height;
            descriptor.colorTextureFormat = descriptor.outputTextureFormat =
                MTLPixelFormatRGBA16Float;
            descriptor.depthTextureFormat = MTLPixelFormatR32Float;
            descriptor.motionTextureFormat = MTLPixelFormatRG16Float;
            descriptor.uiTextureFormat = MTLPixelFormatRGBA16Float;
            effect = Own([descriptor
                newFrameInterpolatorWithDevice:(__bridge id<MTLDevice>)device.NativeDevice()]);
            config = d;
            if (!effect)
                std::fprintf(stderr, "Frame generation: no interpolator for %ux%u -> %ux%u\n",
                             d.width, d.height, d.out_width, d.out_height);
            return bool(effect);
        }
#endif
        (void)d;
        return false;
    }
};

Generator::Generator(Metal::Device& device) : impl{std::make_unique<Impl>(device)} {}
Generator::~Generator() = default;

FramePtr Generator::Prepare(const Description& d) {
    auto& s = *impl;
    if (!s.ready || !Valid(d))
        return {};
    FramePtr frame;
    for (auto& slot : s.pool) {
        if (slot.use_count() == 1) {
            if (SameSize(slot->description, d)) {
                frame = slot;
                break;
            }
            slot.reset();
        }
    }
    std::erase(s.pool, FramePtr{});
    if (!frame) {
        if (s.pool.size() == MaxSnapshotSets)
            return {}; // no wait, no unbounded allocation
        frame = std::make_shared<Frame>();
        @autoreleasepool {
            const auto make = [&](MTLPixelFormat f, uint32_t w, uint32_t h, bool write) {
                return Own(
                    Texture(s.device, f, w, h,
                            MTLTextureUsageShaderRead |
                                (write ? MTLTextureUsageShaderWrite : MTLTextureUsageUnknown) |
                                (write ? MTLTextureUsageRenderTarget : MTLTextureUsageUnknown)));
            };
            frame->depth = make(MTLPixelFormatR32Float, d.width, d.height, true);
            frame->motion = make(MTLPixelFormatRG16Float, d.width, d.height, true);
            frame->before_ui = make(MTLPixelFormat(d.ui_format), d.out_width, d.out_height, false);
            frame->ui_mask = make(MTLPixelFormatR8Unorm, d.out_width, d.out_height, true);
            frame->ui_change = make(MTLPixelFormatR8Unorm, d.out_width, d.out_height, true);
            frame->color = make(MTLPixelFormatRGBA16Float, d.out_width, d.out_height, true);
            frame->generated = make(MTLPixelFormatRGBA16Float, d.out_width, d.out_height, true);
        }
        if (!frame->depth || !frame->motion || !frame->before_ui || !frame->ui_change ||
            !frame->ui_mask || !frame->color || !frame->generated)
            return {};
        s.pool.push_back(frame);
    }
    frame->description = d;
    frame->interpolated.store(false, std::memory_order_release);
    // Each use has its own lease control block. The pool owns the allocation,
    // while weak references in unpresented display buffers observe only this
    // use: they expire before the slot can be reused for another real frame.
    // A plain weak_ptr to the pool's control block would never expire and could
    // resurrect a reused slot. Explicitly release the allocation on lease expiry
    // even if the standard library keeps the deleter alive for weak references.
    auto* const leased = frame.get();
    return FramePtr{leased, [owned = std::move(frame)](Frame*) mutable { owned.reset(); }};
}

void Generator::Capture(Metal::Encoder& e, const FramePtr& frame, void* depth, void* motion,
                        void* ui) {
    const auto& d = frame->description;
    impl->Dispatch(e, impl->depth, {depth, motion, frame->depth.Get(), frame->motion.Get()},
                   d.width, d.height);
    e.CopyTexture(ui, 0, 0, {}, frame->before_ui.Get(), 0, 0, {}, {d.out_width, d.out_height, 1});
    Pin(e, frame);
}
void Generator::FinishUi(Metal::Encoder& e, const FramePtr& frame, void* ui) {
    const auto& d = frame->description;
    impl->Dispatch(e, impl->change, {frame->before_ui.Get(), ui, frame->ui_change.Get()},
                   d.out_width, d.out_height);
    impl->Dispatch(e, impl->mask, {frame->ui_change.Get(), frame->ui_mask.Get()}, d.out_width,
                   d.out_height);
    Pin(e, frame);
}
void Generator::Interpolate(Metal::Encoder& e, const FramePtr& frame, void* displayed) {
    auto& s = *impl;
    const auto& d = frame->description;
    s.Dispatch(e, s.color, {displayed, frame->color.Get()}, d.out_width, d.out_height);
    Pin(e, frame);
    if (!s.EnsureEffect(d)) {
        s.previous.reset();
        e.CopyTexture(frame->color.Get(), 0, 0, {}, frame->generated.Get(), 0, 0, {},
                      {d.out_width, d.out_height, 1});
        return;
    }
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
    if (@available(macOS 26.0, *)) {
        const bool reset = !s.previous || !Continuous(s.previous->description, d);
        auto fx = (__bridge id<MTLFXFrameInterpolator>)s.effect.Get();
        fx.colorTexture = Tex(frame->color);
        fx.prevColorTexture = Tex(reset ? frame->color : s.previous->color);
        fx.depthTexture = Tex(frame->depth);
        fx.motionTexture = Tex(frame->motion);
        fx.outputTexture = Tex(frame->generated);
        fx.uiTexture = nil;
        fx.uiTextureComposited = NO;
        fx.motionVectorScaleX = fx.motionVectorScaleY = 1;
        fx.depthReversed = NO;
        fx.shouldResetHistory = reset;
        fx.deltaTime = d.interval;
        fx.nearPlane = d.near_plane;
        fx.farPlane = d.far_plane;
        fx.fieldOfView = d.vertical_fov * 180.f / 3.141592654f;
        fx.aspectRatio = float(d.out_width) / d.out_height;
        // The temporal upscale has already removed the scene jitter.
        fx.jitterOffsetX = fx.jitterOffsetY = 0;
        e.EndEncoding();
        [fx encodeToCommandBuffer:(__bridge id<MTLCommandBuffer>)e.CommandBuffer()];
        e.KeepAlive(Metal::Object::Retain(s.effect.Get()));
        if (s.previous)
            Pin(e, s.previous);
        s.Dispatch(e, s.hud, {frame->color.Get(), frame->ui_mask.Get(), frame->generated.Get()},
                   d.out_width, d.out_height);
        frame->interpolated.store(!reset, std::memory_order_release);
        if (!reset && (++s.generated == 1 || s.generated % 300 == 0)) {
            std::printf("Frame generation: %llu intermediate frames encoded (experimental)\n",
                        static_cast<unsigned long long>(s.generated));
        }
        s.previous = frame;
    }
#endif
}
void Generator::Invalidate() { impl->previous.reset(); }
} // namespace BbFrameGen
