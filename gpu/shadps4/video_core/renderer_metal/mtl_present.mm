// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: presenting to a CAMetalLayer (Metal-native renderer, M4). See mtl_present.h.

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_metal/mtl_blit_shaders.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_device_impl.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_present.h"
#include "video_core/renderer_metal/mtl_render_pipeline.h"
#include "video_core/renderer_metal/mtl_texture.h"

namespace Metal {

FitRect FitImage(u32 src_width, u32 src_height, u32 dst_width, u32 dst_height) {
    if (src_width == 0 || src_height == 0 || dst_width == 0 || dst_height == 0) {
        return {0, 0, 0, 0};
    }
    // vk_presenter.cpp FitImage: float aspects, truncation towards zero.
    const float src_aspect = static_cast<float>(src_width) / static_cast<float>(src_height);
    const float dst_aspect = static_cast<float>(dst_width) / static_cast<float>(dst_height);
    u32 width = dst_width;
    u32 height = dst_height;
    if (src_aspect > dst_aspect) {
        height = static_cast<u32>(static_cast<float>(dst_width) / src_aspect);
    } else {
        width = static_cast<u32>(static_cast<float>(dst_height) * src_aspect);
    }
    return {static_cast<s32>((dst_width - width) / 2), static_cast<s32>((dst_height - height) / 2),
            width, height};
}

bool ShouldAcquireDrawable(bool window_visible, s32 width, s32 height, bool snapshot_requested) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    return window_visible || snapshot_requested;
}

struct PresentPasses::Impl {
    Object library;
    Object post_process; ///< id<MTLRenderPipelineState>, colour frame_format
    Object present;      ///< id<MTLRenderPipelineState>, colour drawable_format
    Object sampler;      ///< linear, clamp to edge
};

PresentPasses::PresentPasses(Device& device, u32 frame_format, u32 drawable_format)
    : impl{std::make_unique<Impl>()} {
    impl->library = CompileLibrary(device, BlitShaders::Source, "Present passes");
    impl->post_process = CreateRenderPipeline(device,
                                              RenderPipelineDesc{
                                                  .library = impl->library.Get(),
                                                  .vertex_function = BlitShaders::FullscreenTriangle,
                                                  .fragment_function = BlitShaders::PostProcess,
                                                  .color_format = frame_format,
                                              },
                                              "Post-process");
    impl->present = CreateRenderPipeline(device,
                                         RenderPipelineDesc{
                                             .library = impl->library.Get(),
                                             .vertex_function = BlitShaders::FullscreenTriangle,
                                             .fragment_function = BlitShaders::Present,
                                             .color_format = drawable_format,
                                         },
                                         "Present");
    impl->sampler = CreateSampler(device, SamplerDesc{.min_filter = MTLSamplerMinMagFilterLinear,
                                                      .mag_filter = MTLSamplerMinMagFilterLinear,
                                                      .mip_filter = MTLSamplerMipFilterNotMipmapped,
                                                      .address_u = MTLSamplerAddressModeClampToEdge,
                                                      .address_v = MTLSamplerAddressModeClampToEdge,
                                                      .address_w = MTLSamplerAddressModeClampToEdge});
    ASSERT_MSG(impl->sampler, "Present passes: no sampler");
}

PresentPasses::~PresentPasses() = default;

void PresentPasses::PostProcess(Encoder& encoder, void* input, void* output, u32 out_width,
                                u32 out_height, const PostProcessSettings& settings) {
    static_assert(sizeof(PostProcessSettings) == 12);
    RenderPassDesc pass{};
    pass.colors[0].texture = output; // every pixel is written: no clear
    pass.width = out_width;
    pass.height = out_height;
    encoder.BeginRenderPass(pass);
    // Fullscreen-triangle state: whatever the rasterizer left is reset.
    encoder.SetRenderPipeline(impl->post_process.Get(), Access::None); // textures only
    encoder.SetDepthStencil(nullptr);
    encoder.SetCullMode(CullMode::None);
    encoder.SetDepthBias(0.0f, 0.0f, 0.0f);
    encoder.SetFillMode(FillMode::Fill);
    encoder.SetDepthClip(DepthClip::Clip);
    encoder.SetViewport({0.0f, 0.0f, float(out_width), float(out_height), 0.0f, 1.0f});
    encoder.SetScissor({0, 0, out_width, out_height});
    encoder.SetTexture(Stage::Fragment, 0, input);
    encoder.SetSampler(Stage::Fragment, 0, impl->sampler.Get());
    encoder.SetBytes(Stage::Fragment, 0, &settings, sizeof(settings));
    encoder.Draw(Primitive::Triangle, 3);
    encoder.EndRenderPass();
}

void PresentPasses::Present(void* command_buffer, void* frame, u32 frame_width, u32 frame_height,
                            void* target, u32 target_width, u32 target_height) {
    id<MTLCommandBuffer> cmd = (__bridge id<MTLCommandBuffer>)command_buffer;
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = (__bridge id<MTLTexture>)target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cmd renderCommandEncoderWithDescriptor:pass];
    e.label = @"Present";
    const FitRect fit = FitImage(frame_width, frame_height, target_width, target_height);
    if (frame != nullptr && fit.width != 0 && fit.height != 0) {
        [e setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)impl->present.Get()];
        [e setViewport:(MTLViewport){double(fit.x), double(fit.y), double(fit.width),
                                     double(fit.height), 0.0, 1.0}];
        [e setFragmentTexture:(__bridge id<MTLTexture>)frame atIndex:0];
        [e setFragmentSamplerState:(__bridge id<MTLSamplerState>)impl->sampler.Get() atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [e endEncoding];
}

void ConfigureLayer(Device& device, void* layer, const LayerConfig& config) {
    CAMetalLayer* l = (__bridge CAMetalLayer*)layer;
    l.device = (__bridge id<MTLDevice>)device.NativeDevice();
    l.pixelFormat = MTLPixelFormat(config.pixel_format);
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    l.colorspace = srgb;
    CGColorSpaceRelease(srgb);
    l.maximumDrawableCount = config.max_drawables;
    l.displaySyncEnabled = config.display_sync;
    l.framebufferOnly = config.framebuffer_only;
}

void SetMaximumDrawables(void* layer, u32 count) {
    ((__bridge CAMetalLayer*)layer).maximumDrawableCount = count;
}

Drawable NextDrawable(void* layer) {
    id<CAMetalDrawable> drawable = [(__bridge CAMetalLayer*)layer nextDrawable];
    if (drawable == nil) {
        return {};
    }
    id<MTLTexture> texture = drawable.texture;
    Drawable result;
    result.texture = (__bridge void*)texture;
    result.width = u32(texture.width);
    result.height = u32(texture.height);
    result.drawable = Object::Adopt((__bridge_retained void*)drawable);
    return result;
}

Object EncodeSnapshot(Device& device, void* command_buffer, void* texture, u32 width,
                      u32 height) {
    if (width == 0 || height == 0) {
        return {};
    }
    // The PNG writer takes 4-byte BGRA texels; an HDR layer (RGBA16Float, BGR10A2) would need
    // a conversion pass.
    const MTLPixelFormat format = ((__bridge id<MTLTexture>)texture).pixelFormat;
    if (format != MTLPixelFormatBGRA8Unorm && format != MTLPixelFormatBGRA8Unorm_sRGB) {
        LOG_WARNING(Render, "Snapshot: drawable pixel format {} is not BGRA8; no snapshot",
                    static_cast<u32>(format));
        return {};
    }
    id<MTLBuffer> buffer =
        [(__bridge id<MTLDevice>)device.NativeDevice() newBufferWithLength:NSUInteger(width) * height * 4
                                                                   options:MTLResourceStorageModeShared];
    if (buffer == nil) {
        return {};
    }
    buffer.label = @"Snapshot";
    id<MTLBlitCommandEncoder> blit =
        [(__bridge id<MTLCommandBuffer>)command_buffer blitCommandEncoder];
    blit.label = @"Snapshot";
    [blit copyFromTexture:(__bridge id<MTLTexture>)texture
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(width, height, 1)
                        toBuffer:buffer
               destinationOffset:0
          destinationBytesPerRow:NSUInteger(width) * 4
        destinationBytesPerImage:NSUInteger(width) * height * 4];
    [blit endEncoding];
    return Object::Adopt((__bridge_retained void*)buffer);
}

bool WriteSnapshotPng(const char* path, void* buffer, u32 width, u32 height) {
    @autoreleasepool {
        id<MTLBuffer> source = (__bridge id<MTLBuffer>)buffer;
        if (source == nil || source.length < NSUInteger(width) * height * 4) {
            return false;
        }
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGDataProviderRef provider = CGDataProviderCreateWithData(
            nullptr, source.contents, size_t(width) * height * 4, nullptr);
        // BGRA8 bytes = little-endian 32-bit pixels with alpha first (ARGB); alpha is 1.
        CGImageRef image = CGImageCreate(width, height, 8, 32, size_t(width) * 4, srgb,
                                         kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst,
                                         provider, nullptr, false, kCGRenderingIntentDefault);
        bool ok = false;
        NSURL* url = [NSURL fileURLWithPath:@(path)];
        CGImageDestinationRef destination = CGImageDestinationCreateWithURL(
            (__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
        if (image != nullptr && destination != nullptr) {
            CGImageDestinationAddImage(destination, image, nullptr);
            ok = CGImageDestinationFinalize(destination);
        }
        if (destination != nullptr) {
            CFRelease(destination);
        }
        CGImageRelease(image);
        CGDataProviderRelease(provider);
        CGColorSpaceRelease(srgb);
        return ok;
    }
}

void WithAutoreleasePool(const std::function<void()>& body) {
    @autoreleasepool {
        body();
    }
}

struct PresentQueue::Impl {
    id<MTLCommandQueue> queue;
    id<MTLSharedEvent> event; ///< the device's (Device::Commit signals it with each tick)
    Device* device{};         ///< counts the present command buffers' GPU time (GpuBusyNs)
    /// What completion handlers touch; the destructor waits for them, so a handler never
    /// outlives it.
    std::mutex mutex;
    std::condition_variable idle;
    u32 in_flight{}; ///< under `mutex`
    u32 shutdown_timeout_ms{};
};

PresentQueue::PresentQueue(Device& device, u32 shutdown_timeout_ms)
    : impl{std::make_unique<Impl>()} {
    impl->shutdown_timeout_ms = shutdown_timeout_ms;
    impl->queue = [(__bridge id<MTLDevice>)device.NativeDevice() newCommandQueue];
    ASSERT_MSG(impl->queue != nil, "No Metal present queue");
    impl->queue.label = @"Present";
    impl->event = device.GetImpl().event;
    impl->device = &device;
}

PresentQueue::~PresentQueue() {
    if (!WaitIdleFor(impl->shutdown_timeout_ms)) {
        LOG_ERROR(Render, "Metal present queue: {} command buffer(s) still in flight after {} ms; "
                          "leaving them behind",
                  InFlight(), impl->shutdown_timeout_ms);
        (void)impl.release(); // their completion handlers still use it
    }
}

void* PresentQueue::Begin() {
    id<MTLCommandBuffer> cmd = [impl->queue commandBuffer];
    ASSERT_MSG(cmd != nil, "No present command buffer");
    // A reference of our own until Commit (the caller may have no autorelease pool).
    return (__bridge_retained void*)cmd;
}

void PresentQueue::WaitForDeviceTick(void* command_buffer, u64 device_tick) {
    if (device_tick == 0) {
        return;
    }
    [(__bridge id<MTLCommandBuffer>)command_buffer encodeWaitForEvent:impl->event
                                                                value:device_tick];
}

void PresentQueue::Commit(void* command_buffer, void* drawable,
                          std::function<void()> on_complete, double min_duration,
                          std::function<void(double)> on_presented) {
    id<MTLCommandBuffer> cmd = (__bridge_transfer id<MTLCommandBuffer>)command_buffer;
    if (drawable && on_presented) {
        [(__bridge id<CAMetalDrawable>)drawable addPresentedHandler:^(id<MTLDrawable> shown) {
          on_presented(shown.presentedTime);
        }];
    }
    if (drawable != nullptr && min_duration > 0) {
        [cmd presentDrawable:(__bridge id<CAMetalDrawable>)drawable
            afterMinimumDuration:min_duration];
    } else if (drawable != nullptr) {
        [cmd presentDrawable:(__bridge id<CAMetalDrawable>)drawable];
    }
    {
        std::scoped_lock lock{impl->mutex};
        ++impl->in_flight;
    }
    impl->device->TrackGpuTime((__bridge void*)cmd);
    Impl* const state = impl.get();
    [cmd addCompletedHandler:^(id<MTLCommandBuffer> done) {
      if (done.status == MTLCommandBufferStatusError) {
          LOG_ERROR(Render, "Metal present command buffer failed: {}",
                    done.error ? done.error.localizedDescription.UTF8String : "unknown error");
      }
      if (on_complete) {
          on_complete();
      }
      std::scoped_lock lock{state->mutex};
      if (--state->in_flight == 0) {
          state->idle.notify_all();
      }
    }];
    [cmd commit];
}

void PresentQueue::WaitIdle() {
    std::unique_lock lock{impl->mutex};
    impl->idle.wait(lock, [this] { return impl->in_flight == 0; });
}

bool PresentQueue::WaitIdleFor(u32 milliseconds) {
    std::unique_lock lock{impl->mutex};
    return impl->idle.wait_for(lock, std::chrono::milliseconds(milliseconds),
                               [this] { return impl->in_flight == 0; });
}

u32 PresentQueue::InFlight() const {
    std::scoped_lock lock{impl->mutex};
    return impl->in_flight;
}

} // namespace Metal
