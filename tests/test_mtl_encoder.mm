// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: unit test for the Metal-native foundation (docs/superpowers/plans/
// 2026-10-16-metal-native-m1-device-encoder.md): Metal::Object, Metal::Device ticks and limits,
// Metal::Encoder blits, dispatches, render passes and draws, on the system's GPU. No window.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <thread>
#include <vector>
#include "video_core/renderer_metal/mtl_blit_shaders.h"
#include "video_core/renderer_metal/mtl_buffer.h"
#include "video_core/renderer_metal/mtl_buffer_barriers.h"
#include "video_core/renderer_metal/mtl_counters.h"
#include "video_core/renderer_metal/mtl_object.h"
#include "video_core/renderer_metal/mtl_order.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_depth_stencil.h"
#include "video_core/renderer_metal/mtl_render_pipeline.h"
#include "video_core/renderer_metal/mtl_specialize.h"
#include "video_core/renderer_metal/mtl_texture.h"
#include "video_core/renderer_metal/mtl_timeline.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

std::atomic<int> probes_alive{0};

std::vector<u32> ReadWords(const char* path) {
    FILE* f = std::fopen(path, "rb");
    std::vector<u32> words;
    if (!f) return words;
    u32 w;
    while (std::fread(&w, 4, 1, f) == 1) words.push_back(w);
    std::fclose(f);
    return words;
}

/// OpConstant (43) result values of the module, by result id.
std::map<u32, u32> Constants(const std::vector<u32>& w) {
    std::map<u32, u32> out;
    for (size_t i = 5; i < w.size();) {
        const u32 count = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 43 && count == 4) out[w[i + 2]] = w[i + 3];
        Expect(op != 50, "no OpSpecConstant left after Specialize");
        i += count ? count : 1;
    }
    return out;
}

void TestSpecialize() {
    const std::vector<u32> spirv = ReadWords("out/spec_const.spv");
    Expect(!spirv.empty(), "out/spec_const.spv exists");
    const Metal::SpecConstant constants[] = {{3, 77}};
    const std::vector<u32> out = Metal::Specialize(spirv, constants);
    bool has77 = false, has1 = false;
    for (const auto& [id, value] : Constants(out)) {
        has77 |= value == 77;
        has1 |= value == 1;
    }
    Expect(has77 && has1, "SpecId 3 takes the given value, SpecId 0 keeps its default");
}

void TestDepthStencilCache() {
    Metal::Device device;
    Metal::DepthStencilCache cache{device};
    Metal::DepthStencilKey a;
    a.depth_test = a.depth_write = true;
    a.depth_compare = 1; // Less
    Metal::DepthStencilKey b = a;
    b.depth_write = false;
    void* sa = cache.Get(a);
    Expect(sa && cache.Get(a) == sa, "same key, same state");
    Expect(cache.Get(b) && cache.Get(b) != sa, "different key, different state");
}

} // namespace

/// Counts live instances, to see when an Object releases what it holds.
@interface Probe : NSObject
@end
@implementation Probe
- (instancetype)init {
    if ((self = [super init])) {
        probes_alive.fetch_add(1);
    }
    return self;
}
- (void)dealloc {
    probes_alive.fetch_sub(1);
}
@end

namespace {

void TestObject() {
    @autoreleasepool {
        {
            Metal::Object a = Metal::Object::Adopt((__bridge_retained void*)[Probe new]);
            Expect(bool(a) && probes_alive == 1, "Object::Adopt holds the object");
            Metal::Object b = std::move(a);
            Expect(!a && bool(b) && probes_alive == 1, "moving an Object moves the reference");
            Metal::Object c = Metal::Object::Retain(b.Get());
            b.Reset();
            Expect(!b && probes_alive == 1, "Object::Retain keeps the object after the first Reset");
        }
        Expect(probes_alive == 0, "the last Object releases the object");
    }
}

void TestTicks() {
    Metal::Device device;
    const Metal::Limits& limits = device.GetLimits();
    Expect(limits.max_buffer_length >= (u64(1) << 30), "max buffer length read from the device");
    Expect(limits.uniform_alignment == 16 && limits.storage_alignment == 16 &&
               limits.max_samplers == 500000,
           "limits keep MoltenVK's values");
    Expect(device.AllocatedSize() > 0, "the device reports its allocated size");
    Expect(device.CurrentTick() == 1 && device.KnownGpuTick() == 0, "ticks start at 1");

    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)device.NativeQueue();
    id<MTLCommandBuffer> empty = [queue commandBuffer];
    const u64 first = device.Commit((__bridge void*)empty);
    Expect(first == 1 && device.CurrentTick() == 2, "Commit signals the current tick and advances");
    device.Wait(first);
    Expect(device.IsFree(first), "an empty command buffer completes");
    Expect(!device.IsFree(2), "an uncommitted tick is not free");

    for (int i = 0; i < 3; ++i) {
        device.Commit((__bridge void*)[queue commandBuffer]);
    }
    device.Finish();
    Expect(device.IsFree(4) && device.KnownGpuTick() >= 4, "Finish waits for every commit");
    Expect(device.ErrorCount() == 0, "no command buffer failed");
}

id<MTLBuffer> SharedBuffer(Metal::Device& device, NSUInteger length) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    id<MTLBuffer> buffer = [dev newBufferWithLength:length options:MTLResourceStorageModeShared];
    std::memset(buffer.contents, 0, length);
    return buffer;
}

Metal::BufferRef Ref(id<MTLBuffer> buffer, u64 offset = 0) {
    return {(__bridge void*)buffer, offset};
}

id<MTLTexture> Texture(Metal::Device& device, u32 width, u32 height) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    MTLTextureDescriptor* desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModePrivate;
    return [dev newTextureWithDescriptor:desc];
}

u32 Pixel(id<MTLBuffer> readback, u32 width, u32 x, u32 y) {
    u32 value;
    std::memcpy(&value, static_cast<const u8*>(readback.contents) + (y * width + x) * 4, 4);
    return value;
}

void TestBlits() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLBuffer> src = SharedBuffer(device, 256);
    id<MTLBuffer> dst = SharedBuffer(device, 256);
    for (u32 i = 0; i < 256; ++i) {
        static_cast<u8*>(src.contents)[i] = u8(i);
    }
    encoder.FillBuffer(Ref(dst), 64, 0x7f7f7f7fu);
    encoder.CopyBuffer(Ref(src, 16), Ref(dst, 128), 32);
    // Buffer -> texture -> texture -> buffer round trip, 4x4 RGBA8.
    id<MTLTexture> a = Texture(device, 4, 4);
    id<MTLTexture> b = Texture(device, 4, 4);
    id<MTLBuffer> pixels = SharedBuffer(device, 64);
    for (u32 i = 0; i < 64; ++i) {
        static_cast<u8*>(pixels.contents)[i] = u8(200 - i);
    }
    id<MTLBuffer> back = SharedBuffer(device, 64);
    encoder.CopyBufferToTexture(Ref(pixels), 16, 64, (__bridge void*)a, 0, 0, {}, {4, 4, 1});
    encoder.CopyTexture((__bridge void*)a, 0, 0, {}, (__bridge void*)b, 0, 0, {}, {4, 4, 1});
    encoder.CopyTextureToBuffer((__bridge void*)b, 0, 0, {}, {4, 4, 1}, Ref(back), 16, 64);
    const u64 tick = encoder.Submit();
    device.Wait(tick);

    const u8* d = static_cast<const u8*>(dst.contents);
    bool filled = true, copied = true;
    for (u32 i = 0; i < 64; ++i) {
        filled &= d[i] == 0x7f;
    }
    for (u32 i = 0; i < 32; ++i) {
        copied &= d[128 + i] == u8(16 + i);
    }
    Expect(filled, "FillBuffer fills the range");
    Expect(copied && d[127] == 0 && d[160] == 0, "CopyBuffer copies exactly the range");
    Expect(std::memcmp(pixels.contents, back.contents, 64) == 0,
           "buffer -> texture -> texture -> buffer keeps the bytes");
    Expect(device.ErrorCount() == 0, "blits ran without errors");
}

/// GPU busy time: the device sums the GPU intervals of the command buffers it commits (tracked
/// and untracked; overlapping intervals count once), read by the frame stats line.
void TestGpuBusyTime() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    Expect(device.GpuBusyNs() == 0, "no GPU time before any commit");
    id<MTLBuffer> big = SharedBuffer(device, 64u << 20);
    encoder.FillBuffer(Ref(big), 64u << 20, 0x11111111u);
    device.Wait(encoder.Submit());
    const u64 after_tracked = device.GpuBusyNs();
    Expect(after_tracked > 0, "a committed command buffer reports GPU busy time");
    Expect(after_tracked < 5'000'000'000ull, "the busy time is plausible (under 5 s)");
    // The reference: the same fill once the buffer is warm (the first touch costs more).
    encoder.FillBuffer(Ref(big), 64u << 20, 0x22222222u);
    device.Wait(encoder.Submit());
    const u64 before_untracked = device.GpuBusyNs();
    const u64 fill_ns = before_untracked - after_tracked;
    encoder.FillBuffer(Ref(big), 64u << 20, 0x33333333u);
    encoder.SubmitUntracked();
    device.Wait(encoder.Submit()); // the queue completes in order: the untracked one is done
    // At least half the fill's time must come from the untracked buffer (the empty tracked one
    // after it adds next to nothing).
    Expect(fill_ns > 0 && device.GpuBusyNs() - before_untracked >= fill_ns / 2,
           "untracked command buffers count too");
}

/// The union of GPU intervals must not depend on the order completion handlers report them
/// (two queues: a present command buffer may complete before the main one it overlaps).
void TestBusyIntervalUnion() {
    const auto near = [](double a, double b) { return a > b - 1e-9 && a < b + 1e-9; };
    Metal::BusyIntervals in_order;
    in_order.Add(10.0, 20.2);
    in_order.Add(20.0, 20.5);
    Expect(near(in_order.TotalSeconds(), 10.5), "in end order: 10.5 s");
    Metal::BusyIntervals out_of_order;
    out_of_order.Add(20.0, 20.5); // present first
    out_of_order.Add(10.0, 20.2); // then the main buffer it overlaps
    Expect(near(out_of_order.TotalSeconds(), 10.5), "out of end order: still 10.5 s");
    Metal::BusyIntervals mixed{100.0}; // seconds apart: keep every span
    mixed.Add(5.0, 6.0);
    mixed.Add(1.0, 2.0);  // before everything counted
    mixed.Add(1.5, 5.5);  // bridges both
    mixed.Add(1.2, 1.8);  // fully covered
    mixed.Add(7.0, 7.0);  // empty
    Expect(near(mixed.TotalSeconds(), 5.0), "disjoint, bridging and covered intervals: 5 s");
}

/// The GPU profiler's sampling: every Metal encoder opened while a TimestampSink is set
/// samples its start and end (render: vertex start, fragment start and end) into the sink's
/// counter buffer; with no room the encoder is not sampled.
struct TestSink final : Metal::TimestampSink {
    Metal::TimestampBuffer& samples;
    u32 used = 0, limit = 0;
    std::vector<u32> counts; ///< samples reserved per encoder
    explicit TestSink(Metal::TimestampBuffer& b, u32 max) : samples{b}, limit{max} {}
    bool Reserve(void*& buffer, u32& index, u32 count) override {
        if (used + count > limit) {
            return false;
        }
        buffer = samples.Native();
        index = used;
        used += count;
        counts.push_back(count);
        return true;
    }
};

void TestTimestampSampling() {
    Metal::Device device;
    Metal::TimestampBuffer samples{device, 4096};
    Expect(samples.Valid() && samples.Count() >= 256, "this GPU samples timestamps at encoder boundaries");
    if (!samples.Valid()) {
        return;
    }
    Metal::Encoder encoder{device};
    TestSink sink{samples, 7};
    encoder.SetTimestampSink(&sink);
    id<MTLBuffer> big = SharedBuffer(device, 16u << 20);
    encoder.FillBuffer(Ref(big), 16u << 20, 0x33333333u); // blit: 2 samples
    encoder.FillBuffer(Ref(big), 16u << 20, 0x01020304u); // compute kernel: 2 samples
    id<MTLTexture> target = Texture(device, 64, 64);
    Metal::RenderPassDesc pass{};
    pass.colors[0].texture = (__bridge void*)target;
    pass.colors[0].clear = true;
    encoder.BeginRenderPass(pass); // a clear-only render encoder: 3 samples
    encoder.EndRenderPass();
    encoder.FillBuffer(Ref(big), 16u << 20, 0x44444444u); // no room left: not sampled
    encoder.SetTimestampSink(nullptr);
    device.Wait(encoder.Submit());
    Expect(sink.counts == std::vector<u32>({2, 2, 3}), "blit, compute and render encoders reserve 2, 2, 3");
    std::vector<u64> t;
    samples.Resolve(0, 7, t);
    Expect(t[0] != 0 && t[1] > t[0], "the blit encoder's end follows its start");
    Expect(t[2] != 0 && t[3] > t[2], "the compute encoder's end follows its start");
    Expect(t[6] != 0 && t[6] >= t[1], "the render encoder's fragment end is sampled, after the blit");
    u64 cpu0, gpu0, cpu1, gpu1;
    Metal::TimestampBuffer::Correlate(device, cpu0, gpu0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Metal::TimestampBuffer::Correlate(device, cpu1, gpu1);
    Expect(gpu1 > gpu0 && cpu1 > cpu0, "CPU and GPU timestamps advance");
    Expect(device.ErrorCount() == 0, "sampled encoders ran without errors");
}

void TestKeepAlive() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    @autoreleasepool {
        encoder.KeepAlive(Metal::Object::Adopt((__bridge_retained void*)[Probe new]));
    }
    Expect(probes_alive == 1, "KeepAlive holds the object while recording");
    const u64 empty = encoder.Submit(); // nothing recorded: still a tick that completes
    device.Wait(empty);
    for (int i = 0; i < 100 && probes_alive != 0; ++i) {
        [NSThread sleepForTimeInterval:0.01]; // the completion handler may run just after the event
    }
    Expect(probes_alive == 0, "KeepAlive releases the object once the GPU is done");
    Expect(encoder.Submit() == empty + 1, "every Submit takes the next tick");
}


/// Completion handlers must never write into a Device that is gone: a new Device (often at the
/// same address) must start with no known GPU work.
void TestDeviceLifetime() {
    bool clean = true;
    for (int round = 0; round < 50 && clean; ++round) {
        {
            Metal::Device old;
            id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)old.NativeQueue();
            for (int i = 0; i < 4; ++i) {
                old.Commit((__bridge void*)[queue commandBuffer]);
            }
        }
        Metal::Device fresh;
        [NSThread sleepForTimeInterval:0.002]; // let late handlers of `old` run
        clean = fresh.KnownGpuTick() == 0 && !fresh.IsFree(1);
    }
    Expect(clean, "a destroyed Device's completion handlers do not touch a new Device");
}

constexpr const char* kTestShaders = R"(
#include <metal_stdlib>
using namespace metal;

kernel void doubles(device uint* out [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    out[i] = i * 2u;
}

kernel void copy_constants(device uint* out [[buffer(0)]], constant uint* in [[buffer(1)]],
                           uint i [[thread_position_in_grid]]) {
    out[i] = in[i];
}

kernel void sample_binding(device float4* out [[buffer(0)]],
                           texture2d<float> image [[texture(100)]], sampler s [[sampler(15)]]) {
    out[0] = image.sample(s, float2(1.25, 0.5));
}

struct VOut { float4 pos [[position]]; };

vertex VOut fullscreen(uint vid [[vertex_id]]) {
    const float2 uv = float2((vid << 1) & 2, vid & 2);
    VOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

vertex VOut indexed_quad(uint vid [[vertex_id]], constant float2* corners [[buffer(0)]]) {
    VOut o;
    o.pos = float4(corners[vid], 0.0, 1.0);
    return o;
}

fragment float4 solid(constant float4& color [[buffer(0)]]) {
    return color;
}
)";

id<MTLLibrary> TestLibrary(Metal::Device& device) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    NSError* error = nil;
    id<MTLLibrary> library = [dev newLibraryWithSource:@(kTestShaders) options:nil error:&error];
    if (!library) {
        std::printf("FAIL: test shaders: %s\n", error.localizedDescription.UTF8String);
        std::exit(1);
    }
    return library;
}

id<MTLComputePipelineState> Kernel(Metal::Device& device, id<MTLLibrary> library, NSString* name) {
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

void TestCompute() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLLibrary> library = TestLibrary(device);
    id<MTLComputePipelineState> doubles = Kernel(device, library, @"doubles");
    id<MTLBuffer> out = SharedBuffer(device, 64 * 4);
    encoder.SetComputePipeline((__bridge void*)doubles);
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out));
    encoder.Dispatch({4, 1, 1}, {16, 1, 1});
    // A blit between two dispatches ends the compute encoder; the second dispatch must get its
    // pipeline and buffer again on the new encoder.
    id<MTLBuffer> scratch = SharedBuffer(device, 16);
    encoder.FillBuffer(Ref(scratch), 16, 0);
    id<MTLBuffer> out2 = SharedBuffer(device, 64 * 4);
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out2));
    encoder.Dispatch({4, 1, 1}, {16, 1, 1});
    device.Wait(encoder.Submit());

    bool ok = true, ok2 = true;
    for (u32 i = 0; i < 64; ++i) {
        ok &= static_cast<const u32*>(out.contents)[i] == i * 2;
        ok2 &= static_cast<const u32*>(out2.contents)[i] == i * 2;
    }
    Expect(ok, "Dispatch runs the kernel over the grid");
    Expect(ok2, "state is reapplied on a compute encoder opened after a blit");
}

void TestResourceBindingCache() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLComputePipelineState> sample = Kernel(device, TestLibrary(device), @"sample_binding");
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    id<MTLTexture> a = Texture(device, 2, 1), b = Texture(device, 2, 1);
    id<MTLBuffer> pixels = SharedBuffer(device, 16);
    const u32 colors[4] = {0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffff0000u};
    std::memcpy(pixels.contents, colors, sizeof(colors));
    encoder.CopyBufferToTexture(Ref(pixels), 8, 8, (__bridge void*)a, 0, 0, {}, {2, 1, 1});
    encoder.CopyBufferToTexture(Ref(pixels, 8), 8, 8, (__bridge void*)b, 0, 0, {}, {2, 1, 1});
    MTLSamplerDescriptor* desc = [MTLSamplerDescriptor new];
    desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> clamp = [dev newSamplerStateWithDescriptor:desc];
    desc.sAddressMode = MTLSamplerAddressModeRepeat;
    id<MTLSamplerState> repeat = [dev newSamplerStateWithDescriptor:desc];
    id<MTLBuffer> out = SharedBuffer(device, 7 * 16), scratch = SharedBuffer(device, 16);
    encoder.SetComputePipeline((__bridge void*)sample, Metal::Access::None);
    for (u32 i = 0; i < 32; ++i) {
        encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out));
        encoder.SetTexture(Metal::Stage::Compute, 100, (__bridge void*)a);
        encoder.SetSampler(Metal::Stage::Compute, 15, (__bridge void*)clamp);
        encoder.Dispatch({1, 1, 1}, {1, 1, 1});
    }
    const char* stats = std::getenv("BB_ENCODER_STATS");
    if (stats && stats[0] == '1') {
        const char* setting = std::getenv("BB_METAL_BINDING_CACHE");
        const bool cached = !(setting && std::strcmp(setting, "0") == 0);
        const auto counts = encoder.GetBindingStats();
        Expect(counts.emitted == (cached ? 3u : 96u) &&
                   counts.skipped == (cached ? 93u : 0u),
               "32 repeated dispatches emit 3 cached bindings or 96 reference bindings");
        std::printf("Resource binding fixture: %llu Metal calls, %llu identical setters skipped\n",
                    (unsigned long long)counts.emitted, (unsigned long long)counts.skipped);
    }
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out, 16));
    encoder.Dispatch({1, 1, 1}, {1, 1, 1});
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out, 32));
    encoder.SetSampler(Metal::Stage::Compute, 15, (__bridge void*)repeat);
    encoder.Dispatch({1, 1, 1}, {1, 1, 1});
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out, 48));
    encoder.SetTexture(Metal::Stage::Compute, 100, (__bridge void*)b);
    encoder.Dispatch({1, 1, 1}, {1, 1, 1});
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out, 64));
    // External kernels overwrite the actual encoder state. Identical setters must
    // preserve the dirty bits that ComputeEncoder() set for the following dispatch.
    id<MTLComputeCommandEncoder> external =
        (__bridge id<MTLComputeCommandEncoder>)encoder.ComputeEncoder(Metal::Access::None);
    [external setBuffer:scratch offset:0 atIndex:0];
    [external setTexture:a atIndex:100];
    [external setSamplerState:clamp atIndex:15];
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out, 64));
    encoder.SetTexture(Metal::Stage::Compute, 100, (__bridge void*)b);
    encoder.SetSampler(Metal::Stage::Compute, 15, (__bridge void*)repeat);
    encoder.Dispatch({1, 1, 1}, {1, 1, 1});
    encoder.FillBuffer(Ref(scratch), 16, 0);
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out, 80));
    encoder.SetTexture(Metal::Stage::Compute, 100, (__bridge void*)b);
    encoder.SetSampler(Metal::Stage::Compute, 15, (__bridge void*)repeat);
    encoder.Dispatch({1, 1, 1}, {1, 1, 1});
    device.Wait(encoder.Submit());
    encoder.SetComputePipeline((__bridge void*)sample, Metal::Access::None);
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out, 96));
    encoder.SetTexture(Metal::Stage::Compute, 100, (__bridge void*)b);
    encoder.SetSampler(Metal::Stage::Compute, 15, (__bridge void*)repeat);
    encoder.Dispatch({1, 1, 1}, {1, 1, 1});
    device.Wait(encoder.Submit());
    const float expected[7][4] = {{0,1,0,1}, {0,1,0,1}, {1,0,0,1}, {0,0,1,1},
                                {0,0,1,1}, {0,0,1,1}, {0,0,1,1}};
    Expect(std::memcmp(out.contents, expected, sizeof(expected)) == 0,
           "buffer offsets, texture/sampler changes, external work, reopen and submit keep GPU results");
    Expect(device.ErrorCount() == 0, "resource binding cache ran without GPU errors");
}

void TestInlineBindingCache() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLComputePipelineState> copy = Kernel(device, TestLibrary(device), @"copy_constants");
    id<MTLBuffer> out = SharedBuffer(device, 9 * 32), input = SharedBuffer(device, 32);
    std::array<u32, 8> values{1,2,3,4,5,6,7,8};
    const auto original = values;
    std::memcpy(input.contents, values.data(), 32);
    encoder.SetComputePipeline((__bridge void*)copy, Metal::Access::None);
    for (u32 i = 0; i < 32; ++i) {
        const auto identical = values; // equal bytes at another address
        encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out));
        encoder.SetBytes(Metal::Stage::Compute, 1, identical.data(), 32);
        encoder.Dispatch({1, 1, 1}, {8, 1, 1});
    }
    const char* stats = std::getenv("BB_ENCODER_STATS");
    if (stats && stats[0] == '1') {
        const char* setting = std::getenv("BB_METAL_BINDING_CACHE");
        const bool cached = !(setting && std::strcmp(setting, "0") == 0);
        const auto counts = encoder.GetBindingStats();
        Expect(counts.emitted == (cached ? 2u : 64u) &&
                   counts.skipped == (cached ? 62u : 0u),
               "32 repeated inline dispatches emit 2 cached bindings or 64 reference bindings");
        std::printf("Inline binding fixture: %llu Metal calls, %llu identical setters skipped\n",
                    (unsigned long long)counts.emitted, (unsigned long long)counts.skipped);
    }
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out, 32));
    encoder.SetBytes(Metal::Stage::Compute, 1, values.data(), 32);
    values[0] = 99; // a reused source pointer must not hide a changed payload
    encoder.SetBytes(Metal::Stage::Compute, 1, values.data(), 32);
    encoder.Dispatch({1, 1, 1}, {8, 1, 1});
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out, 64));
    encoder.SetBytes(Metal::Stage::Compute, 1, values.data(), 16);
    encoder.Dispatch({1, 1, 1}, {4, 1, 1});
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out, 96));
    encoder.SetBuffer(Metal::Stage::Compute, 1, Ref(input));
    encoder.Dispatch({1, 1, 1}, {8, 1, 1});
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out, 128));
    encoder.SetBytes(Metal::Stage::Compute, 1, values.data(), 32);
    encoder.Dispatch({1, 1, 1}, {8, 1, 1});
    std::vector<u32> large(2048, 17);
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out, 160));
    encoder.SetBytes(Metal::Stage::Compute, 1, large.data(), u32(large.size() * 4));
    encoder.Dispatch({1, 1, 1}, {8, 1, 1});
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out, 192));
    encoder.SetBytes(Metal::Stage::Compute, 1, values.data(), 32);
    encoder.Dispatch({1, 1, 1}, {8, 1, 1});
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out, 224));
    id<MTLComputeCommandEncoder> external =
        (__bridge id<MTLComputeCommandEncoder>)encoder.ComputeEncoder(Metal::Access::None);
    const std::array<u32, 8> zero{};
    [external setBytes:zero.data() length:32 atIndex:1];
    encoder.SetBytes(Metal::Stage::Compute, 1, values.data(), 32);
    encoder.Dispatch({1, 1, 1}, {8, 1, 1});
    encoder.EndEncoding();
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out, 256));
    encoder.SetBytes(Metal::Stage::Compute, 1, values.data(), 32);
    encoder.Dispatch({1, 1, 1}, {8, 1, 1});
    device.Wait(encoder.Submit());
    const u32* result = static_cast<const u32*>(out.contents);
    for (u32 row = 0; row < 9; ++row) {
        for (u32 i = 0; i < 8; ++i) {
            const u32 expected = row == 5 ? 17 : row == 2 && i >= 4 ? 0 :
                                 row == 0 || row == 3 ? original[i] : values[i];
            Expect(result[row * 8 + i] == expected,
                   "inline identity includes exact bytes, size, buffer/large transitions and reapply");
        }
    }
    Expect(device.ErrorCount() == 0, "inline binding cache ran without GPU errors");
}

void TestLargeBytes() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLLibrary> library = TestLibrary(device);
    id<MTLComputePipelineState> copy = Kernel(device, library, @"copy_constants");
    std::vector<u32> small(64), large(2048); // 256 B inline; 8 KiB over the setBytes limit
    for (u32 i = 0; i < small.size(); ++i) {
        small[i] = 1000 + i;
    }
    for (u32 i = 0; i < large.size(); ++i) {
        large[i] = 50000 + i;
    }
    id<MTLBuffer> out_small = SharedBuffer(device, small.size() * 4);
    id<MTLBuffer> out_large = SharedBuffer(device, large.size() * 4);
    encoder.SetComputePipeline((__bridge void*)copy);
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out_small));
    encoder.SetBytes(Metal::Stage::Compute, 1, small.data(), u32(small.size() * 4));
    encoder.Dispatch({1, 1, 1}, {64, 1, 1});
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out_large));
    encoder.SetBytes(Metal::Stage::Compute, 1, large.data(), u32(large.size() * 4));
    std::fill(large.begin(), large.end(), 0u); // the encoder must have copied the bytes already
    encoder.Dispatch({32, 1, 1}, {64, 1, 1});
    device.Wait(encoder.Submit());

    Expect(std::memcmp(out_small.contents, small.data(), small.size() * 4) == 0,
           "SetBytes up to 4 KiB arrives");
    bool ok = true;
    for (u32 i = 0; i < 2048; ++i) {
        ok &= static_cast<const u32*>(out_large.contents)[i] == 50000 + i;
    }
    Expect(ok, "SetBytes over 4 KiB arrives (temporary buffer)");
    Expect(device.ErrorCount() == 0, "compute ran without errors");
}

constexpr u32 kSize = 64;
constexpr u32 kRed = 0xff0000ffu, kGreen = 0xff00ff00u, kBlue = 0xffff0000u, kBlack = 0xff000000u,
              kYellow = 0xff00ffffu; // RGBA8 bytes read as a little-endian u32

id<MTLRenderPipelineState> SolidPipeline(Metal::Device& device, id<MTLLibrary> library,
                                         NSString* vertex) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = [library newFunctionWithName:vertex];
    desc.fragmentFunction = [library newFunctionWithName:@"solid"];
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    NSError* error = nil;
    id<MTLRenderPipelineState> pipeline = [dev newRenderPipelineStateWithDescriptor:desc
                                                                              error:&error];
    if (!pipeline) {
        std::printf("FAIL: render pipeline: %s\n", error.localizedDescription.UTF8String);
        std::exit(1);
    }
    return pipeline;
}

Metal::RenderPassDesc ColorPass(id<MTLTexture> target, bool clear, u32 rgba) {
    Metal::RenderPassDesc desc;
    desc.colors[0].texture = (__bridge void*)target;
    desc.colors[0].clear = clear;
    desc.colors[0].clear_color = {double(rgba & 0xff) / 255.0, double((rgba >> 8) & 0xff) / 255.0,
                                  double((rgba >> 16) & 0xff) / 255.0, 1.0};
    desc.width = kSize;
    desc.height = kSize;
    return desc;
}

/// Records a copy of `texture` into a new shared buffer (kSize x kSize RGBA8).
id<MTLBuffer> Readback(Metal::Device& device, Metal::Encoder& encoder, id<MTLTexture> texture) {
    id<MTLBuffer> out = SharedBuffer(device, kSize * kSize * 4);
    encoder.CopyTextureToBuffer((__bridge void*)texture, 0, 0, {}, {kSize, kSize, 1}, Ref(out),
                                kSize * 4, kSize * kSize * 4);
    return out;
}

void FillTexture(Metal::Device& device, Metal::Encoder& encoder, id<MTLTexture> texture, u32 rgba) {
    id<MTLBuffer> pixels = SharedBuffer(device, kSize * kSize * 4);
    for (u32 i = 0; i < kSize * kSize; ++i) {
        static_cast<u32*>(pixels.contents)[i] = rgba;
    }
    encoder.CopyBufferToTexture(Ref(pixels), kSize * 4, kSize * kSize * 4, (__bridge void*)texture,
                                0, 0, {}, {kSize, kSize, 1});
    encoder.KeepAlive(Metal::Object::Retain((__bridge void*)pixels));
}

void SetColor(Metal::Encoder& encoder, u32 rgba) {
    const float color[4] = {float(rgba & 0xff) / 255.0f, float((rgba >> 8) & 0xff) / 255.0f,
                            float((rgba >> 16) & 0xff) / 255.0f, 1.0f};
    encoder.SetBytes(Metal::Stage::Fragment, 0, color, sizeof(color));
}

void TestClearOnlyPass() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLTexture> cleared = Texture(device, kSize, kSize);
    id<MTLTexture> untouched = Texture(device, kSize, kSize);
    FillTexture(device, encoder, cleared, kBlue);
    FillTexture(device, encoder, untouched, kBlue);
    encoder.BeginRenderPass(ColorPass(cleared, true, kYellow));
    encoder.EndRenderPass();
    encoder.BeginRenderPass(ColorPass(untouched, false, 0));
    encoder.EndRenderPass();
    id<MTLBuffer> a = Readback(device, encoder, cleared);
    id<MTLBuffer> b = Readback(device, encoder, untouched);
    device.Wait(encoder.Submit());
    Expect(Pixel(a, kSize, 5, 5) == kYellow && Pixel(a, kSize, 63, 63) == kYellow,
           "a pass that only clears still clears");
    Expect(Pixel(b, kSize, 5, 5) == kBlue, "a pass with no draws and no clears leaves the target");
}

void TestPassReopenKeepsDrawing() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLLibrary> library = TestLibrary(device);
    id<MTLRenderPipelineState> solid = SolidPipeline(device, library, @"fullscreen");
    id<MTLTexture> target = Texture(device, kSize, kSize);
    id<MTLBuffer> scratch = SharedBuffer(device, 16);

    encoder.BeginRenderPass(ColorPass(target, true, kBlack));
    encoder.SetRenderPipeline((__bridge void*)solid);
    encoder.SetViewport({0, 0, float(kSize), float(kSize), 0, 1});
    encoder.SetScissor({0, 0, kSize / 2, kSize});
    SetColor(encoder, kRed);
    encoder.Draw(Metal::Primitive::Triangle, 3);
    encoder.FillBuffer(Ref(scratch), 16, 0); // interrupts the pass
    encoder.SetScissor({kSize / 2, 0, kSize / 2, kSize});
    SetColor(encoder, kGreen);
    encoder.Draw(Metal::Primitive::Triangle, 3); // reopens the pass
    encoder.EndRenderPass();
    id<MTLBuffer> out = Readback(device, encoder, target);
    device.Wait(encoder.Submit());
    Expect(Pixel(out, kSize, 1, 1) == kRed, "the draw before the interruption is kept (Load)");
    Expect(Pixel(out, kSize, kSize - 2, 1) == kGreen, "the draw after the interruption lands");
    Expect(device.ErrorCount() == 0, "no errors reopening a pass");
}

void TestStatePersistsAcrossPasses() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLLibrary> library = TestLibrary(device);
    id<MTLRenderPipelineState> solid = SolidPipeline(device, library, @"fullscreen");
    id<MTLTexture> first = Texture(device, kSize, kSize);
    id<MTLTexture> second = Texture(device, kSize, kSize);

    encoder.BeginRenderPass(ColorPass(first, true, kBlack));
    encoder.SetRenderPipeline((__bridge void*)solid);
    encoder.SetViewport({0, 0, float(kSize), float(kSize), 0, 1});
    encoder.SetScissor({0, 0, kSize, kSize / 2}); // top half only
    SetColor(encoder, kGreen);
    encoder.Draw(Metal::Primitive::Triangle, 3);
    encoder.EndRenderPass();
    // Second pass: nothing set again; pipeline, viewport, scissor and the color must carry over.
    encoder.BeginRenderPass(ColorPass(second, true, kBlack));
    encoder.Draw(Metal::Primitive::Triangle, 3);
    encoder.EndRenderPass();
    id<MTLBuffer> out = Readback(device, encoder, second);
    device.Wait(encoder.Submit());
    Expect(Pixel(out, kSize, 3, 3) == kGreen, "state set in one pass applies in the next");
    Expect(Pixel(out, kSize, 3, kSize - 3) == kBlack, "including the scissor");
}

void TestIndexedDraw() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLLibrary> library = TestLibrary(device);
    id<MTLRenderPipelineState> quad = SolidPipeline(device, library, @"indexed_quad");
    id<MTLTexture> target = Texture(device, kSize, kSize);
    // Left half of clip space as two triangles from four corners.
    const float corners[8] = {-1, -1, 0, -1, -1, 1, 0, 1};
    id<MTLBuffer> indices = SharedBuffer(device, 12);
    const u16 index_data[6] = {0, 1, 2, 2, 1, 3};
    std::memcpy(indices.contents, index_data, sizeof(index_data));

    encoder.BeginRenderPass(ColorPass(target, true, kBlack));
    encoder.SetRenderPipeline((__bridge void*)quad);
    encoder.SetViewport({0, 0, float(kSize), float(kSize), 0, 1});
    encoder.SetScissor({0, 0, kSize, kSize});
    encoder.SetBytes(Metal::Stage::Vertex, 0, corners, sizeof(corners));
    SetColor(encoder, kBlue);
    encoder.DrawIndexed(Metal::Primitive::Triangle, 6, Metal::IndexType::U16, Ref(indices));
    encoder.EndRenderPass();
    id<MTLBuffer> out = Readback(device, encoder, target);
    device.Wait(encoder.Submit());
    Expect(Pixel(out, kSize, 2, kSize / 2) == kBlue, "DrawIndexed draws the left half");
    Expect(Pixel(out, kSize, kSize - 2, kSize / 2) == kBlack, "and not the right half");
}


/// IsFree/Wait mean the command buffer completed (its handlers ran), not only that the GPU
/// reached the signal: owners free resources at that point, and Metal (validation layer) counts
/// a command buffer as using them until it completes.
void TestWaitMeansCompleted() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLBuffer> buffer = SharedBuffer(device, 4096);
    bool completed = true;
    for (int i = 0; i < 200 && completed; ++i) {
        encoder.FillBuffer(Ref(buffer), 4096, u32(i & 0xff) * 0x01010101u);
        id<MTLCommandBuffer> cmd = (__bridge id<MTLCommandBuffer>)encoder.CommandBuffer();
        const u64 tick = encoder.Submit();
        device.Wait(tick);
        completed = cmd.status == MTLCommandBufferStatusCompleted;
    }
    Expect(completed, "after Wait(tick) the tick's command buffer has completed");
}


/// Bindings end with their command buffer (as in Vulkan): a slot left bound must not be applied
/// again after Submit, when its object may be gone (here an 8 KiB SetBytes temporary).
void TestBindingsEndAtSubmit() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLLibrary> library = TestLibrary(device);
    id<MTLComputePipelineState> copy = Kernel(device, library, @"copy_constants");
    std::vector<u32> large(2048, 7u), small(64, 9u);
    id<MTLBuffer> out = SharedBuffer(device, 64 * 4);
    encoder.SetComputePipeline((__bridge void*)copy);
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out));
    encoder.SetBytes(Metal::Stage::Compute, 1, small.data(), u32(small.size() * 4));
    encoder.SetBytes(Metal::Stage::Compute, 2, large.data(), u32(large.size() * 4)); // unused slot
    encoder.Dispatch({1, 1, 1}, {64, 1, 1});
    device.Wait(encoder.Submit());
    for (int i = 0; i < 100; ++i) {
        [NSThread sleepForTimeInterval:0.001]; // the temporary is released after completion
    }
    id<MTLBuffer> out2 = SharedBuffer(device, 64 * 4);
    encoder.SetComputePipeline((__bridge void*)copy);
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out2));
    encoder.SetBytes(Metal::Stage::Compute, 1, small.data(), u32(small.size() * 4));
    encoder.Dispatch({1, 1, 1}, {64, 1, 1}); // must not touch slot 2's freed buffer
    device.Wait(encoder.Submit());
    Expect(static_cast<const u32*>(out2.contents)[63] == 9u, "a new command buffer starts unbound");
}

/// The recorder is a plain thread with no autorelease pool: recording must not leave
/// autoreleased objects behind on it (they would pile up until the thread exits). A pass
/// descriptor holds its attachment textures, so a leaked descriptor keeps the texture alive.
void TestNoLeakOnPoollessThread() {
    Metal::Device device;
    bool released = false;
    std::thread recorder([&] {
        Metal::Encoder encoder{device};
        __weak id<MTLTexture> weak;
        {
            id<MTLTexture> target = Texture(device, kSize, kSize);
            weak = target;
            encoder.BeginRenderPass(ColorPass(target, false, 0));
            encoder.EndRenderPass();
        }
        released = weak == nil; // still on the thread: its implicit pool has not drained
        device.Wait(encoder.Submit());
    });
    recorder.join();
    Expect(released, "recording on a thread without a pool leaves nothing autoreleased");
}

/// A copy recorded inside a clearing pass before its first draw sees the cleared target.
void TestCopyInPassSeesClear() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLTexture> target = Texture(device, kSize, kSize);
    FillTexture(device, encoder, target, kBlue);
    encoder.BeginRenderPass(ColorPass(target, true, kYellow));
    id<MTLBuffer> out = Readback(device, encoder, target); // before any draw
    encoder.EndRenderPass();
    device.Wait(encoder.Submit());
    Expect(Pixel(out, kSize, 4, 4) == kYellow, "the pass's clear happens before a copy inside it");
}

/// Work encoded straight into CommandBuffer() after EndEncoding() (as MetalFX is), inside a
/// clearing pass whose encoder has not opened, comes after the pass's clear.
void TestOutsideWorkAfterPendingClear() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLTexture> target = Texture(device, kSize, kSize);
    FillTexture(device, encoder, target, kBlue);
    encoder.BeginRenderPass(ColorPass(target, true, kYellow));
    encoder.EndEncoding();
    id<MTLBuffer> out = [(__bridge id<MTLDevice>)device.NativeDevice()
        newBufferWithLength:kSize * kSize * 4
                    options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cmd = (__bridge id<MTLCommandBuffer>)encoder.CommandBuffer();
    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(kSize, kSize, 1) toBuffer:out destinationOffset:0
      destinationBytesPerRow:kSize * 4 destinationBytesPerImage:kSize * kSize * 4];
    [blit endEncoding];
    encoder.EndRenderPass();
    device.Wait(encoder.Submit());
    Expect(Pixel(out, kSize, 4, 4) == kYellow,
           "EndEncoding runs a pending clear before work encoded outside the Encoder");
}

/// A scissor carried from a bigger pass is clamped to the next, smaller target (Metal requires
/// the scissor inside the render target; Vulkan does not).
void TestScissorClampedToTarget() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLLibrary> library = TestLibrary(device);
    id<MTLRenderPipelineState> solid = SolidPipeline(device, library, @"fullscreen");
    id<MTLTexture> big = Texture(device, kSize, kSize);
    id<MTLTexture> small = Texture(device, kSize / 2, kSize / 2);
    encoder.BeginRenderPass(ColorPass(big, true, kBlack));
    encoder.SetRenderPipeline((__bridge void*)solid);
    encoder.SetViewport({0, 0, float(kSize), float(kSize), 0, 1});
    encoder.SetScissor({0, 0, kSize, kSize});
    SetColor(encoder, kRed);
    encoder.Draw(Metal::Primitive::Triangle, 3);
    encoder.EndRenderPass();
    Metal::RenderPassDesc second = ColorPass(small, true, kBlack);
    second.width = second.height = kSize / 2;
    encoder.BeginRenderPass(second);
    encoder.SetViewport({0, 0, float(kSize / 2), float(kSize / 2), 0, 1});
    encoder.Draw(Metal::Primitive::Triangle, 3); // scissor 64x64 carried into a 32x32 target
    encoder.EndRenderPass();
    id<MTLBuffer> out = SharedBuffer(device, kSize * kSize);
    encoder.CopyTextureToBuffer((__bridge void*)small, 0, 0, {}, {kSize / 2, kSize / 2, 1}, Ref(out),
                                kSize / 2 * 4, kSize * kSize);
    device.Wait(encoder.Submit());
    Expect(Pixel(out, kSize / 2, kSize / 2 - 1, kSize / 2 - 1) == kRed,
           "a draw with a carried, too-large scissor fills the smaller target");
}

void TestFillPattern() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLBuffer> b = SharedBuffer(device, 64);
    encoder.FillBuffer(Ref(b), 32, 0x0000000Fu);       // HTile-style pattern: bytes differ
    encoder.FillBuffer(Ref(b, 32), 32, 0x5a5a5a5au);   // uniform bytes: blit
    device.Wait(encoder.Submit());
    const u32* w = static_cast<const u32*>(b.contents);
    bool ok = true;
    for (int i = 0; i < 8; ++i) ok &= w[i] == 0x0000000Fu;
    for (int i = 8; i < 16; ++i) ok &= w[i] == 0x5a5a5a5au;
    Expect(ok, "FillBuffer writes the exact 32-bit pattern");
}

void TestSubmitResetsBindings() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLBuffer> b = SharedBuffer(device, 16);
    encoder.SetBuffer(Metal::Stage::Compute, 3, Ref(b));
    Expect(encoder.HasBindings(), "a binding is recorded");
    device.Wait(encoder.Submit());
    Expect(!encoder.HasBindings(), "Submit clears bindings (objects may be freed after it)");
}

void TestUntrackedRunsFirst() {
    Metal::Device device;
    Metal::Encoder upload{device}, main_encoder{device};
    id<MTLBuffer> src = SharedBuffer(device, 64), mid = SharedBuffer(device, 64),
                  dst = SharedBuffer(device, 64);
    std::memset(src.contents, 0x11, 64);
    upload.CopyBuffer(Ref(src), Ref(mid), 64);   // "hoisted" upload
    main_encoder.CopyBuffer(Ref(mid), Ref(dst), 64);
    upload.SubmitUntracked();
    const u64 tick = main_encoder.Submit();
    Expect(tick == 1, "an untracked commit takes no tick");
    device.Wait(tick);
    Expect(static_cast<const u8*>(dst.contents)[63] == 0x11,
           "the untracked command buffer ran before the tracked one");
}

void TestIndirectResidency() {
    // A kernel reads a buffer only through its GPU address (as shaders read the arena).
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    NSError* error = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@R"(
#include <metal_stdlib>
using namespace metal;
kernel void through_address(device uint* out [[buffer(0)]], constant ulong& address [[buffer(1)]],
                            uint i [[thread_position_in_grid]]) {
    out[i] = ((device const uint*)address)[i];
}
)" options:nil error:&error];
    id<MTLComputePipelineState> k =
        [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"through_address"] error:&error];
    id<MTLBuffer> hidden = [dev newBufferWithLength:256 options:MTLResourceStorageModePrivate];
    id<MTLBuffer> source = SharedBuffer(device, 256), out = SharedBuffer(device, 256);
    for (u32 i = 0; i < 64; ++i) static_cast<u32*>(source.contents)[i] = 300 + i;
    device.MakeResident((__bridge void*)hidden);
    encoder.DeclareIndirect((__bridge void*)hidden);
    encoder.CopyBuffer(Ref(source), Ref(hidden), 256);  // written by a blit, read by address
    const u64 address = hidden.gpuAddress;
    encoder.SetComputePipeline((__bridge void*)k);
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out));
    encoder.SetBytes(Metal::Stage::Compute, 1, &address, sizeof(address));
    encoder.Dispatch({1, 1, 1}, {64, 1, 1});
    device.Wait(encoder.Submit());
    Expect(static_cast<const u32*>(out.contents)[63] == 363, "a resident buffer reads by address");
    device.Evict((__bridge void*)hidden);
}

void TestDepthAspectCopy() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    MTLTextureDescriptor* d = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:4 height:4 mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> ds = [dev newTextureWithDescriptor:d];
    Metal::RenderPassDesc pass;
    pass.depth.texture = (__bridge void*)ds;
    pass.depth.has_stencil = true;
    pass.depth.clear_depth = pass.depth.clear_stencil = true;
    pass.depth.depth = 0.5;
    pass.depth.stencil = 7;
    encoder.BeginRenderPass(pass);
    encoder.EndRenderPass();
    id<MTLBuffer> depth = SharedBuffer(device, 64), stencil = SharedBuffer(device, 16);
    encoder.CopyTextureToBuffer((__bridge void*)ds, 0, 0, {}, {4, 4, 1}, Ref(depth), 16, 64,
                                Metal::BlitAspect::Depth);
    encoder.CopyTextureToBuffer((__bridge void*)ds, 0, 0, {}, {4, 4, 1}, Ref(stencil), 4, 16,
                                Metal::BlitAspect::Stencil);
    device.Wait(encoder.Submit());
    Expect(static_cast<const float*>(depth.contents)[5] == 0.5f, "depth aspect copied");
    Expect(static_cast<const u8*>(stencil.contents)[5] == 7, "stencil aspect copied");
}

/// MSAA resolves through a render pass: a pass that clears and resolves, and a resolve-only pass
/// (no clears, no draws) after a separate clear pass, both into 1x RGBA8 textures.
void TestResolvePasses() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    MTLTextureDescriptor* d = [MTLTextureDescriptor new];
    d.textureType = MTLTextureType2DMultisample;
    d.pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.width = kSize;
    d.height = kSize;
    d.sampleCount = 4;
    d.usage = MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> msaa_a = [dev newTextureWithDescriptor:d];
    id<MTLTexture> msaa_b = [dev newTextureWithDescriptor:d];
    id<MTLTexture> resolved_a = Texture(device, kSize, kSize);
    id<MTLTexture> resolved_b = Texture(device, kSize, kSize);
    FillTexture(device, encoder, resolved_a, kBlue);
    FillTexture(device, encoder, resolved_b, kBlue);

    Metal::RenderPassDesc clear_resolve = ColorPass(msaa_a, true, kYellow);
    clear_resolve.colors[0].resolve_texture = (__bridge void*)resolved_a;
    encoder.BeginRenderPass(clear_resolve);
    encoder.EndRenderPass();

    encoder.BeginRenderPass(ColorPass(msaa_b, true, kRed));
    encoder.EndRenderPass();
    Metal::RenderPassDesc resolve_only = ColorPass(msaa_b, false, 0);
    resolve_only.colors[0].resolve_texture = (__bridge void*)resolved_b;
    encoder.BeginRenderPass(resolve_only);
    encoder.EndRenderPass();

    id<MTLBuffer> a = Readback(device, encoder, resolved_a);
    id<MTLBuffer> b = Readback(device, encoder, resolved_b);
    device.Wait(encoder.Submit());
    Expect(Pixel(a, kSize, 5, 5) == kYellow && Pixel(a, kSize, 63, 63) == kYellow,
           "a 4x MSAA clear + resolve pass resolves");
    Expect(Pixel(b, kSize, 5, 5) == kRed && Pixel(b, kSize, 63, 63) == kRed,
           "a resolve-only pass (no clears, no draws) still resolves");
    Expect(device.ErrorCount() == 0, "resolve passes ran without errors");
}


/// The blit helper's MSL (mtl_blit_shaders.h) through CompileLibrary/CreateRenderPipeline and the
/// encoder, as BlitHelper records it: every program compiles into a pipeline; a 1x RGBA32Float
/// texture copies into every sample of a 4x one, that one copies sample by sample into another
/// 4x one, and its channels become the four samples of a 4x D32S8 depth texture.
void TestBlitShaders() {
    namespace BS = Metal::BlitShaders;
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    const Metal::Object library = Metal::CompileLibrary(device, BS::Source, "blit test");
    Metal::DepthStencilCache depth_states{device};
    const auto pipeline = [&](const char* fragment, MTLPixelFormat color, MTLPixelFormat depth,
                              MTLPixelFormat stencil, u32 samples) {
        return Metal::CreateRenderPipeline(device,
                                           {.library = library.Get(),
                                            .vertex_function = BS::FullscreenTriangle,
                                            .fragment_function = fragment,
                                            .color_format = u32(color),
                                            .depth_format = u32(depth),
                                            .stencil_format = u32(stencil),
                                            .samples = samples},
                                           fragment);
    };
    constexpr MTLPixelFormat flavours[3] = {MTLPixelFormatRGBA32Float, MTLPixelFormatRGBA32Uint,
                                            MTLPixelFormatRGBA32Sint};
    bool all_built = true;
    for (u32 f = 0; f < 3; ++f) {
        all_built &= bool(pipeline(BS::MsImageBlit[f], flavours[f], MTLPixelFormatInvalid,
                                   MTLPixelFormatInvalid, 4));
        all_built &= bool(pipeline(BS::MsImageBlitMsaa[f], flavours[f], MTLPixelFormatInvalid,
                                   MTLPixelFormatInvalid, 1));
    }
    all_built &= bool(pipeline(BS::DepthResample, MTLPixelFormatInvalid, MTLPixelFormatDepth32Float,
                               MTLPixelFormatInvalid, 1));
    all_built &= bool(pipeline(BS::DepthResampleStencil, MTLPixelFormatInvalid,
                               MTLPixelFormatDepth32Float_Stencil8,
                               MTLPixelFormatDepth32Float_Stencil8, 1));
    all_built &= bool(pipeline(BS::DepthResampleStencilBits, MTLPixelFormatInvalid,
                               MTLPixelFormatDepth32Float_Stencil8,
                               MTLPixelFormatDepth32Float_Stencil8, 1));
    Expect(all_built, "every blit program builds a render pipeline");

    constexpr u32 n = 16, samples = 4;
    const auto texture = [&](MTLPixelFormat format, u32 sample_count) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor new];
        d.textureType = sample_count > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
        d.pixelFormat = format;
        d.width = n;
        d.height = n;
        d.sampleCount = sample_count;
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModePrivate;
        return [dev newTextureWithDescriptor:d];
    };
    id<MTLTexture> color = texture(MTLPixelFormatRGBA32Float, 1);
    id<MTLTexture> ms_a = texture(MTLPixelFormatRGBA32Float, samples);
    id<MTLTexture> ms_b = texture(MTLPixelFormatRGBA32Float, samples);
    id<MTLTexture> ms_depth = texture(MTLPixelFormatDepth32Float_Stencil8, samples);

    // Texel (x, y) = (x, y, 0.25, 0.75) / 16: four distinct depths per pixel.
    id<MTLBuffer> pixels = SharedBuffer(device, n * n * 16);
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            float* t = static_cast<float*>(pixels.contents) + (y * n + x) * 4;
            t[0] = float(x) / 16.0f;
            t[1] = float(y) / 16.0f;
            t[2] = 0.25f;
            t[3] = 0.75f;
        }
    }
    encoder.CopyBufferToTexture(Ref(pixels), n * 16, n * n * 16, (__bridge void*)color, 0, 0, {},
                                {n, n, 1});

    Metal::DepthStencilKey always_write{};
    always_write.depth_test = always_write.depth_write = true;
    always_write.depth_compare = 7;
    const auto draw = [&](const Metal::RenderPassDesc& pass, const Metal::Object& pso,
                          void* depth_state, id<MTLTexture> source) {
        encoder.BeginRenderPass(pass);
        encoder.SetRenderPipeline(pso.Get());
        encoder.SetDepthStencil(depth_state);
        encoder.SetCullMode(Metal::CullMode::None);
        encoder.SetViewport({0, 0, float(n), float(n), 0, 1});
        encoder.SetScissor({0, 0, n, n});
        encoder.SetTexture(Metal::Stage::Fragment, 0, (__bridge void*)source);
        encoder.Draw(Metal::Primitive::Triangle, 3);
        encoder.EndRenderPass();
    };
    const Metal::Object copy_1x = pipeline(BS::MsImageBlit[0], MTLPixelFormatRGBA32Float,
                                           MTLPixelFormatInvalid, MTLPixelFormatInvalid, samples);
    const Metal::Object copy_ms = pipeline(BS::MsImageBlitMsaa[0], MTLPixelFormatRGBA32Float,
                                           MTLPixelFormatInvalid, MTLPixelFormatInvalid, samples);
    const Metal::Object to_depth =
        pipeline(BS::ColorToMsDepth, MTLPixelFormatInvalid, MTLPixelFormatDepth32Float_Stencil8,
                 MTLPixelFormatDepth32Float_Stencil8, samples);
    Metal::RenderPassDesc pass{};
    pass.width = pass.height = n;
    pass.colors[0].texture = (__bridge void*)ms_a;
    pass.colors[0].clear = true;
    draw(pass, copy_1x, depth_states.Get({}), color);
    pass.colors[0].texture = (__bridge void*)ms_b;
    draw(pass, copy_ms, depth_states.Get({}), ms_a);
    Metal::RenderPassDesc depth_pass{};
    depth_pass.width = depth_pass.height = n;
    depth_pass.depth.texture = (__bridge void*)ms_depth;
    depth_pass.depth.has_stencil = true;
    depth_pass.depth.clear_depth = true;
    draw(depth_pass, to_depth, depth_states.Get(always_write), color);

    // Every sample of the results, read by kernels.
    NSError* error = nil;
    id<MTLLibrary> readers = [dev newLibraryWithSource:@R"(
#include <metal_stdlib>
using namespace metal;
kernel void read_ms_color(texture2d_ms<float> t [[texture(0)]], device float4* out [[buffer(0)]],
                          uint2 p [[thread_position_in_grid]]) {
    for (uint s = 0; s < 4; ++s) out[(p.y * 16 + p.x) * 4 + s] = t.read(p, s);
}
kernel void read_ms_depth(depth2d_ms<float> t [[texture(0)]], device float* out [[buffer(0)]],
                          uint2 p [[thread_position_in_grid]]) {
    for (uint s = 0; s < 4; ++s) out[(p.y * 16 + p.x) * 4 + s] = t.read(p, s);
}
)" options:nil error:&error];
    if (!readers) {
        std::printf("FAIL: reader kernels: %s\n", error.localizedDescription.UTF8String);
        std::exit(1);
    }
    id<MTLBuffer> out_b = SharedBuffer(device, n * n * samples * 16);
    id<MTLBuffer> out_depth = SharedBuffer(device, n * n * samples * 4);
    id<MTLComputePipelineState> read_color = Kernel(device, readers, @"read_ms_color");
    id<MTLComputePipelineState> read_depth = Kernel(device, readers, @"read_ms_depth");
    encoder.SetComputePipeline((__bridge void*)read_color);
    encoder.SetTexture(Metal::Stage::Compute, 0, (__bridge void*)ms_b);
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out_b));
    encoder.Dispatch({1, 1, 1}, {n, n, 1});
    encoder.SetComputePipeline((__bridge void*)read_depth);
    encoder.SetTexture(Metal::Stage::Compute, 0, (__bridge void*)ms_depth);
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out_depth));
    encoder.Dispatch({1, 1, 1}, {n, n, 1});
    encoder.KeepAlive(Metal::Object::Retain((__bridge void*)pixels));
    device.Wait(encoder.Submit());

    bool copies_ok = true, depth_ok = true;
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            const float expect[4] = {float(x) / 16.0f, float(y) / 16.0f, 0.25f, 0.75f};
            for (u32 s = 0; s < samples; ++s) {
                const float* c = static_cast<const float*>(out_b.contents) + ((y * n + x) * 4 + s) * 4;
                for (u32 ch = 0; ch < 4; ++ch) {
                    copies_ok &= c[ch] == expect[ch];
                }
                const float d = static_cast<const float*>(out_depth.contents)[(y * n + x) * 4 + s];
                depth_ok &= d == expect[s];
            }
        }
    }
    Expect(copies_ok, "1x -> 4x and 4x -> 4x MSAA copies keep every sample");
    Expect(depth_ok, "colour channel N becomes depth sample N (4x D32S8)");
    Expect(device.ErrorCount() == 0, "blit draws ran without errors");
}
} // namespace

/// Two schedulers' timelines on one device: each counts its own commits 1, 2, ... and a tick of
/// one timeline means that timeline's command buffer, whatever the other committed in between.
void TestTimelines() {
    Metal::Device device;
    Metal::Encoder enc_a{device}, enc_b{device};
    Metal::Timeline a{device}, b{device};
    Expect(a.CurrentTick() == 1 && b.CurrentTick() == 1 && a.KnownGpuTick() == 0,
           "timelines start at 1");
    Expect(a.IsFree(0), "tick 0 is always free");
    Expect(!a.IsFree(1), "an uncommitted logical tick is not free");

    id<MTLBuffer> buffer = SharedBuffer(device, 4096);
    enc_a.FillBuffer(Ref(buffer), 4096, 0x01010101u);
    const u64 a1 = a.Committed(enc_a.Submit());
    enc_b.FillBuffer(Ref(buffer), 4096, 0x02020202u);
    const u64 b1 = b.Committed(enc_b.Submit());
    enc_b.FillBuffer(Ref(buffer), 4096, 0x03030303u);
    const u64 b2 = b.Committed(enc_b.Submit());

    // A's second command buffer waits for an event the CPU signals later.
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    id<MTLSharedEvent> gate = [dev newSharedEvent];
    id<MTLCommandBuffer> gated = (__bridge id<MTLCommandBuffer>)enc_a.CommandBuffer();
    [gated encodeWaitForEvent:gate value:1];
    enc_a.FillBuffer(Ref(buffer), 4096, 0x04040404u);
    const u64 a2 = a.Committed(enc_a.Submit());
    Expect(a1 == 1 && a2 == 2 && b1 == 1 && b2 == 2, "each timeline numbers its own commits");
    Expect(a.CurrentTick() == 3 && b.CurrentTick() == 3, "each timeline advances on its own");

    b.Wait(b2);
    Expect(b.IsFree(2) && b.KnownGpuTick() == 2, "B's tick 2 is its own second command buffer");
    a.Wait(a1);
    Expect(a.IsFree(1), "A's tick 1 done");
    Expect(!a.IsFree(2), "A's tick 2 is A's gated command buffer, not B's completed one");
    Expect(a.KnownGpuTick() == 1, "A knows only its own completed ticks");
    Expect(!a.IsFree(3), "A's uncommitted tick is not free");

    std::atomic<bool> waited{false};
    std::thread waiter([&] {
        a.Wait(a2);
        waited.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Expect(!waited.load(), "Wait on A's tick 2 blocks while A's command buffer is gated");
    gate.signaledValue = 1;
    waiter.join();
    Expect(gated.status == MTLCommandBufferStatusCompleted, "Wait returned after A's own buffer");
    Expect(a.IsFree(2) && a.KnownGpuTick() == 2, "A's tick 2 done after the gate opened");

    // Many commits, waited on out of order: pruning keeps every answer right and the map small.
    u64 last_known = a.KnownGpuTick();
    bool monotonic = true;
    for (int i = 0; i < 300; ++i) {
        enc_a.FillBuffer(Ref(buffer), 4096, u32(i & 0xff) * 0x01010101u);
        a.Committed(enc_a.Submit());
        if (i % 3 == 0) {
            enc_b.FillBuffer(Ref(buffer), 4096, 0);
            b.Committed(enc_b.Submit());
        }
        const u64 known = a.KnownGpuTick();
        monotonic = monotonic && known >= last_known;
        last_known = known;
    }
    Expect(monotonic, "KnownGpuTick never goes back");
    Expect(a.CurrentTick() == 303 && b.CurrentTick() == 103, "ticks after many commits");
    a.Wait(150);
    Expect(a.IsFree(150), "a tick in the middle waited for");
    a.Wait(302);
    bool all_free = true;
    for (u64 t = 0; t <= 302; ++t) {
        all_free = all_free && a.IsFree(t);
    }
    Expect(all_free && a.KnownGpuTick() == 302, "every committed tick is free after the last");
    Expect(!a.IsFree(303), "the next tick is still not free");
    Expect(a.PendingCount() == 0, "completed mappings are dropped");
    device.Finish();
    Expect(b.KnownGpuTick() == 102 && b.PendingCount() == 0, "B's mappings pruned too");
}

void TestBufferCreation() {
    Metal::Device device;
    u64 stream_address = 0, private_address = 0;
    u8 *stream_contents = nullptr, *private_contents = nullptr;
    Metal::Object stream = Metal::CreateBuffer(device, 64, VideoCore::MemoryType::Stream, "test",
                                               &stream_address, &stream_contents);
    Metal::Object priv = Metal::CreateBuffer(device, 64, VideoCore::MemoryType::DeviceLocal, "",
                                             &private_address, &private_contents);
    Expect(stream && priv, "buffers created");
    Expect(stream_contents != nullptr, "Stream buffer has CPU contents");
    Expect(private_contents == nullptr, "DeviceLocal buffer has no CPU contents");
    Expect(stream_address != 0 && private_address != 0, "both buffers have a GPU address");
    Expect(([(__bridge id<MTLBuffer>)priv.Get() hazardTrackingMode] ==
            MTLHazardTrackingModeUntracked) != device.HazardTracking(),
           "DeviceLocal buffers are untracked (the Encoder orders them) unless "
           "BB_METAL_HAZARD_TRACKING=1");
    Expect([(__bridge id<MTLBuffer>)stream.Get() hazardTrackingMode] != MTLHazardTrackingModeUntracked,
           "shared buffers stay tracked");
}

void TestTextures() {
    Metal::Device device;
    u64 rgba_size = 0, bc1_size = 0;
    Metal::TextureDesc rgba{};
    rgba.pixel_format = MTLPixelFormatRGBA8Unorm;
    rgba.width = rgba.height = 4;
    rgba.render_target = rgba.shader_write = rgba.format_views = true;
    Metal::Object rgba_tex = Metal::CreateTexture(device, rgba, "rgba", &rgba_size);
    Metal::TextureDesc bc1{};
    bc1.pixel_format = MTLPixelFormatBC1_RGBA;
    bc1.width = bc1.height = 8;
    Metal::Object bc1_tex = Metal::CreateTexture(device, bc1, "", &bc1_size);
    Expect(rgba_tex && bc1_tex, "4x4 RGBA8 and 8x8 BC1 textures created");
    Expect(rgba_size > 0 && bc1_size > 0, "textures report an allocated size");
    if (rgba_tex) {
        id<MTLTexture> t = (__bridge id<MTLTexture>)rgba_tex.Get();
        Expect(t.storageMode == MTLStorageModePrivate, "texture storage is private");
        Expect((t.usage & (MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget |
                           MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView)) ==
                   (MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget |
                    MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView),
               "texture usage follows the desc");
    }
    if (bc1_tex) {
        id<MTLTexture> t = (__bridge id<MTLTexture>)bc1_tex.Get();
        Expect(t.usage == MTLTextureUsageShaderRead, "BC1 texture is shader-read only");
    }
    Expect(Metal::BytesPerBlock(MTLPixelFormatBC1_RGBA) == 8, "BC1 block is 8 bytes");
    Expect(Metal::BlockWidth(MTLPixelFormatBC1_RGBA) == 4 &&
               Metal::BlockHeight(MTLPixelFormatBC1_RGBA) == 4,
           "BC1 block is 4x4");
    Expect(Metal::BytesPerBlock(MTLPixelFormatRGBA8Unorm) == 4 &&
               Metal::BlockWidth(MTLPixelFormatRGBA8Unorm) == 1,
           "RGBA8 texel is 4 bytes");
    Expect(Metal::BytesPerBlock(MTLPixelFormatInvalid) == 0, "unknown format has no size");
    Expect(Metal::StencilViewPixelFormat(MTLPixelFormatDepth32Float_Stencil8) ==
                   MTLPixelFormatX32_Stencil8 &&
               Metal::StencilViewPixelFormat(MTLPixelFormatDepth32Float) == 0,
           "stencil view formats");

    Metal::Object view = Metal::CreateTextureView(
        rgba_tex.Get(), MTLPixelFormatRGBA8Unorm_sRGB, Metal::TextureType::Tex2D, 0, 1, 0, 1,
        {MTLTextureSwizzleBlue, MTLTextureSwizzleGreen, MTLTextureSwizzleRed,
         MTLTextureSwizzleOne});
    Expect(bool(view), "swizzled sRGB view created");
    if (view) {
        id<MTLTexture> v = (__bridge id<MTLTexture>)view.Get();
        Expect(v.swizzle.red == MTLTextureSwizzleBlue && v.swizzle.alpha == MTLTextureSwizzleOne,
               "view carries the swizzle");
        Expect(v.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB, "view has the requested format");
    }

    Metal::SamplerDesc sd{};
    sd.min_filter = sd.mag_filter = MTLSamplerMinMagFilterLinear;
    sd.mip_filter = MTLSamplerMipFilterLinear;
    sd.address_u = sd.address_v = sd.address_w = MTLSamplerAddressModeClampToEdge;
    sd.max_lod = 4.0f;
    sd.max_anisotropy = 64; // clamped to 16
    sd.compare = true;
    sd.compare_function = MTLCompareFunctionLessEqual;
    Metal::Object sampler = Metal::CreateSampler(device, sd);
    Expect(bool(sampler), "sampler created");
    sd.lod_bias = -0.5f;
    Metal::Object biased = Metal::CreateSampler(device, sd);
    Expect(bool(biased), "sampler with a LOD bias created");
    Expect(Metal::SupportsSampleCount(device, 1) && Metal::SupportsSampleCount(device, 4),
           "1x and 4x samples supported");
}

void TestIntegerPixelFormats() {
    Expect(Metal::IsIntegerPixelFormat(MTLPixelFormatRGBA8Uint), "RGBA8Uint is integer");
    Expect(Metal::IsIntegerPixelFormat(MTLPixelFormatR32Sint), "R32Sint is integer");
    Expect(!Metal::IsIntegerPixelFormat(MTLPixelFormatRGBA8Unorm), "RGBA8Unorm is not integer");
    Expect(!Metal::IsIntegerPixelFormat(MTLPixelFormatRGBA16Float), "RGBA16Float is not integer");
}

void TestSafeViewFormats() {
    using Metal::SafeViewPixelFormat;
    Expect(SafeViewPixelFormat(MTLPixelFormatBC1_RGBA, MTLPixelFormatRG32Uint, false) ==
               MTLPixelFormatBC1_RGBA,
           "BC1 viewed as RG32Uint falls back to BC1");
    Expect(SafeViewPixelFormat(MTLPixelFormatBC1_RGBA, MTLPixelFormatBC4_RUnorm, false) ==
               MTLPixelFormatBC1_RGBA,
           "BC1 viewed as BC4 falls back to BC1");
    Expect(SafeViewPixelFormat(MTLPixelFormatBC1_RGBA, MTLPixelFormatBC1_RGBA_sRGB, false) ==
               MTLPixelFormatBC1_RGBA_sRGB,
           "BC1 viewed as its sRGB twin");
    Expect(SafeViewPixelFormat(MTLPixelFormatDepth32Float_Stencil8, MTLPixelFormatX32_Stencil8,
                               true) == MTLPixelFormatX32_Stencil8,
           "D32S8 viewed as X32_Stencil8");
    Expect(SafeViewPixelFormat(MTLPixelFormatDepth32Float, MTLPixelFormatR32Float, true) ==
               MTLPixelFormatDepth32Float,
           "D32 viewed as R32Float falls back to D32");
    Expect(SafeViewPixelFormat(MTLPixelFormatDepth32Float, MTLPixelFormatX32_Stencil8, true) ==
               MTLPixelFormatDepth32Float,
           "D32 has no stencil view");
    Expect(SafeViewPixelFormat(MTLPixelFormatRGBA8Unorm, MTLPixelFormatR32Uint, true) ==
               MTLPixelFormatR32Uint,
           "RGBA8 with format views viewed as R32Uint");
    Expect(SafeViewPixelFormat(MTLPixelFormatRGBA8Unorm, MTLPixelFormatR32Uint, false) ==
               MTLPixelFormatRGBA8Unorm,
           "RGBA8 without format views keeps its format");
    Expect(SafeViewPixelFormat(MTLPixelFormatRGBA8Unorm, MTLPixelFormatRG32Uint, true) ==
               MTLPixelFormatRGBA8Unorm,
           "different texel sizes fall back");
    Expect(SafeViewPixelFormat(MTLPixelFormatRGBA8Unorm, MTLPixelFormatDepth32Float, true) ==
               MTLPixelFormatRGBA8Unorm,
           "colour textures have no depth views");

    // One real view per allowed case: none may abort.
    Metal::Device device;
    const auto make = [&](u32 format, u32 w, bool views) {
        Metal::TextureDesc d{};
        d.pixel_format = format;
        d.width = d.height = w;
        d.format_views = views;
        d.render_target = views;
        return Metal::CreateTexture(device, d, "", nullptr);
    };
    const auto view_ok = [&](const Metal::Object& tex, u32 requested, u32 texture_format,
                             bool views) {
        const u32 format = SafeViewPixelFormat(texture_format, requested, views);
        return bool(Metal::CreateTextureView(tex.Get(), format, Metal::TextureType::Tex2D, 0, 1,
                                             0, 1,
                                             {MTLTextureSwizzleRed, MTLTextureSwizzleGreen,
                                              MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha}));
    };
    Metal::Object bc1 = make(MTLPixelFormatBC1_RGBA, 8, false);
    Metal::Object d32s8 = make(MTLPixelFormatDepth32Float_Stencil8, 4, true);
    Metal::Object d32 = make(MTLPixelFormatDepth32Float, 4, true);
    Metal::Object rgba = make(MTLPixelFormatRGBA8Unorm, 4, true);
    Expect(bc1 && d32s8 && d32 && rgba, "view-format textures created");
    Expect(view_ok(bc1, MTLPixelFormatRG32Uint, MTLPixelFormatBC1_RGBA, false) &&
               view_ok(bc1, MTLPixelFormatBC4_RUnorm, MTLPixelFormatBC1_RGBA, false) &&
               view_ok(bc1, MTLPixelFormatBC1_RGBA_sRGB, MTLPixelFormatBC1_RGBA, false),
           "BC1 views");
    Expect(view_ok(d32s8, MTLPixelFormatX32_Stencil8, MTLPixelFormatDepth32Float_Stencil8, true),
           "D32S8 stencil view");
    Expect(view_ok(d32, MTLPixelFormatR32Float, MTLPixelFormatDepth32Float, true), "D32 view");
    Expect(view_ok(rgba, MTLPixelFormatR32Uint, MTLPixelFormatRGBA8Unorm, true), "RGBA8 view");
}

/// The GC measures images as AllocatedSize() - ResidentBytes(): the private guest arena is
/// allocated in full at once and must not count as texture memory.
void TestResidentBytes() {
    Metal::Device device;
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    const u64 before = device.ResidentBytes();
    id<MTLBuffer> arena = [dev newBufferWithLength:64ull << 20 options:MTLResourceStorageModePrivate];
    device.MakeResident((__bridge void*)arena);
    Expect(device.ResidentBytes() == before + (64ull << 20),
           "MakeResident counts the buffer's length in ResidentBytes");
    device.MakeResident((__bridge void*)arena);
    Expect(device.ResidentBytes() == before + (64ull << 20), "a buffer counts once");
    device.Evict((__bridge void*)arena);
    Expect(device.ResidentBytes() == before, "Evict takes the buffer's length off again");
}

/// Metal aborts on zero-length blits under MTL_DEBUG_LAYER; guest sizes can be 0.
void TestZeroSizeBlits() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLBuffer> a = SharedBuffer(device, 64), b = SharedBuffer(device, 64);
    encoder.FillBuffer(Ref(b), 0, 0x7f7f7f7fu);
    encoder.FillBuffer(Ref(b), 0, 0x0000000Fu);
    encoder.CopyBuffer(Ref(a), Ref(b), 0);
    device.Wait(encoder.Submit());
    const u8* d = static_cast<const u8*>(b.contents);
    Expect(std::all_of(d, d + 64, [](u8 v) { return v == 0; }), "size-0 fill and copy do nothing");
    Expect(device.ErrorCount() == 0, "size-0 blits ran without errors");
}

/// HTile fills come with guest sizes: an unaligned size fills the whole words and never aborts.
void TestUnalignedPatternFill() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLBuffer> b = SharedBuffer(device, 64);
    std::memset(b.contents, 0xee, 64);
    encoder.FillBuffer(Ref(b), 30, 0x0000000Fu);
    encoder.FillBuffer(Ref(b, 34), 8, 0x0000000Fu); // unaligned offset: skipped
    device.Wait(encoder.Submit());
    const u32* w = static_cast<const u32*>(b.contents);
    const u8* d = static_cast<const u8*>(b.contents);
    bool ok = true;
    for (int i = 0; i < 7; ++i) ok &= w[i] == 0x0000000Fu;
    Expect(ok, "a size-30 pattern fill fills 28 bytes");
    Expect(d[28] == 0xee && d[29] == 0xee, "bytes 28-29 are untouched");
    Expect(std::all_of(d + 30, d + 64, [](u8 v) { return v == 0xee; }),
           "a fill at an unaligned offset is skipped");
    Expect(device.ErrorCount() == 0, "unaligned pattern fills ran without errors");
}

/// GPU-side ordering across schedulers: B (the presenter, say) waits for the device tick of A's
/// frame. A must be committed first: a wait for a later commit on the one queue never ends (the
/// queue starts command buffers in commit order; measured: GPU timeout error).
void TestCrossEncoderWait() {
    Metal::Device device;
    Metal::Encoder enc_a{device}, enc_b{device};
    Metal::Timeline timeline_a{device};
    Expect(!timeline_a.DeviceTickOf(1).has_value(), "an uncommitted tick has no device tick");
    Expect(timeline_a.DeviceTickOf(0) == u64(0), "tick 0 maps to 0 (already complete)");

    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    id<MTLSharedEvent> gate = [dev newSharedEvent];
    id<MTLBuffer> src = SharedBuffer(device, 256), dst = SharedBuffer(device, 256);
    std::atomic<int> order{0}, a_done{0}, b_done{0};
    std::atomic<int>* order_p = &order;
    std::atomic<int>* a_done_p = &a_done;
    std::atomic<int>* b_done_p = &b_done;

    // A's frame: held by a CPU-signalled gate so it is still pending while B is recorded.
    [(__bridge id<MTLCommandBuffer>)enc_a.CommandBuffer() encodeWaitForEvent:gate value:1];
    enc_a.FillBuffer(Ref(src), 256, 0x3c3c3c3cu);
    [(__bridge id<MTLCommandBuffer>)enc_a.CommandBuffer() addCompletedHandler:^(id<MTLCommandBuffer>) {
        a_done_p->store(++*order_p);
    }];
    const u64 a_device = enc_a.Submit();
    const u64 a_logical = timeline_a.Committed(a_device);
    const std::optional<u64> wait_tick = timeline_a.DeviceTickOf(a_logical);
    Expect(wait_tick == a_device, "a pending logical tick maps to the device tick it signals");

    enc_b.WaitForDeviceTick(0); // no-op
    enc_b.WaitForDeviceTick(wait_tick.value_or(0));
    enc_b.CopyBuffer(Ref(src), Ref(dst), 256);
    [(__bridge id<MTLCommandBuffer>)enc_b.CommandBuffer() addCompletedHandler:^(id<MTLCommandBuffer>) {
        b_done_p->store(++*order_p);
    }];
    const u64 b_device = enc_b.Submit();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Expect(b_done.load() == 0 && a_done.load() == 0, "B does not complete while A is held");

    gate.signaledValue = 1;
    device.Wait(b_device);
    for (int i = 0; i < 100 && (a_done == 0 || b_done == 0); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const u8* d = static_cast<const u8*>(dst.contents);
    Expect(std::all_of(d, d + 256, [](u8 v) { return v == 0x3c; }), "B's copy sees what A wrote");
    Expect(a_done.load() == 1 && b_done.load() == 2, "A completes before B");
    Expect(timeline_a.DeviceTickOf(a_logical) == u64(0),
           "a completed logical tick maps to 0 (already complete)");
    Expect(device.ErrorCount() == 0, "cross-encoder wait ran without errors");
}


// ---- Task M3.1: encoder capabilities for the rasterizer ----

constexpr const char* kRasterShaders = R"(
#include <metal_stdlib>
using namespace metal;

struct VOut { float4 pos [[position]]; };
struct VColorOut { float4 pos [[position]]; float4 color; };
struct StrideIn { float2 pos [[attribute(0)]]; float4 color [[attribute(1)]]; };

vertex VColorOut strided(StrideIn in [[stage_in]]) {
    VColorOut o;
    o.pos = float4(in.pos, 0.0, 1.0);
    o.color = in.color;
    return o;
}
fragment float4 vcolor(VColorOut in [[stage_in]]) { return in.color; }

// 6 vertices per quad, quad q covers quadrant (q & 1, q >> 1) of clip space.
vertex VOut quarters(uint vid [[vertex_id]], constant uint* indices [[buffer(1)]]) {
    const uint v = vid;
    const uint q = v / 6, c = v % 6;
    const float2 corner[6] = {float2(0, 0), float2(1, 0), float2(0, 1),
                              float2(0, 1), float2(1, 0), float2(1, 1)};
    const float2 base = float2(float(q & 1), float(q >> 1)) - 1.0;
    VOut o;
    o.pos = float4(base + corner[c], 0.0, 1.0);
    return o;
}

// A triangle in the middle of the target at depth z (buffer 1).
vertex VOut small_triangle(uint vid [[vertex_id]], constant float& z [[buffer(1)]]) {
    const float2 p[3] = {float2(-0.8, -0.8), float2(0.8, -0.8), float2(0.0, 0.8)};
    VOut o;
    o.pos = float4(p[vid], z, 1.0);
    return o;
}

fragment float4 solid(constant float4& color [[buffer(0)]]) { return color; }

fragment float4 tex100(VOut in [[stage_in]], texture2d<float> t [[texture(100)]]) {
    constexpr sampler s(filter::nearest, coord::pixel);
    return t.sample(s, in.pos.xy);
}

// The left half of clip space as one tessellated patch.
[[patch(quad, 4)]] vertex VOut post_tess_quad(float2 uv [[position_in_patch]]) {
    VOut o;
    o.pos = float4(uv.x - 1.0, uv.y * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

kernel void doubles(device uint* out [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    out[i] = i * 2u;
}
kernel void add_one(device uint* out [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    out[i] += 1u;
}
)";

id<MTLLibrary> RasterLibrary(Metal::Device& device) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    NSError* error = nil;
    id<MTLLibrary> library = [dev newLibraryWithSource:@(kRasterShaders) options:nil error:&error];
    if (!library) {
        std::printf("FAIL: raster shaders: %s\n", error.localizedDescription.UTF8String);
        std::exit(1);
    }
    return library;
}

id<MTLRenderPipelineState> RasterPipeline(Metal::Device& device, id<MTLLibrary> library,
                                          NSString* vertex, NSString* fragment,
                                          MTLRenderPipelineDescriptor* desc = nil) {
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
        std::printf("FAIL: raster pipeline %s: %s\n", vertex.UTF8String,
                    error.localizedDescription.UTF8String);
        std::exit(1);
    }
    return pipeline;
}

void FullViewport(Metal::Encoder& encoder) {
    encoder.SetViewport({0, 0, float(kSize), float(kSize), 0, 1});
    encoder.SetScissor({0, 0, kSize, kSize});
}

/// Object motion's history: vertex shaders write a buffer they reach by GPU address and the next
/// frame's vertex shaders read it, in another render encoder of a later command buffer. Declared
/// with IndirectUse::VertexWrite, hazard tracking orders the two; RemoveIndirect lets the buffer go.
void TestIndirectVertexWrite() {
    static const char* kSource = R"(
#include <metal_stdlib>
using namespace metal;
struct PointOut { float4 position [[position]]; float size [[point_size]]; };
vertex PointOut store_by_address(uint vid [[vertex_id]], constant ulong& address [[buffer(1)]]) {
    ((device uint*)address)[vid] = 500 + vid;
    return {float4(0, 0, 0, 1), 1};
}
vertex PointOut load_by_address(uint vid [[vertex_id]], constant ulong& address [[buffer(1)]],
                                device uint* out [[buffer(2)]]) {
    out[vid] = ((device const uint*)address)[vid];
    return {float4(0, 0, 0, 1), 1};
}
fragment float4 white() { return float4(1); }
)";
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    NSError* error = nil;
    id<MTLLibrary> library = [dev newLibraryWithSource:@(kSource) options:nil error:&error];
    if (!library) {
        std::printf("FAIL: vertex-write shaders: %s\n", error.localizedDescription.UTF8String);
        ++failures;
        return;
    }
    id<MTLRenderPipelineState> store = RasterPipeline(device, library, @"store_by_address", @"white");
    id<MTLRenderPipelineState> load = RasterPipeline(device, library, @"load_by_address", @"white");
    constexpr u32 kCount = 4096;
    id<MTLBuffer> history = [dev newBufferWithLength:kCount * 4 options:MTLResourceStorageModePrivate];
    id<MTLBuffer> out = SharedBuffer(device, kCount * 4);
    id<MTLTexture> target = Texture(device, kSize, kSize);
    const u64 address = history.gpuAddress;
    device.MakeResident((__bridge void*)history);
    encoder.DeclareIndirect((__bridge void*)history, Metal::IndirectUse::VertexWrite);
    // Frame 1 stores; frame 2 (next command buffer, not waited for) loads.
    for (id<MTLRenderPipelineState> pipeline : {store, load}) {
        encoder.BeginRenderPass(ColorPass(target, true, 0));
        FullViewport(encoder);
        encoder.SetRenderPipeline((__bridge void*)pipeline);
        encoder.SetBytes(Metal::Stage::Vertex, 1, &address, sizeof(address));
        encoder.SetBuffer(Metal::Stage::Vertex, 2, Ref(out));
        encoder.Draw(Metal::Primitive::Point, kCount);
        encoder.EndRenderPass();
        encoder.Submit();
    }
    device.Finish();
    bool all = true;
    for (u32 i = 0; i < kCount; ++i) {
        all &= static_cast<const u32*>(out.contents)[i] == 500 + i;
    }
    Expect(all, "vertex shaders read what the previous frame's vertex shaders wrote by address");
    // Undeclared and released: a new encoder must not reference it.
    encoder.RemoveIndirect((__bridge void*)history);
    device.Evict((__bridge void*)history);
    history = nil;
    encoder.BeginRenderPass(ColorPass(target, true, 0));
    encoder.EndRenderPass();
    device.Wait(encoder.Submit());
    Expect(device.ErrorCount() == 0, "encoders after RemoveIndirect run without the buffer");
}

void TestVertexBufferStride() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLLibrary> library = RasterLibrary(device);
    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat2;
    vd.attributes[0].offset = 0;
    vd.attributes[0].bufferIndex = 3;
    vd.attributes[1].format = MTLVertexFormatFloat4;
    vd.attributes[1].offset = 16;
    vd.attributes[1].bufferIndex = 3;
    vd.layouts[3].stride = MTLBufferLayoutStrideDynamic;
    desc.vertexDescriptor = vd;
    id<MTLRenderPipelineState> pipeline = RasterPipeline(device, library, @"strided", @"vcolor", desc);
    // Three vertices, 32 bytes apart: position at +0, red at +16 (the rest is padding).
    const float pos[3][2] = {{-1, -1}, {3, -1}, {-1, 3}};
    id<MTLBuffer> vertices = SharedBuffer(device, 3 * 32);
    for (u32 i = 0; i < 3; ++i) {
        float* v = reinterpret_cast<float*>(static_cast<u8*>(vertices.contents) + i * 32);
        v[0] = pos[i][0];
        v[1] = pos[i][1];
        v[4] = 1; v[5] = 0; v[6] = 0; v[7] = 1;
        v[2] = v[3] = 9; // padding that a 16-byte stride would read as vertex data
    }
    id<MTLTexture> target = Texture(device, kSize, kSize);
    encoder.BeginRenderPass(ColorPass(target, true, kBlack));
    encoder.SetRenderPipeline((__bridge void*)pipeline);
    FullViewport(encoder);
    encoder.SetVertexBuffer(3, Ref(vertices), 32);
    encoder.Draw(Metal::Primitive::Triangle, 3);
    encoder.EndRenderPass();
    id<MTLBuffer> out = Readback(device, encoder, target);
    device.Wait(encoder.Submit());
    Expect(Pixel(out, kSize, 5, 5) == kRed && Pixel(out, kSize, 60, 60) == kRed,
           "SetVertexBuffer: attributes are read with the given stride");
    // The same object and offset, first at stride 32 (a degenerate triangle),
    // then stride 64 (fullscreen). Stride is part of the binding identity.
    id<MTLBuffer> strided = SharedBuffer(device, 3 * 64);
    for (u32 i = 0; i < 3; ++i) {
        float* v = reinterpret_cast<float*>(static_cast<u8*>(strided.contents) + i * 64);
        v[0] = pos[i][0]; v[1] = pos[i][1];
        v[4] = 1; v[7] = 1;
    }
    float* middle = reinterpret_cast<float*>(static_cast<u8*>(strided.contents) + 32);
    middle[0] = 3; middle[1] = -1; middle[4] = 1; middle[7] = 1;
    encoder.BeginRenderPass(ColorPass(target, true, kBlack));
    encoder.SetRenderPipeline((__bridge void*)pipeline);
    FullViewport(encoder);
    encoder.SetScissor({0, 0, kSize / 2, kSize});
    encoder.SetVertexBuffer(3, Ref(strided), 32);
    encoder.Draw(Metal::Primitive::Triangle, 3);
    encoder.SetScissor({kSize / 2, 0, kSize / 2, kSize});
    encoder.SetVertexBuffer(3, Ref(strided), 64);
    encoder.SetVertexBuffer(3, Ref(strided), 64);
    encoder.Draw(Metal::Primitive::Triangle, 3);
    encoder.EndRenderPass();
    out = Readback(device, encoder, target);
    device.Wait(encoder.Submit());
    Expect(Pixel(out, kSize, 5, 5) == kBlack && Pixel(out, kSize, 60, 60) == kRed,
           "same buffer/offset with changed or repeated dynamic stride keeps the right vertices");
    Expect(device.ErrorCount() == 0, "strided vertex buffer ran without errors");
}

void TestIndirectDraw() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLLibrary> library = TestLibrary(device);
    id<MTLRenderPipelineState> solid = SolidPipeline(device, library, @"fullscreen");
    id<MTLRenderPipelineState> quad = SolidPipeline(device, library, @"indexed_quad");
    id<MTLTexture> target = Texture(device, kSize, kSize);
    // MTLDrawPrimitivesIndirectArguments at offset 16 of a shared buffer.
    id<MTLBuffer> args = SharedBuffer(device, 64);
    const u32 record[4] = {3, 1, 0, 0};
    std::memcpy(static_cast<u8*>(args.contents) + 16, record, sizeof(record));
    // MTLDrawIndexedPrimitivesIndirectArguments: six indices of the left-half quad.
    id<MTLBuffer> indexed_args = SharedBuffer(device, 32);
    const u32 indexed_record[5] = {6, 1, 0, 0, 0};
    std::memcpy(indexed_args.contents, indexed_record, sizeof(indexed_record));
    const float corners[8] = {-1, -1, 0, -1, -1, 1, 0, 1};
    id<MTLBuffer> indices = SharedBuffer(device, 12);
    const u16 index_data[6] = {0, 1, 2, 2, 1, 3};
    std::memcpy(indices.contents, index_data, sizeof(index_data));
    id<MTLTexture> target2 = Texture(device, kSize, kSize);

    encoder.BeginRenderPass(ColorPass(target, true, kBlack));
    encoder.SetRenderPipeline((__bridge void*)solid);
    FullViewport(encoder);
    SetColor(encoder, kGreen);
    encoder.DrawIndirect(Metal::Primitive::Triangle, Ref(args, 16));
    encoder.EndRenderPass();
    encoder.BeginRenderPass(ColorPass(target2, true, kBlack));
    encoder.SetRenderPipeline((__bridge void*)quad);
    FullViewport(encoder);
    encoder.SetBytes(Metal::Stage::Vertex, 0, corners, sizeof(corners));
    SetColor(encoder, kBlue);
    encoder.DrawIndexedIndirect(Metal::Primitive::Triangle, Metal::IndexType::U16, Ref(indices),
                                Ref(indexed_args));
    encoder.EndRenderPass();
    id<MTLBuffer> out = Readback(device, encoder, target);
    id<MTLBuffer> out2 = Readback(device, encoder, target2);
    device.Wait(encoder.Submit());
    Expect(Pixel(out, kSize, 10, 10) == kGreen && Pixel(out, kSize, 50, 50) == kGreen,
           "DrawIndirect draws the triangle the GPU-side arguments describe");
    Expect(Pixel(out2, kSize, 2, kSize / 2) == kBlue && Pixel(out2, kSize, kSize - 2, 2) == kBlack,
           "DrawIndexedIndirect draws the left half");
    Expect(device.ErrorCount() == 0, "indirect draws ran without errors");
}

/// Four records at `stride` bytes, record i drawing quad i; count 2 draws quads 0 and 1 only.
void RunCountClamp(bool indexed) {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLLibrary> library = RasterLibrary(device);
    id<MTLRenderPipelineState> pipeline = RasterPipeline(device, library, @"quarters", @"solid");
    id<MTLComputePipelineState> add_one = Kernel(device, library, @"add_one");
    id<MTLTexture> target = Texture(device, kSize, kSize);
    constexpr u32 stride = 32;
    id<MTLBuffer> args = SharedBuffer(device, 8 + 4 * stride);
    for (u32 i = 0; i < 4; ++i) {
        u32* r = reinterpret_cast<u32*>(static_cast<u8*>(args.contents) + 8 + i * stride);
        if (indexed) {
            const u32 rec[5] = {6, 1, 6 * i, 0, 0};
            std::memcpy(r, rec, sizeof(rec));
        } else {
            const u32 rec[4] = {6, 1, 6 * i, 0};
            std::memcpy(r, rec, sizeof(rec));
        }
    }
    id<MTLBuffer> count = SharedBuffer(device, 64);
    *reinterpret_cast<u32*>(static_cast<u8*>(count.contents) + 20) = 2;
    id<MTLBuffer> indices = SharedBuffer(device, 24 * 4);
    for (u32 i = 0; i < 24; ++i) {
        reinterpret_cast<u32*>(indices.contents)[i] = i;
    }
    id<MTLBuffer> counter = SharedBuffer(device, 16 * 4);
    id<MTLBuffer> count_all = SharedBuffer(device, 64);
    *reinterpret_cast<u32*>(static_cast<u8*>(count_all.contents) + 20) = 4;
    const std::vector<u8> args_before(static_cast<u8*>(args.contents),
                                      static_cast<u8*>(args.contents) + args.length);

    // The caller's compute state must survive: set it, run a dispatch, clamp-draw, dispatch again.
    encoder.SetComputePipeline((__bridge void*)add_one);
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(counter));
    encoder.Dispatch({1, 1, 1}, {16, 1, 1});
    encoder.BeginRenderPass(ColorPass(target, true, kBlack));
    encoder.SetRenderPipeline((__bridge void*)pipeline);
    FullViewport(encoder);
    SetColor(encoder, kRed);
    if (indexed) {
        encoder.SetBuffer(Metal::Stage::Vertex, 1, Ref(indices)); // unused by the shader
        encoder.DrawIndirectCount(Metal::Primitive::Triangle, Ref(args, 8), stride, Ref(count, 20), 4,
                                  true, Metal::IndexType::U32, Ref(indices));
    } else {
        encoder.DrawIndirectCount(Metal::Primitive::Triangle, Ref(args, 8), stride, Ref(count, 20), 4,
                                  false);
    }
    // Same argument table again with a bigger count (green): a clamp that had modified the
    // records would leave quads 2 and 3 empty.
    id<MTLTexture> target2 = Texture(device, kSize, kSize);
    encoder.EndRenderPass();
    id<MTLBuffer> out = Readback(device, encoder, target);
    encoder.BeginRenderPass(ColorPass(target2, true, kBlack));
    encoder.SetRenderPipeline((__bridge void*)pipeline);
    FullViewport(encoder);
    SetColor(encoder, kGreen);
    if (indexed) {
        encoder.DrawIndirectCount(Metal::Primitive::Triangle, Ref(args, 8), stride, Ref(count_all, 20),
                                  4, true, Metal::IndexType::U32, Ref(indices));
    } else {
        encoder.DrawIndirectCount(Metal::Primitive::Triangle, Ref(args, 8), stride,
                                  Ref(count_all, 20), 4, false);
    }
    encoder.EndRenderPass();
    encoder.Dispatch({1, 1, 1}, {16, 1, 1});
    id<MTLBuffer> out2 = Readback(device, encoder, target2);
    device.Wait(encoder.Submit());
    Expect(std::memcmp(args_before.data(), args.contents, args.length) == 0,
           "DrawIndirectCount leaves the argument records untouched");
    bool all_green = true;
    for (u32 q = 0; q < 4; ++q) {
        all_green &= Pixel(out2, kSize, (q & 1) * 32 + 16, (q >> 1) == 0 ? 48 : 16) == kGreen;
    }
    Expect(all_green, "the same records drawn later with a bigger count all draw");

    // Quad q covers x in [(q&1)*32, +32), and rows: NDC y -1..0 is the bottom half (rows 32..63).
    const auto quarter = [&](u32 q) {
        return Pixel(out, kSize, (q & 1) * 32 + 16, (q >> 1) == 0 ? 48 : 16);
    };
    const char* what = indexed ? "indexed: records below the count draw"
                               : "records below the count draw";
    Expect(quarter(0) == kRed && quarter(1) == kRed, what);
    Expect(quarter(2) == kBlack && quarter(3) == kBlack,
           indexed ? "indexed: records at or above the count draw nothing"
                   : "records at or above the count draw nothing");
    Expect(static_cast<const u32*>(counter.contents)[0] == 2 &&
               static_cast<const u32*>(counter.contents)[15] == 2,
           "the caller's compute state survives DrawIndirectCount");
    Expect(device.ErrorCount() == 0, "count-clamped draws ran without errors");
}

void TestIndirectCountClamp() {
    RunCountClamp(false);
    RunCountClamp(true);
}

void TestDispatchIndirect() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLLibrary> library = RasterLibrary(device);
    id<MTLComputePipelineState> doubles = Kernel(device, library, @"doubles");
    id<MTLBuffer> out = SharedBuffer(device, 128 * 4);
    id<MTLBuffer> args = SharedBuffer(device, 32);
    const u32 groups[3] = {4, 1, 1};
    std::memcpy(static_cast<u8*>(args.contents) + 12, groups, sizeof(groups));
    encoder.SetComputePipeline((__bridge void*)doubles);
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(out));
    encoder.DispatchIndirect(Ref(args, 12), {16, 1, 1});
    device.Wait(encoder.Submit());
    bool ok = true;
    for (u32 i = 0; i < 64; ++i) {
        ok &= static_cast<const u32*>(out.contents)[i] == i * 2;
    }
    Expect(ok, "DispatchIndirect runs the GPU-side group count");
    Expect(static_cast<const u32*>(out.contents)[64] == 0, "and no more groups than that");
    Expect(device.ErrorCount() == 0, "indirect dispatch ran without errors");
}

void TestFillModeAndDepthClip() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLLibrary> library = RasterLibrary(device);
    id<MTLRenderPipelineState> triangle = RasterPipeline(device, library, @"small_triangle", @"solid");
    id<MTLBuffer> scratch = SharedBuffer(device, 16);
    id<MTLTexture> lines = Texture(device, kSize, kSize);
    id<MTLTexture> filled = Texture(device, kSize, kSize);
    id<MTLTexture> clipped = Texture(device, kSize, kSize);
    id<MTLTexture> clamped = Texture(device, kSize, kSize);
    const auto draw = [&](id<MTLTexture> t, Metal::FillMode fill, Metal::DepthClip clip, float z,
                          bool interrupt) {
        encoder.BeginRenderPass(ColorPass(t, true, kBlack));
        encoder.SetRenderPipeline((__bridge void*)triangle);
        FullViewport(encoder);
        encoder.SetBytes(Metal::Stage::Vertex, 1, &z, sizeof(z));
        SetColor(encoder, kGreen);
        encoder.SetFillMode(fill);
        encoder.SetDepthClip(clip);
        encoder.Draw(Metal::Primitive::Triangle, 3);
        if (interrupt) {
            encoder.FillBuffer(Ref(scratch), 16, 0); // the reopened encoder must get the modes again
            encoder.Draw(Metal::Primitive::Triangle, 3);
        }
        encoder.EndRenderPass();
    };
    draw(lines, Metal::FillMode::Lines, Metal::DepthClip::Clip, 0.5f, true);
    draw(filled, Metal::FillMode::Fill, Metal::DepthClip::Clip, 0.5f, false);
    draw(clipped, Metal::FillMode::Fill, Metal::DepthClip::Clip, 1.5f, false);
    draw(clamped, Metal::FillMode::Fill, Metal::DepthClip::Clamp, 1.5f, false);
    id<MTLBuffer> a = Readback(device, encoder, lines);
    id<MTLBuffer> b = Readback(device, encoder, filled);
    id<MTLBuffer> c = Readback(device, encoder, clipped);
    id<MTLBuffer> d = Readback(device, encoder, clamped);
    device.Wait(encoder.Submit());
    bool edge = false;
    for (u32 y = 54; y < 62; ++y) {
        edge |= Pixel(a, kSize, kSize / 2, y) == kGreen; // the bottom edge, y = -0.8 in clip space
    }
    Expect(edge, "FillMode::Lines draws the edges");
    Expect(Pixel(a, kSize, kSize / 2, 40) == kBlack, "FillMode::Lines leaves the interior unfilled");
    Expect(Pixel(b, kSize, kSize / 2, 40) == kGreen, "FillMode::Fill fills the interior");
    Expect(Pixel(c, kSize, kSize / 2, 40) == kBlack, "DepthClip::Clip drops a triangle beyond the far plane");
    Expect(Pixel(d, kSize, kSize / 2, 40) == kGreen, "DepthClip::Clamp keeps it");
    Expect(device.ErrorCount() == 0, "fill and depth-clip modes ran without errors");
}

void TestDrawPatches() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLLibrary> library = RasterLibrary(device);
    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.maxTessellationFactor = 1;
    desc.tessellationPartitionMode = MTLTessellationPartitionModeInteger;
    desc.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionConstant;
    desc.tessellationOutputWindingOrder = MTLWindingCounterClockwise;
    desc.tessellationControlPointIndexType = MTLTessellationControlPointIndexTypeNone;
    id<MTLRenderPipelineState> pipeline = RasterPipeline(device, library, @"post_tess_quad", @"solid", desc);
    // Factors for one quad patch: four edges and two inside factors, all 1.0 (half).
    id<MTLBuffer> factors = SharedBuffer(device, 64);
    u16* f = reinterpret_cast<u16*>(static_cast<u8*>(factors.contents) + 16);
    for (u32 i = 0; i < 6; ++i) {
        f[i] = 0x3c00;
    }
    id<MTLTexture> target = Texture(device, kSize, kSize);
    encoder.BeginRenderPass(ColorPass(target, true, kBlack));
    encoder.SetRenderPipeline((__bridge void*)pipeline);
    FullViewport(encoder);
    SetColor(encoder, kBlue);
    encoder.SetTessellationFactorBuffer(Ref(factors, 16), 0);
    encoder.DrawPatches(4, 0, 1, {}, 1, 0);
    encoder.EndRenderPass();
    id<MTLBuffer> out = Readback(device, encoder, target);
    device.Wait(encoder.Submit());
    Expect(Pixel(out, kSize, 3, 3) == kBlue && Pixel(out, kSize, 28, 60) == kBlue &&
               Pixel(out, kSize, 16, 32) == kBlue,
           "DrawPatches fills the tessellated quad");
    Expect(Pixel(out, kSize, kSize - 3, kSize / 2) == kBlack, "and nothing outside it");
    Expect(device.ErrorCount() == 0, "patch draw ran without errors");
}

void TestTextureSlot100() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLLibrary> library = RasterLibrary(device);
    id<MTLLibrary> basic = TestLibrary(device);
    // Vertex "fullscreen" lives in the basic library; build the pipeline by hand.
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = [basic newFunctionWithName:@"fullscreen"];
    desc.fragmentFunction = [library newFunctionWithName:@"tex100"];
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    NSError* error = nil;
    id<MTLRenderPipelineState> pipeline = [dev newRenderPipelineStateWithDescriptor:desc error:&error];
    if (!pipeline) {
        std::printf("FAIL: tex100 pipeline: %s\n", error.localizedDescription.UTF8String);
        std::exit(1);
    }
    id<MTLTexture> source = Texture(device, kSize, kSize);
    FillTexture(device, encoder, source, kGreen);
    id<MTLTexture> target = Texture(device, kSize, kSize);
    encoder.BeginRenderPass(ColorPass(target, true, kBlack));
    encoder.SetRenderPipeline((__bridge void*)pipeline);
    FullViewport(encoder);
    encoder.SetTexture(Metal::Stage::Fragment, 100, (__bridge void*)source);
    encoder.Draw(Metal::Primitive::Triangle, 3);
    encoder.FillBuffer(Ref(SharedBuffer(device, 16)), 16, 0); // forces the slot onto a new encoder
    encoder.Draw(Metal::Primitive::Triangle, 3);
    encoder.EndRenderPass();
    id<MTLBuffer> out = Readback(device, encoder, target);
    device.Wait(encoder.Submit());
    Expect(Pixel(out, kSize, 10, 10) == kGreen, "a texture bound at slot 100 is sampled");
    Expect(device.ErrorCount() == 0, "texture slot 100 ran without errors");
}

void TestDebugGroups() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLLibrary> library = TestLibrary(device);
    id<MTLRenderPipelineState> solid = SolidPipeline(device, library, @"fullscreen");
    id<MTLTexture> target = Texture(device, kSize, kSize);
    id<MTLBuffer> scratch = SharedBuffer(device, 16);
    encoder.PushDebugGroup("frame");       // no encoder open: on the command buffer
    encoder.BeginRenderPass(ColorPass(target, true, kBlack));
    encoder.SetRenderPipeline((__bridge void*)solid);
    FullViewport(encoder);
    SetColor(encoder, kRed);
    encoder.Draw(Metal::Primitive::Triangle, 3);
    encoder.PushDebugGroup("draw");        // on the open render encoder
    encoder.Draw(Metal::Primitive::Triangle, 3);
    encoder.PushDebugGroup("interrupted"); // the encoder ends before this group is popped
    encoder.FillBuffer(Ref(scratch), 16, 0);
    encoder.PopDebugGroup();
    encoder.PopDebugGroup();
    encoder.Draw(Metal::Primitive::Triangle, 3); // reopened encoder
    encoder.EndRenderPass();
    encoder.PushDebugGroup("compute");
    encoder.FillBuffer(Ref(scratch), 16, 0x01020304u);
    encoder.PopDebugGroup();
    encoder.PopDebugGroup();
    id<MTLBuffer> out = Readback(device, encoder, target);
    device.Wait(encoder.Submit());
    // A command-buffer group popped while an encoder is open, and an encoder group left open.
    encoder.PushDebugGroup("outer");
    encoder.BeginRenderPass(ColorPass(target, false, 0));
    encoder.SetRenderPipeline((__bridge void*)solid); // bindings ended with the Submit
    FullViewport(encoder);
    SetColor(encoder, kRed);
    encoder.Draw(Metal::Primitive::Triangle, 3);
    encoder.PopDebugGroup();
    encoder.PushDebugGroup("left open");
    encoder.Draw(Metal::Primitive::Triangle, 3);
    encoder.EndRenderPass();
    encoder.PopDebugGroup(); // its encoder is gone; nothing to pop
    device.Wait(encoder.Submit());
    Expect(Pixel(out, kSize, 5, 5) == kRed, "drawing inside debug groups works");
    Expect(device.ErrorCount() == 0, "balanced debug groups ran without errors");
}

void TestComputeEncoderAccess() {
    Metal::Device device;
    Metal::Encoder encoder{device};
    id<MTLLibrary> library = RasterLibrary(device);
    id<MTLComputePipelineState> add_one = Kernel(device, library, @"add_one");
    id<MTLComputePipelineState> doubles = Kernel(device, library, @"doubles");
    id<MTLBuffer> counter = SharedBuffer(device, 16 * 4);
    id<MTLBuffer> other = SharedBuffer(device, 16 * 4);
    encoder.SetComputePipeline((__bridge void*)add_one);
    encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(counter));
    encoder.Dispatch({1, 1, 1}, {16, 1, 1});
    id<MTLComputeCommandEncoder> raw = (__bridge id<MTLComputeCommandEncoder>)encoder.ComputeEncoder();
    [raw setComputePipelineState:doubles];
    [raw setBuffer:other offset:0 atIndex:0];
    [raw dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(16, 1, 1)];
    encoder.Dispatch({1, 1, 1}, {16, 1, 1}); // the caller's pipeline and buffer come back
    device.Wait(encoder.Submit());
    Expect(static_cast<const u32*>(counter.contents)[3] == 2, "the caller's compute state is re-applied");
    Expect(static_cast<const u32*>(other.contents)[3] == 6, "the raw compute encoder ran its kernel");
    Expect(device.ErrorCount() == 0, "compute encoder access ran without errors");
}

// ---- Explicit ordering of untracked buffers (mtl_order.h) ----

void TestOrderTracker() {
    using Metal::Access;
    using Metal::HazardClass;
    using Metal::Lane;
    using T = Metal::OrderTracker;
    const auto end = [](u32 k) { return T::SlotFence(HazardClass::Memory, k); };
    const auto vtx = [](u32 k) { return T::SlotFence(HazardClass::Memory, k, T::VertexDone); };
    const auto motion = [](u32 k) { return T::SlotFence(HazardClass::MotionHistory, k); };
    const T::Accesses read{Access::Read, Access::None}, write{Access::Write, Access::None};
    // Waits on the slots' fences (not the uploads' one, which every Memory encoder waits for).
    const auto slot_waits = [](const Metal::OrderPlan& p) {
        return p.num_waits - (p.Waits(T::AheadFence) ? 1 : 0);
    };
    // A command `cmd` in one encoder of its own: opens it (with the waits requested for the
    // command), closes it, and moves on to the next command.
    const auto one = [](T& t, Lane lane, u64 cmd, u64 tick = 0, u64 completed = 0) {
        const auto plan = t.Open({Access::Read, Access::None}, lane, tick, completed, cmd);
        t.Close(cmd, cmd);
        t.EndCommand();
        return plan;
    };
    Metal::OrderRequest copy1, render12, vertex1, kinds123;
    copy1.copy.Add(1, 1);
    render12.render_end.Add(1, 2);
    vertex1.render_vertex.Add(1, 1);
    kinds123.copy.Add(1, 3);
    {
        T t; // no request, no barrier: encoders do not wait for each other (RAR, no conflict)
        const auto a = t.Open(read), b = t.Open(read, Lane::Render);
        Expect(slot_waits(a) == 0 && slot_waits(b) == 0,
               "order: encoders without a request or barrier do not wait");
        Expect(a.Updates(end(0)) && a.num_updates == 1, "order: a compute or blit encoder updates its end");
        Expect(b.Updates(vtx(1)) && b.Updates(end(1)),
               "order: a render encoder updates its vertex stage and its end");
        Expect(a.Waits(T::AheadFence) && b.Waits(T::AheadFence),
               "order: every Memory encoder waits for the uploads ahead");
    }
    // ---- Requests (per command) ----
    {
        T t; // a command that conflicts with no earlier command waits for nothing
        one(t, Lane::Copy, 1);
        one(t, Lane::Render, 2);
        Expect(!t.Request(Metal::OrderRequest{}).end_open, "order: an empty request ends nothing");
        Metal::OrderRequest unheld;
        unheld.copy.Add(7, 9); // no encoder holds these commands
        t.Request(unheld);
        Expect(slot_waits(one(t, Lane::Compute, 10)) == 0,
               "order: a command that conflicts with nothing held gets no wait");
    }
    {
        T t; // RAW/WAW: exactly the writer's encoder
        one(t, Lane::Copy, 1);    // slot 0: the writer
        one(t, Lane::Copy, 2);    // slot 1: another copy
        one(t, Lane::Render, 3);  // slot 2
        Expect(!t.Request(copy1).end_open, "order: a request with no open encoder ends nothing");
        const auto p = one(t, Lane::Render, 4);
        Expect(p.Waits(end(0)) && slot_waits(p) == 1,
               "order: a conflicting command waits for exactly the writer's encoder");
        // A later command conflicting with the same writer asks again (no carried waits).
        const auto q = one(t, Lane::Compute, 5);
        Expect(slot_waits(q) == 0, "order: a command after it that does not conflict waits for nothing");
        t.Request(copy1);
        Expect(one(t, Lane::Compute, 6).Waits(end(0)), "order: ... one that does waits again");
    }
    {
        T t; // WAR: exactly the readers' encoders, of the kind named
        one(t, Lane::Render, 1); // slot 0: reader
        one(t, Lane::Render, 2); // slot 1: reader
        one(t, Lane::Render, 3); // slot 2: not a reader
        t.Request(render12);
        const auto p = one(t, Lane::Copy, 4);
        Expect(p.Waits(end(0)) && p.Waits(end(1)) && !p.Waits(end(2)) && slot_waits(p) == 2,
               "order: WAR waits for exactly the readers' ends");
        t.Request(vertex1);
        const auto v = one(t, Lane::Copy, 5);
        Expect(v.Waits(vtx(0)) && !v.Waits(end(0)) && slot_waits(v) == 1,
               "order: WAR on vertex-stage reads waits for that vertex stage only");
    }
    {
        T t; // kinds: a window names commands of one kind of encoder
        one(t, Lane::Render, 1);  // slot 0
        one(t, Lane::Copy, 2);    // slot 1
        one(t, Lane::Compute, 3); // slot 2
        t.Request(kinds123);
        const auto p = one(t, Lane::Render, 4);
        Expect(p.Waits(end(1)) && !p.Waits(end(0)) && !p.Waits(vtx(0)) && !p.Waits(end(2)),
               "order: a copy window waits for copy encoders only");
    }
    {
        T t; // the open encoder holds the command: it ends, the next one waits for it
        t.Open(read, Lane::Copy, 0, 0, 1); // slot 0, open, holds 1..2
        Expect(t.Request(copy1, 0, 1, 2).end_open,
               "order: a request naming the open encoder's command ends it");
        t.Close(1, 2);
        Expect(t.Open(read, Lane::Render, 0, 0, 3).Waits(end(0)),
               "order: ... and the next encoder waits for it");
    }
    {
        T t; // the open encoder goes on: it is ordered after the writer, or waits for it now
        one(t, Lane::Copy, 1);                      // slot 0
        t.Request(copy1);
        t.Open(read, Lane::Render, 0, 0, 2);        // slot 1 waits for slot 0
        {
            const auto r = t.Request(copy1, 0, 2, 3);
            Expect(!r.end_open && r.waits.num_waits == 0,
                   "order: an open encoder that waited for the writer goes on, no new wait");
        }
        t.Close(2, 3);
        t.EndCommand();
        t.Open(read, Lane::Render, 0, 0, 4);        // slot 2: not ordered after slot 0
        {
            const auto r = t.Request(copy1, 0, 4, 4);
            Expect(!r.end_open && r.waits.Waits(end(0)) && r.waits.num_waits == 1,
                   "order: an open encoder not ordered after the writer waits for it now (no "
                   "pass break)");
            Expect(t.Request(copy1, 0, 4, 4).waits.num_waits == 0, "order: ... once");
        }
        t.Close(4, 4);
        Expect(t.Open(read, Lane::Render, 0, 0, 4).Waits(end(0)),
               "order: ... and another encoder of the command waits too");
    }
    {
        T t; // transitive pruning: a wait for an encoder that waited for another stands for it
        one(t, Lane::Copy, 1);  // slot 0
        t.Request(copy1);
        one(t, Lane::Render, 2); // slot 1, waits for slot 0
        Metal::OrderRequest both;
        both.copy.Add(1, 1);
        both.render_end.Add(2, 2);
        t.Request(both);
        const auto p = one(t, Lane::Compute, 3);
        Expect(p.Waits(end(1)) && !p.Waits(end(0)), "order: a wait implied by another is dropped");
        // ... and a vertex stage stands for what its encoder waited for.
        Metal::OrderRequest v2;
        v2.copy.Add(1, 1);
        v2.render_vertex.Add(2, 2);
        t.Request(v2);
        const auto q = one(t, Lane::Compute, 4);
        Expect(q.Waits(vtx(1)) && !q.Waits(end(0)), "order: a vertex stage stands for its waits");
    }
    {
        T t; // encoders of command buffers known complete are never waited for
        one(t, Lane::Copy, 1, 5); // slot 0, tick 5
        t.Request(copy1, 4);
        Expect(one(t, Lane::Copy, 2, 6, 4).Waits(end(0)), "order: tick 5 not complete: waited");
        t.Request(copy1, 5);
        Expect(!one(t, Lane::Copy, 3, 6, 5).Waits(end(0)), "order: tick 5 complete: dropped");
    }
    {
        T t; // more encoders than slots: the reused slot stands for both (conservative)
        for (u32 i = 0; i < T::Slots; ++i) {
            Expect(slot_waits(one(t, Lane::Copy, 1 + i)) == 0, "order: no chain while slots are free");
        }
        Expect(t.BusyReuses(HazardClass::Memory) == 0, "order: no busy reuse while slots are free");
        const auto wrapped = one(t, Lane::Render, 1 + T::Slots);
        Expect(wrapped.Updates(end(0)) && wrapped.Waits(end(0)),
               "order: a slot still in flight is waited for before it is reused");
        Expect(t.BusyReuses(HazardClass::Memory) == 1, "order: a busy reuse is counted");
        // The first encoder's command is still found: through its slot, now the new encoder's.
        t.Request(copy1);
        const auto p = one(t, Lane::Compute, 2 + T::Slots);
        Expect(p.Waits(end(0)) && !p.Waits(vtx(0)),
               "order: a command of a reused slot is waited for through the slot's end");
    }
    {
        T t; // the uploads ahead: the open encoder waits again only for uploads encoded after it opened
        Metal::OrderRequest up;
        up.ahead = true;
        t.Open(read, Lane::Render, 0, 0, 1);
        {
            const auto r = t.Request(up, 0, 1, 1);
            Expect(!r.end_open && r.waits.num_waits == 0,
                   "order: the open encoder waited for the uploads before it");
        }
        t.OpenAhead();
        {
            const auto r = t.Request(up, 0, 1, 1);
            Expect(!r.end_open && r.waits.Waits(T::AheadFence),
                   "order: ... and waits now for an upload encoded since");
            Expect(t.Request(up, 0, 1, 1).waits.num_waits == 0, "order: ... once");
        }
        t.Close(1, 1);
        Expect(t.Open(read, Lane::Render, 0, 0, 2).Waits(T::AheadFence),
               "order: ... the next waits for the uploads");
    }
    {
        T t; // every encoder of the command waits; the next command does not
        one(t, Lane::Copy, 1);
        t.Request(copy1);
        const auto a = t.Open(read, Lane::Compute, 0, 0, 2);
        t.Close(2, 2);
        const auto b = t.Open(read, Lane::Render, 0, 0, 2);
        t.Close(2, 2);
        t.EndCommand();
        Expect(a.Waits(end(0)) && b.Waits(end(0)), "order: every encoder of the command waits");
        Expect(!one(t, Lane::Compute, 3).Waits(end(0)), "order: ... the next command does not");
    }
    // ---- Barriers (what the renderer does not number) ----
    {
        T t; // a full barrier: the next encoders wait for the ends of every encoder before it
        t.Open(read);
        t.Open(read, Lane::Render);
        t.Barrier(HazardClass::Memory, Metal::ScopeAll);
        const auto c = t.Open(read), d = t.Open(read);
        Expect(c.Waits(end(0)) && c.Waits(end(1)) && d.Waits(end(0)) && d.Waits(end(1)),
               "order: after a full barrier every encoder waits for the encoders before");
        Expect(!c.Waits(vtx(1)), "order: ... for their ends, not their vertex stages");
        Expect(!d.Waits(c.updates[0]), "order: encoders after a barrier do not wait for each other");
        Expect(!c.Updates(end(0)) && !c.Updates(end(1)), "order: slots in flight are not reused");
        t.Barrier(HazardClass::Memory, Metal::ScopeAll);
        const auto e = t.Open(read);
        Expect(e.Waits(c.updates[0]) && e.Waits(d.updates[0]) && !e.Waits(end(0)),
               "order: the next barrier waits for the encoders after the last (earlier ones "
               "transitively)");
    }
    {
        T t; // scoped barriers: a barrier on copies does not wait for render passes
        t.Open(read, Lane::Render);  // slot 0, render pass
        t.Open(read, Lane::Copy);    // slot 1, copy
        t.Open(read, Lane::Compute); // slot 2, dispatch
        t.Barrier(HazardClass::Memory, Metal::ScopeCopyEnd);
        const auto a = t.Open(read, Lane::Copy); // slot 3
        Expect(a.Waits(end(1)) && !a.Waits(end(0)) && !a.Waits(vtx(0)),
               "order: a barrier on copies waits for copy encoders only, not render passes");
        Expect(!a.Waits(end(2)), "order: ... nor compute dispatches");
        t.Barrier(HazardClass::Memory, Metal::ScopeRenderVertex);
        const auto b = t.Open(read); // slot 4
        Expect(b.Waits(vtx(0)) && !b.Waits(end(0)), "order: a vertex barrier waits for vertex stages");
        t.Barrier(HazardClass::Memory, Metal::ScopeRenderEnd);
        const auto c = t.Open(read);
        Expect(c.Waits(end(0)), "order: a render barrier waits for render ends");
        Expect(!c.Waits(end(3)) && !c.Waits(end(2)), "order: ... and not for copies or dispatches");
    }
    {
        T t; // barrier waits are pruned once an encoder that waited for them is waited for
        t.Open(read, Lane::Copy);   // slot 0, copy
        t.Open(read, Lane::Render); // slot 1, render
        t.Barrier(HazardClass::Memory, Metal::ScopeCopyEnd);
        t.Open(read, Lane::Render); // slot 2 waits for 0
        t.Barrier(HazardClass::Memory, Metal::ScopeRenderVertex); // 1 and 2 (vertex)
        const auto d = t.Open(read, Lane::Render);
        Expect(d.Waits(vtx(1)) && d.Waits(vtx(2)), "order: the vertex barrier waits for both passes");
        Expect(!d.Waits(end(0)), "order: slot 2 waited for slot 0 before its vertex stage: implied");
    }
    {
        T t; // barrier waits end when their encoders complete
        t.Open(read, Lane::Copy, 5, 0); // slot 0, tick 5
        t.Barrier(HazardClass::Memory, Metal::ScopeCopyEnd);
        Expect(t.Open(read, Lane::Copy, 6, 4).Waits(end(0)), "order: barrier, tick 5 not complete: waited");
        Expect(!t.Open(read, Lane::Copy, 6, 5).Waits(end(0)), "order: barrier, tick 5 complete: dropped");
    }
    {
        T t; // a barrier with nothing new keeps the waits
        t.Open(read);
        t.Barrier(HazardClass::Memory);
        t.Barrier(HazardClass::Memory);
        t.Barrier(HazardClass::Memory, Metal::ScopeRenderVertex);
        Expect(t.Open(read).Waits(end(0)), "order: a barrier with nothing since keeps the waits");
    }
    {
        T t; // a write the caller does not order is isolated: after everything, before everything
        t.Open(read, Lane::Render);
        const auto w = t.Open(write);
        Expect(w.Waits(end(0)), "order: an isolated write waits for every encoder before it");
        const auto r = t.Open(read);
        Expect(r.Waits(w.updates[0]), "order: the encoder after an isolated write waits for it");
        Expect(!r.Waits(end(0)), "order: ... and not again for what the write already waited for");
    }
    {
        T t; // classes are independent; no access, no plan
        t.Open(read);
        t.Barrier(HazardClass::Memory);
        const auto m = t.Open({Access::None, Access::Read}, Lane::Render);
        Expect(m.num_waits == 0 && m.Updates(motion(0)) && m.num_updates == 1,
               "order: motion history has its own barriers (one fence per encoder)");
        Expect(t.Open({Access::None, Access::None}).Empty(), "order: no access, no fences");
        t.Open({Access::None, Access::Read}, Lane::Render);
        t.Barrier(HazardClass::MotionHistory);
        const auto next = t.Open({Access::None, Access::Read}, Lane::Render);
        Expect(next.Waits(motion(0)) && next.Waits(motion(1)) && !next.Waits(T::AheadFence),
               "order: a motion barrier orders the next frame's motion passes");
    }
    {
        // Uploads committed ahead: chained among themselves.
        T t;
        const auto ahead = t.OpenAhead();
        Expect(ahead.num_waits == 1 && ahead.Waits(T::AheadFence) &&
                   ahead.num_updates == 1 && ahead.Updates(T::AheadFence),
               "order: an upload ahead waits for the uploads before it, and updates their fence");
    }
    {
        T t; // wait statistics
        t.TakeWaitStats();
        one(t, Lane::Copy, 1);
        one(t, Lane::Copy, 2);
        Metal::OrderRequest r;
        r.copy.Add(1, 2);
        t.Request(r);
        one(t, Lane::Render, 3);
        const auto stats = t.TakeWaitStats();
        Expect(stats.encoders == 3 && stats.max == 3 && stats.waits == 5,
               "order: wait statistics count the waits per encoder (uploads' fence included)");
    }
}

/// The process's physical footprint (bytes), as Activity Monitor counts memory.
u64 Footprint() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count);
    return info.phys_footprint;
}

void TestBufferBarriers() {
    using Metal::ReadStage;
    using W = Metal::CommandWindow;
    constexpr u8 render = Metal::EncoderRender, copy = Metal::EncoderCopy;
    Metal::BufferBarriers b;
    int arena_storage = 0, other_storage = 0;
    const void* arena = &arena_storage;
    const void* unregistered = &other_storage;
    b.Register(arena, 64ull << 20);
    Expect(b.Registered(arena) && !b.Registered(unregistered), "barriers: registered buffers only");
    Expect(b.BeginCommand() == 1 && b.Command() == 1, "barriers: commands are numbered from 1");
    // RAW: exactly the writer
    b.Mark(arena, 0x1000, 0x100, true, copy); // command 1
    {
        const auto r = b.Conflicts(arena, 0x1080, 4, false);
        Expect(r.copy.Span() == W{1, 1} && r.Scopes() == Metal::ScopeCopyEnd,
               "barriers: RAW, a read of a copy's write waits for that command (copies)");
    }
    Expect(b.Conflicts(arena, 0x3000, 4, false).Empty(), "barriers: a read elsewhere waits for nothing");
    b.BeginCommand(); // 2: a write somewhere else does not join the window
    b.Mark(arena, 0x5000, 0x100, true, copy);
    Expect(b.Conflicts(arena, 0x1080, 4, false).copy.Span() == W{1, 1},
           "barriers: a non-overlapping write is not waited for");
    Expect(b.Conflicts(arena, 0x1080, 4, false).copy.Span() == W{1, 1},
           "barriers: nothing is retired by asking (a later command asks again)");
    // WAW, and a later writer replaces the earlier one
    Expect(b.Conflicts(arena, 0x1004, 8, true).copy.Span() == W{1, 1}, "barriers: WAW waits for the writer");
    b.BeginCommand(); // 3: a render pass writes the block (after waiting for 1)
    b.Mark(arena, 0x1000, 0x10, true, render);
    {
        const auto r = b.Conflicts(arena, 0x1000, 4, false);
        Expect(r.render_end.Span() == W{3, 3} && r.copy.Empty(),
               "barriers: a read waits for the last writer only (it waited for the one before)");
    }
    // RAR, then WAR (late reads: render ends; vertex reads only: vertex stages)
    b.BeginCommand(); // 4 (a whole page, then blocks of the next)
    b.Mark(arena, 0x8000, 0x1f00, false, render);
    Expect(b.Conflicts(arena, 0x8000, 0x10, false).Empty(), "barriers: RAR, reads do not conflict");
    b.BeginCommand(); // 5
    b.Mark(arena, 0x9000, 0x100, false, copy);
    {
        const auto r = b.Conflicts(arena, 0x9000, 0x10, true);
        Expect(r.render_end.Span() == W{4, 5} && r.copy.Span() == W{4, 5} && r.render_vertex.Empty(),
               "barriers: WAR waits for the readers since the last write (their window, kinds)");
        Expect(b.Conflicts(arena, 0x8000, 0x10, true).render_end.Span() == W{4, 4} &&
                   b.Conflicts(arena, 0x8000, 0x10, true).copy.Empty(),
               "barriers: ... of that block only");
    }
    b.BeginCommand(); // 6: a write over the readers (it waited for them)
    b.Mark(arena, 0x9000, 0x100, true, copy);
    {
        const auto r = b.Conflicts(arena, 0x9000, 0x10, true);
        Expect(r.copy.Span() == W{6, 6} && r.render_end.Empty(),
               "barriers: a write replaces the readers before it (WAW on it stands for them)");
    }
    b.BeginCommand(); // 7: reads at the vertex stage only
    b.Mark(arena, 0xa000, 0x100, false, render, ReadStage::Vertex);
    {
        const auto r = b.Conflicts(arena, 0xa000, 0x10, true);
        Expect(r.render_vertex.Span() == W{7, 7} && r.render_end.Empty(),
               "barriers: WAR, a write over vertex-stage reads waits for vertex stages");
    }
    // A tessellated draw reads in kernels and its render pass; one command reads and writes.
    b.BeginCommand(); // 8
    b.Mark(arena, 0x40000, 0x100, false, render | Metal::EncoderCompute, ReadStage::Vertex);
    b.Mark(arena, 0x40000, 0x100, true, Metal::EncoderCompute);
    {
        const auto r = b.Conflicts(arena, 0x40000, 4, true);
        Expect(r.compute.Span() == W{8, 8} && r.render_vertex.Span() == W{8, 8},
               "barriers: a command's reads stay beside its own write");
    }
    // Retired commands are never waited for
    b.Retire(5);
    Expect(b.Conflicts(arena, 0x8000, 0x10, true).Empty(), "barriers: retired readers are not waited for");
    Expect(b.Conflicts(arena, 0x40000, 4, true).compute.Span() == W{8, 8}, "barriers: ... live ones are");
    // Large ranges (whole chunks) and a page inside one
    b.BeginCommand(); // 9
    b.Mark(arena, 0, 8ull << 20, false, copy);
    Expect(b.Conflicts(arena, (5ull << 20) + 12, 4, true).copy.Span() == W{9, 9},
           "barriers: a write inside a range read as whole chunks conflicts");
    Expect(b.Conflicts(arena, 9ull << 20, 4, true).Empty(), "barriers: ... and outside it does not");
    b.BeginCommand(); // 10
    b.Mark(arena, (12ull << 20) + 0x4000, 4, true, render);
    Expect(b.Conflicts(arena, 9ull << 20, 16ull << 20, false).render_end.Span() == W{10, 10},
           "barriers: a read over many chunks sees one written page");
    Expect(b.Conflicts(arena, 9ull << 20, 3ull << 20, false).Empty(), "barriers: ... only where it is");
    // Whole pages are one tag; a query over whole pages sees their blocks together.
    b.BeginCommand(); // 11
    b.Mark(arena, 0x200000, 0x3000, true, copy);
    Expect(b.Conflicts(arena, 0x201080, 4, false).copy.Span() == W{11, 11},
           "barriers: a read inside a page written as a whole waits for that write");
    b.BeginCommand(); // 12
    b.Mark(arena, 0x204000, 4, true, copy);
    b.BeginCommand(); // 13
    b.Mark(arena, 0x204f00, 4, true, Metal::EncoderCompute);
    {
        const auto r = b.Conflicts(arena, 0x204000, 0x1000, false);
        Expect(r.copy.Span() == W{12, 13} && r.compute.Span() == W{12, 13},
               "barriers: a read of a whole page waits for its blocks' writes (their window)");
        Expect(b.Conflicts(arena, 0x204100, 0x100, false).Empty(),
               "barriers: ... a read of one block only for that block's");
    }
    // Copies and dispatches are separate kinds; uploads ahead are their own
    b.BeginCommand(); // 14
    b.Mark(arena, 0x50000, 0x100, true, Metal::EncoderCompute);
    Expect(b.Conflicts(arena, 0x50000, 4, false).Scopes() == Metal::ScopeComputeEnd,
           "barriers: a read of a dispatch's write waits for dispatches only");
    b.BeginCommand(); // 15
    b.Mark(arena, 0x60000, 0x40, true, Metal::EncoderAhead);
    Expect(b.Conflicts(arena, 0x60010, 4, false).Scopes() == Metal::ScopeAhead,
           "barriers: a read of an upload's range waits for the uploads ahead");
    // 256-byte blocks: neighbours in one page do not conflict (the chunk-wide reads before
    // are retired)
    b.Retire(15);
    b.BeginCommand(); // 16
    b.Mark(arena, 0x70000, 0x40, false, render);
    Expect(b.Conflicts(arena, 0x70100, 0x40, true).Empty(),
           "barriers: a write to another 256-byte block of the page does not conflict");
    Expect(b.Conflicts(arena, 0x700c0, 0x80, true).render_end.Span() == W{16, 16},
           "barriers: ... a write to the same block does");
    // Whole-buffer accesses (direct memory access) are one tag, and seen by any range.
    b.BeginCommand(); // 17
    b.Mark(arena, 0, 64ull << 20, false, render);
    Expect(b.Conflicts(arena, 0x123400, 4, true).render_end.Span() == W{17, 17},
           "barriers: a write anywhere conflicts with a whole-buffer read");
    b.BeginCommand(); // 18
    b.Mark(arena, 0x9000, 4, true, copy);
    Expect(b.Conflicts(arena, 0, 64ull << 20, false).copy.Meets(18, 18),
           "barriers: a whole-buffer read sees one written block");
    b.Retire(18);
    Expect(b.Conflicts(arena, 0, 64ull << 20, true).Empty(),
           "barriers: ... and nothing once every command is retired");
    // Registering is free: the tag arrays are committed only where accesses touch them.
    {
        const u64 before = Footprint();
        Metal::BufferBarriers big;
        int big_storage = 0;
        big.Register(&big_storage, 16ull << 30);
        const u64 after = Footprint();
        Expect(after < before + (8ull << 20),
               "barriers: registering a 16 GiB buffer commits (almost) no memory");
        // A buffer whose tags cannot be allocated is ordered conservatively: every access
        // waits for every live command.
        int huge_storage = 0;
        big.Register(&huge_storage, 1ull << 62);
        Expect(big.Registered(&huge_storage), "barriers: a buffer without tags is still tracked");
        big.BeginCommand();
        big.Mark(&huge_storage, 0, 4, false, render);
        big.BeginCommand();
        const auto r = big.Conflicts(&huge_storage, 0, 4, false);
        Expect(r.render_end.Span() == W{1, 2} && r.copy.Span() == W{1, 2} && r.compute.Span() == W{1, 2} && r.ahead,
               "barriers: ... and every access to it waits for every live command");
    }
    // Writers are named exactly: a read over the blocks of two writers does not name the
    // commands between them.
    {
        Metal::BufferBarriers x;
        int storage = 0;
        x.Register(&storage, 1u << 20);
        x.BeginCommand(); // 1
        x.Mark(&storage, 0x1000, 4, true, copy);
        x.BeginCommand(); // 2, elsewhere
        x.Mark(&storage, 0x5000, 4, true, copy);
        x.BeginCommand(); // 3
        x.Mark(&storage, 0x1100, 4, true, copy);
        const auto r = x.Conflicts(&storage, 0x1000, 0x200, false);
        Expect(r.copy.Meets(1, 1) && r.copy.Meets(3, 3) && !r.copy.Meets(2, 2),
               "barriers: the writers are named exactly, not the commands between them");
    }
    // Unregistered buffers: the Encoder orders them itself
    Expect(b.Conflicts(unregistered, 0, 4, true).Empty(), "barriers: unregistered buffers never conflict");
}

constexpr const char* kOrderShaders = R"(
#include <metal_stdlib>
using namespace metal;
// Spins, then stores `args.x` (spins keep the store late enough that an unordered reader in the
// next encoder overtakes it).
kernel void slow_store(device uint* buf [[buffer(0)]], constant uint2& args [[buffer(1)]],
                       uint i [[thread_position_in_grid]]) {
    uint x = i;
    for (uint s = 0; s < args.y; ++s) x = x * 1664525u + 1013904223u;
    buf[i] = x == 0xdeadbeefu && args.y == 3u ? 0u : args.x;
}
kernel void slow_copy(device const uint* buf [[buffer(0)]], device uint* out [[buffer(1)]],
                      constant uint2& args [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    uint x = i;
    for (uint s = 0; s < args.y; ++s) x = x * 1664525u + 1013904223u;
    out[i] = x == 0xdeadbeefu && args.y == 3u ? 0u : buf[i];
}
)";

constexpr u32 kOrderThreads = 16384;
constexpr u32 kOrderSpins = 1u << 16;

bool AllEqual(id<MTLBuffer> b, u32 value) {
    for (u32 i = 0; i < kOrderThreads; ++i) {
        if (static_cast<const u32*>(b.contents)[i] != value) {
            return false;
        }
    }
    return true;
}

void TestUntrackedOrdering() {
    Metal::Device device;
    if (device.HazardTracking()) {
        return; // BB_METAL_HAZARD_TRACKING=1: no explicit ordering to test
    }
    Metal::Encoder encoder{device};
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    NSError* error = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(kOrderShaders) options:nil error:&error];
    if (!lib) {
        std::printf("FAIL: order shaders: %s\n", error.localizedDescription.UTF8String);
        ++failures;
        return;
    }
    id<MTLComputePipelineState> store = Kernel(device, lib, @"slow_store");
    id<MTLComputePipelineState> copy = Kernel(device, lib, @"slow_copy");
    constexpr u32 kBytes = kOrderThreads * 4;
    id<MTLBuffer> buf = [dev newBufferWithLength:kBytes
                                         options:MTLResourceStorageModePrivate |
                                                 MTLResourceHazardTrackingModeUntracked];
    // `buf` is not ordered by the caller: the store is an isolated write (Access::Write, the
    // default), the copy a read.
    const auto Store = [&](Metal::Encoder& e, u32 value, u32 spins) {
        const u32 args[2] = {value, spins};
        e.SetComputePipeline((__bridge void*)store);
        e.SetBuffer(Metal::Stage::Compute, 0, Ref(buf));
        e.SetBytes(Metal::Stage::Compute, 1, args, sizeof(args));
        e.Dispatch({kOrderThreads / 64, 1, 1}, {64, 1, 1});
        e.EndEncoding(); // the next command opens another Metal encoder
    };
    const auto Copy = [&](Metal::Encoder& e, id<MTLBuffer> out, u32 spins) {
        const u32 args[2] = {0, spins};
        e.SetComputePipeline((__bridge void*)copy, Metal::Access::Read);
        e.SetBuffer(Metal::Stage::Compute, 0, Ref(buf));
        e.SetBuffer(Metal::Stage::Compute, 1, Ref(out));
        e.SetBytes(Metal::Stage::Compute, 2, args, sizeof(args));
        e.Dispatch({kOrderThreads / 64, 1, 1}, {64, 1, 1});
        e.EndEncoding();
    };
    id<MTLBuffer> raw = SharedBuffer(device, kBytes), raw_cb = SharedBuffer(device, kBytes),
                  war_old = SharedBuffer(device, kBytes), war_new = SharedBuffer(device, kBytes),
                  ahead_out = SharedBuffer(device, kBytes), src = SharedBuffer(device, kBytes);
    // RAW in one command buffer: a slow store, then a read in the next encoder.
    Store(encoder, 1, kOrderSpins);
    Copy(encoder, raw, 0);
    device.Wait(encoder.Submit());
    Expect(AllEqual(raw, 1), "untracked: a read in the next encoder sees a compute write");
    // RAW across command buffers (the first is not waited for).
    Store(encoder, 2, kOrderSpins);
    encoder.Submit();
    Copy(encoder, raw_cb, 0);
    device.Wait(encoder.Submit());
    Expect(AllEqual(raw_cb, 2), "untracked: a read in the next command buffer sees a compute write");
    // WAR: a slow read, then a write in the next encoder must not overtake it.
    Copy(encoder, war_old, kOrderSpins);
    Store(encoder, 3, 0);
    Copy(encoder, war_new, 0);
    device.Wait(encoder.Submit());
    Expect(AllEqual(war_old, 2), "untracked: a write does not overtake the read before it");
    Expect(AllEqual(war_new, 3), "untracked: the write lands before the next read");
    // WAR across command buffers: a slow read committed (not waited for), then a write in the
    // next command buffer must not overtake it (fails without the fences).
    {
        id<MTLBuffer> old_value = SharedBuffer(device, kBytes), new_value = SharedBuffer(device, kBytes);
        for (u32 round = 0; round < 3; ++round) {
            Store(encoder, 20 + round, 0);
            device.Wait(encoder.Submit());
            Copy(encoder, old_value, kOrderSpins * 4);
            encoder.Submit();
            Store(encoder, 30 + round, 0);
            Copy(encoder, new_value, 0);
            device.Wait(encoder.Submit());
            Expect(AllEqual(old_value, 20 + round),
                   "untracked: a write in the next command buffer does not overtake a slow read");
            Expect(AllEqual(new_value, 30 + round), "untracked: ... and lands before the next read");
        }
    }
    // Hoisted upload, as the scheduler records one: its range is not in use by work in flight
    // (the store before it completed), and a read recorded after it behind a barrier on the
    // uploads (as the renderer records one before a read of an uploaded range) sees it.
    for (u32 i = 0; i < kOrderThreads; ++i) static_cast<u32*>(src.contents)[i] = 5;
    Metal::Encoder upload{device, Metal::Ordering::Ahead};
    Store(encoder, 4, kOrderSpins);
    device.Wait(encoder.Submit());
    upload.CopyBuffer(Ref(src), Ref(buf), kBytes);
    encoder.OrderBarrier(Metal::HazardClass::Memory, Metal::ScopeAhead);
    Copy(encoder, ahead_out, 0);
    upload.SubmitUntracked();
    device.Wait(encoder.Submit());
    Expect(AllEqual(ahead_out, 5),
           "untracked: a read after a barrier on the uploads sees the upload committed ahead");
    // A buffer the caller orders: its writes are plain accesses, ordered by OrderBarrier.
    id<MTLBuffer> arena = [dev newBufferWithLength:kBytes
                                           options:MTLResourceStorageModePrivate |
                                                   MTLResourceHazardTrackingModeUntracked];
    device.OrderByCaller((__bridge void*)arena);
    id<MTLBuffer> ordered_out = SharedBuffer(device, kBytes);
    {
        const u32 args[2] = {6, kOrderSpins};
        encoder.SetComputePipeline((__bridge void*)store, Metal::Access::Read);
        encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(arena));
        encoder.SetBytes(Metal::Stage::Compute, 1, args, sizeof(args));
        encoder.Dispatch({kOrderThreads / 64, 1, 1}, {64, 1, 1});
        encoder.EndEncoding();
        encoder.OrderBarrier(Metal::HazardClass::Memory);
        const u32 copy_args[2] = {0, 0};
        encoder.SetComputePipeline((__bridge void*)copy, Metal::Access::Read);
        encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(arena));
        encoder.SetBuffer(Metal::Stage::Compute, 1, Ref(ordered_out));
        encoder.SetBytes(Metal::Stage::Compute, 2, copy_args, sizeof(copy_args));
        encoder.Dispatch({kOrderThreads / 64, 1, 1}, {64, 1, 1});
        device.Wait(encoder.Submit());
    }
    Expect(AllEqual(ordered_out, 6), "untracked: a barrier orders a caller-ordered write and read");
    // A barrier on copies: a slow copy-lane kernel write, then a blit read of the same
    // caller-ordered memory in the next encoder.
    id<MTLBuffer> chain_out = SharedBuffer(device, kBytes);
    for (u32 round = 0; round < 3; ++round) {
        const u32 args[2] = {7 + round, kOrderSpins};
        encoder.SetComputePipeline((__bridge void*)store, Metal::Access::Read, Metal::Lane::Copy);
        encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(arena));
        encoder.SetBytes(Metal::Stage::Compute, 1, args, sizeof(args));
        encoder.Dispatch({kOrderThreads / 64, 1, 1}, {64, 1, 1});
        encoder.OrderBarrier(Metal::HazardClass::Memory, Metal::ScopeCopyEnd);
        encoder.CopyBuffer(Ref(arena), Ref(chain_out), kBytes);
        device.Wait(encoder.Submit());
        Expect(AllEqual(chain_out, 7 + round), "untracked: a barrier on copies orders a copy's write");
    }
    // Per-command ordering (the renderer's requests, mtl_buffer_barriers.h) on caller-ordered
    // memory: a command waits for the commands it names, across encoders and command buffers.
    const auto StoreArena = [&](u32 value, u32 spins) {
        const u32 args[2] = {value, spins};
        encoder.SetComputePipeline((__bridge void*)store, Metal::Access::Read);
        encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(arena));
        encoder.SetBytes(Metal::Stage::Compute, 1, args, sizeof(args));
        encoder.Dispatch({kOrderThreads / 64, 1, 1}, {64, 1, 1});
    };
    const auto ReadArena = [&](id<MTLBuffer> out, u32 spins) {
        const u32 args[2] = {0, spins};
        encoder.SetComputePipeline((__bridge void*)copy, Metal::Access::Read);
        encoder.SetBuffer(Metal::Stage::Compute, 0, Ref(arena));
        encoder.SetBuffer(Metal::Stage::Compute, 1, Ref(out));
        encoder.SetBytes(Metal::Stage::Compute, 2, args, sizeof(args));
        encoder.Dispatch({kOrderThreads / 64, 1, 1}, {64, 1, 1});
    };
    const auto Ask = [](u64 first, u64 last) {
        Metal::OrderRequest r;
        r.compute.Add(first, last);
        return r;
    };
    {
        id<MTLBuffer> raw_out = SharedBuffer(device, kBytes);
        for (u32 round = 0; round < 3; ++round) {
            const u64 base = 1000 + round * 10;
            encoder.SetOrderCommand(base); // a slow write
            StoreArena(40 + round, kOrderSpins);
            encoder.SetOrderCommand(base + 1); // a blit read of it, in the same command buffer
            encoder.OrderWait(Ask(base, base));
            encoder.CopyBuffer(Ref(arena), Ref(raw_out), kBytes);
            device.Wait(encoder.Submit());
            Expect(AllEqual(raw_out, 40 + round),
                   "untracked: a command waits for the write it asks for (RAW)");
        }
    }
    {
        // The open encoder is not ordered after the write it asks for: it waits for it now and
        // goes on (no pass break).
        id<MTLBuffer> mid_out = SharedBuffer(device, kBytes), other = SharedBuffer(device, kBytes);
        for (u32 round = 0; round < 3; ++round) {
            const u64 base = 3000 + round * 10;
            encoder.SetOrderCommand(base); // a slow write, in an encoder of its own
            StoreArena(70 + round, kOrderSpins);
            encoder.EndEncoding();
            encoder.SetOrderCommand(base + 1); // an unrelated read opens the next encoder
            ReadArena(other, 0);
            void* const open = encoder.ComputeEncoder(Metal::Access::Read);
            encoder.SetOrderCommand(base + 2); // a read asking for the write, in that encoder
            encoder.OrderWait(Ask(base, base));
            ReadArena(mid_out, 0);
            Expect(encoder.ComputeEncoder(Metal::Access::Read) == open,
                   "untracked: a request the open encoder can wait for keeps it open");
            device.Wait(encoder.Submit());
            Expect(AllEqual(mid_out, 70 + round),
                   "untracked: an open encoder that waits for a write mid-way sees it");
        }
    }
    {
        id<MTLBuffer> old_value = SharedBuffer(device, kBytes), new_value = SharedBuffer(device, kBytes);
        for (u32 round = 0; round < 3; ++round) {
            const u64 base = 2000 + round * 10;
            encoder.SetOrderCommand(base);
            StoreArena(50 + round, 0);
            device.Wait(encoder.Submit());
            encoder.SetOrderCommand(base + 1); // a slow read, committed and not waited for
            ReadArena(old_value, kOrderSpins * 4);
            encoder.Submit();
            encoder.SetOrderCommand(base + 2); // a write in the next command buffer (WAR)
            encoder.OrderWait(Ask(base + 1, base + 1));
            StoreArena(60 + round, 0);
            encoder.SetOrderCommand(base + 3); // a read of it (RAW)
            encoder.OrderWait(Ask(base + 2, base + 2));
            ReadArena(new_value, 0);
            device.Wait(encoder.Submit());
            Expect(AllEqual(old_value, 50 + round),
                   "untracked: a write asking for a slow read in an earlier command buffer does "
                   "not overtake it (WAR)");
            Expect(AllEqual(new_value, 60 + round), "untracked: ... and the read after it sees it");
        }
    }
    Expect(device.ErrorCount() == 0, "untracked ordering ran without errors");
}

int main() {
    @autoreleasepool {
        TestObject();
        TestTicks();
        TestDeviceLifetime();
        TestBlits();
        TestGpuBusyTime();
        TestBusyIntervalUnion();
        TestTimestampSampling();
        TestKeepAlive();
        TestWaitMeansCompleted();
        TestCompute();
        TestResourceBindingCache();
        TestInlineBindingCache();
        TestLargeBytes();
        TestClearOnlyPass();
        TestPassReopenKeepsDrawing();
        TestStatePersistsAcrossPasses();
        TestIndexedDraw();
        TestBindingsEndAtSubmit();
        TestNoLeakOnPoollessThread();
        TestCopyInPassSeesClear();
        TestOutsideWorkAfterPendingClear();
        TestScissorClampedToTarget();
        TestFillPattern();
        TestSubmitResetsBindings();
        TestUntrackedRunsFirst();
        TestIndirectResidency();
        TestIndirectVertexWrite();
        TestDepthAspectCopy();
        TestResolvePasses();
        TestBlitShaders();
        TestSpecialize();
        TestDepthStencilCache();
        TestTimelines();
        TestBufferCreation();
        TestTextures();
        TestSafeViewFormats();
        TestIntegerPixelFormats();
        TestResidentBytes();
        TestZeroSizeBlits();
        TestUnalignedPatternFill();
        TestCrossEncoderWait();
        TestVertexBufferStride();
        TestIndirectDraw();
        TestIndirectCountClamp();
        TestDispatchIndirect();
        TestFillModeAndDepthClip();
        TestDrawPatches();
        TestTextureSlot100();
        TestDebugGroups();
        TestComputeEncoderAccess();
        TestOrderTracker();
        TestBufferBarriers();
        TestUntrackedOrdering();
    }
    if (failures) {
        std::printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    std::printf("PASS: Metal-native foundation (object, device ticks, encoder)\n");
    return 0;
}
