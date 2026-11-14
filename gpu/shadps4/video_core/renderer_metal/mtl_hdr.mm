// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from lKaniel/bloodborne-server at 8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1:
// bbport/gpu/shadps4/video_core/host_shaders/hdr_{exposure,gain}.comp and hdr_highlight.glsl.
// The initial gain/exposure adaptation is replaced by native scene-radiance tone mapping.
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
// Tone-map actual exposed scene radiance. The shoulder is continuous in value and
// slope at the knee and reduces to 1-exp(-x) at peak=1. Hue is preserved by mapping
// the largest channel and scaling RGB together.
struct NativeParams {
    float headroom, paper_white, peak, gamma;
    float4 matrix[3];
    uint has_lut, has_ui, ui_linear, flags;
};
float3 encoded_hdr(float3 c) {
    c = max(c, 0.0f);
    return select(1.055f * pow(c, float3(1.0f / 2.4f)) - 0.055f,
                  12.92f * c, c <= 0.0031308f);
}
float shoulder(float value, float peak) {
    constexpr float knee = 0.25f;
    float anchor = 1.0f - exp(-knee);
    if (value <= knee) return 1.0f - exp(-value);
    float room = max(peak - anchor, 1e-4f);
    return anchor + room * (1.0f - exp(-exp(-knee) * (value - knee) / room));
}
float3 bloom_energy(float3 b) {
    // Extend the original screen-blend bloom continuously above its old clipping
    // threshold. Differently bright blooms remain distinct even above SDR white.
    return select(-log(max(1.0f - b, 0.1f)),
                  float3(-log(0.1f)) + 10.0f * (b - 0.9f), b > 0.9f);
}
kernel void hdr_native(texture2d<float> lighting [[texture(0)]],
                       texture2d<float> bloom [[texture(1)]],
                       texture3d<float> grading [[texture(2)]],
                       texture2d<float> ui_image [[texture(3)]],
                       texture2d<float> ui_white [[texture(4)]],
                       texture2d<float, access::write> output [[texture(5)]],
                       constant NativeParams& p [[buffer(0)]],
                       uint2 at [[thread_position_in_grid]]) {
    if (at.x >= output.get_width() || at.y >= output.get_height()) return;
    float2 uv = (float2(at) + 0.5f) / float2(output.get_width(), output.get_height());
    float4 light = lighting.sample(sample_linear, uv);
    float3 energy = finite_color(light.rgb) + bloom_energy(finite_color(bloom.sample(sample_linear, uv).rgb));
    float white = clamp(p.paper_white, 0.5f, 2.0f);
    float headroom = p.peak > 0.0f ? min(p.headroom, p.peak) : p.headroom;
    float cap = pow(encoded_hdr(float3(headroom / white)).x, 1.0f / p.gamma);
    float brightest = max(energy.r, max(energy.g, energy.b));
    float3 tone = energy * (shoulder(brightest, cap) / max(brightest, 1e-6f));
    float4 c = float4(tone, 1.0f);
    tone = max(float3(dot(p.matrix[0], c), dot(p.matrix[1], c), dot(p.matrix[2], c)), 0.0f);
    float noise = isfinite(light.a) ? light.a : 0.0f;
    float3 rgb = finite_color(pow(tone, float3(p.gamma)) + noise);
    if (p.has_lut) {
        // The game's SDR LUT is defined over [0,1]. Apply it to normalised chroma
        // above white and restore the excursion, preserving real HDR luminance.
        float excursion = max(1.0f, max(rgb.r, max(rgb.g, rgb.b)));
        float n = float(grading.get_width());
        float3 input = (p.flags & 2u) ? rgb.bgr : rgb;
        float3 coord = clamp(input / excursion, 0.0f, 1.0f) * ((n - 1.0f) / n) + 0.5f / n;
        rgb = finite_color(grading.sample(sample_linear, coord).rgb) * excursion;
    }
    float3 result = clamp(linear(rgb) * white, 0.0f, headroom);
    if (p.has_ui) {
        float4 ui = ui_image.sample(sample_linear, uv);
        ui.rgb = finite_color(ui.rgb);
        ui.a = isfinite(ui.a) ? clamp(ui.a, 0.0f, 1.0f) : 0.0f;
        if (p.flags & 1u) ui.rgb = ui.bgr;
        if (p.ui_linear) ui.rgb = encoded_hdr(ui.rgb);
        float3 transmission = float3(1.0f - ui.a);
        if (p.flags & 4u) {
            float3 white_ui = finite_color(ui_white.sample(sample_linear, uv).rgb);
            if (p.flags & 1u) white_ui = white_ui.bgr;
            transmission = white_ui - ui.rgb;
        }
        if (all(abs(transmission - (1.0f - ui.a)) < 0.004f)) {
            float3 ui_rgb = linear(clamp(ui.rgb / max(ui.a, 1e-5f), 0.0f, 1.0f));
            result = result * (1.0f - ui.a) + ui_rgb * ui.a;
        } else {
            // Actual affine RGB blend coefficients from replay, including multiply
            // and additive UI. No scene-image comparison or guessed HUD mask.
            result = clamp(linear(max(transmission * encoded_hdr(result) + clamp(ui.rgb, 0.0f, 1.0f), 0.0f)),
                           0.0f, headroom);
        }
    }
    output.write(float4(result, 1.0f), at);
}
kernel void hdr_copy(texture2d<float> source [[texture(0)]],
                     texture2d<float, access::write> output [[texture(1)]],
                     constant uint& native [[buffer(0)]],
                     uint2 at [[thread_position_in_grid]]) {
    if (at.x >= output.get_width() || at.y >= output.get_height()) return;
    float2 uv = (float2(at) + 0.5f) / float2(output.get_width(), output.get_height());
    float3 color = source.sample(sample_linear, uv).rgb;
    output.write(float4(finite_color(native ? color : linear(color)), 1.0f), at);
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
    Object library, native, copy, present, snapshot, dummy_lut;
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
    impl->native = Object::Adopt((__bridge_retained void*)[native newComputePipelineStateWithFunction:
        [library newFunctionWithName:@"hdr_native"] error:&error]);
    ASSERT_MSG(impl->native, "HDR native pipeline failed");
    impl->copy = Object::Adopt((__bridge_retained void*)[native newComputePipelineStateWithFunction:
        [library newFunctionWithName:@"hdr_copy"] error:&error]);
    ASSERT_MSG(impl->copy, "HDR copy pipeline failed");
    impl->dummy_lut = CreateTexture(device, {.type = TextureType::Tex3D,
        .pixel_format = u32(MTLPixelFormatRGBA8Unorm)}, "HDR unused LUT binding", nullptr);
    for (auto [slot, fragment, format] : {
        std::tuple{&impl->present, "hdr_present", u32(MTLPixelFormatRGBA16Float)},
        std::tuple{&impl->snapshot, "hdr_snapshot", u32(MTLPixelFormatBGRA8Unorm)}}) {
        *slot = CreateRenderPipeline(device, {.library = impl->library.Get(),
            .vertex_function = "hdr_triangle", .fragment_function = fragment, .color_format = format}, fragment);
    }
}
HdrPasses::~HdrPasses() = default;

void HdrPasses::ConvertNative(Encoder& encoder, void* lighting, void* bloom, void* lut, void* ui,
                              void* output, const NativeHdrParams& params, void* ui_white) {
    encoder.EndEncoding();
    auto p = params;
    p.headroom = SafeHeadroom(p.headroom);
    p.paper_white = std::isfinite(p.paper_white) ? std::clamp(p.paper_white, 0.5f, 2.0f) : 1.0f;
    p.peak = std::isfinite(p.peak) && p.peak > 0.0f ? SafeHeadroom(p.peak) : 0.0f;
    p.gamma = std::isfinite(p.gamma) ? std::clamp(p.gamma, 0.1f, 4.0f) : 0.45454545f;
    id<MTLTexture> grading = (__bridge id<MTLTexture>)lut;
    p.has_lut = p.has_lut && grading && grading.textureType == MTLTextureType3D &&
        grading.width >= 2 && grading.width == grading.height && grading.width == grading.depth;
    if (!p.has_lut) lut = impl->dummy_lut.Get();
    p.has_ui = p.has_ui && ui;
    if (ui_white) p.flags |= 4u; else p.flags &= ~4u;
    p.ui_linear = ui && (TexturePixelFormat(ui) == MTLPixelFormatRGBA8Unorm_sRGB ||
                        TexturePixelFormat(ui) == MTLPixelFormatBGRA8Unorm_sRGB);
    id<MTLCommandBuffer> cmd = (__bridge id<MTLCommandBuffer>)encoder.CommandBuffer();
    id<MTLComputeCommandEncoder> e = [cmd computeCommandEncoder];
    e.label = @"HDR scene radiance tone mapping and UI composition";
    [e setComputePipelineState:(__bridge id<MTLComputePipelineState>)impl->native.Get()];
    u32 slot = 0;
    for (void* texture : {lighting, bloom, lut, ui ? ui : lighting, ui_white ? ui_white : lighting, output})
        [e setTexture:(__bridge id<MTLTexture>)texture atIndex:slot++];
    [e setBytes:&p length:sizeof(p) atIndex:0];
    id<MTLTexture> dst = (__bridge id<MTLTexture>)output;
    [e dispatchThreads:MTLSizeMake(dst.width, dst.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [e endEncoding];
    for (void* object : {lighting, bloom, lut, ui, ui_white, output})
        if (object) encoder.KeepAlive(Object::Retain(object));
}
void HdrPasses::CopyFrame(Encoder& encoder, void* source, void* output, bool native) {
    encoder.EndEncoding();
    id<MTLCommandBuffer> cmd = (__bridge id<MTLCommandBuffer>)encoder.CommandBuffer();
    id<MTLComputeCommandEncoder> e = [cmd computeCommandEncoder];
    e.label = @"HDR frame copy / SDR fallback";
    [e setComputePipelineState:(__bridge id<MTLComputePipelineState>)impl->copy.Get()];
    [e setTexture:(__bridge id<MTLTexture>)source atIndex:0];
    [e setTexture:(__bridge id<MTLTexture>)output atIndex:1];
    u32 mode = native;
    [e setBytes:&mode length:sizeof(mode) atIndex:0];
    id<MTLTexture> dst = (__bridge id<MTLTexture>)output;
    [e dispatchThreads:MTLSizeMake(dst.width, dst.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [e endEncoding];
    encoder.KeepAlive(Object::Retain(source)); encoder.KeepAlive(Object::Retain(output));
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
