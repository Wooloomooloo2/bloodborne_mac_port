// SPDX-License-Identifier: GPL-2.0-or-later
// Horizon integration adapted from lKaniel/bloodborne-server,
// 8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1,
// bbport/gpu/shadps4/video_core/host_shaders/screen_lighting.{glsl,comp} (GPL-2.0-or-later).
// Native Metal dispatch, guide buffer, bilateral reconstruction and blended apply are local work.
#import <Metal/Metal.h>
#include <algorithm>
#include <cmath>
#include <utility>
#include "common/assert.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_screen_lighting.h"
#include "video_core/renderer_metal/mtl_texture.h"

namespace Metal {
namespace {
constexpr const char* Source = R"msl(
#include <metal_stdlib>
using namespace metal;
struct Params {
    float4 projection;
    uint2 size, trace_size;
    float radius, max_radiance, ao, gi;
    uint directions, steps, view, reserved;
};
float3 finite_positive(float3 c) {
    return select(float3(0.0f), max(c, 0.0f), isfinite(c));
}
float3 view_position(texture2d<float> depth, int2 xy, constant Params& p) {
    xy = clamp(xy, int2(0), int2(p.size) - 1);
    float d = depth.read(uint2(xy)).r;
    if (!isfinite(d) || d >= 1.0f || d < 0.0f) return float3(0.0f);
    float z = p.projection.w / min(d - p.projection.z, -1e-8f);
    float2 ndc = (float2(xy) + 0.5f) / float2(p.size) * 2.0f - 1.0f;
    return float3(ndc * z / p.projection.xy, z);
}
float3 surface_normal(texture2d<float> depth, int2 at, float3 c, constant Params& p) {
    float3 l = view_position(depth, at + int2(-1,0), p);
    float3 r = view_position(depth, at + int2(1,0), p);
    float3 u = view_position(depth, at + int2(0,-1), p);
    float3 d = view_position(depth, at + int2(0,1), p);
    // Choose the nearer neighbour on each axis; silhouettes must not tilt the surface.
    float3 dx = abs(r.z-c.z) < abs(c.z-l.z) ? r-c : c-l;
    float3 dy = abs(d.z-c.z) < abs(c.z-u.z) ? d-c : c-u;
    float3 n = cross(dy, dx);
    float length2 = dot(n,n);
    if (!isfinite(length2) || length2 < 1e-12f) return -normalize(c);
    n *= rsqrt(length2);
    return dot(n,c) > 0.0f ? -n : n;
}
float noise(float2 p) { return fract(52.9829189f * fract(dot(p, float2(0.06711056f,0.00583715f)))); }
kernel void screen_trace(texture2d<float> depth [[texture(0)]],
                         texture2d<float> scene [[texture(1)]],
                         texture2d<half,access::write> trace [[texture(2)]],
                         texture2d<half,access::write> guide [[texture(3)]],
                         constant Params& p [[buffer(0)]], uint2 at [[thread_position_in_grid]]) {
    if (any(at >= p.trace_size)) return;
    int2 pixel = int2((float2(at)+0.5f) * float2(p.size) / float2(p.trace_size));
    float3 pos = view_position(depth, pixel, p);
    if (pos.z <= 0.0f) {
        trace.write(half4(0,0,0,1), at); guide.write(half4(0), at); return;
    }
    float3 normal = surface_normal(depth, pixel, pos, p);
    float reach = clamp(p.radius * abs(p.projection.x) * float(p.size.x) * 0.5f / pos.z,
                        2.0f, max(2.0f, 0.25f * float(p.size.x)));
    float rotation = noise(float2(at)), offset = noise(float2(at)+float2(47,17));
    float3 light = float3(0);
    float visibility = 0.0f;
    for (uint d=0; d<p.directions; ++d) {
        float angle = (float(d)+rotation) * M_PI_F / float(p.directions);
        float2 direction = float2(cos(angle),sin(angle));
        for (uint side=0; side<2; ++side) {
            float2 dir = side ? -direction : direction;
            float horizon = 0.0f;
            for (uint s=0; s<p.steps; ++s) {
                float t = (float(s)+offset) / float(p.steps);
                float2 q = float2(pixel)+0.5f + dir * max(t*t*reach, float(s)+1.0f);
                if (any(q < 0.0f) || any(q >= float2(p.size))) break;
                float3 sample = view_position(depth, int2(q), p);
                if (sample.z <= 0.0f) continue;
                float3 v = sample-pos;
                float distance2 = dot(v,v);
                if (distance2 < 1e-8f || distance2 >= p.radius*p.radius) continue;
                // A small world-space bias avoids self-shadowing on nearly coplanar samples.
                float elevation = max(dot(v,normal)-0.02f, 0.0f) * rsqrt(distance2) *
                                  (1.0f-distance2/(p.radius*p.radius));
                if (elevation > horizon) {
                    float3 radiance = finite_positive(scene.read(uint2(q)).rgb);
                    float peak = max(radiance.r,max(radiance.g,radiance.b));
                    radiance *= min(1.0f,p.max_radiance/max(peak,1e-6f));
                    light += radiance*(elevation-horizon);
                    horizon = elevation;
                }
            }
            visibility += 1.0f-horizon;
        }
    }
    float count = float(2*p.directions);
    trace.write(half4(half3(light/count), half(clamp(visibility/count,0.0f,1.0f))), at);
    guide.write(half4(half3(normal),half(pos.z)), at);
}
vertex float4 screen_triangle(uint id [[vertex_id]]) {
    float2 xy = float2(float((id&1u)<<2u),float((id&2u)<<1u));
    return float4(xy-1.0f,0.0f,1.0f);
}
fragment float4 screen_apply(float4 position [[position]],
                            texture2d<float> depth [[texture(0)]],
                            texture2d<half> trace [[texture(1)]],
                            texture2d<half> guide [[texture(2)]],
                            texture2d<float> albedo [[texture(3)]],
                            constant Params& p [[buffer(0)]]) {
    int2 at = int2(position.xy);
    float3 pos = view_position(depth,at,p);
    if (pos.z <= 0.0f) return float4(0,0,0,1); // leave the sky untouched
    float3 normal = surface_normal(depth,at,pos,p);
    float2 tc = position.xy * float2(p.trace_size) / float2(p.size) - 0.5f;
    int2 base = int2(floor(tc));
    float4 sum = float4(0); float weights = 0.0f;
    for (int y=0; y<2; ++y) for (int x=0; x<2; ++x) {
        int2 coord = base+int2(x,y);
        if (any(coord < 0) || any(coord >= int2(p.trace_size))) continue;
        float4 g = float4(guide.read(uint2(coord)));
        if (g.w <= 0.0f) continue;
        float2 distance = abs(float2(coord)-tc);
        float agreement = clamp(dot(normal,g.xyz),0.0f,1.0f);
        float w = max(1.0f-distance.x,0.0f)*max(1.0f-distance.y,0.0f) *
                  exp(-abs(g.w-pos.z)/max(0.02f*pos.z,0.02f)) *
                  agreement*agreement*agreement*agreement;
        sum += float4(trace.read(uint2(coord)))*w; weights += w;
    }
    float4 light = weights > 1e-6f ? sum/weights : float4(0,0,0,1);
    float visibility = mix(1.0f,clamp(light.a,0.0f,1.0f),p.ao);
    uint2 ap = min(uint2(position.xy * float2(albedo.get_width(),albedo.get_height()) / float2(p.size)),
                   uint2(albedo.get_width()-1,albedo.get_height()-1));
    float3 bounce = finite_positive(albedo.read(ap).rgb) * light.rgb * p.gi;
    if (p.view == 1) return float4(float3(0.06f*visibility),0);
    if (p.view == 2) return float4(bounce*4.0f,0);
    // RGB = source + destination*sourceAlpha. The pipeline masks alpha writes, so
    // the scene's material/coverage alpha survives for particles and post-processing.
    return float4(bounce,visibility);
}
)msl";
}

struct ScreenLightingPasses::Impl {
    Device& device;
    Object library, trace_pipeline, apply_pipeline, trace, guide;
    u32 width{}, height{};
    explicit Impl(Device& d) : device{d} {}
};
ScreenLightingPasses::ScreenLightingPasses(Device& device) : impl{std::make_unique<Impl>(device)} {
    auto native = (__bridge id<MTLDevice>)device.NativeDevice();
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.mathMode = MTLMathModeSafe; // retain finite-value guards
    NSError* error = nil;
    auto library = [native newLibraryWithSource:@(Source) options:options error:&error];
    ASSERT_MSG(library, "Screen lighting MSL: {}",
               error ? error.localizedDescription.UTF8String : "unknown");
    impl->library = Object::Adopt((__bridge_retained void*)library);
    auto compute =
        [native newComputePipelineStateWithFunction:[library newFunctionWithName:@"screen_trace"]
                                              error:&error];
    ASSERT_MSG(compute, "Screen lighting trace pipeline failed");
    impl->trace_pipeline = Object::Adopt((__bridge_retained void*)compute);
    auto desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = [library newFunctionWithName:@"screen_triangle"];
    desc.fragmentFunction = [library newFunctionWithName:@"screen_apply"];
    auto color = desc.colorAttachments[0];
    color.pixelFormat = MTLPixelFormatRGBA16Float;
    color.blendingEnabled = YES;
    color.sourceRGBBlendFactor = MTLBlendFactorOne;
    color.destinationRGBBlendFactor = MTLBlendFactorSourceAlpha;
    color.writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
    auto apply = [native newRenderPipelineStateWithDescriptor:desc error:&error];
    ASSERT_MSG(apply, "Screen lighting blend pipeline failed");
    impl->apply_pipeline = Object::Adopt((__bridge_retained void*)apply);
}
ScreenLightingPasses::~ScreenLightingPasses() = default;
void* ScreenLightingPasses::TraceTexture() const { return impl->trace.Get(); }
void* ScreenLightingPasses::GuideTexture() const { return impl->guide.Get(); }

bool ScreenLightingPasses::Render(Encoder& e, void* depth, void* scene, void* albedo,
                                  const ScreenLightingParams& params) {
    auto d = (__bridge id<MTLTexture>)depth;
    auto c = (__bridge id<MTLTexture>)scene;
    auto a = (__bridge id<MTLTexture>)albedo;
    if (!d || !c || !a || !(c.usage & MTLTextureUsageRenderTarget) ||
        c.pixelFormat != MTLPixelFormatRGBA16Float || c.textureType != MTLTextureType2D ||
        d.textureType != MTLTextureType2D || a.textureType != MTLTextureType2D ||
        c.width != d.width || c.height != d.height || c.width != params.size[0] ||
        c.height != params.size[1] || !params.trace_size[0] || !params.trace_size[1] ||
        params.trace_size[0] > c.width || params.trace_size[1] > c.height)
        return false;
    auto p = params;
    for (float v : p.projection)
        if (!std::isfinite(v))
            return false;
    if (std::abs(p.projection[0]) < 0.01f || std::abs(p.projection[1]) < 0.01f ||
        p.projection[2] < 1.0f || p.projection[3] >= 0.0f)
        return false;
    p.ao = std::isfinite(p.ao) ? std::clamp(p.ao, 0.0f, 1.0f) : 0.0f;
    p.gi = std::isfinite(p.gi) ? std::clamp(p.gi, 0.0f, 2.0f) : 0.0f;
    if (p.ao == 0.0f && p.gi == 0.0f && p.view == 0)
        return false;
    p.radius = std::isfinite(p.radius) ? std::clamp(p.radius, 0.1f, 3.0f) : 1.5f;
    p.max_radiance = std::isfinite(p.max_radiance) ? std::clamp(p.max_radiance, 0.1f, 8.0f) : 4.0f;
    p.directions = std::clamp(p.directions, 1u, 4u);
    p.steps = std::clamp(p.steps, 1u, 12u);
    p.view = std::min(p.view, 2u);
    if (!impl->trace || impl->width != p.trace_size[0] || impl->height != p.trace_size[1]) {
        impl->width = p.trace_size[0];
        impl->height = p.trace_size[1];
        TextureDesc desc{.pixel_format = MTLPixelFormatRGBA16Float,
                         .width = impl->width,
                         .height = impl->height,
                         .shader_write = true};
        impl->trace =
            CreateTexture(impl->device, desc, "Screen lighting radiance and visibility", nullptr);
        impl->guide =
            CreateTexture(impl->device, desc, "Screen lighting normals and view depth", nullptr);
        ASSERT_MSG(impl->trace && impl->guide, "Screen lighting trace textures failed");
    }
    e.EndEncoding();
    e.SetComputePipeline(impl->trace_pipeline.Get(), Access::None);
    e.SetTexture(Stage::Compute, 0, depth);
    e.SetTexture(Stage::Compute, 1, scene);
    e.SetTexture(Stage::Compute, 2, impl->trace.Get());
    e.SetTexture(Stage::Compute, 3, impl->guide.Get());
    e.SetBytes(Stage::Compute, 0, &p, sizeof(p));
    e.Dispatch({(p.trace_size[0] + 15) / 16, (p.trace_size[1] + 7) / 8, 1}, {16, 8, 1});
    e.EndEncoding();
    RenderPassDesc pass{};
    pass.colors[0].texture = scene; // Load and RGB-only blend; preserve scene alpha.
    e.BeginRenderPass(pass);
    e.SetRenderPipeline(impl->apply_pipeline.Get(), Access::None);
    e.SetDepthStencil(nullptr);
    e.SetCullMode(CullMode::None);
    e.SetDepthBias(0, 0, 0);
    e.SetFillMode(FillMode::Fill);
    e.SetDepthClip(DepthClip::Clip);
    e.SetViewport({0, 0, float(p.size[0]), float(p.size[1]), 0, 1});
    e.SetScissor({0, 0, p.size[0], p.size[1]});
    for (auto [slot, texture] :
         {std::pair{0u, depth}, {1u, impl->trace.Get()}, {2u, impl->guide.Get()}, {3u, albedo}})
        e.SetTexture(Stage::Fragment, slot, texture);
    e.SetBytes(Stage::Fragment, 0, &p, sizeof(p));
    e.Draw(Primitive::Triangle, 3);
    e.EndRenderPass();
    for (void* object : {depth, scene, albedo, impl->trace.Get(), impl->guide.Get(),
                         impl->trace_pipeline.Get(), impl->apply_pipeline.Get()})
        e.KeepAlive(Object::Retain(object));
    return true;
}
} // namespace Metal
