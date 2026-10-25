// SPDX-License-Identifier: GPL-2.0-or-later
// Render a triangle using the real index binding returned by BufferCache.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_object.h"

bool RenderIndexTriangle(Metal::Device& device, Metal::BufferRef indices) { @autoreleasepool {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    NSString* source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint id [[vertex_id]]) { const float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return id<3?float4(p[id],0,1):float4(0); }\n"
        "fragment float4 f() {return float4(1,0,0,1);}";
    NSError* error = nil;
    id<MTLLibrary> library = [dev newLibraryWithSource:source options:nil error:&error];
    if (!library) return false;
    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [library newFunctionWithName:@"v"];
    pd.fragmentFunction = [library newFunctionWithName:@"f"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pipeline = [dev newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!pipeline) return false;
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0,0,0,1);
    id<MTLCommandQueue> queue = [dev newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:3 indexType:MTLIndexTypeUInt16 indexBuffer:(__bridge id<MTLBuffer>)indices.buffer indexBufferOffset:indices.offset instanceCount:1 baseVertex:0 baseInstance:0];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    unsigned char pixel[4] = {};
    [texture getBytes:pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(32,32,1,1) mipmapLevel:0];
    return command.status == MTLCommandBufferStatusCompleted && pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0;
}}

// bbport: drawPrimitives:indirectBuffer from the binding returned by BufferCache (the
// indirectBufferOffset is 32-bit like the index offset).
bool RenderIndirectTriangle(Metal::Device& device, Metal::BufferRef args) { @autoreleasepool {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    NSString* source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint id [[vertex_id]]) { const float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return id<3?float4(p[id],0,1):float4(0); }\n"
        "fragment float4 f() {return float4(0,1,0,1);}";
    NSError* error = nil;
    id<MTLLibrary> library = [dev newLibraryWithSource:source options:nil error:&error];
    if (!library) return false;
    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [library newFunctionWithName:@"v"];
    pd.fragmentFunction = [library newFunctionWithName:@"f"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pipeline = [dev newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!pipeline) return false;
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64 height:64 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0,0,0,1);
    id<MTLCommandQueue> queue = [dev newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle indirectBuffer:(__bridge id<MTLBuffer>)args.buffer indirectBufferOffset:args.offset];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    unsigned char pixel[4] = {};
    [texture getBytes:pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(32,32,1,1) mipmapLevel:0];
    return command.status == MTLCommandBufferStatusCompleted && pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0;
}}

// bbport: dispatchThreadgroupsWithIndirectBuffer from the binding returned by BufferCache: each
// thread group adds one to a counter, so the result is the group count the GPU read.
unsigned RunIndirectDispatch(Metal::Device& device, Metal::BufferRef args) { @autoreleasepool {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    NSString* source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void k(device atomic_uint* out [[buffer(0)]]) { atomic_fetch_add_explicit(out, 1u, memory_order_relaxed); }";
    NSError* error = nil;
    id<MTLLibrary> library = [dev newLibraryWithSource:source options:nil error:&error];
    if (!library) return ~0u;
    id<MTLComputePipelineState> pipeline =
        [dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"k"] error:&error];
    if (!pipeline) return ~0u;
    id<MTLBuffer> out = [dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
    *static_cast<unsigned*>(out.contents) = 0;
    id<MTLCommandQueue> queue = [dev newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:out offset:0 atIndex:0];
    [encoder dispatchThreadgroupsWithIndirectBuffer:(__bridge id<MTLBuffer>)args.buffer indirectBufferOffset:args.offset threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    return command.status == MTLCommandBufferStatusCompleted ? *static_cast<unsigned*>(out.contents) : ~0u;
}}
