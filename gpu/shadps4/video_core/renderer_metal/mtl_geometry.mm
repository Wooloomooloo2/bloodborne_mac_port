// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: from Huax-1839/bloodborne-macos (ee3120a, GPL-2.0-or-later), renderer_metal/;
// the geometry draws themselves are in mtl_draw.mm (Runs::DrawGeometry, DrawGeometryIndirect).

#import <Metal/Metal.h>

#include <algorithm>
#include <bit>
#include <cstring>

#include <fmt/format.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_metal/mtl_device_impl.h"
#include "video_core/renderer_metal/mtl_geometry.h"

namespace Metal::Geometry {

namespace {

constexpr u64 kAlign = 16;

constexpr u64 Section(u64 bytes) {
    // At least one element's worth, so that an unused buffer is still a valid binding.
    return (std::max<u64>(bytes, kAlign) + kAlign - 1) / kAlign * kAlign;
}

NSUInteger ThreadsPerGroup(const TessKernel& kernel) {
    return std::max<NSUInteger>(1, std::min<NSUInteger>(kernel.max_total_threads, kGeometryThreads));
}

u64 BytesPerPrimitive(const GeometryPipeline& pipeline) {
    const GeometryInfo& info = pipeline.info;
    return u64(info.input_vertices) * pipeline.es_stride +
           u64(info.max_vertices) * info.RecordStride() + u64(info.IndexSlots()) * 4;
}

const char* ScalarName(ScalarType type) {
    switch (type) {
    case ScalarType::Int:
        return "int";
    case ScalarType::Uint:
        return "uint";
    default:
        return "float";
    }
}

} // namespace

u32 MaxPrimitivesPerChunk(const GeometryPipeline& pipeline) {
    // (Sections round up to 16 bytes each: leave room for three.)
    const u64 per_prim = std::max<u64>(BytesPerPrimitive(pipeline), 1);
    const u64 count = (kMaxChunkBytes - 3 * kAlign) / per_prim;
    return static_cast<u32>(std::clamp<u64>(count, 1, 1u << 20));
}

Plan PlanScratch(const GeometryPipeline& pipeline, u32 prims) {
    const GeometryInfo& info = pipeline.info;
    Plan plan;
    plan.prims = prims;
    plan.es_size = Section(u64(prims) * info.input_vertices * pipeline.es_stride);
    plan.record_size = Section(u64(prims) * info.max_vertices * info.RecordStride());
    plan.index_size = Section(u64(prims) * info.IndexSlots() * 4);
    plan.es_offset = 0;
    plan.record_offset = plan.es_offset + plan.es_size;
    plan.index_offset = plan.record_offset + plan.record_size;
    plan.total = plan.index_offset + plan.index_size;
    return plan;
}

void EncodeKernels(void* compute_encoder, const GeometryPipeline& pipeline,
                   const Tessellation::DrawParams& draw, u32 first_prim,
                   std::span<const Tessellation::VertexBinding> vertex_buffers, void* scratch,
                   u64 scratch_offset, const Plan& plan, const Tessellation::BindStage& bind_es,
                   const Tessellation::BindStage& bind_gs) {
    id<MTLComputeCommandEncoder> enc = (__bridge id<MTLComputeCommandEncoder>)compute_encoder;
    id<MTLBuffer> scratch_buffer = (__bridge id<MTLBuffer>)scratch;

    // 1. The export shader: one output struct per input vertex.
    Tessellation::EncodeVertexKernel(compute_encoder, pipeline.es, draw, vertex_buffers, scratch,
                                     scratch_offset + plan.es_offset, bind_es);

    // 2. The geometry shader: one thread per primitive.
    const TessKernel& kernel = pipeline.gs;
    ASSERT_MSG(kernel.state, "the geometry kernel was not built");
    [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)kernel.state];
    bind_gs(*kernel.shader);
    [enc setBuffer:scratch_buffer offset:scratch_offset + plan.es_offset atIndex:kGeoEsBuffer];
    [enc setBuffer:scratch_buffer offset:scratch_offset + plan.record_offset
           atIndex:kGeoRecordBuffer];
    [enc setBuffer:scratch_buffer offset:scratch_offset + plan.index_offset
           atIndex:kGeoIndexBuffer];
    const u32 params[2] = {plan.prims, first_prim};
    [enc setBytes:params length:sizeof(params) atIndex:kGeoParamsBuffer];
    [enc dispatchThreads:MTLSizeMake(plan.prims, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(ThreadsPerGroup(kernel), 1, 1)];
}


// ---- Indirect draws -------------------------------------------------------------------------------

IndirectPlan PlanIndirect(const GeometryPipeline& pipeline, bool indexed) {
    const u64 per_prim =
        BytesPerPrimitive(pipeline) + (indexed ? u64(pipeline.info.input_vertices) * 4 : 0);
    // (Sections round up to 16 bytes each: leave room for four.)
    const u64 budget = kMaxIndirectBytes - kControlSize - 4 * kAlign;
    IndirectPlan out;
    out.capacity =
        static_cast<u32>(std::clamp<u64>(budget / std::max<u64>(per_prim, 1), 1, 1u << 20));
    out.plan = PlanScratch(pipeline, out.capacity);
    out.plan.es_offset += kControlSize;
    out.plan.record_offset += kControlSize;
    out.plan.index_offset += kControlSize;
    out.copy_size = indexed ? Section(u64(out.capacity) * pipeline.info.input_vertices * 4) : 0;
    out.copy_offset = out.plan.index_offset + out.plan.index_size;
    out.total = out.copy_offset + out.copy_size;
    out.plan.total = out.total;
    return out;
}

namespace {

/// The prep kernel (one thread) and the index copy kernel. Control block layout: see
/// mtl_geometry.h (word index = byte offset / 4).
std::string IndirectSource() {
    const auto word = [](u64 bytes) { return std::to_string(bytes / 4); };
    const auto set = [&](u64 base, u32 index, const std::string& value) {
        return "    c[" + word(base) + " + " + std::to_string(index) + "] = " + value + ";\n";
    };
    std::string s = R"(#include <metal_stdlib>
using namespace metal;

struct Config {
    uint input_vertices;
    uint capacity;
    uint index_slots;
    uint index_limit;
    uint index_size; // 0: not indexed
};

kernel void bb_gs_prep(const device uint* args [[buffer(0)]], device uint* c [[buffer(1)]],
                       constant Config& cfg [[buffer(2)]], device atomic_uint* stats [[buffer(3)]]) {
    const bool indexed = cfg.index_size != 0;
    const uint count = args[0];
    const uint instances = args[1];
    const uint first = args[2];
    const uint base_vertex = indexed ? args[3] : 0u;
    const uint first_instance = indexed ? args[4] : args[3];
    const uint n = cfg.input_vertices;
    uint prims = count / n;
    if (instances == 0u) {
        prims = 0u;
    }
    if (instances > 1u) {
        atomic_fetch_add_explicit(stats + 1, 1u, memory_order_relaxed);
    }
    if (indexed) {
        const uint avail = cfg.index_limit > first ? cfg.index_limit - first : 0u;
        prims = min(prims, avail / n);
    }
    if (prims > cfg.capacity) {
        prims = cfg.capacity;
        atomic_fetch_add_explicit(stats + 2, 1u, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(stats, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(stats + 3, prims, memory_order_relaxed);
    atomic_fetch_max_explicit(stats + 4, prims, memory_order_relaxed);
    const uint verts = prims * n;
)";
    s += set(kControlCopyGroups, 0, "(verts + 63u) / 64u") + set(kControlCopyGroups, 1, "1u") +
         set(kControlCopyGroups, 2, "1u");
    // The export kernel runs one thread per threadgroup: the grid is exactly `verts`.
    s += set(kControlEsGroups, 0, "verts") + set(kControlEsGroups, 1, "1u") +
         set(kControlEsGroups, 2, "1u");
    s += set(kControlGsGroups, 0, "(prims + 63u) / 64u") + set(kControlGsGroups, 1, "1u") +
         set(kControlGsGroups, 2, "1u");
    // Stage-in region: origin (first vertex or base vertex, first instance, 0), size (verts, 1, 1).
    s += set(kControlStageIn, 0, "indexed ? base_vertex : first") +
         set(kControlStageIn, 1, "first_instance") + set(kControlStageIn, 2, "0u") +
         set(kControlStageIn, 3, "max(verts, 1u)") + set(kControlStageIn, 4, "1u") +
         set(kControlStageIn, 5, "1u");
    s += set(kControlDraw, 0, "prims * cfg.index_slots") +
         set(kControlDraw, 1, "prims != 0u ? 1u : 0u") + set(kControlDraw, 2, "0u") +
         set(kControlDraw, 3, "0u") + set(kControlDraw, 4, "0u");
    s += set(kControlCopyInfo, 0, "first") + set(kControlCopyInfo, 1, "verts") +
         set(kControlCopyInfo, 2, "cfg.index_size");
    s += set(kControlParams, 0, "prims") + set(kControlParams, 1, "0u");
    s += "}\n\n";
    s += R"(kernel void bb_gs_copy_indices(const device uchar* src [[buffer(0)]],
                                device uint* dst [[buffer(1)]],
                                const device uint* c [[buffer(2)]],
                                uint i [[thread_position_in_grid]]) {
)";
    s += "    const uint count = c[" + word(kControlCopyInfo) + " + 1];\n";
    s += "    if (i >= count) {\n        return;\n    }\n";
    s += "    const uint size = c[" + word(kControlCopyInfo) + " + 2];\n";
    s += "    const ulong at = (ulong(c[" + word(kControlCopyInfo) + "]) + i) * size;\n";
    s += R"(    uint v = src[at];
    if (size >= 2u) {
        v |= uint(src[at + 1]) << 8;
    }
    if (size == 4u) {
        v |= (uint(src[at + 2]) << 16) | (uint(src[at + 3]) << 24);
    }
    dst[i] = v;
}
)";
    return s;
}

} // namespace

IndirectKernels::IndirectKernels(Device& device) {
    @autoreleasepool {
        id<MTLDevice> mtl_device = device.GetImpl().device;
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion3_1;
        NSError* ns_error = nil;
        const std::string source = IndirectSource();
        id<MTLLibrary> library =
            [mtl_device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                                     options:options
                                       error:&ns_error];
        if (!library) {
            error = fmt::format("indirect geometry kernels: {}",
                                ns_error ? ns_error.localizedDescription.UTF8String : "?");
            return;
        }
        const auto make = [&](NSString* name) -> id<MTLComputePipelineState> {
            id<MTLFunction> function = [library newFunctionWithName:name];
            return function ? [mtl_device newComputePipelineStateWithFunction:function error:nil]
                            : nil;
        };
        id<MTLComputePipelineState> prep_state = make(@"bb_gs_prep");
        id<MTLComputePipelineState> copy_state = make(@"bb_gs_copy_indices");
        id<MTLBuffer> stats = [mtl_device newBufferWithLength:64
                                                      options:MTLResourceStorageModeShared];
        if (!prep_state || !copy_state || !stats) {
            error = "indirect geometry kernels: a pipeline or buffer could not be created";
            return;
        }
        stats.label = @"bbport geometry indirect counters";
        std::memset(stats.contents, 0, 64);
        prep = (__bridge_retained void*)prep_state;
        copy = (__bridge_retained void*)copy_state;
        counters = (__bridge_retained void*)stats;
    }
}

IndirectKernels::~IndirectKernels() {
    for (void* object : {prep, copy, counters}) {
        if (object) {
            id released = (__bridge_transfer id)object;
            (void)released;
        }
    }
}

IndirectKernels::Counters IndirectKernels::Read() const {
    Counters out;
    if (counters) {
        const u32* words = static_cast<const u32*>(((__bridge id<MTLBuffer>)counters).contents);
        out.draws = words[0];
        out.instanced = words[1];
        out.truncated = words[2];
        out.primitives = words[3];
        out.max_primitives = words[4];
    }
    return out;
}

void EncodeIndirectKernels(void* compute_encoder, const GeometryPipeline& pipeline,
                           const IndirectKernels& kernels, const IndirectDraw& draw,
                           std::span<const Tessellation::VertexBinding> vertex_buffers,
                           void* scratch, u64 scratch_offset, const IndirectPlan& plan,
                           const Tessellation::BindStage& bind_es,
                           const Tessellation::BindStage& bind_gs) {
    ASSERT(kernels.Ok());
    id<MTLComputeCommandEncoder> enc = (__bridge id<MTLComputeCommandEncoder>)compute_encoder;
    id<MTLBuffer> scratch_buffer = (__bridge id<MTLBuffer>)scratch;
    const bool indexed = draw.index_size != 0;
    const auto barrier = [&] { [enc memoryBarrierWithScope:MTLBarrierScopeBuffers]; };

    // 0. prep: the guest's arguments -> the control block.
    [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)kernels.prep];
    [enc setBuffer:(__bridge id<MTLBuffer>)draw.args offset:draw.args_offset atIndex:0];
    [enc setBuffer:scratch_buffer offset:scratch_offset atIndex:1];
    const u32 config[5] = {pipeline.info.input_vertices, plan.capacity, pipeline.info.IndexSlots(),
                           static_cast<u32>(std::min<u64>(draw.index_limit, ~0u)),
                           draw.index_size};
    [enc setBytes:config length:sizeof(config) atIndex:2];
    [enc setBuffer:(__bridge id<MTLBuffer>)kernels.counters offset:0 atIndex:3];
    [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    barrier();

    // 0b. the draw's indices as uint32 at the start of a buffer.
    if (indexed) {
        [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)kernels.copy];
        [enc setBuffer:(__bridge id<MTLBuffer>)draw.index_buffer
                offset:draw.index_offset
               atIndex:0];
        [enc setBuffer:scratch_buffer offset:scratch_offset + plan.copy_offset atIndex:1];
        [enc setBuffer:scratch_buffer offset:scratch_offset atIndex:2];
        [enc dispatchThreadgroupsWithIndirectBuffer:scratch_buffer
                               indirectBufferOffset:scratch_offset + kControlCopyGroups
                              threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        barrier();
    }

    // 1. the export kernel, its extent from the control block.
    Tessellation::DrawParams es_draw;
    if (indexed) {
        es_draw.index_type = 2;
        es_draw.index_buffer = scratch;
        es_draw.index_offset = scratch_offset + plan.copy_offset;
    }
    const Tessellation::IndirectArgs es_args{scratch, scratch_offset + kControlStageIn,
                                             scratch_offset + kControlEsGroups};
    Tessellation::EncodeVertexKernel(compute_encoder, pipeline.es, es_draw, vertex_buffers, scratch,
                                     scratch_offset + plan.plan.es_offset, bind_es, &es_args);
    barrier();

    // 2. the geometry kernel: indirect threadgroups, bounded by prim_count in the kernel.
    const TessKernel& kernel = pipeline.gs;
    ASSERT_MSG(kernel.state, "the geometry kernel was not built");
    [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)kernel.state];
    bind_gs(*kernel.shader);
    [enc setBuffer:scratch_buffer offset:scratch_offset + plan.plan.es_offset atIndex:kGeoEsBuffer];
    [enc setBuffer:scratch_buffer
            offset:scratch_offset + plan.plan.record_offset
           atIndex:kGeoRecordBuffer];
    [enc setBuffer:scratch_buffer
            offset:scratch_offset + plan.plan.index_offset
           atIndex:kGeoIndexBuffer];
    [enc setBuffer:scratch_buffer offset:scratch_offset + kControlParams atIndex:kGeoParamsBuffer];
    [enc dispatchThreadgroupsWithIndirectBuffer:scratch_buffer
                           indirectBufferOffset:scratch_offset + kControlGsGroups
                          threadsPerThreadgroup:MTLSizeMake(ThreadsPerGroup(kernel), 1, 1)];
}


// ---- BB_GS_DEBUG ------------------------------------------------------------------------------------

bool DebugWantsDraw(const GeometryPipeline& pipeline) {
    static std::mutex mutex;
    static std::map<const GeometryPipeline*, u32> draws;
    std::scoped_lock lock{mutex};
    return draws[&pipeline]++ < 2;
}

void DebugLog(const GeometryPipeline& pipeline, const u8* span, const Plan& plan, bool indirect,
              u32 prims) {
    const GeometryInfo& info = pipeline.info;
    const auto key = pipeline.render ? pipeline.render->key : Vulkan::GraphicsPipelineKey{};
    if (indirect) {
        prims = reinterpret_cast<const u32*>(span + kControlParams)[0];
    }
    const u32 stride_words = info.RecordStride() / 4;
    const u32* records = reinterpret_cast<const u32*>(span + plan.record_offset);
    const u32* indices = reinterpret_cast<const u32*>(span + plan.index_offset);
    std::string text = fmt::format("GS debug (gs {:#x}): {} primitives, max {} vertices; records 0..7 "
                                   "(x y z w):",
                                   key.stage_hashes[u32(Shader::SwStage::Geometry)], prims,
                                   info.max_vertices);
    const u32 records_shown = std::min<u32>(8, prims * info.max_vertices);
    for (u32 i = 0; i < records_shown; ++i) {
        const u32* r = records + u64(i) * stride_words;
        text += fmt::format(" [{}: {} {} {} {}]", i, std::bit_cast<float>(r[0]),
                            std::bit_cast<float>(r[1]), std::bit_cast<float>(r[2]),
                            std::bit_cast<float>(r[3]));
    }
    text += "; indices of the first primitives:";
    for (u32 p = 0; p < std::min<u32>(2, prims); ++p) {
        text += fmt::format(" prim {}:", p);
        for (u32 s = 0; s < info.IndexSlots(); ++s) {
            const u32 index = indices[u64(p) * info.IndexSlots() + s];
            text += index == 0xFFFFFFFFu ? " R" : fmt::format(" {}", index);
        }
    }
    LOG_WARNING(Render, "{}", text);
}

std::string PassThroughSource(const GeometryInfo& info, std::span<const StageInput> fs_inputs,
                              bool clip_minus_w_to_w, std::string& error) {
    const u32 stride_words = info.RecordStride() / 4;
    std::string members, body;
    for (const StageInput& input : fs_inputs) {
        if (input.type == ScalarType::Other || input.components < 1 || input.components > 4) {
            error = fmt::format("fragment input at location {} has a type the pass-through "
                                "function cannot declare",
                                input.location);
            return {};
        }
        const char* scalar = ScalarName(input.type);
        const std::string type =
            input.components == 1 ? scalar : fmt::format("{}{}", scalar, input.components);
        members += fmt::format("    {} locn{} [[user(locn{})]];\n", type, input.location,
                               input.location);
        // The geometry shader's slot for the location, if it writes one.
        const GeometryVarying* varying = nullptr;
        u32 slot = 0;
        for (u32 i = 0; i < info.varyings.size(); ++i) {
            if (info.varyings[i].location == input.location) {
                varying = &info.varyings[i];
                slot = i + 1;
            }
        }
        std::string value;
        for (u32 j = 0; j < input.components; ++j) {
            if (j) {
                value += ", ";
            }
            if (varying && j < varying->components) {
                value += fmt::format("as_type<{}>(r[{}])", scalar, slot * 4 + j);
            } else {
                value += fmt::format("{}(0)", scalar);
            }
        }
        body += fmt::format("    o.locn{} = {}({});\n", input.location,
                            input.components == 1 ? scalar : type, value);
    }
    std::string source = "#include <metal_stdlib>\nusing namespace metal;\n\n";
    source += "struct GsOut {\n    float4 position [[position]];\n" + members + "};\n\n";
    source += fmt::format("vertex GsOut bb_gs_pass(uint vid [[vertex_id]],\n"
                          "                      const device uint* records [[buffer(0)]]) {{\n"
                          "    const device uint* r = records + ulong(vid) * {}ul;\n"
                          "    GsOut o;\n"
                          "    float4 p = as_type<float4>(uint4(r[0], r[1], r[2], r[3]));\n",
                          stride_words);
    if (clip_minus_w_to_w) {
        // ConvertDepthMode of the recompiler (emit_spirv_special.cpp), for stages it does not
        // run in.
        source += "    p.z = (p.z + p.w) * 0.5f;\n";
    }
    source += "    o.position = p;\n" + body + "    return o;\n}\n";
    return source;
}

PassThroughCache::PassThroughCache(Device& device_) : device{device_} {}

PassThroughCache::~PassThroughCache() = default;

const CompiledShader* PassThroughCache::Get(const GeometryInfo& info,
                                            std::span<const StageInput> fs_inputs,
                                            bool clip_minus_w_to_w, std::string& error) {
    const std::string source = PassThroughSource(info, fs_inputs, clip_minus_w_to_w, error);
    if (source.empty()) {
        return nullptr;
    }
    std::scoped_lock lock{mutex};
    if (const auto it = entries.find(source); it != entries.end()) {
        error = it->second.error;
        return it->second.shader.get();
    }
    Entry& entry = entries[source];
    @autoreleasepool {
        id<MTLDevice> mtl_device = device.GetImpl().device;
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion3_1;
        if (@available(macOS 15.0, *)) {
            options.mathMode = MTLMathModeSafe;
        } else {
            options.fastMathEnabled = NO;
        }
        NSError* ns_error = nil;
        id<MTLLibrary> library =
            [mtl_device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                                     options:options
                                       error:&ns_error];
        id<MTLFunction> function =
            library ? [library newFunctionWithName:@"bb_gs_pass"] : nil;
        if (!function) {
            entry.error = fmt::format("pass-through function: {}",
                                      ns_error ? ns_error.localizedDescription.UTF8String
                                               : "function missing");
            LOG_ERROR(Render, "Metal: {}\n{}", entry.error, source);
        } else {
            library.label = @"bbport geometry pass-through";
            auto shader = std::make_unique<CompiledShader>();
            shader->stage = ShaderStage::Vertex;
            shader->entry = "bb_gs_pass";
            shader->bindings.push_back({~0u, ~0u, ShaderBinding::Kind::PushConstants,
                                        kPushConstantBuffer, 1});
            shader->uses_push_constants = false; // buffer 0 holds the records
            shader->num_buffers = 1;
            shader->library = (__bridge_retained void*)library;
            shader->function = (__bridge_retained void*)function;
            entry.shader = std::move(shader);
        }
    }
    error = entry.error;
    return entry.shader.get();
}

} // namespace Metal::Geometry
