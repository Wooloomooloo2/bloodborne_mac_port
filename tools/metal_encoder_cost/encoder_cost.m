// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// Same work, split into N encoders vs one encoder: per-encoder GPU overhead on Apple GPUs.
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
static NSString* src = @"#include <metal_stdlib>\nusing namespace metal;\n"
"kernel void k(device float* b [[buffer(0)]], uint i [[thread_position_in_grid]]) { float x=b[i]; for(int j=0;j<256;j++) x=x*1.0001f+0.5f; b[i]=x; }\n"
"struct V { float4 p [[position]]; };\n"
"vertex V vs(uint id [[vertex_id]], constant float& s [[buffer(0)]]) { V v; float2 c=float2((id<<1)&2, id&2); v.p=float4((c*2-1)*s,0,1); return v; }\n"
"fragment float4 fs(V in [[stage_in]]) { float x=in.p.x; for(int j=0;j<16;j++) x=sin(x)*1.01; return float4(x*0.001, 0.2, 0.3, 1); }\n";
int main(int argc, char** argv) {
  @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    NSError* err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
    id<MTLComputePipelineState> cps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&err];
    MTLRenderPipelineDescriptor* rd = [MTLRenderPipelineDescriptor new];
    rd.vertexFunction = [lib newFunctionWithName:@"vs"]; rd.fragmentFunction = [lib newFunctionWithName:@"fs"];
    rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    rd.colorAttachments[1].pixelFormat = MTLPixelFormatRGBA8Unorm;
    rd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    id<MTLRenderPipelineState> rps = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
    if (!rps) { printf("%s\n", err.description.UTF8String); return 1; }
    id<MTLBuffer> buf = [dev newBufferWithLength:4*65536 options:MTLResourceStorageModePrivate];
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:1920 height:1080 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> rt0 = [dev newTextureWithDescriptor:td];
    td.pixelFormat = MTLPixelFormatRGBA8Unorm; id<MTLTexture> rt1 = [dev newTextureWithDescriptor:td];
    td.pixelFormat = MTLPixelFormatDepth32Float; id<MTLTexture> dt = [dev newTextureWithDescriptor:td];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    float scale = 0.15f; // each draw covers a small part of the screen (like a typical game draw)
    int counts[] = {50, 100, 200, 400};
    for (int split = 0; split < 2; ++split)
    for (int kind = 0; kind < 2; ++kind)
    for (int ci = 0; ci < 4; ++ci) {
      int n = counts[ci];
      double best = 1e9;
      for (int rep = 0; rep < 9; ++rep) {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLRenderCommandEncoder> re = nil; id<MTLComputeCommandEncoder> ce = nil;
        for (int e = 0; e < n; ++e) {
          if (kind == 0) {
            if (!re) {
              MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
              rp.colorAttachments[0].texture = rt0; rp.colorAttachments[0].loadAction = MTLLoadActionLoad; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
              rp.colorAttachments[1].texture = rt1; rp.colorAttachments[1].loadAction = MTLLoadActionLoad; rp.colorAttachments[1].storeAction = MTLStoreActionStore;
              rp.depthAttachment.texture = dt; rp.depthAttachment.loadAction = MTLLoadActionLoad; rp.depthAttachment.storeAction = MTLStoreActionStore;
              re = [cb renderCommandEncoderWithDescriptor:rp]; [re setRenderPipelineState:rps];
            }
            [re setVertexBytes:&scale length:4 atIndex:0];
            [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            if (split) { [re endEncoding]; re = nil; }
          } else {
            if (!ce) { ce = [cb computeCommandEncoder]; [ce setComputePipelineState:cps]; [ce setBuffer:buf offset:0 atIndex:0]; }
            else [ce memoryBarrierWithScope:MTLBarrierScopeBuffers];
            [ce dispatchThreads:MTLSizeMake(16384,1,1) threadsPerThreadgroup:MTLSizeMake(256,1,1)];
            if (split) { [ce endEncoding]; ce = nil; }
          }
        }
        if (re) [re endEncoding]; if (ce) [ce endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        double t = (cb.GPUEndTime - cb.GPUStartTime) * 1e3;
        if (rep > 1 && t < best) best = t;
      }
      printf("%-7s %-13s n=%4d  GPU %.3f ms  (%.1f us each)\n", kind ? "compute" : "render",
             split ? "own encoders" : "one encoder", n, best, best * 1e3 / n);
    }
  }
  return 0;
}
