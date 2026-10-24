// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: render pipelines for the renderer's own hand-written MSL programs (Metal-native
// renderer).

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <string>
#include "common/assert.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_render_pipeline.h"

namespace Metal {

namespace {

NSString* ToNSString(std::string_view text) {
    return [[NSString alloc] initWithBytes:text.data()
                                    length:text.size()
                                  encoding:NSUTF8StringEncoding];
}

} // namespace

Object CompileLibrary(Device& device, std::string_view source, std::string_view name) {
    id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device.NativeDevice();
    NSError* error = nil;
    id<MTLLibrary> library = [mtl_device newLibraryWithSource:ToNSString(source)
                                                      options:nil
                                                        error:&error];
    ASSERT_MSG(library != nil, "MSL library {} did not compile: {}", name,
               error ? error.localizedDescription.UTF8String : "unknown error");
    library.label = ToNSString(name);
    return Object::Adopt((__bridge_retained void*)library);
}

Object CreateRenderPipeline(Device& device, const RenderPipelineDesc& desc,
                            std::string_view label) {
    id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)device.NativeDevice();
    id<MTLLibrary> library = (__bridge id<MTLLibrary>)desc.library;
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.label = ToNSString(label);
    d.vertexFunction = [library newFunctionWithName:@(desc.vertex_function)];
    d.fragmentFunction = [library newFunctionWithName:@(desc.fragment_function)];
    ASSERT_MSG(d.vertexFunction != nil && d.fragmentFunction != nil,
               "Render pipeline {}: no function {} or {}", label, desc.vertex_function,
               desc.fragment_function);
    d.colorAttachments[0].pixelFormat = MTLPixelFormat(desc.color_format);
    d.depthAttachmentPixelFormat = MTLPixelFormat(desc.depth_format);
    d.stencilAttachmentPixelFormat = MTLPixelFormat(desc.stencil_format);
    d.rasterSampleCount = desc.samples;
    NSError* error = nil;
    id<MTLRenderPipelineState> pipeline = [mtl_device newRenderPipelineStateWithDescriptor:d
                                                                                    error:&error];
    ASSERT_MSG(pipeline != nil, "Render pipeline {} not created: {}", label,
               error ? error.localizedDescription.UTF8String : "unknown error");
    return Object::Adopt((__bridge_retained void*)pipeline);
}

} // namespace Metal
