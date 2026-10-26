// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from lKaniel/bloodborne-server at 8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1:
// bbport/gpu/shadps4/video_core/host_shaders/hdr_{exposure,gain}.comp and hdr_highlight.glsl.
// Native Metal dispatches, EDR output and per-frame exposure replace the Vulkan/PQ backend.
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <tuple>
#include "common/assert.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_hdr.h"
#include "video_core/renderer_metal/mtl_present.h"
#include "video_core/renderer_metal/mtl_render_pipeline.h"
#include "video_core/renderer_metal/mtl_texture.h"

namespace Metal {
namespace {
constexpr const char* Source = R"msl(
#include <metal_stdlib>
using namespace metal;
float3 linear(float3 c) {
    c = max(c, 0.0f);
    return select(pow((c + 0.055f) / 1.055f, float3(2.4f)), c / 12.92f, c <= 0.04045f);
}
float3 encoded(float3 c) {
    c = clamp(c, 0.0f, 1.0f);
    return select(1.055f * pow(c, float3(1.0f / 2.4f)) - 0.055f, c * 12.92f, c <= 0.0031308f);
}
float luma(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }
float3 finite_color(float3 c) { return select(float3(0.0f), max(c, 0.0f), isfinite(c)); }
constexpr sampler sample_linear(coord::normalized, address::clamp_to_edge, filter::linear);
struct Params { float headroom, highlights; uint captures; uint padding; };

// One group, with a 64x36 stratified sample of the matching scene and UI-free frame.
kernel void hdr_exposure(texture2d<float> scene [[texture(0)]],
                         texture2d<float> clean [[texture(1)]],
                         device float2& exposure [[buffer(0)]],
                         uint id [[thread_index_in_threadgroup]]) {
    threadgroup float sums[256], counts[256];
    float sum = 0.0f, count = 0.0f;
    for (uint i = id; i < 64u * 36u; i += 256u) {
        float2 uv = (float2(i % 64u, i / 64u) + 0.5f) / float2(64.0f, 36.0f);
        float ls = luma(clean.sample(sample_linear, uv).rgb);
        float lh = luma(scene.sample(sample_linear, uv).rgb);
        if (ls > 0.03f && ls < 0.3f && lh > 1e-5f && isfinite(lh)) {
            sum += log(ls / lh); count += 1.0f;
        }
    }
    sums[id] = sum; counts[id] = count;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint stride = 128u; stride; stride /= 2u) {
        if (id < stride) { sums[id] += sums[id + stride]; counts[id] += counts[id + stride]; }
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (id == 0u) exposure = counts[0] >= 32.0f ? float2(sums[0] / counts[0], 1.0f) : float2(0.0f);
}
float highlight_gain(float ls, float headroom, float highlights) {
    if (highlights <= 0.0f || headroom <= 1.0f) return 1.0f;
    float h = clamp(highlights, 0.0f, 1.0f);
    float knee = h <= 0.5f ? mix(log2(0.25f), log2(0.12f), 2.0f * h)
                           : mix(log2(0.12f), log2(0.03f), 2.0f * h - 1.0f);
    float t = clamp((log2(max(ls, 1e-6f)) - knee) / (log2(0.7f) - knee), 0.0f, 1.0f);
    float top = mix(0.7f, headroom, min(1.0f, 2.0f * h)) / 0.7f;
    float shape = mix(2.0f, 1.25f, clamp(2.0f * h - 1.0f, 0.0f, 1.0f));
    return min(mix(1.0f, top, pow(t, shape)), max(headroom / max(ls, 1e-4f), 1.0f));
}
kernel void hdr_convert(texture2d<float> frame [[texture(0)]],
                        texture2d<float> scene [[texture(1)]],
                        texture2d<float> clean [[texture(2)]],
                        texture2d<float, access::write> output [[texture(3)]],
                        constant Params& p [[buffer(0)]],
                        device const float2& exposure [[buffer(1)]],
                        uint2 at [[thread_position_in_grid]]) {
    if (at.x >= output.get_width() || at.y >= output.get_height()) return;
    float2 uv = (float2(at) + 0.5f) / float2(output.get_width(), output.get_height());
    float3 sdr = finite_color(linear(frame.sample(sample_linear, uv).rgb));
    float gain = 1.0f;
    if (p.captures && p.highlights > 0.0f && p.headroom > 1.0f) {
        float3 c = finite_color(clean.sample(sample_linear, uv).rgb);
        float3 diff = abs(encoded(sdr) - encoded(c));
        float ui = smoothstep(0.10f, 0.25f, max(diff.r, max(diff.g, diff.b)));
        float ls = luma(c);
        float2 px = 1.5f / float2(clean.get_width(), clean.get_height());
        float area = 0.25f * (luma(clean.sample(sample_linear, uv - px).rgb) +
                     luma(clean.sample(sample_linear, uv + px).rgb) +
                     luma(clean.sample(sample_linear, uv + float2(px.x, -px.y)).rgb) +
                     luma(clean.sample(sample_linear, uv + float2(-px.x, px.y)).rgb));
        float expand = highlight_gain(min(ls, mix(area, ls, 0.5f)), p.headroom, p.highlights);
        float scene_gain = 1.0f;
        if (exposure.y > 0.5f) {
            float lh = luma(finite_color(scene.sample(sample_linear, uv).rgb)) * exp(exposure.x);
            float g = isfinite(lh) ? clamp(lh / max(ls, 1e-4f), 1.0f, p.headroom) : 1.0f;
            scene_gain = exp(log(g) * smoothstep(0.25f, 0.8f, ls));
        }
        gain = mix(max(expand, scene_gain), 1.0f, ui);
    }
    output.write(float4(clamp(sdr * gain, 0.0f, p.headroom), 1.0f), at);
}
struct Varying { float4 position [[position]]; float2 uv; };
vertex Varying hdr_triangle(uint id [[vertex_id]]) {
    float2 pos = float2(float((id & 1u) << 2u), float((id & 2u) << 1u));
    return {float4(pos.x - 1.0f, 1.0f - pos.y, 0.0f, 1.0f), pos * 0.5f};
}
struct PresentParams { float4 fit; float headroom; };
fragment float4 hdr_present(Varying in [[stage_in]], texture2d<float> frame [[texture(0)]],
                            texture2d<float> overlay [[texture(1)]],
                            constant PresentParams& p [[buffer(0)]]) {
    float2 uv = (in.position.xy - p.fit.xy) / max(p.fit.zw, 1.0f);
    float3 color = float3(0.0f);
    if (p.fit.z > 0.0f && p.fit.w > 0.0f && all(uv >= 0.0f) && all(uv <= 1.0f))
        color = finite_color(frame.sample(sample_linear, uv).rgb);
    float4 ui = overlay.sample(sample_linear, in.uv);
    // ImGui's BGRA8 target contains premultiplied, gamma-encoded RGB.
    float3 ui_linear = linear(ui.rgb / max(ui.a, 1e-5f)) * ui.a;
    return float4(clamp(color, 0.0f, p.headroom) * (1.0f - ui.a) + ui_linear, 1.0f);
}
fragment float4 hdr_snapshot(Varying in [[stage_in]], texture2d<float> frame [[texture(0)]]) {
    return float4(encoded(finite_color(frame.sample(sample_linear, in.uv).rgb)), 1.0f);
}
)msl";
float SafeHeadroom(float value) {
    return std::isfinite(value) ? std::clamp(value, 1.0f, 16.0f) : 1.0f;
}
}

DisplayRange QueryDisplayRange(void* window) {
    NSWindow* native = (__bridge NSWindow*)window;
    NSScreen* screen = native.screen;
    return screen ? DisplayRange{SafeHeadroom(screen.maximumExtendedDynamicRangeColorComponentValue),
                                 SafeHeadroom(screen.maximumPotentialExtendedDynamicRangeColorComponentValue)}
                  : DisplayRange{};
}

struct HdrPasses::Impl {
    Object library, exposure, convert, present, snapshot, exposure_buffer;
};
HdrPasses::HdrPasses(Device& device) : impl{std::make_unique<Impl>()} {
    id<MTLDevice> native = (__bridge id<MTLDevice>)device.NativeDevice();
    NSError* error = nil;
    // The scene may contain non-finite values: fast math must not remove the guards.
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.mathMode = MTLMathModeSafe;
    id<MTLLibrary> library = [native newLibraryWithSource:@(Source) options:options error:&error];
    ASSERT_MSG(library, "HDR shaders failed to compile");
    impl->library = Object::Adopt((__bridge_retained void*)library);
    impl->exposure = Object::Adopt((__bridge_retained void*)[native newComputePipelineStateWithFunction:
        [library newFunctionWithName:@"hdr_exposure"] error:&error]);
    ASSERT_MSG(impl->exposure, "HDR exposure pipeline failed");
    impl->convert = Object::Adopt((__bridge_retained void*)[native newComputePipelineStateWithFunction:
        [library newFunctionWithName:@"hdr_convert"] error:&error]);
    ASSERT_MSG(impl->convert, "HDR convert pipeline failed");
    impl->exposure_buffer = Object::Adopt((__bridge_retained void*)[native newBufferWithLength:8
        options:MTLResourceStorageModePrivate]);
    ASSERT_MSG(impl->exposure_buffer, "HDR exposure buffer failed");
    for (auto [slot, fragment, format] : {
        std::tuple{&impl->present, "hdr_present", u32(MTLPixelFormatRGBA16Float)},
        std::tuple{&impl->snapshot, "hdr_snapshot", u32(MTLPixelFormatBGRA8Unorm)}}) {
        *slot = CreateRenderPipeline(device, {.library = impl->library.Get(),
            .vertex_function = "hdr_triangle", .fragment_function = fragment, .color_format = format}, fragment);
    }
}
HdrPasses::~HdrPasses() = default;

void HdrPasses::Convert(Encoder& encoder, void* frame, void* scene, void* hudless, void* output,
                        float headroom, float highlights) {
    encoder.EndEncoding();
    id<MTLCommandBuffer> cmd = (__bridge id<MTLCommandBuffer>)encoder.CommandBuffer();
    const bool captures = scene && hudless;
    id<MTLTexture> src = (__bridge id<MTLTexture>)frame;
    id<MTLTexture> dst = (__bridge id<MTLTexture>)output;
    id<MTLBuffer> exposure = (__bridge id<MTLBuffer>)impl->exposure_buffer.Get();
    if (captures) {
        id<MTLComputeCommandEncoder> e = [cmd computeCommandEncoder];
        e.label = @"HDR exposure";
        [e setComputePipelineState:(__bridge id<MTLComputePipelineState>)impl->exposure.Get()];
        [e setTexture:(__bridge id<MTLTexture>)scene atIndex:0];
        [e setTexture:(__bridge id<MTLTexture>)hudless atIndex:1];
        [e setBuffer:exposure offset:0 atIndex:0];
        [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [e endEncoding];
    }
    struct Params { float headroom, highlights; u32 captures, padding; } params{
        SafeHeadroom(headroom), std::isfinite(highlights) ? std::clamp(highlights, 0.0f, 1.0f) : 0.5f,
        u32(captures), 0};
    id<MTLComputeCommandEncoder> e = [cmd computeCommandEncoder];
    e.label = @"HDR scene highlights";
    [e setComputePipelineState:(__bridge id<MTLComputePipelineState>)impl->convert.Get()];
    [e setTexture:src atIndex:0];
    [e setTexture:captures ? (__bridge id<MTLTexture>)scene : src atIndex:1];
    [e setTexture:captures ? (__bridge id<MTLTexture>)hudless : src atIndex:2];
    [e setTexture:dst atIndex:3];
    [e setBytes:&params length:sizeof(params) atIndex:0];
    [e setBuffer:exposure offset:0 atIndex:1];
    [e dispatchThreads:MTLSizeMake(dst.width, dst.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [e endEncoding];
    for (void* object : {frame, scene, hudless, output, impl->exposure_buffer.Get()})
        if (object) encoder.KeepAlive(Object::Retain(object));
}

void HdrPasses::ClearOverlay(void* command_buffer, void* texture) {
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = (__bridge id<MTLTexture>)texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [(__bridge id<MTLCommandBuffer>)command_buffer renderCommandEncoderWithDescriptor:pass];
    [e endEncoding];
}

void HdrPasses::Present(void* command_buffer, void* frame, u32 width, u32 height, void* overlay,
                        void* target, u32 target_width, u32 target_height, float headroom) {
    const auto fit = FitImage(width, height, target_width, target_height);
    struct alignas(16) Params { float fit[4]; float headroom; float padding[3]{}; } params{
        {float(fit.x), float(fit.y), float(fit.width), float(fit.height)}, SafeHeadroom(headroom)};
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = (__bridge id<MTLTexture>)target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [(__bridge id<MTLCommandBuffer>)command_buffer renderCommandEncoderWithDescriptor:pass];
    e.label = @"HDR EDR present";
    [e setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)impl->present.Get()];
    [e setViewport:(MTLViewport){0, 0, double(target_width), double(target_height), 0, 1}];
    [e setFragmentTexture:(__bridge id<MTLTexture>)(frame ? frame : overlay) atIndex:0];
    [e setFragmentTexture:(__bridge id<MTLTexture>)overlay atIndex:1];
    if (!frame) params.fit[2] = params.fit[3] = 0; // no scene: only overlay over black
    [e setFragmentBytes:&params length:sizeof(params) atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

Object HdrPasses::Snapshot(Device& device, void* command_buffer, void* texture, u32 width, u32 height) {
    Object sdr = CreateTexture(device, {.pixel_format = MTLPixelFormatBGRA8Unorm,
        .width = width, .height = height, .render_target = true}, "HDR SDR snapshot", nullptr);
    if (!sdr) return {};
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = (__bridge id<MTLTexture>)sdr.Get();
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [(__bridge id<MTLCommandBuffer>)command_buffer renderCommandEncoderWithDescriptor:pass];
    [e setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)impl->snapshot.Get()];
    [e setFragmentTexture:(__bridge id<MTLTexture>)texture atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    // The draw scheduler uses unretained command buffers. Keep this temporary until its
    // conversion and readback have both finished, including the standalone fixture path.
    id<MTLTexture> retained = (__bridge id<MTLTexture>)sdr.Get();
    [(__bridge id<MTLCommandBuffer>)command_buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
        (void)retained;
    }];
    return EncodeSnapshot(device, command_buffer, sdr.Get(), width, height);
}
} // namespace Metal
