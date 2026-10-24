// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the rasterizer's draw helper (Metal-native renderer, M3): binds a draw's resources by
// the shaders' slot maps, applies its state, and encodes plain, rect-list, indexed, indirect,
// tessellated and geometry-shader draws and dispatches through Metal::Encoder. Ported from the
// interception-era mtl_frame.mm (BindStage, PrepareDraw, DrawSnap, scratch, TessellatedDraw,
// GeometryDraw, FlushPending, Draw*, Dispatch*).
//
// Bindings do not survive Encoder::Submit, so every draw and dispatch here binds everything it
// uses: pipeline, the whole DrawState, every slot of its shaders (null stand-ins for missing
// resources), push constants and vertex buffers. Callers pass the complete state each time.
//
// Split (unit tests link the core alone): the slot binding, draw preparation, plain / rect /
// indexed / indirect draws, dispatches, scratch memory and the tessellation run queue are in
// mtl_draw_core.mm (bbmetal_core, Vulkan-free); the members taking the PSO cache's pipeline
// types (RenderPipeline, TessellationPipeline, GeometryPipeline, ComputePipeline) and running the
// tessellation / geometry kernels are in mtl_draw.mm (bbgpu).

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_object.h"
#include "video_core/renderer_metal/mtl_shader.h"

namespace Metal {
class Device;
struct DepthStencilKey;
struct RenderPipeline;
struct TessellationPipeline;
struct GeometryPipeline;
struct ComputePipeline;
namespace Geometry {
class IndirectKernels;
}
} // namespace Metal

namespace Metal::Draw {

/// One bound resource element (buffer, texture or sampler), indexed by the shader's unified
/// binding (ShaderBinding::binding; array element e of a binding is at binding + e). Empty
/// fields: the null stand-in is bound.
struct Bound {
    BufferRef buffer{};
    u64 size{};         ///< bytes the shader may reach (the buffer-size table, OpArrayLength)
    void* texture{};    ///< id<MTLTexture>
    void* sampler{};    ///< id<MTLSamplerState>
};

/// Stand-ins bound for resources a shader declares but the draw does not provide.
struct Nulls {
    BufferRef buffer{};
    u32 buffer_size{};
    void* texture{}; ///< 2D RGBA8, zero
    void* sampler{}; ///< default sampler
};

/// Owns the stand-ins: a zeroed 64 KiB buffer, a zeroed 1x1 RGBA8 texture, a default sampler.
class NullResources {
public:
    explicit NullResources(Device& device);
    [[nodiscard]] Nulls Get() const;

private:
    Object buffer, texture, sampler;
};

/// Binds every resource a stage's compiled shader uses (null fallbacks for missing ones), push
/// constants at the shader's push-constant slot, and the buffer-size table when the shader has
/// one. Resources of a descriptor set other than 0 get the stand-ins.
void BindStage(Encoder& e, Stage stage, const CompiledShader& shader, std::span<const Bound> bound,
               std::span<const u8> push, const Nulls& nulls);
/// The same on a native compute encoder (id<MTLComputeCommandEncoder>, Encoder::ComputeEncoder):
/// the kernels of tessellated and geometry draws.
void BindKernelStage(void* compute_encoder, const CompiledShader& shader,
                     std::span<const Bound> bound, std::span<const u8> push, const Nulls& nulls);

/// What the helper needs of a render pipeline (RenderPipeline, View() in mtl_draw.mm); the
/// pointers belong to the pipeline cache and live as long as it.
struct DrawPipeline {
    void* state{}; ///< id<MTLRenderPipelineState>; null: the pipeline failed (draws skipped)
    Primitive primitive{Primitive::Triangle};
    /// The guest topology has no Metal equivalent and is not expanded (fans, quad lists,
    /// adjacency): skipped unless a rect list, tessellation or geometry pipeline.
    bool emulated{};
    bool rect_list{};    ///< the RectList rewrite: 6 vertices per rectangle of 3
    bool tessellation{}; ///< the evaluation stage: drawn by the tessellation path only
    bool geometry{};     ///< the pass-through stage: drawn by the geometry path only
    FillMode fill{FillMode::Fill};
    DepthClip depth_clip{DepthClip::Clip};
    bool has_depth{};   ///< the pipeline has a depth attachment format
    bool has_stencil{}; ///< ... a stencil attachment format
    const CompiledShader* vs{};
    const CompiledShader* fs{}; ///< null: depth-only
    std::span<const VertexBufferSlot> vertex_slots;
    /// What the shaders do to untracked buffers (Encoder::SetRenderPipeline); the cache's
    /// pipelines do not know, the rasterizer sets it from the guest shaders.
    Access buffers{Access::Write};
};

/// The dynamic state of one draw, in the guest's (Vulkan) conventions.
struct DrawState {
    /// Vulkan viewport; converted with RasterUtil::ToMetalViewport (negative Metal height, depth
    /// range clamped to [0, 1]). The [-w, w] depth convention is the vertex stage's: nothing
    /// here converts depth.
    float viewport_x{}, viewport_y{}, viewport_width{}, viewport_height{};
    float min_depth{}, max_depth{1.0f};
    /// Vulkan scissor (the offset may be negative), clamped to the target; empty: skipped.
    s32 scissor_x{}, scissor_y{};
    u32 scissor_width{}, scissor_height{};
    u32 target_width{}, target_height{}; ///< the render pass's size
    CullMode cull{CullMode::None};
    bool cull_all{}; ///< front and back culled (Metal has no such mode): skipped
    Winding front_face{Winding::CounterClockwise};
    bool depth_bias_enable{};
    float depth_bias_constant{}, depth_bias_clamp{}, depth_bias_slope{};
    u32 stencil_ref_front{}, stencil_ref_back{};
    std::array<float, 4> blend_color{};
    void* depth_stencil{}; ///< id<MTLDepthStencilState> (DepthStencilCache; see MaskDepthStencil)
    bool rasterizer_discard{}; ///< skipped (Metal pipelines cannot disable rasterization here)
};

/// The depth/stencil key reduced to what the pipeline's attachments have (no depth format: no
/// depth test; no stencil format: no stencil test), ignored fields zeroed so equal states share
/// one MTLDepthStencilState.
DepthStencilKey MaskDepthStencil(const DepthStencilKey& key, bool has_depth, bool has_stencil);

enum class Skip : u8 {
    None,
    NoPass,       ///< no render pass in progress
    NoPipeline,   ///< the pipeline failed or is not built
    Emulated,     ///< a topology Metal lacks and nothing expands
    Discard,      ///< rasterizer discard
    CullAll,      ///< front-and-back culling
    EmptyScissor, ///< the scissor misses the target
};
/// Why the draw must be skipped (Skip::None: it is drawn).
Skip CheckDraw(const Encoder& e, const DrawPipeline& p, const DrawState& s);

/// Sets the pipeline and the whole DrawState on the encoder. False (nothing set) when the draw
/// must be skipped (CheckDraw).
bool Prepare(Encoder& e, const DrawPipeline& p, const DrawState& s);
/// RenderPipeline form (mtl_draw.mm).
bool Prepare(Encoder& e, const RenderPipeline& p, const DrawState& s);
/// The helper's view of a cache pipeline (mtl_draw.mm).
DrawPipeline View(const RenderPipeline& p);

/// A guest vertex stream, by attribute location: the buffer and the V#'s stride.
struct VertexStream {
    BufferRef buffer{};
    u32 stride{};
};

/// Bytes per index.
constexpr u32 IndexSize(IndexType type) {
    return type == IndexType::U32 ? 4 : 2;
}

/// The resources of one draw or dispatch. Spans are read during the call (tessellation runs
/// copy them).
struct Resources {
    std::span<const Bound> bound; ///< by unified binding, shared by the stages
    std::span<const u8> push;     ///< push constants (all stages)
    std::span<const VertexStream> streams;
    BufferRef indices{}; ///< indexed draws: the index buffer at the bound offset
    IndexType index_type{IndexType::U16};
};

/// The attribute stride a vertex slot is bound with, for draws and for the tessellation /
/// geometry kernels' stage-in streams alike: the V#'s stride (0 repeats element 0, as in
/// Vulkan); 0 for constant (per-instance, divisor 0) slots and for missing streams (the null
/// stand-in's element 0, however far the draw indexes). Under MTL_DEBUG_LAYER Metal accepts, and
/// reads correctly, these attribute strides on dynamic-stride per-vertex layouts: 0 (30000
/// vertices on a 16-byte buffer or on the stand-in), and strides and offsets that are not
/// multiples of 4 (stride 6 with a 2-byte format, at offset 0 and 2; stride 6 at offset 2 with a
/// 4-byte float; stride 3 is accepted too). Guest strides are passed through unchanged
/// (tests/test_mtl_draw.mm TestPerVertexStrideEdgeCases).
constexpr u32 StreamStride(const VertexBufferSlot& slot, const VertexStream* stream) {
    return stream && stream->buffer.buffer && !slot.constant ? stream->stride : 0;
}

/// Binds the vertex streams at the pipeline's vertex slots (StreamStride; missing streams get
/// the null stand-in).
void BindVertexStreams(Encoder& e, std::span<const VertexBufferSlot> slots,
                       std::span<const VertexStream> streams, const Nulls& nulls);

struct DrawArgs {
    u32 count{};          ///< vertices, or indices for indexed draws
    u32 instances{1};
    u32 first{};          ///< first vertex, or first index for indexed draws
    s32 vertex_offset{};  ///< indexed draws: added to each index
    u32 first_instance{};
    bool indexed{};
};

struct IndirectArgs {
    BufferRef args{}; ///< VkDrawIndirectCommand / VkDrawIndexedIndirectCommand records
    u32 count{1};     ///< records (with `count_buffer`: the most records)
    u32 stride{};     ///< bytes between records
    bool indexed{};
    BufferRef count_buffer{}; ///< GPU record count (vkCmdDraw*IndirectCount), or none
};

/// What one compute dispatch needs of a compute pipeline (View() in mtl_draw.mm).
struct ComputeView {
    void* state{}; ///< id<MTLComputePipelineState>
    const CompiledShader* shader{};
    Size3 threads_per_group{};
    Access buffers{Access::Write}; ///< as DrawPipeline::buffers
};
ComputeView View(const ComputePipeline& p);

/// A patch draw whose kernels already ran (the scratch spans the evaluation stage reads).
struct PatchDraw {
    BufferRef control{}; ///< control points (kTessInputBuffer)
    BufferRef patch{};   ///< patch constants (kTessPatchBuffer)
    BufferRef factors{}; ///< tessellation factors (kTessFactorBuffer, factor buffer)
    u32 patches{};
    u32 output_control_points{};
    u32 instance{}; ///< the instance this patch draw stands for (its base instance)
};

/// The skip counters already reported (DescribeSkips).
struct SkipTotals {
    std::array<u64, 8> values{};
};

/// A draw/dispatch recorder for one Encoder (one thread): counters, scratch memory and the
/// tessellation runs. Tessellated draws run their kernels at once but defer the patch draws to
/// the end of the run of tessellated draws, so the kernels of a run share one compute pass and
/// the patch draws one reopened render pass; each deferred draw keeps a copy of its state and
/// bound slots. Every non-tessellated draw and dispatch here draws the waiting patches first;
/// callers end passes and submit through EndRenderPass() / Submit() here (which flush first)
/// and call Flush() before anything else that needs the run ordered (copies, clears).
class Runs {
public:
    Runs(Device& device, const Nulls& nulls);
    ~Runs();
    Runs(const Runs&) = delete;
    Runs& operator=(const Runs&) = delete;

    /// A plain, rect-list or indexed draw. False when skipped (counted).
    bool Draw(Encoder& e, const DrawPipeline& p, const DrawState& s, const Resources& r,
              const DrawArgs& a);
    /// Indirect draws (`a.count` records, or a GPU count). Rect lists are skipped.
    bool DrawIndirect(Encoder& e, const DrawPipeline& p, const DrawState& s, const Resources& r,
                      const IndirectArgs& a);
    /// Queues a patch draw (whose kernels ran) for the end of the run: `s` and `r` are copied.
    /// The pipeline must have no vertex slots (post-tessellation functions take no attributes;
    /// asserted), so the vertex streams are not part of the copy.
    void QueuePatchDraw(const DrawPipeline& p, const DrawState& s, const Resources& r,
                        const PatchDraw& patch);
    /// Encodes the waiting patch draws, in order, each with its own state and slots. Asserts that
    /// a render pass is in progress when any are waiting.
    void Flush(Encoder& e);
    /// Flush, then Encoder::EndRenderPass.
    void EndRenderPass(Encoder& e);
    /// Flush (inside a pass), Encoder::Submit, then Submitted() with its tick; returns the tick.
    u64 Submit(Encoder& e);
    [[nodiscard]] bool HasPending() const noexcept {
        return !pending.empty();
    }

    bool Dispatch(Encoder& e, const ComputeView& c, std::span<const Bound> bound,
                  std::span<const u8> push, Size3 groups);
    bool DispatchIndirect(Encoder& e, const ComputeView& c, std::span<const Bound> bound,
                          std::span<const u8> push, BufferRef args);

    // ---- Cache pipelines (mtl_draw.mm) ----
    /// The pipelines of a GraphicsPipeline (Render(), Tessellation(), Geometry()).
    struct Graphics {
        const RenderPipeline* render{};
        const TessellationPipeline* tessellation{};
        const GeometryPipeline* geometry{};
        Access buffers{Access::Write}; ///< as DrawPipeline::buffers (every stage, kernels too)
    };
    /// Routes to the tessellation, geometry or plain path.
    bool Draw(Encoder& e, const Graphics& g, const DrawState& s, const Resources& r,
              const DrawArgs& a);
    /// Indirect: geometry draws through the indirect kernels; tessellated ones are skipped.
    bool DrawIndirect(Encoder& e, const Graphics& g, const DrawState& s, const Resources& r,
                      const IndirectArgs& a);
    bool Dispatch(Encoder& e, const ComputePipeline& c, std::span<const Bound> bound,
                  std::span<const u8> push, Size3 groups);
    bool DispatchIndirect(Encoder& e, const ComputePipeline& c, std::span<const Bound> bound,
                          std::span<const u8> push, BufferRef args);

    /// Scratch memory for kernels (16-byte aligned, private storage) from 8 MiB chunks, kept
    /// alive by the encoder's command buffer; chunks return to a pool once Submitted()'s tick is
    /// done.
    BufferRef Scratch(Encoder& e, u64 size);
    /// Call after Encoder::Submit with its tick (Submit() does): the chunks used so far belong to
    /// it. Asserts that no patch draw is pending.
    void Submitted(u64 device_tick);

    struct Counters {
        std::atomic<u64> draws{}, dispatches{}, tess_draws{}, tess_after_tess{},
            geometry_draws{};
        // Dropped draws and dispatches, by reason (published by ReportSkips).
        std::atomic<u64> skipped_no_pass{}, skipped_no_pipeline{}, skipped_tess{},
            skipped_emulated{}, unsupported{}, skipped_discard{}, skipped_cull_all{},
            skipped_empty_scissor{};
    };
    [[nodiscard]] const Counters& GetCounters() const noexcept {
        return counters;
    }
    /// Logs the dropped draws by reason since the last report (DescribeSkips), at most every
    /// 5 seconds and only when there are any. Called after each submission.
    void ReportSkips();

private:
    struct Pending {
        DrawPipeline pipeline;
        DrawState state;
        std::vector<Bound> bound;
        std::vector<u8> push;
        PatchDraw patch;
    };
    struct Chunk {
        Object buffer;
        u64 length{};
        u64 tick{};
    };

    /// Counts the skip reason; true when the draw goes ahead.
    bool Count(Skip skip);
    /// Prepare + resources of both stages + vertex streams.
    bool Begin(Encoder& e, const DrawPipeline& p, const DrawState& s, const Resources& r);
    /// Ends a tessellation run before any other command.
    void EndRun(Encoder& e);

    bool DrawTessellated(Encoder& e, const TessellationPipeline& t, Access buffers,
                         const DrawState& s, const Resources& r, const DrawArgs& a);
    bool DrawGeometry(Encoder& e, const GeometryPipeline& g, Access buffers, const DrawState& s,
                      const Resources& r, const DrawArgs& a);
    bool DrawGeometryIndirect(Encoder& e, const GeometryPipeline& g, Access buffers,
                              const DrawState& s, const Resources& r, const IndirectArgs& a);

    Device& device;
    Nulls nulls;
    Counters counters;
    std::vector<Pending> pending;
    bool last_draw_tess{};
    // Scratch.
    Object chunk;     ///< current chunk (id<MTLBuffer>)
    u64 chunk_length{};
    u64 chunk_used{};
    std::vector<Chunk> used;     ///< chunks of the command buffer being recorded
    std::vector<Chunk> inflight; ///< chunks of submitted command buffers (tick set)
    std::vector<Chunk> pool;     ///< free chunks
    /// The indirect geometry kernels, made on the first indirect geometry draw (mtl_draw.mm;
    /// shared_ptr: deleted where it was made).
    std::shared_ptr<Geometry::IndirectKernels> geometry_kernels;
    bool geometry_kernels_failed{};
    // ReportSkips.
    SkipTotals skips_reported;
    std::chrono::steady_clock::time_point skips_reported_at{std::chrono::steady_clock::now()};
};

/// The dropped draws per reason since `last` ("no pass 1, discard 2", non-zero reasons only;
/// empty when nothing new), then `last` = `now`.
std::string DescribeSkips(const Runs::Counters& now, SkipTotals& last);

} // namespace Metal::Draw
