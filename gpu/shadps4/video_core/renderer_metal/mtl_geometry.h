// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: from Huax-1839/bloodborne-macos (ee3120a, GPL-2.0-or-later), renderer_metal/;
// the geometry draws themselves are in mtl_draw.mm (Runs::DrawGeometry, DrawGeometryIndirect).

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>

#include "common/types.h"
#include "video_core/renderer_metal/mtl_pso_cache.h"
#include "video_core/renderer_metal/mtl_shader.h"
#include "video_core/renderer_metal/mtl_tessellation.h"

/// Geometry-shader draws on Metal (docs/metal/geometry_shader_design.md, plan P7.1). A PS4 draw
/// with an export and a geometry shader (SPIR-V: vertex, geometry, then the fragment shader) is
/// three steps of one command buffer:
///   1. the export shader as a compute kernel (CompileVariant::TessVertex, the same as the
///      vertex stage of a tessellated draw): one thread per input vertex, outputs into scratch;
///   2. the geometry shader as a compute kernel (CompileVariant::GeometryKernel,
///      mtl_spirv_geometry.h): one thread per input primitive, reads step 1's outputs and writes
///      vertex records and a strip index list per primitive;
///   3. the render pass: one indexed strip draw over all index slots (0xFFFFFFFF restarts the
///      strip, also for the unused slots), the vertex function is a generated pass-through that
///      reads the record of its vertex_id (MakePassThrough).
/// The buffers of one chunk of primitives are one span of the staging ring (Plan); a draw is
/// split into chunks that fit one span (MaxPrimitivesPerChunk). Steps 1 and 2 need a compute
/// encoder, step 3 the render encoder (as in mtl_tessellation.h). Direct draws of one instance
/// only. Objective-C objects cross this header as `void*` (bridged).
namespace Metal {
class Device;
}

namespace Metal::Geometry {

/// One chunk's scratch buffers are at most this large (the staging ring's blocks are 16 MiB).
constexpr u64 kMaxChunkBytes = 8ull << 20;

/// Offsets (from the start of the span) and sizes of the three scratch buffers of one chunk.
struct Plan {
    u32 prims{};
    u64 es_offset{}, es_size{};           ///< the export kernel's outputs
    u64 record_offset{}, record_size{};   ///< vertex records, max_vertices per primitive
    u64 index_offset{}, index_size{};     ///< uint32 strip indices, 2 * max_vertices per primitive
    u64 total{};                          ///< bytes of the span (every offset is a multiple of 16)
};

/// The most primitives one chunk may hold (at least 1).
u32 MaxPrimitivesPerChunk(const GeometryPipeline& pipeline);
Plan PlanScratch(const GeometryPipeline& pipeline, u32 prims);

/// Steps 1 and 2 of one chunk into `compute_encoder` (open). `draw` describes the export
/// kernel's vertices of the chunk (num_vertices = prims * input vertices; for an indexed draw
/// the index_offset already points at the chunk's first index, for a plain draw first_vertex
/// at its first vertex); `first_prim` is the chunk's first primitive in the draw (the geometry
/// shader's gl_PrimitiveID of thread 0). `bind_es` / `bind_gs` bind each kernel's resources and
/// push constants after its pipeline is set.
void EncodeKernels(void* compute_encoder, const GeometryPipeline& pipeline,
                   const Tessellation::DrawParams& draw, u32 first_prim,
                   std::span<const Tessellation::VertexBinding> vertex_buffers, void* scratch,
                   u64 scratch_offset, const Plan& plan, const Tessellation::BindStage& bind_es,
                   const Tessellation::BindStage& bind_gs);

// ---- Indirect draws (P7.1b) -------------------------------------------------------------------
//
// The guest's arguments are on the GPU, so nothing about the draw's size is known when it is
// recorded. One scratch span of a fixed budget (kMaxIndirectBytes: `capacity` primitives) holds a
// control block and the chunk's buffers, and three or four dispatches fill and use it:
//   0. prep (one thread): reads the guest's VkDrawIndirectCommand / VkDrawIndexedIndirectCommand
//      and writes into the control block: prims = min(count / input vertices, capacity, what the
//      index buffer holds), the threadgroup counts of the next dispatches, the export kernel's
//      stage-in region, the geometry kernel's params {prims, 0}, the strip draw's
//      MTLDrawIndexedPrimitivesIndirectArguments (indexCount = prims * 2 * max_vertices);
//   0b. (indexed draws) the used indices (firstIndex ...) are copied as uint32 into scratch: the
//      stage-in reads its index buffer from its binding offset, which is not known on the CPU;
//   1. the export kernel: dispatchThreadgroupsWithIndirectBuffer, one thread per threadgroup
//      (the grid is exactly the vertex count: the SPIRV-Cross kernel bounds its threads by the
//      grid size only), stage-in region from the control block;
//   2. the geometry kernel: indirect threadgroups of 64, bounded by prim_count in the kernel;
//   3. the strip draw: drawIndexedPrimitives with the control block's arguments.
// Instances > 1 draw instance 0 only (counted, with the truncations, in a small GPU counter buffer
// the CPU reads for the stats line).

/// Scratch budget of one indirect draw (the staging ring's blocks are 16 MiB).
constexpr u64 kMaxIndirectBytes = 12ull << 20;

// Byte offsets in the control block (the scratch span starts with it; 256-aligned span).
constexpr u64 kControlCopyGroups = 0;  ///< MTLDispatchThreadgroupsIndirectArguments of the copy
constexpr u64 kControlEsGroups = 16;   ///< ... of the export kernel
constexpr u64 kControlGsGroups = 32;   ///< ... of the geometry kernel
constexpr u64 kControlStageIn = 48;    ///< MTLStageInRegionIndirectArguments (24 bytes)
constexpr u64 kControlDraw = 80;       ///< MTLDrawIndexedPrimitivesIndirectArguments (20 bytes)
constexpr u64 kControlCopyInfo = 112;  ///< {first index, count, index size} for the copy kernel
constexpr u64 kControlParams = 256;    ///< the geometry kernel's {prim_count, first_prim} (constant
                                       ///< buffer: 256-aligned)
constexpr u64 kControlSize = 512;

struct IndirectPlan {
    u32 capacity{};      ///< primitives the buffers hold
    Plan plan;           ///< the chunk's buffers (offsets include the control block)
    u64 copy_offset{}, copy_size{}; ///< uint32 copy of the draw's indices (indexed draws)
    u64 total{};
};
IndirectPlan PlanIndirect(const GeometryPipeline& pipeline, bool indexed);

/// The prep and copy kernels and the counter buffer; compiled once (the Rasterizer makes it on the
/// first indirect geometry draw). Objective-C objects as `void*`.
class IndirectKernels {
public:
    explicit IndirectKernels(Device& device);
    ~IndirectKernels();
    [[nodiscard]] bool Ok() const {
        return prep && copy && counters;
    }
    [[nodiscard]] const std::string& Error() const {
        return error;
    }
    struct Counters {
        u64 draws{};     ///< indirect geometry draws run
        u64 instanced{}; ///< ... with more than one instance (only instance 0 was drawn)
        u64 truncated{}; ///< ... with more primitives than the capacity
        u64 primitives{};     ///< input primitives drawn (wraps at 2^32)
        u64 max_primitives{}; ///< the most of one draw
    };
    /// Read from the GPU's counter buffer (may lag the running work by a frame).
    [[nodiscard]] Counters Read() const;

    void* prep{};     ///< id<MTLComputePipelineState> (+1)
    void* copy{};     ///< id<MTLComputePipelineState> (+1)
    void* counters{}; ///< id<MTLBuffer> (+1), shared storage

private:
    std::string error;
};

/// The guest's draw (arguments and, indexed, index buffer).
struct IndirectDraw {
    void* args{};          ///< id<MTLBuffer> of the guest's arguments
    u64 args_offset{};
    u32 index_size{};      ///< 0: not indexed, else the guest's index size in bytes (1, 2, 4)
    void* index_buffer{};  ///< id<MTLBuffer>
    u64 index_offset{};
    u64 index_limit{};     ///< indices the buffer holds from index_offset
};

/// Steps 0 to 2 into `compute_encoder` (open); the barriers between the dependent dispatches are
/// inserted here.
void EncodeIndirectKernels(void* compute_encoder, const GeometryPipeline& pipeline,
                           const IndirectKernels& kernels, const IndirectDraw& draw,
                           std::span<const Tessellation::VertexBinding> vertex_buffers,
                           void* scratch, u64 scratch_offset, const IndirectPlan& plan,
                           const Tessellation::BindStage& bind_es,
                           const Tessellation::BindStage& bind_gs);

// ---- BB_GS_DEBUG=1 (P7.1c; see GeometryDebugEnabled in mtl_pso_cache.h) ----------------------------
/// True for the first two draws of `pipeline`: the caller then reads the scratch span back with
/// DebugLog once the GPU is done with the tick (Scheduler::DeferOperation; the staging span is
/// not reused before the tick's command buffer has completed).
bool DebugWantsDraw(const GeometryPipeline& pipeline);
/// Logs the first 8 vertex records' positions and the strip indices of the first 2 primitives of
/// the span `span` (host pointer to the start of the draw's scratch span). `indirect`: the
/// primitive count is the one the prep kernel wrote into the control block, else `prims`.
void DebugLog(const GeometryPipeline& pipeline, const u8* span, const Plan& plan, bool indirect,
              u32 prims);

/// The MSL of the pass-through vertex function for records of `info` and a fragment shader with
/// the stage inputs `fs_inputs` (the function declares each as `[[user(locnK)]]` of the
/// fragment shader's type, zero for the locations the geometry shader does not write). With
/// `clip_minus_w_to_w` the depth is converted from [-w, w] to [0, w]: z = (z + w) * 0.5.
/// Empty with `error` set for an input type it cannot declare.
std::string PassThroughSource(const GeometryInfo& info, std::span<const StageInput> fs_inputs,
                              bool clip_minus_w_to_w, std::string& error);

/// The pass-through functions, compiled once per distinct source (record layout x fragment
/// inputs x depth conversion). Thread-safe; the shaders live as long as the cache.
class PassThroughCache {
public:
    explicit PassThroughCache(Device& device);
    ~PassThroughCache();

    /// A vertex-stage CompiledShader (no resources, no push constants), or null with `error`.
    const CompiledShader* Get(const GeometryInfo& info, std::span<const StageInput> fs_inputs,
                              bool clip_minus_w_to_w, std::string& error);

private:
    struct Entry {
        std::unique_ptr<CompiledShader> shader;
        std::string error;
    };
    Device& device;
    std::mutex mutex;
    std::map<std::string, Entry> entries;
};

} // namespace Metal::Geometry
