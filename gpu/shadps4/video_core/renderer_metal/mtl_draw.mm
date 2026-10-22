// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the draw helper's part that needs the PSO cache's pipeline types (mtl_draw.h): views of
// RenderPipeline / ComputePipeline, the routing of a GraphicsPipeline's draws, and the
// tessellated and geometry-shader draws, whose kernels run on the Encoder's native compute
// encoder. Ported from mtl_frame.mm (TessellatedDraw, GeometryDraw, GeometryDrawIndirect) and
// mtl_shadow.mm (the indirect geometry kernels, now owned here).
#import <Metal/Metal.h>

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <vector>
#include "common/logging/log.h"
#include "video_core/renderer_metal/mtl_draw.h"
#include "video_core/renderer_metal/mtl_geometry.h"
#include "video_core/renderer_metal/mtl_pso_cache.h"
#include "video_core/renderer_metal/mtl_tessellation.h"

namespace Metal::Draw {
namespace {

#define BB_DRAW_WARN_ONCE(...)                                                                     \
    do {                                                                                           \
        static std::once_flag once_;                                                               \
        std::call_once(once_, [&] { LOG_WARNING(Render, __VA_ARGS__); });                          \
    } while (0)

/// The kernels' stage-in streams: the pipeline's vertex slots with the draw's buffers, at the
/// same strides as a plain draw's (StreamStride).
u32 KernelStreams(std::span<const VertexBufferSlot> slots, const Resources& r, const Nulls& nulls,
                  std::array<Tessellation::VertexBinding, 32>& out) {
    const u32 n = u32(std::min(slots.size(), out.size()));
    for (u32 j = 0; j < n; ++j) {
        const VertexBufferSlot& slot = slots[j];
        const VertexStream* stream =
            slot.location < r.streams.size() ? &r.streams[slot.location] : nullptr;
        const bool present = stream && stream->buffer.buffer;
        out[j] = {present ? stream->buffer.buffer : nulls.buffer.buffer,
                  present ? stream->buffer.offset : 0,
                  StreamStride(slot, stream), slot.slot};
    }
    return n;
}

Primitive ToPrimitive(u32 mtl_primitive_type) {
    switch (MTLPrimitiveType(mtl_primitive_type)) {
    case MTLPrimitiveTypePoint:
        return Primitive::Point;
    case MTLPrimitiveTypeLine:
        return Primitive::Line;
    case MTLPrimitiveTypeLineStrip:
        return Primitive::LineStrip;
    case MTLPrimitiveTypeTriangleStrip:
        return Primitive::TriangleStrip;
    case MTLPrimitiveTypeTriangle:
        break;
    }
    return Primitive::Triangle;
}

} // namespace

DrawPipeline View(const RenderPipeline& p) {
    DrawPipeline v;
    v.state = p.state;
    v.primitive = ToPrimitive(p.primitive_type);
    v.emulated = p.primitive_emulated;
    v.rect_list = p.rect_list;
    v.tessellation = p.tessellation;
    v.geometry = p.geometry;
    v.fill = p.fill_mode == u32(MTLTriangleFillModeLines) ? FillMode::Lines : FillMode::Fill;
    v.depth_clip = p.depth_clip ? DepthClip::Clip : DepthClip::Clamp;
    v.has_depth = p.depth_pixel_format != 0;
    v.has_stencil = p.stencil_pixel_format != 0;
    v.vs = p.vs;
    v.fs = p.fs;
    v.vertex_slots = std::span{p.vertex_slots.data(), p.num_vertex_slots};
    return v;
}

ComputeView View(const ComputePipeline& p) {
    return {p.state, p.shader,
            {p.threads_per_group[0], p.threads_per_group[1], p.threads_per_group[2]}};
}

bool Prepare(Encoder& e, const RenderPipeline& p, const DrawState& s) {
    return Prepare(e, View(p), s);
}

bool Runs::Draw(Encoder& e, const Graphics& g, const DrawState& s, const Resources& r,
                const DrawArgs& a) {
    if (g.tessellation) {
        // A tessellated draw continues the run: the waiting patch draws stay queued.
        return DrawTessellated(e, *g.tessellation, g.buffers, s, r, a);
    }
    if (g.geometry) {
        EndRun(e);
        return DrawGeometry(e, *g.geometry, g.buffers, s, r, a);
    }
    if (!g.render) {
        EndRun(e);
        ++counters.skipped_no_pipeline;
        return false;
    }
    DrawPipeline view = View(*g.render);
    view.buffers = g.buffers;
    view.writes_images = g.writes_images;
    return Draw(e, view, s, r, a);
}

bool Runs::DrawIndirect(Encoder& e, const Graphics& g, const DrawState& s, const Resources& r,
                        const IndirectArgs& a) {
    EndRun(e);
    if (g.tessellation) {
        // The patch draw needs the kernels' output sized on the CPU (MoltenVK skips these too).
        ++counters.skipped_tess;
        BB_DRAW_WARN_ONCE("Metal: indirect tessellated draws are not encoded (skipped)");
        return false;
    }
    if (g.geometry) {
        return DrawGeometryIndirect(e, *g.geometry, g.buffers, s, r, a);
    }
    if (!g.render) {
        ++counters.skipped_no_pipeline;
        return false;
    }
    DrawPipeline view = View(*g.render);
    view.buffers = g.buffers;
    return DrawIndirect(e, view, s, r, a);
}

bool Runs::Dispatch(Encoder& e, const ComputePipeline& c, std::span<const Bound> bound,
                    std::span<const u8> push, Size3 groups) {
    return Dispatch(e, View(c), bound, push, groups);
}

bool Runs::DispatchIndirect(Encoder& e, const ComputePipeline& c, std::span<const Bound> bound,
                            std::span<const u8> push, BufferRef args) {
    return DispatchIndirect(e, View(c), bound, push, args);
}

/// A tessellated draw (as Huax-1839's RecordTessellationDraw): the vertex and control shaders as
/// compute kernels writing scratch memory now, the patch draw at the end of the run (Flush).
bool Runs::DrawTessellated(Encoder& e, const TessellationPipeline& t, Access buffers,
                           const DrawState& s, const Resources& r, const DrawArgs& a) {
    ++counters.tess_draws;
    if (last_draw_tess) {
        ++counters.tess_after_tess;
    }
    last_draw_tess = true;
    const u32 patches = t.input_control_points ? a.count / t.input_control_points : 0;
    if (!patches || !a.instances) {
        return false;
    }
    if (a.instances > 1) {
        // bbport: one tessellated draw per instance, each with its own kernels, scratch and
        // instance number (the vertex kernel's stage-in region, the patch draw's base
        // instance). They were skipped: whole objects missing (2026-10-21, ~1-7 draws a frame
        // in some areas). BB_TESS_INSTANCES=0 skips them again (A/B).
        static const bool skip = [] {
            const char* env = std::getenv("BB_TESS_INSTANCES");
            return env && env[0] == '0';
        }();
        if (skip) {
            ++counters.skipped_tess;
            BB_DRAW_WARN_ONCE("Metal: instanced tessellated draws are not encoded (skipped)");
            return false;
        }
        BB_DRAW_WARN_ONCE("Metal: instanced tessellated draws are drawn one instance at a time");
        DrawArgs one = a;
        one.instances = 1;
        bool drawn = false;
        for (u32 i = 0; i < a.instances; ++i) {
            one.first_instance = a.first_instance + i;
            drawn |= DrawTessellated(e, t, buffers, s, r, one);
        }
        return drawn;
    }
    if (!t.render) {
        ++counters.skipped_no_pipeline;
        return false;
    }
    DrawPipeline view = View(*t.render);
    view.buffers = buffers;
    // The patch draw would be skipped with the same state: spare the kernels.
    if (!Count(CheckDraw(e, view, s))) {
        return false;
    }
    Tessellation::DrawParams params;
    params.num_vertices = patches * t.input_control_points;
    params.first_vertex = a.indexed ? u32(a.vertex_offset) : a.first;
    params.first_instance = a.first_instance;
    if (a.indexed) {
        if (!r.indices.buffer) {
            ++counters.unsupported;
            BB_DRAW_WARN_ONCE("Metal: a tessellated indexed draw without indices (skipped)");
            return false;
        }
        const u32 size = IndexSize(r.index_type);
        params.index_type = size == 4 ? 2 : 1;
        params.index_buffer = r.indices.buffer;
        params.index_offset = r.indices.offset + u64(a.first) * size;
    }
    std::array<Tessellation::VertexBinding, 32> streams;
    const u32 num_streams =
        KernelStreams(std::span{t.vertex_slots.data(), t.num_vertex_slots}, r, nulls, streams);
    // Steps 1 and 2 (the run's kernels share one compute pass; the render pass reopens later).
    const auto plan = Tessellation::PlanScratch(t, patches);
    const BufferRef scratch = Scratch(e, plan.total);
    void* enc = e.ComputeEncoder(buffers);
    const auto bind = [&](const CompiledShader& shader) {
        BindKernelStage(enc, shader, r.bound, r.push, nulls);
    };
    Tessellation::EncodeKernels(enc, t, params, std::span{streams.data(), num_streams},
                                scratch.buffer, scratch.offset, plan, bind, bind);
    // Step 3 waits for the end of the run.
    PatchDraw patch;
    patch.control = {scratch.buffer, scratch.offset + plan.control_offset};
    patch.patch = {scratch.buffer, scratch.offset + plan.patch_offset};
    patch.factors = {scratch.buffer, scratch.offset + plan.factor_offset};
    patch.patches = plan.patches;
    patch.output_control_points = t.output_control_points;
    patch.instance = a.first_instance;
    QueuePatchDraw(view, s, r, patch);
    // Its render pass reads come with the run's end: the encoders until then hold this command.
    e.HoldOrderCommand();
    ++counters.draws;
    return true;
}

/// A geometry-shader draw (as Huax-1839's RecordGeometryDraw): per chunk of primitives the
/// export and geometry shaders as compute kernels writing vertex records and strip indices,
/// then the strip draws (pass-through vertex function) in the reopened render pass.
bool Runs::DrawGeometry(Encoder& e, const GeometryPipeline& g, Access buffers,
                        const DrawState& s, const Resources& r, const DrawArgs& a) {
    const u32 prims = g.info.input_vertices ? a.count / g.info.input_vertices : 0;
    if (!prims || !a.instances) {
        return false;
    }
    if (a.instances > 1) {
        BB_DRAW_WARN_ONCE("Metal: instanced geometry-shader draws draw instance 0 only");
    }
    if (!g.render) {
        ++counters.skipped_no_pipeline;
        return false;
    }
    DrawPipeline view = View(*g.render);
    view.buffers = buffers;
    if (!Count(CheckDraw(e, view, s))) {
        return false;
    }
    Tessellation::DrawParams params;
    params.num_vertices = prims * g.info.input_vertices;
    params.first_vertex = a.indexed ? u32(a.vertex_offset) : a.first;
    params.first_instance = a.first_instance;
    u64 index_size = 0;
    if (a.indexed) {
        if (!r.indices.buffer) {
            ++counters.unsupported;
            BB_DRAW_WARN_ONCE("Metal: a geometry-shader indexed draw without indices (skipped)");
            return false;
        }
        index_size = IndexSize(r.index_type);
        params.index_type = index_size == 4 ? 2 : 1;
        params.index_buffer = r.indices.buffer;
        params.index_offset = r.indices.offset + u64(a.first) * index_size;
    }
    std::array<Tessellation::VertexBinding, 32> streams;
    const u32 num_streams =
        KernelStreams(std::span{g.vertex_slots.data(), g.num_vertex_slots}, r, nulls, streams);
    struct ChunkDraw {
        Geometry::Plan plan;
        BufferRef span;
    };
    std::vector<ChunkDraw> chunks;
    const u32 chunk_prims = Geometry::MaxPrimitivesPerChunk(g);
    for (u32 at = 0; at < prims; at += chunk_prims) {
        const u32 n = std::min(chunk_prims, prims - at);
        ChunkDraw c;
        c.plan = Geometry::PlanScratch(g, n);
        c.span = Scratch(e, c.plan.total);
        Tessellation::DrawParams draw = params;
        draw.num_vertices = n * g.info.input_vertices;
        const u64 first_vertex = u64(at) * g.info.input_vertices;
        if (a.indexed) {
            draw.index_offset = params.index_offset + first_vertex * index_size;
        } else {
            draw.first_vertex = params.first_vertex + u32(first_vertex);
        }
        // (Asked per chunk: Scratch may not end the compute encoder, but stays correct if so.)
        void* enc = e.ComputeEncoder(buffers);
        const auto bind = [&](const CompiledShader& shader) {
            BindKernelStage(enc, shader, r.bound, r.push, nulls);
        };
        Geometry::EncodeKernels(enc, g, draw, at, std::span{streams.data(), num_streams},
                                c.span.buffer, c.span.offset, c.plan, bind, bind);
        chunks.push_back(c);
    }
    if (!Begin(e, view, s, r)) {
        return false;
    }
    for (const ChunkDraw& c : chunks) {
        // The pass-through function reads the records at its buffer 0.
        e.SetBuffer(Stage::Vertex, 0, {c.span.buffer, c.span.offset + c.plan.record_offset});
        e.DrawIndexed(view.primitive, u32(u64(c.plan.prims) * g.info.IndexSlots()),
                      IndexType::U32, {c.span.buffer, c.span.offset + c.plan.index_offset});
    }
    ++counters.draws;
    ++counters.geometry_draws;
    return true;
}

/// An indirect geometry-shader draw (as Huax-1839's RecordGeometryIndirectDraw): per record a
/// prep kernel reads the guest's arguments into a control block that sizes the export and
/// geometry kernels and the strip draw (mtl_geometry.h, "Indirect draws").
bool Runs::DrawGeometryIndirect(Encoder& e, const GeometryPipeline& g, Access buffers,
                                const DrawState& s, const Resources& r, const IndirectArgs& a) {
    if (!a.args.buffer || !a.count) {
        return false;
    }
    if (a.count_buffer.buffer) {
        ++counters.unsupported;
        BB_DRAW_WARN_ONCE("Metal: geometry-shader draws with a GPU draw count are not encoded "
                          "(skipped)");
        return false;
    }
    if (!g.render) {
        ++counters.skipped_no_pipeline;
        return false;
    }
    if (!geometry_kernels && !geometry_kernels_failed) {
        auto kernels = std::make_shared<Geometry::IndirectKernels>(device);
        if (kernels->Ok()) {
            geometry_kernels = std::move(kernels);
        } else {
            geometry_kernels_failed = true;
            LOG_ERROR(Render, "Metal: indirect geometry kernels: {}", kernels->Error());
        }
    }
    if (!geometry_kernels) {
        ++counters.skipped_emulated;
        return false;
    }
    DrawPipeline view = View(*g.render);
    view.buffers = buffers;
    if (!Count(CheckDraw(e, view, s))) {
        return false;
    }
    u32 index_size = 0;
    if (a.indexed) {
        if (!r.indices.buffer) {
            ++counters.unsupported;
            BB_DRAW_WARN_ONCE("Metal: an indirect geometry-shader indexed draw without indices "
                              "(skipped)");
            return false;
        }
        index_size = IndexSize(r.index_type);
    }
    std::array<Tessellation::VertexBinding, 32> streams;
    const u32 num_streams =
        KernelStreams(std::span{g.vertex_slots.data(), g.num_vertex_slots}, r, nulls, streams);
    const Geometry::IndirectPlan plan = Geometry::PlanIndirect(g, a.indexed);
    std::vector<BufferRef> spans;
    spans.reserve(a.count);
    for (u32 i = 0; i < a.count; ++i) {
        // (256-aligned: the geometry kernel's params are a constant buffer at +256.)
        const BufferRef raw = Scratch(e, plan.total + 256);
        const BufferRef span{raw.buffer, (raw.offset + 255) & ~u64(255)};
        spans.push_back(span);
        Geometry::IndirectDraw draw;
        draw.args = a.args.buffer;
        draw.args_offset = a.args.offset + u64(i) * a.stride;
        draw.index_size = index_size;
        if (a.indexed) {
            const u64 length = [(__bridge id<MTLBuffer>)r.indices.buffer length];
            draw.index_buffer = r.indices.buffer;
            draw.index_offset = r.indices.offset;
            draw.index_limit =
                r.indices.offset < length ? (length - r.indices.offset) / index_size : 0;
        }
        void* enc = e.ComputeEncoder(buffers);
        const auto bind = [&](const CompiledShader& shader) {
            BindKernelStage(enc, shader, r.bound, r.push, nulls);
        };
        Geometry::EncodeIndirectKernels(enc, g, *geometry_kernels, draw,
                                        std::span{streams.data(), num_streams}, span.buffer,
                                        span.offset, plan, bind, bind);
    }
    if (!Begin(e, view, s, r)) {
        return false;
    }
    for (const BufferRef& span : spans) {
        e.SetBuffer(Stage::Vertex, 0, {span.buffer, span.offset + plan.plan.record_offset});
        e.DrawIndexedIndirect(view.primitive, IndexType::U32,
                              {span.buffer, span.offset + plan.plan.index_offset},
                              {span.buffer, span.offset + Geometry::kControlDraw});
        ++counters.draws;
        ++counters.geometry_draws;
    }
    return true;
}

} // namespace Metal::Draw
