// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_metalfx.h"

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <atomic>
#include <chrono>
#include <cstdio>

#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"

// Spatial MetalFX requires its input texture to exactly match the descriptor.
// Cache a cropped input for the guest's aligned/padded render targets.
@interface BbSpatialState : NSObject
@property(nonatomic, strong) id<MTLFXSpatialScaler> scaler;
@property(nonatomic, strong) id<MTLTexture> input;
@end
@implementation BbSpatialState
@end

namespace BbMetalFx {

bool Supported(Metal::Device& device) {
    id<MTLDevice> mtl = (__bridge id<MTLDevice>)device.NativeDevice();
    return mtl && [MTLFXTemporalScalerDescriptor supportsDevice:mtl];
}

bool SpatialSupported(Metal::Device& device) {
    return [MTLFXSpatialScalerDescriptor supportsDevice:(__bridge id<MTLDevice>)device.NativeDevice()];
}

Metal::Object CreateScaler(Metal::Device& device, const Config& c) {
    if (c.spatial) {
        auto d = [MTLFXSpatialScalerDescriptor new];
        d.colorTextureFormat = MTLPixelFormat(c.color_format);
        d.outputTextureFormat = MTLPixelFormat(c.output_format);
        d.inputWidth = c.width; d.inputHeight = c.height;
        d.outputWidth = c.out_width; d.outputHeight = c.out_height;
        d.colorProcessingMode = c.hdr ? MTLFXSpatialScalerColorProcessingModeHDR
                                     : MTLFXSpatialScalerColorProcessingModePerceptual;
        auto scaler = [d newSpatialScalerWithDevice:(__bridge id<MTLDevice>)device.NativeDevice()];
        if (!scaler) std::fprintf(stderr, "MetalFX Spatial: cannot create %ux%u -> %ux%u scaler\n",
                                  c.width, c.height, c.out_width, c.out_height);
        if (!scaler) return {};
        auto state = [BbSpatialState new]; state.scaler = scaler;
        return Metal::Object::Adopt((__bridge_retained void*)state);
    }
    const auto start = std::chrono::steady_clock::now();
    id<MTLFXTemporalScaler> scaler = nil;
    @autoreleasepool {
        MTLFXTemporalScalerDescriptor* d = [MTLFXTemporalScalerDescriptor new];
        d.colorTextureFormat = MTLPixelFormat(c.color_format);
        d.depthTextureFormat = MTLPixelFormat(c.depth_format);
        d.motionTextureFormat = MTLPixelFormat(c.motion_format);
        d.outputTextureFormat = MTLPixelFormat(c.output_format);
        d.inputWidth = c.width;
        d.inputHeight = c.height;
        d.outputWidth = c.out_width;
        d.outputHeight = c.out_height;
        d.autoExposureEnabled = c.hdr; // HDR scene colour, as FSR's AUTO_EXPOSURE
        scaler = [d newTemporalScalerWithDevice:(__bridge id<MTLDevice>)device.NativeDevice()];
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!scaler) {
        std::printf("MetalFX: scaler creation failed (color %u depth %u motion %u output %u, "
                    "%ux%u -> %ux%u)\n",
                    c.color_format, c.depth_format, c.motion_format, c.output_format, c.width,
                    c.height, c.out_width, c.out_height);
        std::fflush(stdout);
        return {};
    }
    std::printf("MetalFX: temporal scaler %ux%u -> %ux%u (%s, %.0f ms)\n", c.width, c.height,
                c.out_width, c.out_height, c.hdr ? "HDR" : "tonemapped", ms);
    return Metal::Object::Adopt((__bridge_retained void*)scaler);
}

void EncodeSpatial(Metal::Encoder& e, void* scaler_ptr, const Frame& f) {
    auto state = (__bridge BbSpatialState*)scaler_ptr;
    auto scaler = state.scaler;
    auto color = (__bridge id<MTLTexture>)f.color;
    e.EndEncoding();
    if (color.width != scaler.inputWidth || color.height != scaler.inputHeight ||
        (color.usage & scaler.colorTextureUsage) != scaler.colorTextureUsage) {
        if (!state.input) {
            auto d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:scaler.colorTextureFormat
                width:scaler.inputWidth height:scaler.inputHeight mipmapped:NO];
            d.storageMode = MTLStorageModePrivate; d.usage = scaler.colorTextureUsage;
            state.input = [color.device newTextureWithDescriptor:d];
        }
        if (!state.input) return;
        auto blit = [(__bridge id<MTLCommandBuffer>)e.CommandBuffer() blitCommandEncoder];
        [blit copyFromTexture:color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
            sourceSize:MTLSizeMake(scaler.inputWidth,scaler.inputHeight,1)
            toTexture:state.input destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
        [blit endEncoding]; color = state.input;
    }
    scaler.colorTexture = color;
    scaler.outputTexture = (__bridge id<MTLTexture>)f.output;
    scaler.inputContentWidth = f.content_width;
    scaler.inputContentHeight = f.content_height;
    [scaler encodeToCommandBuffer:(__bridge id<MTLCommandBuffer>)e.CommandBuffer()];
    e.KeepAlive(Metal::Object::Retain(scaler_ptr));
}

void Encode(Metal::Encoder& e, void* scaler_ptr, const Frame& f) {
    static std::atomic<bool> usage_warned{false}, encoded_once{false};
    id<MTLFXTemporalScaler> scaler = (__bridge id<MTLFXTemporalScaler>)scaler_ptr;
    id<MTLTexture> color = (__bridge id<MTLTexture>)f.color;
    id<MTLTexture> output = (__bridge id<MTLTexture>)f.output;
    if (((color.usage & scaler.colorTextureUsage) != scaler.colorTextureUsage ||
         (output.usage & scaler.outputTextureUsage) != scaler.outputTextureUsage) &&
        !usage_warned.exchange(true)) {
        std::printf("MetalFX: texture usage short of the scaler's (color %lu/%lu, output %lu/%lu)\n",
                    (unsigned long)color.usage, (unsigned long)scaler.colorTextureUsage,
                    (unsigned long)output.usage, (unsigned long)scaler.outputTextureUsage);
    }
    // MetalFX opens encoders of its own on the command buffer: none of ours may be open.
    e.EndEncoding();
    @autoreleasepool {
        scaler.colorTexture = color;
        scaler.depthTexture = (__bridge id<MTLTexture>)f.depth;
        scaler.motionTexture = (__bridge id<MTLTexture>)f.motion;
        scaler.outputTexture = output;
        scaler.inputContentWidth = f.content_width;
        scaler.inputContentHeight = f.content_height;
        scaler.jitterOffsetX = f.jitter_x;
        scaler.jitterOffsetY = f.jitter_y;
        scaler.motionVectorScaleX = f.motion_scale_x;
        scaler.motionVectorScaleY = f.motion_scale_y;
        scaler.reset = f.reset;
        scaler.depthReversed = NO;
        [scaler encodeToCommandBuffer:(__bridge id<MTLCommandBuffer>)e.CommandBuffer()];
    }
    e.KeepAlive(Metal::Object::Retain(scaler_ptr));
    if (!encoded_once.exchange(true)) {
        std::printf("MetalFX: first frame encoded\n");
    }
}

} // namespace BbMetalFx
