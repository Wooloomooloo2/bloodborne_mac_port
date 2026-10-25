// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: unit test for the rasterizer's draw helper (renderer_metal/mtl_draw.h, Metal-native M3
// Task 4): slot binding with null stand-ins, draw preparation and skips, rect lists, indirect
// draws, rebinding after Submit, the tessellation run queue and scratch memory, with hand-written
// MSL pipelines on the system's GPU. Links bbmetal_core alone (the core part of the helper).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <array>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "video_core/renderer_metal/mtl_depth_stencil.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_draw.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_shader.h"

namespace {

using namespace Metal;
using namespace Metal::Draw;
using Kind = ShaderBinding::Kind;

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

constexpr u32 kSize = 64;
constexpr u32 kRed = 0xff0000ffu, kGreen = 0xff00ff00u, kBlue = 0xffff0000u, kBlack = 0xff000000u,
              kYellow = 0xff00ffffu; // RGBA8 bytes read as a little-endian u32

constexpr const char* kShaders = R"(
#include <metal_stdlib>
using namespace metal;

struct VOut { float4 pos [[position]]; };

vertex VOut fullscreen(uint vid [[vertex_id]]) {
    const float2 uv = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

// The RectList rewrite's shape: two triangles (6 vertices) covering the whole target.
vertex VOut rect6(uint vid [[vertex_id]]) {
    const float2 c[6] = {float2(-1, -1), float2(1, -1), float2(-1, 1),
                         float2(-1, 1),  float2(1, -1), float2(1, 1)};
    VOut o;
    o.pos = float4(c[vid % 6], 0.0, 1.0);
    return o;
}

// A vertex attribute (location 0, Metal vertex buffer 30) passed to the fragment.
struct AttrIn { float4 color [[attribute(0)]]; };
struct AttrOut { float4 pos [[position]]; float4 color; };
vertex AttrOut fullscreen_attr(AttrIn in [[stage_in]], uint vid [[vertex_id]]) {
    const float2 uv = float2((vid << 1) & 2, vid & 2);
    AttrOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.color = in.color;
    return o;
}
fragment float4 attr_color(AttrOut in [[stage_in]]) { return in.color; }

// A compute kernel's stage-in stream (as the tessellation vertex and geometry export kernels):
// thread i reads its vertex's attribute 0 from Metal buffer 30.
struct KernelIn { float4 color [[attribute(0)]]; };
kernel void stage_in_copy(KernelIn in [[stage_in]], device float4* out [[buffer(1)]],
                          uint i [[thread_position_in_grid]]) {
    out[i] = in.color;
}

fragment float4 push_color(constant float4& color [[buffer(0)]]) { return color; }
fragment float4 bound_color(constant float4& color [[buffer(1)]]) { return color; }

// Stand-ins: a missing buffer, texture and sampler read as zero.
fragment float4 null_inputs(VOut in [[stage_in]], constant float4& missing [[buffer(1)]],
                            texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {
    return float4(tex.sample(smp, float2(0.5)).rgb + missing.rgb, 1.0);
}

// The left half of clip space as one tessellated quad patch.
[[patch(quad, 4)]] vertex VOut post_tess_left(float2 uv [[position_in_patch]]) {
    VOut o;
    o.pos = float4(uv.x - 1.0, uv.y * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

kernel void probe(constant uint4& push [[buffer(0)]], device uint* out [[buffer(1)]],
                  device const uint* missing [[buffer(2)]], device const uint* arr0 [[buffer(3)]],
                  device const uint* arr1 [[buffer(4)]], constant uint* sizes [[buffer(5)]],
                  device const uint* set_one [[buffer(6)]], texture2d<float> tex [[texture(2)]],
                  sampler smp [[sampler(1)]], uint i [[thread_position_in_grid]]) {
    if (i != 0) return;
    // (An unbound slot reads as a null pointer: the stand-in must be bound.)
    out[0] = missing == nullptr ? 0xbadu : missing[0];
    out[10] = arr1 == nullptr ? 0xbadu : 0u;
    out[1] = sizes[1];
    out[2] = sizes[2];
    out[3] = sizes[3];
    out[4] = sizes[4];
    out[5] = push.x;
    out[6] = uint(tex.sample(smp, float2(0.5)).a * 255.0) + 7u;
    out[7] = arr0[0];
    out[8] = arr1[0];
    out[9] = set_one[15];
}

kernel void touch(device uint* out [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    out[i] += 1u;
}
)";

id<MTLLibrary> Library(Device& device) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    NSError* error = nil;
    id<MTLLibrary> library = [dev newLibraryWithSource:@(kShaders) options:nil error:&error];
    if (!library) {
        std::printf("FAIL: test shaders: %s\n", error.localizedDescription.UTF8String);
        std::exit(1);
    }
    return library;
}

id<MTLRenderPipelineState> Pipeline(Device& device, id<MTLLibrary> library, NSString* vertex,
                                    NSString* fragment, MTLRenderPipelineDescriptor* desc = nil) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    if (!desc) {
        desc = [MTLRenderPipelineDescriptor new];
    }
    desc.vertexFunction = [library newFunctionWithName:vertex];
    desc.fragmentFunction = [library newFunctionWithName:fragment];
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    NSError* error = nil;
    id<MTLRenderPipelineState> pipeline = [dev newRenderPipelineStateWithDescriptor:desc
                                                                              error:&error];
    if (!pipeline) {
        std::printf("FAIL: pipeline %s: %s\n", vertex.UTF8String,
                    error.localizedDescription.UTF8String);
        std::exit(1);
    }
    return pipeline;
}

id<MTLRenderPipelineState> PatchPipeline(Device& device, id<MTLLibrary> library) {
    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.maxTessellationFactor = 1;
    desc.tessellationPartitionMode = MTLTessellationPartitionModeInteger;
    desc.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionConstant;
    desc.tessellationOutputWindingOrder = MTLWindingCounterClockwise;
    desc.tessellationControlPointIndexType = MTLTessellationControlPointIndexTypeNone;
    return Pipeline(device, library, @"post_tess_left", @"bound_color", desc);
}

id<MTLComputePipelineState> Kernel(Device& device, id<MTLLibrary> library, NSString* name) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    NSError* error = nil;
    id<MTLComputePipelineState> pipeline =
        [dev newComputePipelineStateWithFunction:[library newFunctionWithName:name] error:&error];
    if (!pipeline) {
        std::printf("FAIL: kernel: %s\n", error.localizedDescription.UTF8String);
        std::exit(1);
    }
    return pipeline;
}

id<MTLBuffer> SharedBuffer(Device& device, NSUInteger length) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    id<MTLBuffer> buffer = [dev newBufferWithLength:length options:MTLResourceStorageModeShared];
    std::memset(buffer.contents, 0, length);
    return buffer;
}

BufferRef Ref(id<MTLBuffer> buffer, u64 offset = 0) {
    return {(__bridge void*)buffer, offset};
}

id<MTLBuffer> ColorBuffer(Device& device, u32 rgba) {
    id<MTLBuffer> b = SharedBuffer(device, 16);
    float* c = static_cast<float*>(b.contents);
    for (u32 i = 0; i < 4; ++i) {
        c[i] = float((rgba >> (8 * i)) & 0xff) / 255.0f;
    }
    return b;
}

std::array<u8, 16> PushColor(u32 rgba) {
    std::array<u8, 16> push{};
    float c[4];
    for (u32 i = 0; i < 4; ++i) {
        c[i] = float((rgba >> (8 * i)) & 0xff) / 255.0f;
    }
    std::memcpy(push.data(), c, sizeof(c));
    return push;
}

id<MTLTexture> Target(Device& device) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:kSize
                                                          height:kSize
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModePrivate;
    return [dev newTextureWithDescriptor:desc];
}

RenderPassDesc Pass(id<MTLTexture> target) {
    RenderPassDesc desc;
    desc.colors[0].texture = (__bridge void*)target;
    desc.colors[0].clear = true;
    desc.colors[0].clear_color = {0.0, 0.0, 0.0, 1.0};
    desc.width = kSize;
    desc.height = kSize;
    return desc;
}

id<MTLBuffer> Readback(Device& device, Encoder& encoder, id<MTLTexture> texture) {
    id<MTLBuffer> out = SharedBuffer(device, kSize * kSize * 4);
    encoder.CopyTextureToBuffer((__bridge void*)texture, 0, 0, {}, {kSize, kSize, 1}, Ref(out),
                                kSize * 4, kSize * kSize * 4);
    return out;
}

u32 Pixel(id<MTLBuffer> readback, u32 x, u32 y) {
    u32 value;
    std::memcpy(&value, static_cast<const u8*>(readback.contents) + (y * kSize + x) * 4, 4);
    return value;
}

DrawState FullState() {
    DrawState s;
    s.viewport_width = float(kSize);
    s.viewport_height = float(kSize);
    s.scissor_width = kSize;
    s.scissor_height = kSize;
    s.target_width = kSize;
    s.target_height = kSize;
    return s;
}

/// Shaders as the compiler describes them (only the binding maps matter here).
struct Shaders {
    CompiledShader none;       ///< no resources (the vertex functions)
    CompiledShader push_color; ///< push constants at buffer 0
    CompiledShader bound;      ///< binding 0 at buffer 1
    CompiledShader null_inputs;
    Shaders() {
        push_color.uses_push_constants = true;
        push_color.bindings = {{~0u, ~0u, Kind::PushConstants, kPushConstantBuffer, 1}};
        bound.bindings = {{0, 0, Kind::Buffer, 1, 1}};
        null_inputs.bindings = {{0, 0, Kind::Buffer, 1, 1},
                                {0, 1, Kind::Texture, 0, 1},
                                {0, 1, Kind::Sampler, 0, 1}};
    }
};

DrawPipeline View(id<MTLRenderPipelineState> state, const CompiledShader* vs,
                  const CompiledShader* fs) {
    DrawPipeline p;
    p.state = (__bridge void*)state;
    p.vs = vs;
    p.fs = fs;
    return p;
}

// ---- Tests ----

void TestBindStageNullFallbacks() {
    Device device;
    Encoder encoder{device};
    NullResources null_resources{device};
    const Nulls nulls = null_resources.Get();
    Runs runs{device, nulls};
    id<MTLLibrary> library = Library(device);

    // Compute: every kind of slot, some provided, the others missing.
    CompiledShader probe;
    probe.uses_push_constants = true;
    probe.buffer_size_buffer = 5;
    probe.num_buffers = 7;
    probe.bindings = {{~0u, ~0u, Kind::PushConstants, kPushConstantBuffer, 1},
                      {0, 0, Kind::Buffer, 1, 1},  // out: provided
                      {0, 1, Kind::Buffer, 2, 1},  // missing (empty element)
                      {0, 2, Kind::Buffer, 3, 2},  // array: element 0 provided, 1 past the table
                      {0, 5, Kind::Texture, 2, 1}, // past the table
                      {0, 6, Kind::Sampler, 1, 1},
                      {1, 0, Kind::Buffer, 6, 1}}; // descriptor set 1: never from the table
    id<MTLComputePipelineState> kernel = Kernel(device, library, @"probe");
    const auto run = [&](bool native) {
        id<MTLBuffer> out = SharedBuffer(device, 64);
        static_cast<u32*>(out.contents)[15] = 0xdeadbeefu; // what set 1 would read if mis-bound
        id<MTLBuffer> arr = SharedBuffer(device, 32);
        static_cast<u32*>(arr.contents)[0] = 0xabcu;
        const std::array<Bound, 3> bound{Bound{Ref(out), 64}, Bound{}, Bound{Ref(arr), 32}};
        const u32 push[4] = {0x1234u, 0, 0, 0};
        const std::span<const u8> push_bytes{reinterpret_cast<const u8*>(push), sizeof(push)};
        if (native) {
            id<MTLComputeCommandEncoder> raw =
                (__bridge id<MTLComputeCommandEncoder>)encoder.ComputeEncoder();
            [raw setComputePipelineState:kernel];
            BindKernelStage((__bridge void*)raw, probe, bound, push_bytes, nulls);
            [raw dispatchThreadgroups:MTLSizeMake(1, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        } else {
            Expect(runs.Dispatch(encoder, ComputeView{(__bridge void*)kernel, &probe, {1, 1, 1}},
                                 bound, push_bytes, {1, 1, 1}),
                   "Dispatch runs");
        }
        device.Wait(encoder.Submit());
        const u32* o = static_cast<const u32*>(out.contents);
        const char* how = native ? "native encoder" : "Encoder";
        char what[160];
        std::snprintf(what, sizeof(what), "%s: a missing buffer reads the zeroed stand-in", how);
        Expect(o[0] == 0, what);
        std::snprintf(what, sizeof(what), "%s: size table has the bound sizes", how);
        Expect(o[1] == 64 && o[3] == 32, what);
        std::snprintf(what, sizeof(what), "%s: size table has the stand-in's size", how);
        Expect(o[2] == nulls.buffer_size && o[4] == nulls.buffer_size, what);
        std::snprintf(what, sizeof(what), "%s: push constants at the push slot", how);
        Expect(o[5] == 0x1234u, what);
        std::snprintf(what, sizeof(what), "%s: a missing texture/sampler sample zero", how);
        Expect(o[6] == 7, what);
        std::snprintf(what, sizeof(what), "%s: array element 0 bound, element 1 the stand-in", how);
        Expect(o[7] == 0xabcu && o[8] == 0, what);
        std::snprintf(what, sizeof(what), "%s: set 1 gets the stand-in", how);
        Expect(o[9] == 0, what);
        std::snprintf(what, sizeof(what), "%s: stand-ins are bound (no null pointers)", how);
        Expect(o[0] != 0xbadu && o[10] != 0xbadu, what);
    };
    run(false);
    run(true);

    // Render: a fragment shader whose buffer, texture and sampler are all missing.
    Shaders shaders;
    id<MTLRenderPipelineState> pipeline = Pipeline(device, library, @"fullscreen", @"null_inputs");
    id<MTLTexture> target = Target(device);
    encoder.BeginRenderPass(Pass(target));
    const auto push = PushColor(kRed);
    Expect(runs.Draw(encoder, View(pipeline, &shaders.none, &shaders.null_inputs), FullState(),
                     Resources{{}, push, {}, {}, IndexType::U16}, DrawArgs{3}),
           "a draw with only stand-ins runs");
    encoder.EndRenderPass();
    id<MTLBuffer> out = Readback(device, encoder, target);
    device.Wait(encoder.Submit());
    Expect(Pixel(out, 20, 20) == kBlack, "stand-ins read as zero in the fragment stage");
    Expect(device.ErrorCount() == 0, "null fallbacks ran without errors");
}

void TestDrawAfterSubmitRebinds() {
    Device device;
    Encoder encoder{device};
    NullResources null_resources{device};
    Runs runs{device, null_resources.Get()};
    Shaders shaders;
    id<MTLLibrary> library = Library(device);
    id<MTLRenderPipelineState> pipeline = Pipeline(device, library, @"fullscreen", @"bound_color");
    id<MTLBuffer> red = ColorBuffer(device, kRed);
    const std::array<Bound, 1> bound{Bound{Ref(red), 16}};
    const Resources r{bound, {}, {}, {}, IndexType::U16};
    const DrawPipeline p = View(pipeline, &shaders.none, &shaders.bound);

    id<MTLTexture> first = Target(device);
    encoder.BeginRenderPass(Pass(first));
    Expect(runs.Draw(encoder, p, FullState(), r, DrawArgs{3}), "first draw");
    encoder.EndRenderPass();
    id<MTLBuffer> out_first = Readback(device, encoder, first);
    const u64 tick = encoder.Submit();
    runs.Submitted(tick);
    Expect(!encoder.HasBindings(), "Submit cleared the encoder's bindings");

    // Same helper call in a new command buffer: it binds pipeline, state and slots again.
    id<MTLTexture> second = Target(device);
    encoder.BeginRenderPass(Pass(second));
    Expect(runs.Draw(encoder, p, FullState(), r, DrawArgs{3}), "draw after Submit");
    encoder.EndRenderPass();
    id<MTLBuffer> out_second = Readback(device, encoder, second);
    device.Wait(encoder.Submit());
    device.Wait(tick);
    Expect(Pixel(out_first, 10, 10) == kRed, "first command buffer drew");
    Expect(Pixel(out_second, 10, 10) == kRed, "the draw after Submit rebinds everything");
    Expect(runs.GetCounters().draws.load() == 2, "two draws counted");
    Expect(device.ErrorCount() == 0, "rebinding after Submit ran without errors");
}

void TestSkippedDraws() {
    Device device;
    Encoder encoder{device};
    NullResources null_resources{device};
    Runs runs{device, null_resources.Get()};
    Shaders shaders;
    id<MTLLibrary> library = Library(device);
    id<MTLRenderPipelineState> solid = Pipeline(device, library, @"fullscreen", @"push_color");
    id<MTLRenderPipelineState> rect = Pipeline(device, library, @"rect6", @"push_color");
    const auto push = PushColor(kRed);
    const Resources r{{}, push, {}, {}, IndexType::U16};
    const DrawPipeline p = View(solid, &shaders.none, &shaders.push_color);

    // Outside a render pass: skipped, no assertion.
    Expect(!runs.Draw(encoder, p, FullState(), r, DrawArgs{3}), "a draw outside a pass is skipped");

    id<MTLTexture> target = Target(device);
    encoder.BeginRenderPass(Pass(target));
    DrawState discard = FullState();
    discard.rasterizer_discard = true;
    Expect(!runs.Draw(encoder, p, discard, r, DrawArgs{3}), "rasterizer discard is skipped");
    DrawState cull_all = FullState();
    cull_all.cull_all = true;
    Expect(!runs.Draw(encoder, p, cull_all, r, DrawArgs{3}), "front-and-back cull is skipped");
    DrawState off_target = FullState();
    off_target.scissor_x = 100;
    Expect(!runs.Draw(encoder, p, off_target, r, DrawArgs{3}), "a scissor off the target is skipped");
    DrawPipeline emulated = p;
    emulated.emulated = true;
    Expect(!runs.Draw(encoder, emulated, FullState(), r, DrawArgs{3}),
           "an unexpanded emulated topology is skipped");
    DrawPipeline failed = p;
    failed.state = nullptr;
    Expect(!runs.Draw(encoder, failed, FullState(), r, DrawArgs{3}), "a failed pipeline is skipped");
    DrawPipeline rect_list = View(rect, &shaders.none, &shaders.push_color);
    rect_list.rect_list = true;
    rect_list.emulated = true; // RectList is emulated, but the rewrite draws it
    id<MTLBuffer> indices = SharedBuffer(device, 64);
    const Resources indexed_r{{}, push, {}, Ref(indices), IndexType::U16};
    DrawArgs indexed{3};
    indexed.indexed = true;
    Expect(!runs.Draw(encoder, rect_list, FullState(), indexed_r, indexed),
           "an indexed rect list is skipped");
    id<MTLBuffer> args = SharedBuffer(device, 64);
    static_cast<u32*>(args.contents)[0] = 3;
    static_cast<u32*>(args.contents)[1] = 1;
    Expect(!runs.DrawIndirect(encoder, rect_list, FullState(), r, IndirectArgs{Ref(args), 1, 16}),
           "an indirect rect list is skipped");
    encoder.EndRenderPass();
    id<MTLBuffer> out = Readback(device, encoder, target);
    device.Wait(encoder.Submit());
    Expect(Pixel(out, 10, 10) == kBlack && Pixel(out, 50, 50) == kBlack,
           "skipped draws leave the target untouched");
    const auto& c = runs.GetCounters();
    Expect(c.skipped_no_pass.load() == 1, "a draw outside a pass counted");
    Expect(c.skipped_discard.load() == 1, "rasterizer discard counted");
    Expect(c.skipped_cull_all.load() == 1, "front-and-back cull counted");
    Expect(c.skipped_empty_scissor.load() == 1, "empty scissor counted");
    Expect(c.skipped_emulated.load() == 1, "emulated topology counted");
    Expect(c.skipped_no_pipeline.load() == 1, "failed pipeline counted");
    Expect(c.unsupported.load() == 2, "indexed and indirect rect lists counted");
    Expect(c.draws.load() == 0, "no draw counted");
    // The published report: per-reason deltas since the last report, empty when none.
    SkipTotals last;
    Expect(DescribeSkips(c, last) ==
               "no pass 1, no pipeline 1, emulated topology 1, unsupported 2, discard 1, "
               "cull all 1, empty scissor 1",
           "the skip report lists every non-zero reason");
    Expect(DescribeSkips(c, last).empty(), "no new skips: an empty report");

    // Drawn: a rect list (3 vertices -> 6), and a scissor with a negative offset (clamped).
    id<MTLTexture> rects = Target(device);
    encoder.BeginRenderPass(Pass(rects));
    Expect(runs.Draw(encoder, rect_list, FullState(), r, DrawArgs{3}), "a rect list draws");
    encoder.EndRenderPass();
    id<MTLTexture> clipped = Target(device);
    encoder.BeginRenderPass(Pass(clipped));
    DrawState left = FullState();
    left.scissor_x = -10;
    left.scissor_width = 42; // covers x 0..31 of the target
    Expect(runs.Draw(encoder, p, left, r, DrawArgs{3}), "a partly off-target scissor draws");
    encoder.EndRenderPass();
    id<MTLBuffer> out_rects = Readback(device, encoder, rects);
    id<MTLBuffer> out_clipped = Readback(device, encoder, clipped);
    device.Wait(encoder.Submit());
    Expect(Pixel(out_rects, 2, 2) == kRed && Pixel(out_rects, kSize - 3, kSize - 3) == kRed &&
               Pixel(out_rects, kSize - 3, 2) == kRed && Pixel(out_rects, 2, kSize - 3) == kRed,
           "a rect list of 3 vertices draws both triangles (6 vertices)");
    Expect(Pixel(out_clipped, 10, 10) == kRed && Pixel(out_clipped, 50, 10) == kBlack,
           "the scissor is clamped to the target");
    Expect(device.ErrorCount() == 0, "skipped draws ran without errors");
}

void TestIndirectDraws() {
    Device device;
    Encoder encoder{device};
    NullResources null_resources{device};
    Runs runs{device, null_resources.Get()};
    Shaders shaders;
    id<MTLLibrary> library = Library(device);
    id<MTLRenderPipelineState> solid = Pipeline(device, library, @"fullscreen", @"push_color");
    const auto push = PushColor(kGreen);
    const Resources r{{}, push, {}, {}, IndexType::U16};
    const DrawPipeline p = View(solid, &shaders.none, &shaders.push_color);
    // Two VkDrawIndirectCommand records 32 bytes apart: 3 vertices, then none.
    id<MTLBuffer> args = SharedBuffer(device, 64);
    u32* a = static_cast<u32*>(args.contents);
    a[0] = 3;
    a[1] = 1;
    a[8] = 0;
    a[9] = 1;
    id<MTLBuffer> zero_count = SharedBuffer(device, 16);

    id<MTLTexture> drawn = Target(device);
    encoder.BeginRenderPass(Pass(drawn));
    Expect(runs.DrawIndirect(encoder, p, FullState(), r, IndirectArgs{Ref(args), 2, 32}),
           "indirect draws");
    encoder.EndRenderPass();
    id<MTLTexture> counted = Target(device);
    encoder.BeginRenderPass(Pass(counted));
    IndirectArgs with_count{Ref(args), 2, 32};
    with_count.count_buffer = Ref(zero_count); // GPU count 0: nothing drawn
    Expect(runs.DrawIndirect(encoder, p, FullState(), r, with_count), "indirect count draws");
    encoder.EndRenderPass();
    id<MTLBuffer> out_drawn = Readback(device, encoder, drawn);
    id<MTLBuffer> out_counted = Readback(device, encoder, counted);
    device.Wait(encoder.Submit());
    Expect(Pixel(out_drawn, 20, 20) == kGreen, "the indirect records draw");
    Expect(Pixel(out_counted, 20, 20) == kBlack, "a GPU count of 0 draws nothing");
    Expect(device.ErrorCount() == 0, "indirect draws ran without errors");
}

void TestTessellationFlushOrder() {
    Device device;
    Encoder encoder{device};
    NullResources null_resources{device};
    Runs runs{device, null_resources.Get()};
    runs.SetHoldDraws(false); // plain draws end runs (TestHeldDraws: they wait in them)
    Shaders shaders;
    id<MTLLibrary> library = Library(device);
    id<MTLRenderPipelineState> patch_state = PatchPipeline(device, library);
    id<MTLRenderPipelineState> solid = Pipeline(device, library, @"fullscreen", @"push_color");
    id<MTLComputePipelineState> touch = Kernel(device, library, @"touch");
    DrawPipeline patch_pipeline = View(patch_state, nullptr, &shaders.bound);
    patch_pipeline.tessellation = true;
    const DrawPipeline plain = View(solid, &shaders.none, &shaders.push_color);

    // One quad patch: four edge and two inside factors of 1.0 (half); control points unused.
    id<MTLBuffer> scratch = SharedBuffer(device, 256);
    u16* f = reinterpret_cast<u16*>(static_cast<u8*>(scratch.contents) + 128);
    for (u32 i = 0; i < 6; ++i) {
        f[i] = 0x3c00;
    }
    PatchDraw patch;
    patch.control = Ref(scratch, 0);
    patch.patch = Ref(scratch, 64);
    patch.factors = Ref(scratch, 128);
    patch.patches = 1;
    patch.output_control_points = 4;
    id<MTLBuffer> blue = ColorBuffer(device, kBlue);
    id<MTLBuffer> red = ColorBuffer(device, kRed);
    id<MTLBuffer> yellow = ColorBuffer(device, kYellow);

    // 1. The deferred draw uses the slots bound when it was queued.
    id<MTLTexture> snap = Target(device);
    encoder.BeginRenderPass(Pass(snap));
    std::array<Bound, 1> bound{Bound{Ref(blue), 16}};
    runs.QueuePatchDraw(patch_pipeline, FullState(), Resources{bound, {}, {}, {}, IndexType::U16},
                        patch);
    bound[0] = Bound{Ref(red), 16}; // the caller's table changes before the run ends
    Expect(runs.HasPending(), "the patch draw waits for the end of the run");
    runs.Flush(encoder);
    Expect(!runs.HasPending(), "Flush drew it");
    encoder.EndRenderPass();

    // 2. A plain draw ends the run: the patches are drawn first, in queue order.
    id<MTLTexture> order = Target(device);
    encoder.BeginRenderPass(Pass(order));
    const std::array<Bound, 1> blue_bound{Bound{Ref(blue), 16}};
    const std::array<Bound, 1> yellow_bound{Bound{Ref(yellow), 16}};
    runs.QueuePatchDraw(patch_pipeline, FullState(),
                        Resources{blue_bound, {}, {}, {}, IndexType::U16}, patch);
    runs.QueuePatchDraw(patch_pipeline, FullState(),
                        Resources{yellow_bound, {}, {}, {}, IndexType::U16}, patch);
    runs.Flush(encoder);
    encoder.EndRenderPass();
    id<MTLTexture> covered = Target(device);
    encoder.BeginRenderPass(Pass(covered));
    runs.QueuePatchDraw(patch_pipeline, FullState(),
                        Resources{blue_bound, {}, {}, {}, IndexType::U16}, patch);
    const auto green = PushColor(kGreen);
    // The plain draw covers the bottom-left quarter, over part of the patch.
    DrawState bottom_left = FullState();
    bottom_left.scissor_y = 32;
    bottom_left.scissor_width = 32;
    bottom_left.scissor_height = 32;
    Expect(runs.Draw(encoder, plain, bottom_left, Resources{{}, green, {}, {}, IndexType::U16},
                     DrawArgs{3}),
           "the plain draw after the run");
    Expect(!runs.HasPending(), "the plain draw flushed the run");
    encoder.EndRenderPass();

    // 3. A dispatch ends the run too (the patch is drawn before the compute work).
    id<MTLTexture> dispatched = Target(device);
    id<MTLBuffer> counter = SharedBuffer(device, 16);
    encoder.BeginRenderPass(Pass(dispatched));
    runs.QueuePatchDraw(patch_pipeline, FullState(),
                        Resources{blue_bound, {}, {}, {}, IndexType::U16}, patch);
    CompiledShader touch_shader;
    touch_shader.bindings = {{0, 0, Kind::Buffer, 1, 1}};
    const std::array<Bound, 1> counter_bound{Bound{Ref(counter), 16}};
    Expect(runs.Dispatch(encoder, ComputeView{(__bridge void*)touch, &touch_shader, {1, 1, 1}},
                         counter_bound, {}, {1, 1, 1}),
           "dispatch inside the pass");
    Expect(!runs.HasPending(), "the dispatch flushed the run");
    encoder.EndRenderPass();

    id<MTLBuffer> out_snap = Readback(device, encoder, snap);
    id<MTLBuffer> out_order = Readback(device, encoder, order);
    id<MTLBuffer> out_covered = Readback(device, encoder, covered);
    id<MTLBuffer> out_dispatched = Readback(device, encoder, dispatched);
    const u64 tick = encoder.Submit();
    runs.Submitted(tick);
    device.Wait(tick);
    Expect(Pixel(out_snap, 10, 32) == kBlue, "the patch draw used its queued (blue) slot");
    Expect(Pixel(out_snap, 50, 32) == kBlack, "the patch covers the left half only");
    Expect(Pixel(out_order, 10, 32) == kYellow, "patch draws of a run keep their order");
    Expect(Pixel(out_covered, 10, 10) == kBlue, "the run was drawn in its pass");
    Expect(Pixel(out_covered, 10, 50) == kGreen, "the run was drawn before the plain draw");
    Expect(Pixel(out_covered, 50, 10) == kBlack, "nothing else drawn");
    Expect(Pixel(out_dispatched, 10, 32) == kBlue && Pixel(out_dispatched, 50, 32) == kBlack,
           "the patch drawn before the dispatch");
    Expect(static_cast<const u32*>(counter.contents)[0] == 1, "the dispatch ran");
    Expect(device.ErrorCount() == 0, "tessellation runs ran without errors");
}

void TestHeldDraws() {
    Device device;
    Encoder encoder{device};
    NullResources null_resources{device};
    Runs runs{device, null_resources.Get()};
    runs.SetHoldDraws(true);
    Shaders shaders;
    id<MTLLibrary> library = Library(device);
    id<MTLRenderPipelineState> patch_state = PatchPipeline(device, library);
    id<MTLRenderPipelineState> solid = Pipeline(device, library, @"fullscreen", @"push_color");
    DrawPipeline patch_pipeline = View(patch_state, nullptr, &shaders.bound);
    patch_pipeline.tessellation = true;
    // As the rasterizer's guest pipelines: buffer accesses ordered by the Runtime (Read).
    DrawPipeline plain = View(solid, &shaders.none, &shaders.push_color);
    plain.buffers = Access::Read;
    DrawPipeline storing = plain;
    storing.writes_images = true;

    // One quad patch over the left half (as in TestTessellationFlushOrder).
    id<MTLBuffer> scratch = SharedBuffer(device, 256);
    u16* f = reinterpret_cast<u16*>(static_cast<u8*>(scratch.contents) + 128);
    for (u32 i = 0; i < 6; ++i) {
        f[i] = 0x3c00;
    }
    PatchDraw patch;
    patch.control = Ref(scratch, 0);
    patch.patch = Ref(scratch, 64);
    patch.factors = Ref(scratch, 128);
    patch.patches = 1;
    patch.output_control_points = 4;
    id<MTLBuffer> blue = ColorBuffer(device, kBlue);
    id<MTLBuffer> yellow = ColorBuffer(device, kYellow);
    const std::array<Bound, 1> blue_bound{Bound{Ref(blue), 16}};
    const std::array<Bound, 1> yellow_bound{Bound{Ref(yellow), 16}};
    const auto green = PushColor(kGreen);
    const auto red = PushColor(kRed);
    DrawState bottom_left = FullState();
    bottom_left.scissor_y = 32;
    bottom_left.scissor_width = 32;
    bottom_left.scissor_height = 32;
    const Resources green_draw{{}, green, {}, {}, IndexType::U16};

    // 1. A plain draw after a patch draw waits in the run and is drawn after it, in its pass.
    id<MTLTexture> after = Target(device);
    encoder.BeginRenderPass(Pass(after));
    runs.QueuePatchDraw(patch_pipeline, FullState(),
                        Resources{blue_bound, {}, {}, {}, IndexType::U16}, patch);
    Expect(runs.Draw(encoder, plain, bottom_left, green_draw, DrawArgs{3}), "held draw");
    Expect(runs.HasPending(), "the plain draw waits in the run");
    runs.EndRenderPass(encoder);
    Expect(!runs.HasPending(), "the pass end drew the run");

    // 2. A patch draw queued after a held draw is drawn after it: the run keeps the order.
    id<MTLTexture> order = Target(device);
    encoder.BeginRenderPass(Pass(order));
    runs.QueuePatchDraw(patch_pipeline, FullState(),
                        Resources{blue_bound, {}, {}, {}, IndexType::U16}, patch);
    Expect(runs.Draw(encoder, plain, bottom_left, green_draw, DrawArgs{3}), "held draw");
    runs.QueuePatchDraw(patch_pipeline, FullState(),
                        Resources{yellow_bound, {}, {}, {}, IndexType::U16}, patch);
    runs.Flush(encoder);
    encoder.EndRenderPass();

    // 3. A draw that stores to images is not held: it ends the run (after it, in order).
    id<MTLTexture> stored = Target(device);
    encoder.BeginRenderPass(Pass(stored));
    runs.QueuePatchDraw(patch_pipeline, FullState(),
                        Resources{blue_bound, {}, {}, {}, IndexType::U16}, patch);
    Expect(runs.Draw(encoder, storing, bottom_left, Resources{{}, red, {}, {}, IndexType::U16},
                     DrawArgs{3}),
           "draw that stores to images");
    Expect(!runs.HasPending(), "it ended the run");
    encoder.EndRenderPass();

    // 4. Without a run waiting, a plain draw is drawn at once.
    id<MTLTexture> alone = Target(device);
    encoder.BeginRenderPass(Pass(alone));
    Expect(runs.Draw(encoder, plain, bottom_left, green_draw, DrawArgs{3}), "plain draw");
    Expect(!runs.HasPending(), "nothing waits");
    encoder.EndRenderPass();

    id<MTLBuffer> out_after = Readback(device, encoder, after);
    id<MTLBuffer> out_order = Readback(device, encoder, order);
    id<MTLBuffer> out_stored = Readback(device, encoder, stored);
    id<MTLBuffer> out_alone = Readback(device, encoder, alone);
    const u64 tick = encoder.Submit();
    runs.Submitted(tick);
    device.Wait(tick);
    Expect(Pixel(out_after, 10, 10) == kBlue, "the patch drawn");
    Expect(Pixel(out_after, 10, 50) == kGreen, "the held draw drawn after the patch");
    Expect(Pixel(out_after, 50, 10) == kBlack, "nothing else drawn");
    Expect(Pixel(out_order, 10, 10) == kYellow, "the second patch drawn last");
    Expect(Pixel(out_order, 10, 50) == kYellow, "the held draw drawn before the second patch");
    Expect(Pixel(out_stored, 10, 10) == kBlue && Pixel(out_stored, 10, 50) == kRed,
           "the storing draw drawn after the run");
    Expect(Pixel(out_alone, 10, 50) == kGreen && Pixel(out_alone, 10, 10) == kBlack,
           "the plain draw alone");
    Expect(runs.GetCounters().held_draws.load() == 2, "two draws were held");
    Expect(runs.GetCounters().runs.load() == 3, "three runs were drawn");
    Expect(device.ErrorCount() == 0, "held draws ran without errors");
}

void TestRunWrappers() {
    Device device;
    Encoder encoder{device};
    NullResources null_resources{device};
    Runs runs{device, null_resources.Get()};
    Shaders shaders;
    id<MTLLibrary> library = Library(device);
    id<MTLRenderPipelineState> patch_state = PatchPipeline(device, library); // (kept alive)
    DrawPipeline patch_pipeline = View(patch_state, nullptr, &shaders.bound);
    patch_pipeline.tessellation = true;
    id<MTLBuffer> blue = ColorBuffer(device, kBlue);
    const std::array<Bound, 1> blue_bound{Bound{Ref(blue), 16}};

    // The patch's factors live in Runs scratch (as the kernels' output would).
    const BufferRef scratch = runs.Scratch(encoder, 256);
    id<MTLBuffer> factors = SharedBuffer(device, 16);
    u16* f = static_cast<u16*>(factors.contents);
    for (u32 i = 0; i < 6; ++i) {
        f[i] = 0x3c00;
    }
    encoder.CopyBuffer(Ref(factors), {scratch.buffer, scratch.offset + 128}, 16);
    PatchDraw patch;
    patch.control = scratch;
    patch.patch = {scratch.buffer, scratch.offset + 64};
    patch.factors = {scratch.buffer, scratch.offset + 128};
    patch.patches = 1;
    patch.output_control_points = 4;

    id<MTLTexture> target = Target(device);
    encoder.BeginRenderPass(Pass(target));
    runs.QueuePatchDraw(patch_pipeline, FullState(),
                        Resources{blue_bound, {}, {}, {}, IndexType::U16}, patch);
    runs.EndRenderPass(encoder); // flushes the run into this pass, then ends it
    Expect(!runs.HasPending() && !encoder.InRenderPass(), "EndRenderPass flushed and ended");
    id<MTLBuffer> out = Readback(device, encoder, target);
    const u64 tick = runs.Submit(encoder);
    Expect(tick != 0, "Submit returns the tick");
    device.Wait(tick);
    for (int i = 0; i < 1000 && !device.IsFree(tick); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Expect(Pixel(out, 10, 32) == kBlue && Pixel(out, 50, 32) == kBlack,
           "the run's patch is drawn in the pass EndRenderPass ended");
    const BufferRef again = runs.Scratch(encoder, 64);
    Expect(again.buffer == scratch.buffer && again.offset == 0,
           "Submit handed the patch scratch to its tick; it is recycled after completion");
    device.Wait(encoder.Submit());
    Expect(device.ErrorCount() == 0, "run wrappers ran without errors");
}

void TestScratchPool() {
    Device device;
    Encoder encoder{device};
    NullResources null_resources{device};
    Runs runs{device, null_resources.Get()};
    const BufferRef a = runs.Scratch(encoder, 100);
    const BufferRef b = runs.Scratch(encoder, 100);
    Expect(a.buffer && a.buffer == b.buffer && a.offset == 0 && b.offset == 112,
           "scratch bump-allocates 16-byte aligned spans from one chunk");
    encoder.FillBuffer(a, 16, 0); // the command buffer uses it
    const u64 tick = encoder.Submit();
    runs.Submitted(tick);
    device.Wait(tick);
    // (Wait returns at the event; the chunk is pooled once the completion handlers ran.)
    for (int i = 0; i < 1000 && !device.IsFree(tick); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const BufferRef c = runs.Scratch(encoder, 64);
    Expect(c.buffer == a.buffer && c.offset == 0,
           "after Submit a new chunk starts; the completed one comes back from the pool");
    const BufferRef big = runs.Scratch(encoder, 9ull << 20);
    Expect(big.buffer && big.buffer != c.buffer &&
               [(__bridge id<MTLBuffer>)big.buffer length] == (16ull << 20),
           "a large request gets a 16 MiB chunk");
    device.Wait(encoder.Submit());
    Expect(device.ErrorCount() == 0, "scratch ran without errors");
}

void TestVertexStreamStrides() {
    Device device;
    Encoder encoder{device};
    NullResources null_resources{device};
    Runs runs{device, null_resources.Get()};
    Shaders shaders;
    id<MTLLibrary> library = Library(device);
    constexpr u32 kSlot = 30;
    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    MTLVertexDescriptor* vd = [MTLVertexDescriptor new];
    vd.attributes[0].format = MTLVertexFormatFloat4;
    vd.attributes[0].bufferIndex = kSlot;
    vd.layouts[kSlot].stride = MTLBufferLayoutStrideDynamic;
    vd.layouts[kSlot].stepFunction = MTLVertexStepFunctionPerVertex;
    desc.vertexDescriptor = vd;
    id<MTLRenderPipelineState> pipeline =
        Pipeline(device, library, @"fullscreen_attr", @"attr_color", desc);
    const std::array<VertexBufferSlot, 1> slots{VertexBufferSlot{0, kSlot, false}};
    DrawPipeline p = View(pipeline, &shaders.none, &shaders.none);
    p.vertex_slots = slots;
    // Three different colors, one per vertex if the stride advanced.
    id<MTLBuffer> colors = SharedBuffer(device, 48);
    float* c = static_cast<float*>(colors.contents);
    const float values[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    std::memcpy(c, values, sizeof(values));

    // A V# stride of 0 (Vulkan: every vertex reads the same element).
    const std::array<VertexStream, 1> same{VertexStream{Ref(colors), 0}};
    id<MTLTexture> constant = Target(device);
    encoder.BeginRenderPass(Pass(constant));
    Expect(runs.Draw(encoder, p, FullState(), Resources{{}, {}, same, {}, IndexType::U16},
                     DrawArgs{3}),
           "a stride-0 stream draws");
    encoder.EndRenderPass();
    // A missing stream: the stand-in at stride 0 (element 0 for every vertex: zero).
    id<MTLTexture> missing = Target(device);
    encoder.BeginRenderPass(Pass(missing));
    Expect(runs.Draw(encoder, p, FullState(), Resources{{}, {}, {}, {}, IndexType::U16},
                     DrawArgs{3}),
           "a draw with a missing stream draws");
    encoder.EndRenderPass();
    id<MTLBuffer> out_constant = Readback(device, encoder, constant);
    id<MTLBuffer> out_missing = Readback(device, encoder, missing);
    device.Wait(encoder.Submit());
    Expect(Pixel(out_constant, 10, 10) == kRed && Pixel(out_constant, 40, 20) == kRed,
           "a stride-0 stream gives every vertex element 0");
    Expect(Pixel(out_missing, 10, 10) == 0u, "a missing stream reads zero");
    Expect(device.ErrorCount() == 0, "vertex stream strides ran without errors");
}

/// The stride rule shared by draws and the tessellation / geometry kernels' stage-in streams.
void TestStreamStrideRule() {
    const VertexBufferSlot per_vertex{0, 30, false};
    const VertexBufferSlot constant{0, 30, true};
    const VertexStream strided{BufferRef{reinterpret_cast<void*>(0x1000), 0}, 24};
    const VertexStream zero{BufferRef{reinterpret_cast<void*>(0x1000), 0}, 0};
    const VertexStream empty{};
    Expect(StreamStride(per_vertex, &strided) == 24, "a present stream keeps its V# stride");
    Expect(StreamStride(per_vertex, &zero) == 0, "a V# stride of 0 stays 0");
    Expect(StreamStride(per_vertex, &empty) == 0, "a stream without a buffer: stride 0");
    Expect(StreamStride(per_vertex, nullptr) == 0, "a missing stream: stride 0");
    Expect(StreamStride(constant, &strided) == 0, "a constant stream: stride 0");
}

/// A kernel stage-in stream bound with the rule's stride 0 (setBuffer:offset:attributeStride:,
/// as Tessellation::EncodeVertexKernel binds them), and a Constant-step render layout at 0.
void TestKernelAndConstantStreamStrides() {
    Device device;
    Encoder encoder{device};
    NullResources null_resources{device};
    Runs runs{device, null_resources.Get()};
    Shaders shaders;
    id<MTLLibrary> library = Library(device);
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    constexpr u32 kSlot = 30;
    id<MTLBuffer> colors = SharedBuffer(device, 48);
    const float values[12] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1};
    std::memcpy(colors.contents, values, sizeof(values));

    // Kernel: four threads, one vertex each, from a V# with stride 0.
    MTLComputePipelineDescriptor* cdesc = [MTLComputePipelineDescriptor new];
    cdesc.computeFunction = [library newFunctionWithName:@"stage_in_copy"];
    cdesc.stageInputDescriptor = [MTLStageInputOutputDescriptor stageInputOutputDescriptor];
    cdesc.stageInputDescriptor.attributes[0].format = MTLAttributeFormatFloat4;
    cdesc.stageInputDescriptor.attributes[0].bufferIndex = kSlot;
    cdesc.stageInputDescriptor.layouts[kSlot].stride = MTLBufferLayoutStrideDynamic;
    cdesc.stageInputDescriptor.layouts[kSlot].stepFunction =
        MTLStepFunctionThreadPositionInGridX;
    NSError* error = nil;
    id<MTLComputePipelineState> kernel =
        [dev newComputePipelineStateWithDescriptor:cdesc
                                           options:MTLPipelineOptionNone
                                        reflection:nil
                                             error:&error];
    Expect(kernel != nil, "the stage-in kernel builds");
    id<MTLBuffer> out = SharedBuffer(device, 4 * 16);
    const VertexBufferSlot kernel_slot{0, kSlot, false};
    const VertexStream same{Ref(colors), 0};
    if (kernel) {
        auto* enc = (__bridge id<MTLComputeCommandEncoder>)encoder.ComputeEncoder();
        [enc setComputePipelineState:kernel];
        [enc setBuffer:colors offset:0 attributeStride:StreamStride(kernel_slot, &same)
               atIndex:kSlot];
        [enc setBuffer:out offset:0 atIndex:1];
        [enc setStageInRegion:MTLRegionMake1D(0, 4)];
        [enc dispatchThreads:MTLSizeMake(4, 1, 1) threadsPerThreadgroup:MTLSizeMake(4, 1, 1)];
    }

    // Render: a Constant-step layout (per-instance, divisor 0) with a V# stride of 32.
    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    MTLVertexDescriptor* vd = [MTLVertexDescriptor new];
    vd.attributes[0].format = MTLVertexFormatFloat4;
    vd.attributes[0].bufferIndex = kSlot;
    vd.layouts[kSlot].stride = MTLBufferLayoutStrideDynamic;
    vd.layouts[kSlot].stepFunction = MTLVertexStepFunctionConstant;
    vd.layouts[kSlot].stepRate = 0;
    desc.vertexDescriptor = vd;
    id<MTLRenderPipelineState> pipeline =
        Pipeline(device, library, @"fullscreen_attr", @"attr_color", desc);
    const std::array<VertexBufferSlot, 1> slots{VertexBufferSlot{0, kSlot, true}};
    DrawPipeline p = View(pipeline, &shaders.none, &shaders.none);
    p.vertex_slots = slots;
    const std::array<VertexStream, 1> strided{VertexStream{Ref(colors), 32}};
    id<MTLTexture> target = Target(device);
    encoder.BeginRenderPass(Pass(target));
    Expect(runs.Draw(encoder, p, FullState(), Resources{{}, {}, strided, {}, IndexType::U16},
                     DrawArgs{3}),
           "a constant stream draws");
    encoder.EndRenderPass();
    id<MTLBuffer> image = Readback(device, encoder, target);
    device.Wait(encoder.Submit());
    const float* o = static_cast<const float*>(out.contents);
    bool all_first = kernel != nil;
    for (u32 i = 0; i < 4; ++i) {
        all_first = all_first && o[i * 4] == 1.0f && o[i * 4 + 1] == 0.0f && o[i * 4 + 3] == 1.0f;
    }
    Expect(all_first, "a kernel stage-in stream at stride 0 gives every vertex element 0");
    Expect(Pixel(image, 10, 10) == kRed && Pixel(image, 40, 20) == kRed,
           "a constant render stream at stride 0 reads element 0");
    Expect(device.ErrorCount() == 0, "kernel and constant stream strides ran without errors");
}

/// Per-vertex layouts under the validation layer: a missing stream over many vertices (the
/// stand-in at stride 0), a V# stride of 0 indexed far past element 0, and V# strides that are
/// not a multiple of 4 (2-byte formats at stride 6, as guest vertex data can be packed).
void TestPerVertexStrideEdgeCases() {
    Device device;
    Encoder encoder{device};
    NullResources null_resources{device};
    Runs runs{device, null_resources.Get()};
    Shaders shaders;
    id<MTLLibrary> library = Library(device);
    constexpr u32 kSlot = 30;
    const auto pipeline_for = [&](MTLVertexFormat format) {
        MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
        MTLVertexDescriptor* vd = [MTLVertexDescriptor new];
        vd.attributes[0].format = format;
        vd.attributes[0].bufferIndex = kSlot;
        vd.layouts[kSlot].stride = MTLBufferLayoutStrideDynamic;
        vd.layouts[kSlot].stepFunction = MTLVertexStepFunctionPerVertex;
        desc.vertexDescriptor = vd;
        return Pipeline(device, library, @"fullscreen_attr", @"attr_color", desc);
    };
    const std::array<VertexBufferSlot, 1> slots{VertexBufferSlot{0, kSlot, false}};

    // Missing stream, 30000 vertices (10000 triangles over the target): the 64 KiB stand-in at
    // stride 0 is read at element 0 only.
    id<MTLRenderPipelineState> float4_pipeline = pipeline_for(MTLVertexFormatFloat4);
    DrawPipeline p4 = View(float4_pipeline, &shaders.none, &shaders.none);
    p4.vertex_slots = slots;
    id<MTLTexture> missing = Target(device);
    encoder.BeginRenderPass(Pass(missing));
    Expect(runs.Draw(encoder, p4, FullState(), Resources{{}, {}, {}, {}, IndexType::U16},
                     DrawArgs{30000}),
           "a missing stream over 30000 vertices draws");
    encoder.EndRenderPass();

    // V# stride 0 on a 16-byte buffer, 30000 vertices: element 0 for all.
    id<MTLBuffer> one = SharedBuffer(device, 16);
    const float red[4] = {1, 0, 0, 1};
    std::memcpy(one.contents, red, sizeof(red));
    const std::array<VertexStream, 1> zero{VertexStream{Ref(one), 0}};
    id<MTLTexture> repeated = Target(device);
    encoder.BeginRenderPass(Pass(repeated));
    Expect(runs.Draw(encoder, p4, FullState(), Resources{{}, {}, zero, {}, IndexType::U16},
                     DrawArgs{30000}),
           "a stride-0 stream over 30000 vertices draws");
    encoder.EndRenderPass();

    // Stride 6 (not a multiple of 4), UChar2Normalized: vertices at bytes 0, 6, 12 are red
    // (255, 0); every other 2-byte pair is green (0, 255), so a stride rounded to 4 or 8 shows.
    id<MTLBuffer> packed = SharedBuffer(device, 64);
    u8* bytes = static_cast<u8*>(packed.contents);
    for (u32 i = 0; i < 64; i += 2) {
        bytes[i] = 0;
        bytes[i + 1] = 255;
    }
    for (u32 v = 0; v < 3; ++v) {
        bytes[v * 6] = 255;
        bytes[v * 6 + 1] = 0;
    }
    id<MTLRenderPipelineState> uchar2_pipeline = pipeline_for(MTLVertexFormatUChar2Normalized);
    DrawPipeline p2 = View(uchar2_pipeline, &shaders.none, &shaders.none);
    p2.vertex_slots = slots;
    const std::array<VertexStream, 1> six{VertexStream{Ref(packed), 6}};
    id<MTLTexture> odd = Target(device);
    encoder.BeginRenderPass(Pass(odd));
    Expect(runs.Draw(encoder, p2, FullState(), Resources{{}, {}, six, {}, IndexType::U16},
                     DrawArgs{3}),
           "a stride-6 stream draws");
    encoder.EndRenderPass();
    // Stride 6 at an offset that is not a multiple of 4 (2): the same pattern shifted 2 bytes.
    id<MTLBuffer> shifted = SharedBuffer(device, 64);
    std::memcpy(static_cast<u8*>(shifted.contents) + 2, bytes, 62);
    const std::array<VertexStream, 1> six_at_2{VertexStream{Ref(shifted, 2), 6}};
    id<MTLTexture> odd_offset = Target(device);
    encoder.BeginRenderPass(Pass(odd_offset));
    Expect(runs.Draw(encoder, p2, FullState(), Resources{{}, {}, six_at_2, {}, IndexType::U16},
                     DrawArgs{3}),
           "a stride-6 stream at offset 2 draws");
    encoder.EndRenderPass();

    id<MTLBuffer> out_missing = Readback(device, encoder, missing);
    id<MTLBuffer> out_repeated = Readback(device, encoder, repeated);
    id<MTLBuffer> out_odd = Readback(device, encoder, odd);
    id<MTLBuffer> out_odd_offset = Readback(device, encoder, odd_offset);
    device.Wait(encoder.Submit());
    Expect(Pixel(out_missing, 10, 10) == 0u && Pixel(out_missing, 40, 20) == 0u,
           "a missing stream over many vertices reads zero");
    Expect(Pixel(out_repeated, 10, 10) == kRed && Pixel(out_repeated, 40, 20) == kRed,
           "a stride-0 stream over many vertices reads element 0");
    Expect(Pixel(out_odd, 10, 10) == kRed && Pixel(out_odd, 40, 20) == kRed,
           "a stride-6 stream reads bytes 0, 6 and 12");
    Expect(Pixel(out_odd_offset, 10, 10) == kRed && Pixel(out_odd_offset, 40, 20) == kRed,
           "a stride-6 stream at offset 2 reads bytes 2, 8 and 14");
    Expect(device.ErrorCount() == 0, "per-vertex stride edge cases ran without errors");
}

void TestMaskDepthStencil() {
    DepthStencilKey key;
    key.depth_test = true;
    key.depth_write = true;
    key.depth_compare = 1;
    key.stencil_test = true;
    key.front.write_mask = 0x0f;
    const DepthStencilKey none = MaskDepthStencil(key, false, false);
    Expect(none == DepthStencilKey{}, "no attachments: the default state");
    const DepthStencilKey depth = MaskDepthStencil(key, true, false);
    Expect(depth.depth_test && depth.depth_write && depth.depth_compare == 1 &&
               !depth.stencil_test && depth.front == StencilFace{},
           "depth only: the stencil part is zeroed");
}

} // namespace

int main() {
    @autoreleasepool {
        TestBindStageNullFallbacks();
        TestDrawAfterSubmitRebinds();
        TestSkippedDraws();
        TestIndirectDraws();
        TestTessellationFlushOrder();
        TestHeldDraws();
        TestRunWrappers();
        TestScratchPool();
        TestMaskDepthStencil();
        TestVertexStreamStrides();
        TestStreamStrideRule();
        TestKernelAndConstantStreamStrides();
        TestPerVertexStrideEdgeCases();
    }
    if (failures) {
        std::printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    std::printf("PASS: Metal-native draw helper\n");
    return 0;
}
