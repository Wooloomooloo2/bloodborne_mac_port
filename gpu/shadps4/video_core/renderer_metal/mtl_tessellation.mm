// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: from Huax-1839/bloodborne-macos (ee3120a, GPL-2.0-or-later), renderer_metal/.

#import <Metal/Metal.h>

#include <algorithm>

#include "common/assert.h"
#include "video_core/renderer_metal/mtl_tessellation.h"

namespace Metal::Tessellation {

u32 OutputWinding(bool vertex_order_cw) {
    // Measured by mtl-tessellation-test (the "winding" cases): with the domain coordinates
    // mapped straight to clip space, VertexOrderCw triangles are clockwise on screen when the
    // tessellator's winding is MTLWindingCounterClockwise, and the other way round (Metal's
    // domain origin is the upper left as Vulkan's default, but its winding names the order in
    // the lower-left domain space).
    return static_cast<u32>(vertex_order_cw ? MTLWindingCounterClockwise : MTLWindingClockwise);
}

u32 PartitionMode(TessSpacing spacing) {
    switch (spacing) {
    case TessSpacing::FractionalOdd:
        return static_cast<u32>(MTLTessellationPartitionModeFractionalOdd);
    case TessSpacing::FractionalEven:
        return static_cast<u32>(MTLTessellationPartitionModeFractionalEven);
    case TessSpacing::Equal:
        break;
    }
    return static_cast<u32>(MTLTessellationPartitionModeInteger);
}

namespace {

constexpr u64 kAlign = 16;

constexpr u64 Section(u64 count, u64 stride) {
    // At least one element's worth, so that an unused buffer is still a valid binding.
    return (std::max<u64>(count * stride, kAlign) + kAlign - 1) / kAlign * kAlign;
}

NSUInteger ThreadsPerGroup(const TessKernel& kernel) {
    return std::max<NSUInteger>(1, std::min<NSUInteger>(kernel.max_total_threads, 64));
}

} // namespace

Plan PlanScratch(const TessellationPipeline& pipeline, u32 patches) {
    Plan plan;
    plan.patches = patches;
    const u64 vertices = u64(patches) * pipeline.input_control_points;
    const u64 control_points = u64(patches) * pipeline.output_control_points;
    plan.vertex_size = Section(vertices, pipeline.vertex_stride);
    plan.control_size = Section(control_points, pipeline.control_point_stride);
    plan.patch_size = Section(patches, pipeline.patch_stride);
    plan.factor_size = Section(patches, pipeline.factor_stride);
    plan.vertex_offset = 0;
    plan.control_offset = plan.vertex_offset + plan.vertex_size;
    plan.patch_offset = plan.control_offset + plan.control_size;
    plan.factor_offset = plan.patch_offset + plan.patch_size;
    plan.total = plan.factor_offset + plan.factor_size;
    return plan;
}

void EncodeVertexKernel(void* compute_encoder, const std::array<TessKernel, 3>& kernels,
                        const DrawParams& draw, std::span<const VertexBinding> vertex_buffers,
                        void* output, u64 output_offset, const BindStage& bind_vertex,
                        const IndirectArgs* indirect) {
    id<MTLComputeCommandEncoder> enc = (__bridge id<MTLComputeCommandEncoder>)compute_encoder;
    ASSERT(draw.index_type < kernels.size());
    const TessKernel& kernel = kernels[draw.index_type];
    ASSERT_MSG(kernel.state, "the vertex kernel of index type {} was not built", draw.index_type);
    [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)kernel.state];
    bind_vertex(*kernel.shader);
    for (const VertexBinding& binding : vertex_buffers) {
        [enc setBuffer:(__bridge id<MTLBuffer>)binding.buffer
                offset:binding.offset
       attributeStride:binding.stride
               atIndex:binding.slot];
    }
    if (draw.index_type != 0) {
        [enc setBuffer:(__bridge id<MTLBuffer>)draw.index_buffer
                offset:draw.index_offset
               atIndex:kTessIndexBuffer];
    }
    [enc setBuffer:(__bridge id<MTLBuffer>)output offset:output_offset atIndex:kTessOutputBuffer];
    // The region's origin is the first vertex (or the base vertex of an indexed draw, which
    // the stage input adds to the index) and the first instance: gl_VertexIndex and
    // gl_InstanceIndex count from it as Vulkan's do.
    if (indirect) {
        id<MTLBuffer> args = (__bridge id<MTLBuffer>)indirect->buffer;
        [enc setStageInRegionWithIndirectBuffer:args indirectBufferOffset:indirect->region_offset];
        [enc dispatchThreadgroupsWithIndirectBuffer:args
                               indirectBufferOffset:indirect->dispatch_offset
                              threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        return;
    }
    [enc setStageInRegion:MTLRegionMake2D(draw.first_vertex, draw.first_instance,
                                          draw.num_vertices, 1)];
    [enc dispatchThreads:MTLSizeMake(draw.num_vertices, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(ThreadsPerGroup(kernel), 1, 1)];
}

void EncodeKernels(void* compute_encoder, const TessellationPipeline& pipeline,
                   const DrawParams& draw, std::span<const VertexBinding> vertex_buffers,
                   void* scratch, u64 scratch_offset, const Plan& plan,
                   const BindStage& bind_vertex, const BindStage& bind_control) {
    id<MTLComputeCommandEncoder> enc = (__bridge id<MTLComputeCommandEncoder>)compute_encoder;
    id<MTLBuffer> scratch_buffer = (__bridge id<MTLBuffer>)scratch;

    // 1. The vertex shader: attributes in, one output struct per input vertex out.
    EncodeVertexKernel(compute_encoder, pipeline.vertex, draw, vertex_buffers, scratch,
                       scratch_offset + plan.vertex_offset, bind_vertex);

    // 2. The control shader: one thread per output control point of every patch.
    {
        const TessKernel& kernel = pipeline.control;
        ASSERT_MSG(kernel.state, "the control kernel was not built");
        [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)kernel.state];
        bind_control(*kernel.shader);
        [enc setBuffer:scratch_buffer offset:scratch_offset + plan.vertex_offset
               atIndex:kTessInputBuffer];
        [enc setBuffer:scratch_buffer offset:scratch_offset + plan.control_offset
               atIndex:kTessOutputBuffer];
        [enc setBuffer:scratch_buffer offset:scratch_offset + plan.patch_offset
               atIndex:kTessPatchBuffer];
        [enc setBuffer:scratch_buffer offset:scratch_offset + plan.factor_offset
               atIndex:kTessFactorBuffer];
        const u32 params[2] = {pipeline.input_control_points, plan.patches};
        [enc setBytes:params length:sizeof(params) atIndex:kTessParamsBuffer];
        [enc dispatchThreads:MTLSizeMake(u64(plan.patches) * pipeline.output_control_points, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(ThreadsPerGroup(kernel), 1, 1)];
    }
}


} // namespace Metal::Tessellation
