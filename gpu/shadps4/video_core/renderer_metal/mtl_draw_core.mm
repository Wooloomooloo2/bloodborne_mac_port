// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the draw helper's Vulkan-free part (mtl_draw.h): slot binding, draw preparation, plain /
// rect / indexed / indirect draws, dispatches, scratch memory and the tessellation run queue.
// Ported from mtl_frame.mm (BindStage, PrepareDraw, DrawSnap/FlushPending, Scratch, Draw*,
// Dispatch*), rewritten against Metal::Encoder.
#import <Metal/Metal.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_metal/mtl_depth_stencil.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_draw.h"
#include "video_core/renderer_metal/mtl_raster_util.h"

namespace Metal::Draw {
namespace {

constexpr u64 kNullBufferSize = 64 * 1024;
/// Push constants are bound as at least this many bytes (the recompiler's push block; Metal's
/// validation wants the whole struct the shader declares).
constexpr size_t kMinPushBytes = 256;
constexpr u64 kChunkSize = 8ull << 20;
constexpr u64 kLargeStep = 16ull << 20;
constexpr size_t kMaxPooledChunks = 16;

/// Walks a shader's bindings and calls the sink for every slot it uses.
template <typename Sink>
void BindWith(const CompiledShader& shader, std::span<const Bound> bound, std::span<const u8> push,
              const Nulls& nulls, Sink&& sink) {
    using Kind = ShaderBinding::Kind;
    std::array<u32, kMaxBuffers> sizes{};
    for (const ShaderBinding& b : shader.bindings) {
        if (b.kind == Kind::PushConstants) {
            continue;
        }
        for (u32 e = 0; e < b.count; ++e) {
            const u64 index = u64(b.binding) + e;
            const Bound* el = b.set == 0 && index < bound.size() ? &bound[index] : nullptr;
            const u32 slot = b.msl_index + e;
            switch (b.kind) {
            case Kind::Buffer: {
                const bool present = el && el->buffer.buffer;
                sink.Buffer(slot, present ? el->buffer : nulls.buffer);
                if (slot < sizes.size()) {
                    sizes[slot] = present ? u32(std::min<u64>(el->size, 0xffffffffu))
                                          : nulls.buffer_size;
                }
                break;
            }
            case Kind::Texture:
            case Kind::StorageTexture:
            case Kind::TexelBuffer:
                sink.Texture(slot, el && el->texture ? el->texture : nulls.texture);
                break;
            case Kind::Sampler:
                sink.Sampler(slot, el && el->sampler ? el->sampler : nulls.sampler);
                break;
            case Kind::PushConstants:
                break;
            }
        }
    }
    if (shader.uses_push_constants) {
        if (push.size() >= kMinPushBytes) {
            sink.Bytes(shader.push_constant_buffer, push.data(), u32(push.size()));
        } else {
            std::array<u8, kMinPushBytes> padded{};
            if (!push.empty()) {
                std::memcpy(padded.data(), push.data(), push.size());
            }
            sink.Bytes(shader.push_constant_buffer, padded.data(), u32(padded.size()));
        }
    }
    if (shader.buffer_size_buffer != ~0u) {
        const u32 n = std::clamp<u32>(shader.num_buffers, 1, u32(sizes.size()));
        sink.Bytes(shader.buffer_size_buffer, sizes.data(), u32(sizeof(u32) * n));
    }
}

struct EncoderSink {
    Encoder& e;
    Stage stage;
    void Buffer(u32 slot, BufferRef ref) {
        e.SetBuffer(stage, slot, ref);
    }
    void Texture(u32 slot, void* texture) {
        e.SetTexture(stage, slot, texture);
    }
    void Sampler(u32 slot, void* sampler) {
        e.SetSampler(stage, slot, sampler);
    }
    void Bytes(u32 slot, const void* data, u32 size) {
        e.SetBytes(stage, slot, data, size);
    }
};

struct KernelSink {
    id<MTLComputeCommandEncoder> enc;
    void Buffer(u32 slot, BufferRef ref) {
        [enc setBuffer:(__bridge id<MTLBuffer>)ref.buffer offset:ref.offset atIndex:slot];
    }
    void Texture(u32 slot, void* texture) {
        [enc setTexture:(__bridge id<MTLTexture>)texture atIndex:slot];
    }
    void Sampler(u32 slot, void* sampler) {
        [enc setSamplerState:(__bridge id<MTLSamplerState>)sampler atIndex:slot];
    }
    void Bytes(u32 slot, const void* data, u32 size) {
        [enc setBytes:data length:size atIndex:slot];
    }
};

} // namespace

// Logged once per call site (per process): skipped kinds of draws repeat every frame.
#define BB_DRAW_WARN_ONCE(...)                                                                     \
    do {                                                                                           \
        static std::once_flag once_;                                                               \
        std::call_once(once_, [&] { LOG_WARNING(Render, __VA_ARGS__); });                          \
    } while (0)

// ---- Null stand-ins ----

NullResources::NullResources(Device& device) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
    @autoreleasepool {
        id<MTLBuffer> b = [dev newBufferWithLength:kNullBufferSize
                                           options:MTLResourceStorageModeShared];
        std::memset(b.contents, 0, kNullBufferSize);
        b.label = @"bbport null buffer";
        buffer = Object::Adopt((__bridge_retained void*)b);

        MTLTextureDescriptor* td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                               width:1
                                                              height:1
                                                           mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> t = [dev newTextureWithDescriptor:td];
        const u32 zero = 0;
        [t replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&zero bytesPerRow:4];
        t.label = @"bbport null texture";
        texture = Object::Adopt((__bridge_retained void*)t);

        id<MTLSamplerState> s = [dev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
        sampler = Object::Adopt((__bridge_retained void*)s);
    }
}

Nulls NullResources::Get() const {
    return {{buffer.Get(), 0}, u32(kNullBufferSize), texture.Get(), sampler.Get()};
}

// ---- Binding ----

void BindStage(Encoder& e, Stage stage, const CompiledShader& shader, std::span<const Bound> bound,
               std::span<const u8> push, const Nulls& nulls) {
    BindWith(shader, bound, push, nulls, EncoderSink{e, stage});
}

void BindKernelStage(void* compute_encoder, const CompiledShader& shader,
                     std::span<const Bound> bound, std::span<const u8> push, const Nulls& nulls) {
    BindWith(shader, bound, push, nulls,
             KernelSink{(__bridge id<MTLComputeCommandEncoder>)compute_encoder});
}

void BindVertexStreams(Encoder& e, std::span<const VertexBufferSlot> slots,
                       std::span<const VertexStream> streams, const Nulls& nulls) {
    for (const VertexBufferSlot& slot : slots) {
        const VertexStream* stream =
            slot.location < streams.size() ? &streams[slot.location] : nullptr;
        const bool present = stream && stream->buffer.buffer;
        e.SetVertexBuffer(slot.slot, present ? stream->buffer : nulls.buffer,
                          StreamStride(slot, stream));
    }
}

// ---- Draw state ----

DepthStencilKey MaskDepthStencil(const DepthStencilKey& key, bool has_depth, bool has_stencil) {
    DepthStencilKey out{};
    out.depth_test = key.depth_test && has_depth;
    if (out.depth_test) {
        out.depth_write = key.depth_write;
        out.depth_compare = key.depth_compare;
    }
    out.stencil_test = key.stencil_test && has_stencil;
    if (out.stencil_test) {
        out.front = key.front;
        out.back = key.back;
    }
    return out;
}

Skip CheckDraw(const Encoder& e, const DrawPipeline& p, const DrawState& s) {
    if (!e.InRenderPass()) {
        return Skip::NoPass;
    }
    if (!p.state) {
        return Skip::NoPipeline;
    }
    if (p.emulated && !p.rect_list && !p.tessellation && !p.geometry) {
        return Skip::Emulated;
    }
    if (s.rasterizer_discard) {
        return Skip::Discard;
    }
    if (s.cull_all) {
        return Skip::CullAll;
    }
    RasterUtil::ScissorRect r;
    if (!RasterUtil::ClampScissor(s.scissor_x, s.scissor_y, s.scissor_width, s.scissor_height,
                                  s.target_width, s.target_height, r)) {
        return Skip::EmptyScissor;
    }
    return Skip::None;
}

bool Prepare(Encoder& e, const DrawPipeline& p, const DrawState& s) {
    if (CheckDraw(e, p, s) != Skip::None) {
        return false;
    }
    e.SetRenderPipeline(p.state, p.buffers);
    e.SetFillMode(p.fill);
    e.SetDepthClip(p.depth_clip);
    e.SetCullMode(s.cull);
    e.SetFrontFace(s.front_face);
    // Negative-height viewport keeps Vulkan's y; depth range clamped (mtl_raster_util.h).
    const auto m = RasterUtil::ToMetalViewport(s.viewport_x, s.viewport_y, s.viewport_width,
                                               s.viewport_height, s.min_depth, s.max_depth);
    e.SetViewport({float(m.origin_x), float(m.origin_y), float(m.width), float(m.height),
                   float(m.znear), float(m.zfar)});
    RasterUtil::ScissorRect r;
    RasterUtil::ClampScissor(s.scissor_x, s.scissor_y, s.scissor_width, s.scissor_height,
                             s.target_width, s.target_height, r);
    e.SetScissor({r.x, r.y, r.width, r.height});
    e.SetDepthStencil(s.depth_stencil);
    e.SetStencilReference(s.stencil_ref_front, s.stencil_ref_back);
    if (s.depth_bias_enable) {
        e.SetDepthBias(s.depth_bias_constant, s.depth_bias_slope, s.depth_bias_clamp);
    } else {
        e.SetDepthBias(0.0f, 0.0f, 0.0f);
    }
    e.SetBlendColor(s.blend_color[0], s.blend_color[1], s.blend_color[2], s.blend_color[3]);
    return true;
}

// ---- Runs ----

namespace {
/// BB_HOLD_DRAWS=0: plain draws end tessellation runs (the behaviour before held draws).
bool HoldDrawsByDefault() {
    const char* env = std::getenv("BB_HOLD_DRAWS");
    return !(env && env[0] == '0');
}
} // namespace

Runs::Runs(Device& device_, const Nulls& nulls_)
    : device{device_}, nulls{nulls_}, hold_draws{HoldDrawsByDefault()} {}

Runs::~Runs() = default;

bool Runs::Count(Skip skip) {
    switch (skip) {
    case Skip::None:
        return true;
    case Skip::NoPass:
        ++counters.skipped_no_pass;
        BB_DRAW_WARN_ONCE("Metal draw outside a render pass (skipped)");
        return false;
    case Skip::NoPipeline:
        ++counters.skipped_no_pipeline;
        return false;
    case Skip::Emulated:
        ++counters.skipped_emulated;
        return false;
    case Skip::Discard:
        ++counters.skipped_discard;
        return false;
    case Skip::CullAll:
        ++counters.skipped_cull_all;
        return false;
    case Skip::EmptyScissor:
        ++counters.skipped_empty_scissor;
        return false;
    }
    return false;
}

std::string DescribeSkips(const Runs::Counters& now, SkipTotals& last) {
    const std::array<std::pair<const char*, const std::atomic<u64>*>, 8> reasons{{
        {"no pass", &now.skipped_no_pass},
        {"no pipeline", &now.skipped_no_pipeline},
        {"tessellation", &now.skipped_tess},
        {"emulated topology", &now.skipped_emulated},
        {"unsupported", &now.unsupported},
        {"discard", &now.skipped_discard},
        {"cull all", &now.skipped_cull_all},
        {"empty scissor", &now.skipped_empty_scissor},
    }};
    static_assert(std::tuple_size_v<decltype(SkipTotals::values)> == 8);
    std::string out;
    for (size_t i = 0; i < reasons.size(); ++i) {
        const u64 value = reasons[i].second->load(std::memory_order_relaxed);
        if (const u64 delta = value - last.values[i]) {
            out += out.empty() ? "" : ", ";
            out += reasons[i].first;
            out += ' ';
            out += std::to_string(delta);
        }
        last.values[i] = value;
    }
    return out;
}

void Runs::ReportSkips() {
    const auto now = std::chrono::steady_clock::now();
    if (now - skips_reported_at < std::chrono::seconds{5}) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now - skips_reported_at).count();
    skips_reported_at = now;
    if (const std::string skips = DescribeSkips(counters, skips_reported); !skips.empty()) {
        LOG_WARNING(Render, "Metal draws skipped in the last {:.0f} s: {}", seconds, skips);
    }
}

bool Runs::Begin(Encoder& e, const DrawPipeline& p, const DrawState& s, const Resources& r) {
    if (!Count(CheckDraw(e, p, s))) {
        return false;
    }
    Prepare(e, p, s);
    if (p.vs) {
        BindStage(e, Stage::Vertex, *p.vs, r.bound, r.push, nulls);
    }
    if (p.fs) {
        BindStage(e, Stage::Fragment, *p.fs, r.bound, r.push, nulls);
    }
    BindVertexStreams(e, p.vertex_slots, r.streams, nulls);
    return true;
}

void Runs::EndRun(Encoder& e) {
    last_draw_tess = false;
    Flush(e);
}

void Runs::EncodeDraw(Encoder& e, const DrawPipeline& p, const Resources& r, const DrawArgs& a) {
    if (a.indexed) {
        const u32 size = IndexSize(r.index_type);
        e.DrawIndexed(p.primitive, a.count, r.index_type,
                      {r.indices.buffer, r.indices.offset + u64(a.first) * size}, a.instances,
                      a.vertex_offset, a.first_instance);
    } else if (p.rect_list) {
        // Two triangles (6 vertices) per rectangle of 3 (mtl_spirv_rectlist.h).
        e.Draw(p.primitive, a.count / 3 * 6, a.instances, a.first / 3 * 6, a.first_instance);
    } else {
        e.Draw(p.primitive, a.count, a.instances, a.first, a.first_instance);
    }
}

Runs::Pending& Runs::NextPending() {
    if (num_pending == pending.size()) {
        pending.emplace_back();
    }
    return pending[num_pending++];
}

bool Runs::Draw(Encoder& e, const DrawPipeline& p, const DrawState& s, const Resources& r,
                const DrawArgs& a) {
    // Held: it waits behind the run's patch draws (see the class comment).
    const bool hold = num_pending && hold_draws && !p.writes_images &&
                      p.buffers != Access::Write && !p.tessellation && !p.geometry;
    if (!hold) {
        EndRun(e);
    }
    if (p.tessellation || p.geometry) {
        // Their kernels need the cache pipelines (the Graphics form).
        ++counters.unsupported;
        return false;
    }
    if (p.rect_list && a.indexed) {
        ++counters.unsupported;
        BB_DRAW_WARN_ONCE("Metal: indexed rect list draws are not encoded (skipped)");
        return false;
    }
    if (a.indexed && !r.indices.buffer) {
        ++counters.unsupported;
        BB_DRAW_WARN_ONCE("Metal: an indexed draw without an index buffer (skipped)");
        return false;
    }
    if (!a.count || !a.instances) {
        return false;
    }
    if (hold) {
        if (!Count(CheckDraw(e, p, s))) {
            return false;
        }
        Pending& entry = NextPending();
        entry.pipeline = p;
        entry.state = s;
        entry.bound.assign(r.bound.begin(), r.bound.end());
        entry.push.assign(r.push.begin(), r.push.end());
        entry.patch = {};
        entry.streams.assign(r.streams.begin(), r.streams.end());
        entry.indices = r.indices;
        entry.index_type = r.index_type;
        entry.args = a;
        entry.request = request_command == e.OrderCommand() ? next_request : OrderRequest{};
        next_request = {};
        last_draw_tess = false;
        ++counters.draws;
        ++counters.held_draws;
        return true;
    }
    if (!Begin(e, p, s, r)) {
        return false;
    }
    EncodeDraw(e, p, r, a);
    ++counters.draws;
    return true;
}

bool Runs::DrawIndirect(Encoder& e, const DrawPipeline& p, const DrawState& s, const Resources& r,
                        const IndirectArgs& a) {
    EndRun(e);
    if (p.tessellation || p.geometry) {
        ++counters.unsupported;
        return false;
    }
    if (p.rect_list) {
        ++counters.unsupported;
        BB_DRAW_WARN_ONCE("Metal: indirect rect list draws are not encoded (skipped)");
        return false;
    }
    if (!a.args.buffer || !a.count) {
        return false;
    }
    if (a.indexed && !r.indices.buffer) {
        ++counters.unsupported;
        BB_DRAW_WARN_ONCE("Metal: an indexed indirect draw without an index buffer (skipped)");
        return false;
    }
    if (!Begin(e, p, s, r)) {
        return false;
    }
    if (a.count_buffer.buffer) {
        e.DrawIndirectCount(p.primitive, a.args, a.stride, a.count_buffer, a.count, a.indexed,
                            r.index_type, r.indices);
        counters.draws += a.count;
        return true;
    }
    for (u32 i = 0; i < a.count; ++i) {
        const BufferRef at{a.args.buffer, a.args.offset + u64(i) * a.stride};
        if (a.indexed) {
            e.DrawIndexedIndirect(p.primitive, r.index_type, r.indices, at);
        } else {
            e.DrawIndirect(p.primitive, at);
        }
        ++counters.draws;
    }
    return true;
}

void Runs::QueuePatchDraw(const DrawPipeline& p, const DrawState& s, const Resources& r,
                          const PatchDraw& patch) {
    // The tessellation render pipeline is built without vertex attributes (PsoCache::
    // GetTessellationPipeline: the evaluation function reads the kernels' scratch), so the
    // deferred draw binds no vertex streams.
    ASSERT_MSG(p.vertex_slots.empty(), "a patch draw pipeline with vertex attributes");
    ASSERT_MSG(patch.patches != 0, "a patch draw without patches");
    Pending& entry = NextPending();
    entry.pipeline = p;
    entry.state = s;
    entry.bound.assign(r.bound.begin(), r.bound.end());
    entry.push.assign(r.push.begin(), r.push.end());
    entry.patch = patch;
    entry.streams.clear();
    entry.request = {};
}

void Runs::OrderWait(Encoder& e, const OrderRequest& request) {
    e.OrderWait(request);
    if (request_command != e.OrderCommand()) {
        next_request = {};
        request_command = e.OrderCommand();
    }
    next_request |= request;
}

void Runs::Flush(Encoder& e) {
    if (!num_pending) {
        return;
    }
    // Outside a pass every patch draw would be skipped (lost): the caller ended the pass or
    // submitted without Runs::EndRenderPass / Runs::Submit.
    ASSERT_MSG(e.InRenderPass(), "tessellated draws pending outside a render pass");
    ++counters.runs;
    const size_t count = num_pending;
    num_pending = 0;
    for (size_t i = 0; i < count; ++i) {
        const Pending& d = pending[i];
        const Resources r{d.bound, d.push, d.streams, d.indices, d.index_type};
        if (!d.patch.patches) {
            // A held draw: its order wait was for encoders of its own command (OrderWait).
            if (!d.request.Empty()) {
                e.OrderWait(d.request);
            }
            if (Begin(e, d.pipeline, d.state, r)) {
                EncodeDraw(e, d.pipeline, r, d.args);
            }
            continue;
        }
        if (!Begin(e, d.pipeline, d.state, r)) {
            continue;
        }
        e.SetTessellationFactorBuffer(d.patch.factors, 0);
        e.SetBuffer(Stage::Vertex, kTessInputBuffer, d.patch.control);
        e.SetBuffer(Stage::Vertex, kTessPatchBuffer, d.patch.patch);
        e.SetBuffer(Stage::Vertex, kTessFactorBuffer, d.patch.factors);
        e.DrawPatches(d.patch.output_control_points, 0, d.patch.patches, {}, 1, d.patch.instance);
    }
    ASSERT_MSG(num_pending == 0, "draws queued while a run was drawn");
    e.ReleaseOrderCommand();
}

void Runs::EndRenderPass(Encoder& e) {
    EndRun(e);
    e.EndRenderPass();
}

u64 Runs::Submit(Encoder& e) {
    if (e.InRenderPass()) {
        EndRun(e);
    }
    const u64 tick = e.Submit();
    Submitted(tick);
    return tick;
}

bool Runs::Dispatch(Encoder& e, const ComputeView& c, std::span<const Bound> bound,
                    std::span<const u8> push, Size3 groups) {
    EndRun(e);
    if (!c.state || !c.shader) {
        ++counters.skipped_no_pipeline;
        return false;
    }
    e.SetComputePipeline(c.state, c.buffers);
    BindStage(e, Stage::Compute, *c.shader, bound, push, nulls);
    e.Dispatch(groups, c.threads_per_group);
    ++counters.dispatches;
    return true;
}

bool Runs::DispatchIndirect(Encoder& e, const ComputeView& c, std::span<const Bound> bound,
                            std::span<const u8> push, BufferRef args) {
    EndRun(e);
    if (!c.state || !c.shader || !args.buffer) {
        ++counters.skipped_no_pipeline;
        return false;
    }
    e.SetComputePipeline(c.state, c.buffers);
    BindStage(e, Stage::Compute, *c.shader, bound, push, nulls);
    e.DispatchIndirect(args, c.threads_per_group);
    ++counters.dispatches;
    return true;
}

// ---- Scratch ----

BufferRef Runs::Scratch(Encoder& e, u64 size) {
    size = (size + 15) & ~u64(15);
    if (!chunk || chunk_used + size > chunk_length) {
        // Larger requests (indirect geometry draws) in 16 MiB steps, so that they pool too.
        const u64 length = size <= kChunkSize ? kChunkSize : (size + kLargeStep - 1) & ~(kLargeStep - 1);
        // Chunks of completed command buffers go back to the pool.
        for (size_t i = 0; i < inflight.size();) {
            if (device.IsFree(inflight[i].tick)) {
                if (pool.size() < kMaxPooledChunks &&
                    (inflight[i].length == kChunkSize || inflight[i].length == kLargeStep)) {
                    pool.push_back(std::move(inflight[i]));
                }
                inflight.erase(inflight.begin() + i);
            } else {
                ++i;
            }
        }
        chunk = {};
        for (size_t i = pool.size(); i-- > 0;) {
            if (pool[i].length == length) {
                chunk = std::move(pool[i].buffer);
                pool.erase(pool.begin() + i);
                break;
            }
        }
        if (!chunk) {
            id<MTLDevice> dev = (__bridge id<MTLDevice>)device.NativeDevice();
            @autoreleasepool {
                id<MTLBuffer> b = [dev newBufferWithLength:length
                                                   options:MTLResourceStorageModePrivate];
                b.label = @"bbport draw scratch";
                chunk = Object::Adopt((__bridge_retained void*)b);
            }
        }
        chunk_length = length;
        chunk_used = 0;
        used.push_back({Object::Retain(chunk.Get()), length, 0});
        // The command buffer keeps it alive if the pool or this helper goes first.
        e.KeepAlive(Object::Retain(chunk.Get()));
    }
    const u64 at = chunk_used;
    chunk_used += size;
    return {chunk.Get(), at};
}

void Runs::Submitted(u64 device_tick) {
    ASSERT_MSG(num_pending == 0, "Submit with tessellated draws pending (Runs::Flush first)");
    for (Chunk& c : used) {
        c.tick = device_tick;
        inflight.push_back(std::move(c));
    }
    used.clear();
    // The next command buffer starts a new chunk: this one's GPU use ends with `device_tick`.
    chunk = {};
    chunk_length = chunk_used = 0;
}

} // namespace Metal::Draw
