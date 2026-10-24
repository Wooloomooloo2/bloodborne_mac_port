// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: from Huax-1839/bloodborne-macos (ee3120a, GPL-2.0-or-later), renderer_metal/.

#pragma once

#include <array>
#include <functional>
#include <span>

#include "common/types.h"
#include "video_core/renderer_metal/mtl_pso_cache.h"
#include "video_core/renderer_metal/mtl_shader.h"

/// Tessellated draws on Metal (docs/METAL_BACKEND_PLAN.md D12, P4.4), the way MoltenVK builds them
/// with SPIRV-Cross. A PS4 draw with a hull and a domain shader (SPIR-V: vertex, tessellation
/// control and evaluation, then the fragment shader) is three steps of one command buffer:
///   1. the vertex shader as a compute kernel (CompileVariant::TessVertex): one thread per input
///      vertex, attributes through the stage-in descriptor (the vertex buffers, or the index
///      buffer for indexed draws), the outputs go to a scratch buffer;
///   2. the control shader as a compute kernel (TessControl): one thread per output control
///      point of every patch, reads step 1's buffer, writes the control points, the patch
///      constants and the tessellation factors (half precision structs) to three more;
///   3. the render pass: the evaluation shader as the post-tessellation vertex function
///      (TessEval) reads step 2's buffers, `drawPatches` with the factor buffer.
/// The buffers of one draw are one span of the staging ring (Plan); steps 1 and 2 need a compute
/// encoder, step 3 the render encoder, which the caller opens after the compute work (the
/// hazard-tracked scratch buffer orders the encoders). Nothing here knows the rasterizer: the
/// callers bind each stage's resources (BindStage) and the vertex/index buffers.
/// Direct draws of one instance (Runs::DrawTessellated encodes an instanced draw as one per instance).
///
/// Objective-C objects cross this header as `void*` (bridged), as in mtl_pso_cache.h.
namespace Metal::Tessellation {

/// maxTessellationFactor of the render pipeline (Metal's limit; the factors are clamped to it).
constexpr u32 kMaxTessellationFactor = 64;

/// MTLWinding of the tessellator's output for the evaluation shader's VertexOrder: Vulkan's
/// VertexOrderCw/Ccw describe the triangles as displayed (window space, y down) when the shader
/// maps the domain coordinates straight to clip space; the front-facing state of the draw is
/// the guest's. Proved by mtl-tessellation-test.
u32 OutputWinding(bool vertex_order_cw);
/// MTLTessellationPartitionMode of SpacingEqual / FractionalOdd / FractionalEven.
u32 PartitionMode(TessSpacing spacing);
/// Bytes of the factors of one patch: MTLTriangleTessellationFactorsHalf (4 halves) or
/// MTLQuadTessellationFactorsHalf (6 halves).
constexpr u32 FactorStride(bool triangles) {
    return triangles ? 8 : 12;
}

/// Offsets (from the start of the span) and sizes of the four scratch buffers of one draw.
struct Plan {
    u32 patches{};
    u64 vertex_offset{}, vertex_size{};   ///< the vertex kernel's outputs (input vertices)
    u64 control_offset{}, control_size{}; ///< control points (patches x output control points)
    u64 patch_offset{}, patch_size{};     ///< patch constants
    u64 factor_offset{}, factor_size{};   ///< tessellation factors
    u64 total{};                          ///< bytes of the span (every offset is a multiple of 16)
};
/// The scratch layout for `patches` patches of `pipeline`.
Plan PlanScratch(const TessellationPipeline& pipeline, u32 patches);

/// A guest vertex stream bound to the vertex kernel's stage input (the buffer slot, the stride of
/// the V#; constant streams (RenderPipeline's VertexBufferSlot::constant) pass any stride).
struct VertexBinding {
    void* buffer{}; ///< id<MTLBuffer>
    u64 offset{};
    u64 stride{};
    u32 slot{};
};

struct DrawParams {
    /// Input vertices (a multiple of the patch size): the guest's index count of the draw.
    u32 num_vertices{};
    /// Not indexed: the first vertex; indexed: the base vertex (added to the index, as Vulkan's
    /// vertexOffset). Both are the stage-in region's origin.
    u32 first_vertex{};
    u32 first_instance{};
    /// 0: not indexed, 1: uint16 indices, 2: uint32 (CompileVariant::index_type).
    u32 index_type{};
    void* index_buffer{}; ///< id<MTLBuffer> of the indices, indexed draws
    u64 index_offset{};   ///< byte offset of the first index
};

using BindStage = std::function<void(const CompiledShader&)>;

/// Step 1 alone (also the export shader of a geometry draw, mtl_geometry.h): the vertex kernel
/// of `draw.index_type` of `kernels` on `compute_encoder`, `bind_vertex` called after its
/// pipeline is set; its outputs go to `output` (an id<MTLBuffer>) at `output_offset`.
/// With `indirect` the stage-in region and the threadgroup count come from a GPU buffer
/// (a geometry draw's indirect pass, P7.1b): `draw.num_vertices` / `first_vertex` /
/// `first_instance` are unused, the dispatch uses one thread per threadgroup so that the grid
/// is exactly the count (the kernel only bounds its threads by the grid size).
struct IndirectArgs {
    void* buffer{};              ///< id<MTLBuffer>
    u64 region_offset{};         ///< MTLStageInRegionIndirectArguments
    u64 dispatch_offset{};       ///< MTLDispatchThreadgroupsIndirectArguments
};
void EncodeVertexKernel(void* compute_encoder, const std::array<TessKernel, 3>& kernels,
                        const DrawParams& draw, std::span<const VertexBinding> vertex_buffers,
                        void* output, u64 output_offset, const BindStage& bind_vertex,
                        const IndirectArgs* indirect = nullptr);

/// Steps 1 and 2 into `compute_encoder` (an id<MTLComputeCommandEncoder>, open): sets each
/// kernel's pipeline, calls `bind_vertex` / `bind_control` after it for the kernel's resources
/// and push constants, binds the vertex/index/scratch buffers and dispatches. `scratch` is the
/// id<MTLBuffer> holding the span at `scratch_offset`.
void EncodeKernels(void* compute_encoder, const TessellationPipeline& pipeline,
                   const DrawParams& draw, std::span<const VertexBinding> vertex_buffers,
                   void* scratch, u64 scratch_offset, const Plan& plan,
                   const BindStage& bind_vertex, const BindStage& bind_control);

// bbport: step 3 (EncodeDraw into their RenderList) comes with our draw path (M2/M4).

} // namespace Metal::Tessellation
