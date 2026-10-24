// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// Fragment inputs the vertex stage does not write (mtl_spirv_inputs.h): Metal refuses the
// pipeline as compiled; after ZeroUnwrittenInputs it builds, and the zeroed inputs read zero.
#import <Metal/Metal.h>

#include <cassert>
#include <cstdio>
#include <span>
#include <string>
#include <vector>
#include <spirv_cross/spirv_msl.hpp>

#include "video_core/host_shaders/unwritten_inputs_frag.h"
#include "video_core/host_shaders/unwritten_inputs_vert.h"
#include "video_core/renderer_metal/mtl_spirv_inputs.h"

namespace {

unsigned failures = 0;

void Check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    failures += !ok;
}

std::string ToMsl(std::span<const u32> spirv) {
    spirv_cross::CompilerMSL msl(spirv.data(), spirv.size());
    spirv_cross::CompilerMSL::Options options;
    options.set_msl_version(3, 1);
    msl.set_msl_options(options);
    return msl.compile();
}

id<MTLFunction> Function(id<MTLDevice> device, const std::string& source) {
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:@(source.c_str()) options:nil error:&error];
    if (!library) {
        std::printf("MSL did not compile: %s\n%s\n", error.localizedDescription.UTF8String,
                    source.c_str());
        return nil;
    }
    return [library newFunctionWithName:@"main0"];
}

id<MTLRenderPipelineState> Pipeline(id<MTLDevice> device, id<MTLFunction> vs, id<MTLFunction> fs,
                                    std::string& error) {
    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = vs;
    desc.fragmentFunction = fs;
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    NSError* ns_error = nil;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:desc
                                                                               error:&ns_error];
    if (!state && ns_error) {
        error = ns_error.localizedDescription.UTF8String;
    }
    return state;
}

} // namespace

int main() {
    @autoreleasepool {
        const std::span<const u32> vert{UNWRITTEN_INPUTS_VERT, std::size(UNWRITTEN_INPUTS_VERT)};
        const std::span<const u32> frag{UNWRITTEN_INPUTS_FRAG, std::size(UNWRITTEN_INPUTS_FRAG)};

        const auto written = Metal::OutputLocations(vert);
        Check(written && *written == 0b11, "OutputLocations: the vertex shader writes 0 and 1");
        Check(!Metal::ZeroUnwrittenInputs(frag, u64(1) << 7).ok,
              "ZeroUnwrittenInputs: no input at the locations fails");

        const auto rewrite = Metal::ZeroUnwrittenInputs(frag, ~*written);
        Check(rewrite.ok && rewrite.zeroed == 3, "ZeroUnwrittenInputs: three inputs zeroed");
        if (!rewrite.ok) {
            std::printf("  error: %s\n", rewrite.error.c_str());
            return 1;
        }
        const std::string msl = ToMsl(rewrite.spirv);
        Check(msl.find("user(locn0)") != std::string::npos &&
                  msl.find("user(locn1)") != std::string::npos &&
                  msl.find("user(locn2)") == std::string::npos &&
                  msl.find("user(locn3)") == std::string::npos &&
                  msl.find("user(locn4)") == std::string::npos,
              "MSL of the rewrite: inputs 0 and 1 only");

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        assert(device);
        id<MTLFunction> vs = Function(device, ToMsl(vert));
        id<MTLFunction> fs_original = Function(device, ToMsl(frag));
        id<MTLFunction> fs_zeroed = Function(device, msl);
        assert(vs && fs_original && fs_zeroed);
        std::string error;
        Check(Pipeline(device, vs, fs_original, error) == nil &&
                  error.find("not written by vertex shader") != std::string::npos,
              "Metal refuses the original pipeline (fragment inputs not written)");
        error.clear();
        Check(Pipeline(device, vs, fs_zeroed, error) != nil, "Metal builds the rewritten pipeline");
        if (!error.empty()) {
            std::printf("  error: %s\n", error.c_str());
        }

        // Draw one pixel of a 1x1 target with the rewritten pipeline.
        MTLTextureDescriptor* td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                               width:1
                                                              height:1
                                                           mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> target = [device newTextureWithDescriptor:td];
        id<MTLRenderPipelineState> state = Pipeline(device, vs, fs_zeroed, error);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> cmd = [queue commandBuffer];
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
        [enc setRenderPipelineState:state];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];
        float px[4]{};
        [target getBytes:px bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
        // At the single pixel's centre the triangle's p is (0.5, 0.5): in0 = (0.5, 0.5, 0, 1),
        // in1 = (1, 0.5, 0.5, 1); the zeroed inputs add nothing.
        const auto near = [](float a, float b) { return a > b - 1e-4f && a < b + 1e-4f; };
        const bool exact = near(px[0], 1.5f) && near(px[1], 1.0f) && near(px[2], 0.5f) &&
                           near(px[3], 2.0f);
        std::printf("  pixel %.3f %.3f %.3f %.3f\n", px[0], px[1], px[2], px[3]);
        Check(exact, "the zeroed inputs read zero (pixel = in0 + in1)");
    }
    std::printf(failures ? "spirv-inputs-test: %u FAILED\n" : "spirv-inputs-test: all passed\n",
                failures);
    return failures ? 1 : 0;
}
