// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: from Huax-1839/bloodborne-macos (ee3120a, GPL-2.0-or-later), renderer_metal/.

#import <Metal/Metal.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <pthread.h>
#include <thread>
#include <cstring>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <xxhash.h>

#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_metal/mtl_device_impl.h"
#include "video_core/renderer_metal/mtl_pso_cache.h"
#include "video_core/renderer_metal/mtl_tessellation.h"
#include "video_core/renderer_metal/vk_to_mtl.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"

namespace Metal {

namespace {

using Clock = std::chrono::steady_clock;

u64 UsSince(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
}

std::string FirstLine(NSError* error) {
    if (!error) {
        return "unknown error";
    }
    std::string text = error.localizedDescription.UTF8String;
    const auto end = text.find('\n');
    return end == std::string::npos ? text : text.substr(0, end);
}


struct RenderMapKey {
    Vulkan::GraphicsPipelineKey key;
    const void* vs{}; ///< the MTLFunction objects (the entry retains them: no address reuse)
    const void* fs{};
    u64 attributes_hash{};
    bool rect_list_triangles{};
    bool tessellation{};
    bool geometry{};

    bool operator==(const RenderMapKey& other) const noexcept {
        return vs == other.vs && fs == other.fs && attributes_hash == other.attributes_hash &&
               rect_list_triangles == other.rect_list_triangles &&
               tessellation == other.tessellation && geometry == other.geometry &&
               key == other.key;
    }
};

struct RenderMapHash {
    size_t operator()(const RenderMapKey& k) const noexcept {
        u64 h = XXH3_64bits(&k.key, sizeof(k.key));
        const u64 rest[4] = {reinterpret_cast<u64>(k.vs), reinterpret_cast<u64>(k.fs),
                             k.attributes_hash,
                             u64(k.rect_list_triangles) | (u64(k.tessellation) << 1) | (u64(k.geometry) << 2)};
        return XXH3_64bits_withSeed(rest, sizeof(rest), h);
    }
};

/// A tessellation pipeline: the stages' functions name the compiled variants.
struct TessMapKey {
    Vulkan::GraphicsPipelineKey key;
    const void* vertex{}; ///< the vertex kernel's function without an index buffer
    const void* control{};
    const void* eval{};
    const void* fs{};
    u64 attributes_hash{};

    bool operator==(const TessMapKey& other) const noexcept {
        return vertex == other.vertex && control == other.control && eval == other.eval &&
               fs == other.fs && attributes_hash == other.attributes_hash && key == other.key;
    }
};

struct TessMapHash {
    size_t operator()(const TessMapKey& k) const noexcept {
        u64 h = XXH3_64bits(&k.key, sizeof(k.key));
        const u64 rest[5] = {reinterpret_cast<u64>(k.vertex), reinterpret_cast<u64>(k.control),
                             reinterpret_cast<u64>(k.eval), reinterpret_cast<u64>(k.fs),
                             k.attributes_hash};
        return XXH3_64bits_withSeed(rest, sizeof(rest), h);
    }
};

/// A geometry pipeline: the stages' functions name the compiled variants.
struct GeometryMapKey {
    Vulkan::GraphicsPipelineKey key;
    const void* es{}; ///< the export kernel's function without an index buffer
    const void* gs{};
    const void* pass_through{};
    const void* fs{};
    u64 attributes_hash{};

    bool operator==(const GeometryMapKey& other) const noexcept {
        return es == other.es && gs == other.gs && pass_through == other.pass_through &&
               fs == other.fs && attributes_hash == other.attributes_hash && key == other.key;
    }
};

struct GeometryMapHash {
    size_t operator()(const GeometryMapKey& k) const noexcept {
        u64 h = XXH3_64bits(&k.key, sizeof(k.key));
        const u64 rest[5] = {reinterpret_cast<u64>(k.es), reinterpret_cast<u64>(k.gs),
                             reinterpret_cast<u64>(k.pass_through), reinterpret_cast<u64>(k.fs),
                             k.attributes_hash};
        return XXH3_64bits_withSeed(rest, sizeof(rest), h);
    }
};

struct ComputeMapKey {
    size_t value{};
    const void* function{};
    bool operator==(const ComputeMapKey&) const = default;
};

struct ComputeMapHash {
    size_t operator()(const ComputeMapKey& k) const noexcept {
        const u64 data[2] = {k.value, reinterpret_cast<u64>(k.function)};
        return XXH3_64bits(data, sizeof(data));
    }
};

std::string StageHashes(const Vulkan::GraphicsPipelineKey& key) {
    return fmt::format("vs {:#x} fs {:#x}", key.stage_hashes[u32(Shader::SwStage::Vertex)],
                       key.stage_hashes[u32(Shader::SwStage::Fragment)]);
}

} // namespace

struct PsoCache::Impl {
    id<MTLDevice> mtl_device;

    std::shared_mutex mutex; ///< guards the three maps
    std::unordered_map<RenderMapKey, std::unique_ptr<RenderPipeline>, RenderMapHash> render;
    std::unordered_map<ComputeMapKey, std::unique_ptr<ComputePipeline>, ComputeMapHash> compute;
    std::unordered_map<TessMapKey, std::unique_ptr<TessellationPipeline>, TessMapHash> tessellation;
    std::unordered_map<GeometryMapKey, std::unique_ptr<GeometryPipeline>, GeometryMapHash> geometry;

    std::mutex debug_mutex; ///< BB_GS_DEBUG: the magenta fragment functions by source
    std::map<std::string, std::unique_ptr<CompiledShader>> debug_fragments;

    // The binary archive. Metal cannot serialize an archive again once pipelines that reuse its
    // functions were added after a serialization (tests/test_mtl_archive.mm, cases k and l), so
    // the archive in use is only looked up, and every write (writer thread, Flush) is a fresh
    // archive built from the descriptors of all pipelines this session created or took from it.
    id<MTLBinaryArchive> archive; ///< the archive loaded at startup (or an empty one); nil: none
    std::mutex archive_mutex;     ///< guards the members up to `stop`
    NSMutableArray* recorded = [NSMutableArray array]; ///< one descriptor per PSO key and stage
    u64 unwritten{};              ///< pipelines compiled since the last successful write
    /// The last write attempt (or the cache's creation): the writer waits kWriteInterval after it.
    std::chrono::steady_clock::time_point last_write = std::chrono::steady_clock::now();
    /// How long the last write took: the next one waits at least kWriteSpacing times that.
    std::chrono::steady_clock::duration last_write_cost{};
    bool stop{};                  ///< the writer thread ends
    std::condition_variable writer_cv;
    std::thread writer;           ///< PsoCache::WriterLoop
    std::mutex flush_mutex;       ///< one write at a time (writer thread or Flush)
    std::string last_error;       ///< guarded by flush_mutex: logged once per distinct error
};

constexpr auto kWriteInterval = std::chrono::seconds{10};
/// A write re-adds every pipeline of the session (~2.4 ms each on an M4 Max: 1.3 s for ~300,
/// 5-7 s for the 2-3k of a long game), on a utility thread that shares the CPU with the game
/// and its compiles, most of all on Macs with fewer cores and in the session that rebuilds a
/// cache. Writes wait at least this many times the last one's duration (~5% of one core); the
/// exit flush (bbgpu_flush_caches) still writes everything left.
constexpr int kWriteSpacing = 20;

RenderPipeline::~RenderPipeline() {
    // Balance the +1 of __bridge_retained; ARC releases at the end of the scope.
    for (void* object : {state, vertex_function, fragment_function}) {
        if (object) {
            id released = (__bridge_transfer id)object;
            (void)released;
        }
    }
}

TessKernel::~TessKernel() {
    for (void* object : {state, function}) {
        if (object) {
            id released = (__bridge_transfer id)object;
            (void)released;
        }
    }
}

ComputePipeline::~ComputePipeline() {
    for (void* object : {state, function}) {
        if (object) {
            id released = (__bridge_transfer id)object;
            (void)released;
        }
    }
}

PsoCache::PsoCache(Device& device_, std::filesystem::path cache_dir)
    : device{device_}, impl{std::make_unique<Impl>()} {
    impl->mtl_device = device.GetImpl().device;
    if (cache_dir.empty()) {
        cache_dir = ShaderCompiler::DefaultCacheDir();
    }
    archive_path = cache_dir / "pipelines.binaryarchive";

    // MTL_SHADER_VALIDATION=1: the GPU validation device crashes loading a binary archive
    // (macOS 26.6, -[MTLGPUDebugDevice newBinaryArchiveWithDescriptor:], null dereference in
    // MTLMetalScriptBuilder::initFromScript); its instrumented pipelines don't belong in the
    // archive either. Compile without one.
    if (const char* env = std::getenv("MTL_SHADER_VALIDATION"); env && env[0] == '1') {
        LOG_WARNING(Render, "Metal binary archive off under MTL_SHADER_VALIDATION");
        return;
    }

    @autoreleasepool {
        std::error_code ec;
        std::filesystem::create_directories(cache_dir, ec);
        const bool exists = std::filesystem::exists(archive_path, ec);
        MTLBinaryArchiveDescriptor* descriptor = [MTLBinaryArchiveDescriptor new];
        if (exists) {
            descriptor.url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:archive_path.c_str()]];
        }
        NSError* error = nil;
        id<MTLBinaryArchive> archive =
            [impl->mtl_device newBinaryArchiveWithDescriptor:descriptor error:&error];
        if (archive && exists) {
            archive_loaded = true;
        } else if (!archive && exists) {
            // Another device or OS build, or a damaged file: start over.
            LOG_WARNING(Render, "Metal binary archive {} unusable ({}), starting a new one",
                        archive_path.string(), FirstLine(error));
            std::filesystem::remove(archive_path, ec);
            descriptor.url = nil;
            error = nil;
            archive = [impl->mtl_device newBinaryArchiveWithDescriptor:descriptor error:&error];
        }
        if (!archive) {
            LOG_WARNING(Render, "Metal binary archive not available ({}): pipelines are compiled "
                                "on every start",
                        FirstLine(error));
        }
        impl->archive = archive;
    }
    if (impl->archive) {
        impl->writer = std::thread{[this] { WriterLoop(); }};
    }
}

PsoCache::~PsoCache() {
    if (impl->writer.joinable()) {
        {
            std::scoped_lock lock{impl->archive_mutex};
            impl->stop = true;
        }
        impl->writer_cv.notify_all();
        impl->writer.join();
    }
    Flush();
}

void PsoCache::WriterLoop() {
    pthread_setname_np("bb:PsoArchive");
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    std::unique_lock lock{impl->archive_mutex};
    while (true) {
        impl->writer_cv.wait(lock, [&] { return impl->stop || impl->unwritten > 0; });
        if (impl->stop) {
            return;
        }
        // Everything compiled until then goes into one write (the destructor or, in the GPU host,
        // bbgpu_flush_caches writes the rest).
        const auto due =
            impl->last_write + std::max<std::chrono::steady_clock::duration>(
                                   kWriteInterval, impl->last_write_cost * kWriteSpacing);
        if (impl->writer_cv.wait_until(lock, due, [&] { return impl->stop; })) {
            return;
        }
        lock.unlock();
        {
            std::scoped_lock flush_lock{impl->flush_mutex};
            WriteArchive();
        }
        lock.lock();
    }
}

bool PsoCache::Flush() {
    std::scoped_lock flush_lock{impl->flush_mutex}; // waits for the writer's write in flight
    return WriteArchive();
}

bool PsoCache::WriteArchive() {
    NSArray* descriptors = nil;
    u64 unwritten = 0;
    {
        std::scoped_lock lock{impl->archive_mutex};
        if (!impl->archive || impl->unwritten == 0) {
            return true;
        }
        descriptors = [impl->recorded copy];
        unwritten = impl->unwritten;
    }
    const auto write_start = std::chrono::steady_clock::now();
    std::string error, first_add_error;
    u64 add_failures = 0;
    @autoreleasepool {
        NSError* ns_error = nil;
        id<MTLBinaryArchive> fresh =
            [impl->mtl_device newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new]
                                                       error:&ns_error];
        if (!fresh) {
            error = FirstLine(ns_error);
        }
        for (id descriptor in descriptors) {
            if (!fresh) {
                break;
            }
            ns_error = nil;
            const BOOL added =
                [descriptor isKindOfClass:[MTLRenderPipelineDescriptor class]]
                    ? [fresh addRenderPipelineFunctionsWithDescriptor:descriptor error:&ns_error]
                    : [fresh addComputePipelineFunctionsWithDescriptor:descriptor error:&ns_error];
            if (!added) {
                ++counters.archive_add_failures;
                ++add_failures;
                if (first_add_error.empty()) {
                    first_add_error = fmt::format("{}: {}", [[descriptor label] UTF8String] ?: "?",
                                                  FirstLine(ns_error));
                }
            }
        }
        if (fresh) {
            std::error_code ec;
            const std::filesystem::path temp = archive_path.string() + ".tmp";
            std::filesystem::remove(temp, ec);
            ns_error = nil;
            const BOOL ok = [fresh
                serializeToURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:temp.c_str()]]
                         error:&ns_error];
            if (ok) {
                std::filesystem::rename(temp, archive_path, ec);
            }
            if (!ok || ec) {
                error = ok ? ec.message() : FirstLine(ns_error);
                std::filesystem::remove(temp, ec);
            }
        }
    }
    if (add_failures) {
        LOG_WARNING(Render, "Metal binary archive: {} pipeline(s) could not be added and are left "
                            "out (first: {})",
                    add_failures, first_add_error);
    }
    std::scoped_lock lock{impl->archive_mutex};
    impl->last_write = std::chrono::steady_clock::now(); // failures back off too
    impl->last_write_cost = impl->last_write - write_start;
    if (!error.empty()) {
        ++counters.archive_write_failures;
        if (error != impl->last_error) {
            impl->last_error = error;
            LOG_WARNING(Render, "Metal binary archive {} could not be written: {}",
                        archive_path.string(), error);
        }
        return false; // `unwritten` stays: the next write tries again
    }
    ++counters.archive_writes;
    impl->unwritten -= unwritten;
    // One line per write (at most every kWriteInterval): whether the archive serves the
    // session (hits) or pipelines still compile.
    // The duration grows with the session's pipeline count (every write re-adds them all).
    std::printf("Metal binary archive: wrote %llu pipelines in %.0f ms; this session %llu from the "
                "archive, %llu compiled\n",
                static_cast<unsigned long long>(descriptors.count - add_failures),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                          write_start)
                    .count(),
                static_cast<unsigned long long>(counters.archive_hits.load()),
                static_cast<unsigned long long>(counters.compiled.load()));
    std::fflush(stdout);
    return true;
}

namespace {

/// Creates one pipeline state, from the loaded binary archive when it has it
/// (FailOnBinaryArchiveMiss: an archive hit skips the compiler), else by compiling it. The
/// descriptor goes to `record` (for the next archive write, RecordPipeline) either way.
template <typename Descriptor, typename Create>
id CreateWithArchive(PsoCache::Impl& impl, bool archive_loaded, Descriptor* descriptor,
                     Create create, NSMutableArray* record, bool& from_archive,
                     std::string& error) {
    from_archive = false;
    NSError* ns_error = nil;
    id state = nil;
    if (impl.archive && archive_loaded) {
        descriptor.binaryArchives = @[ impl.archive ];
        state = create(MTLPipelineOptionFailOnBinaryArchiveMiss, &ns_error);
        from_archive = state != nil;
        ns_error = nil;
    }
    if (!state) {
        state = create(MTLPipelineOptionNone, &ns_error);
    }
    descriptor.binaryArchives = nil;
    if (!state) {
        error = FirstLine(ns_error);
        return nil;
    }
    if (impl.archive) {
        [record addObject:descriptor];
    }
    return state;
}

/// Keeps the descriptors of a pipeline new in the cache for the archive writes (called once per
/// PSO key, so the list holds no duplicates); `compiled`: not all from the archive, so the
/// archive file lacks it.
void RecordPipeline(PsoCache::Impl& impl, NSArray* descriptors, bool compiled) {
    if (!impl.archive || descriptors.count == 0) {
        return;
    }
    {
        std::scoped_lock lock{impl.archive_mutex};
        [impl.recorded addObjectsFromArray:descriptors];
        impl.unwritten += compiled;
    }
    if (compiled) {
        impl.writer_cv.notify_one(); // the writer thread; this thread never writes
    }
}

} // namespace

const RenderPipeline* PsoCache::GetRenderPipeline(const Vulkan::GraphicsPipelineKey& key,
                                                       const CompiledShader* vs,
                                                       const CompiledShader* fs,
                                                       const RenderPipelineInput& input) {
    ASSERT(vs && vs->function);
    const RenderMapKey map_key{
        .key = key,
        .vs = vs->function,
        .fs = fs ? fs->function : nullptr,
        .attributes_hash = XXH3_64bits(input.vertex_attributes.data(),
                                       input.vertex_attributes.size_bytes()),
        .rect_list_triangles = input.rect_list_triangles,
        .tessellation = input.tessellation_eval,
        .geometry = input.geometry,
    };
    {
        std::shared_lock lock{impl->mutex};
        const auto it = impl->render.find(map_key);
        if (it != impl->render.end()) {
            return it->second->state ? it->second.get() : nullptr;
        }
    }

    namespace L = Vulkan::LiverpoolToVK;
    auto pipeline = std::make_unique<RenderPipeline>();
    pipeline->key = key;
    pipeline->vs = vs;
    pipeline->fs = fs;
    std::string error;
    bool from_archive = false;
    u64 elapsed_us = 0;
    NSMutableArray* record = [NSMutableArray array]; ///< the descriptors for the archive
    @autoreleasepool {
        do {
            // ---- primitive and encoder state ----
            const auto topology = L::PrimitiveType(key.prim_type);
            VkToMtl::PrimitiveInfo primitive = VkToMtl::PrimitiveType(topology);
            if (input.rect_list_triangles) {
                // The rewritten vertex shader emits the two triangles of each rectangle.
                primitive = {MTLPrimitiveTypeTriangle, false};
            }
            const bool tessellation = input.tessellation_eval;
            if (tessellation) {
                // `vs` is the evaluation shader: the tessellator makes the triangles, drawPatches
                // is the draw (the guest's patch list is not an emulated primitive).
                if (vs->variant.kind != CompileVariant::Kind::TessEval) {
                    error = "the vertex stage of a tessellation pipeline is not an evaluation shader";
                    break;
                }
                primitive = {MTLPrimitiveTypeTriangle, false};
                pipeline->tessellation = true;
                pipeline->patch_control_points = vs->variant.output_vertices;
            }
            if (input.geometry) {
                // `vs` is the pass-through function: it draws the strips of the geometry kernel.
                primitive = {static_cast<u32>(input.geometry_output == GsOutputPrimitive::LineStrip
                                                  ? MTLPrimitiveTypeLineStrip
                                                  : MTLPrimitiveTypeTriangleStrip),
                             false};
                pipeline->geometry = true;
            }
            pipeline->primitive_type = primitive.type;
            pipeline->primitive_emulated = primitive.needs_emulation;
            pipeline->rect_list = key.prim_type == AmdGpu::PrimitiveType::RectList;
            pipeline->quad_list = key.prim_type == AmdGpu::PrimitiveType::QuadList;
            const VkToMtl::FillInfo fill = VkToMtl::PolygonMode(L::PolygonMode(key.polygon_mode));
            pipeline->fill_mode = fill.mode;
            pipeline->fill_point = fill.needs_emulation;
            pipeline->depth_clip = key.depth_clip_enable;
            pipeline->provoking_vertex_last =
                key.provoking_vtx_last != AmdGpu::ProvokingVtxLast::First;
            pipeline->num_color_attachments = key.num_color_attachments;

            MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
            desc.label = [NSString stringWithUTF8String:StageHashes(key).c_str()];
            desc.vertexFunction = (__bridge id<MTLFunction>)vs->function;
            if (pipeline->fs) {
                desc.fragmentFunction = (__bridge id<MTLFunction>)fs->function;
            }
            if (tessellation) {
                // The tessellator's state comes from the evaluation shader's execution modes:
                // equal / fractional spacing, the order of the triangles it makes, a half
                // precision factor per patch (the control kernel's structs).
                const TessellationInfo& modes = vs->tess;
                desc.tessellationPartitionMode = static_cast<MTLTessellationPartitionMode>(
                    Tessellation::PartitionMode(modes.spacing));
                desc.tessellationFactorFormat = MTLTessellationFactorFormatHalf;
                desc.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
                desc.tessellationControlPointIndexType = MTLTessellationControlPointIndexTypeNone;
                desc.tessellationOutputWindingOrder = static_cast<MTLWinding>(
                    Tessellation::OutputWinding(modes.clockwise));
                desc.maxTessellationFactor = Tessellation::kMaxTessellationFactor;
            } else {
                switch (primitive.type) {
                case MTLPrimitiveTypePoint:
                    desc.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint;
                    break;
                case MTLPrimitiveTypeLine:
                case MTLPrimitiveTypeLineStrip:
                    desc.inputPrimitiveTopology = MTLPrimitiveTopologyClassLine;
                    break;
                default:
                    desc.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
                    break;
                }
            }

            // ---- sample count ----
            u32 samples = std::max<u32>(key.num_samples, 1);
            while (samples > 1 && ![impl->mtl_device supportsTextureSampleCount:samples]) {
                samples >>= 1;
            }
            pipeline->sample_count = samples;
            desc.rasterSampleCount = samples;

            // ---- attachments ----
            {
                for (u32 i = 0; i < key.num_color_attachments; ++i) {
                    if (!(key.mrt_mask & (1u << i))) {
                        continue; // a hole: not written by the fragment shader
                    }
                    const auto& cb = key.color_buffers[i];
                    const vk::Format format = L::TrySurfaceFormat(cb.data_format, cb.num_format);
                    if (format == vk::Format::eUndefined) {
                        continue; // written by the shader but not bound: Vulkan leaves it unused
                    }
                    const u32 pixel_format = VkToMtl::PixelFormat(format);
                    if (!pixel_format) {
                        error = fmt::format("color attachment {} format {} has no Metal pixel "
                                            "format",
                                            i, vk::to_string(format));
                        break;
                    }
                    pipeline->color_pixel_formats[i] = pixel_format;
                    MTLRenderPipelineColorAttachmentDescriptor* att = desc.colorAttachments[i];
                    att.pixelFormat = static_cast<MTLPixelFormat>(pixel_format);
                    att.writeMask = static_cast<MTLColorWriteMask>(
                        VkToMtl::ColorWriteMask(key.write_masks[i]));
                    const Vulkan::AttachmentBlend blend = Vulkan::AttachmentBlendState(key, i);
                    att.blendingEnabled = blend.enable;
                    if (blend.enable) {
                        att.sourceRGBBlendFactor =
                            static_cast<MTLBlendFactor>(VkToMtl::BlendFactor(blend.src_color));
                        att.destinationRGBBlendFactor =
                            static_cast<MTLBlendFactor>(VkToMtl::BlendFactor(blend.dst_color));
                        att.rgbBlendOperation =
                            static_cast<MTLBlendOperation>(VkToMtl::BlendOp(blend.color_op));
                        att.sourceAlphaBlendFactor =
                            static_cast<MTLBlendFactor>(VkToMtl::BlendFactor(blend.src_alpha));
                        att.destinationAlphaBlendFactor =
                            static_cast<MTLBlendFactor>(VkToMtl::BlendFactor(blend.dst_alpha));
                        att.alphaBlendOperation =
                            static_cast<MTLBlendOperation>(VkToMtl::BlendOp(blend.alpha_op));
                    }
                }
                if (!error.empty()) {
                    break;
                }
                const vk::Format depth_format = L::DepthFormat(key.z_format, key.stencil_format);
                const u32 depth_pixel = VkToMtl::PixelFormat(depth_format);
                if (depth_format != vk::Format::eUndefined && !depth_pixel) {
                    error = fmt::format("depth format {} has no Metal pixel format",
                                        vk::to_string(depth_format));
                    break;
                }
                if (key.z_format != AmdGpu::DepthBuffer::ZFormat::Invalid) {
                    pipeline->depth_pixel_format = depth_pixel;
                    desc.depthAttachmentPixelFormat = static_cast<MTLPixelFormat>(depth_pixel);
                }
                if (key.stencil_format != AmdGpu::DepthBuffer::StencilFormat::Invalid) {
                    pipeline->stencil_pixel_format = depth_pixel;
                    desc.stencilAttachmentPixelFormat = static_cast<MTLPixelFormat>(depth_pixel);
                }
            }

            // ---- vertex input ----
            const auto attributes = input.vertex_attributes;
            if (attributes.size() > kMaxVertexBuffers) {
                error = fmt::format("{} vertex attributes, at most {} fit", attributes.size(),
                                    kMaxVertexBuffers);
                break;
            }
            if (!attributes.empty()) {
                MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
                for (u32 i = 0; i < attributes.size(); ++i) {
                    const VertexAttribute& attribute = attributes[i];
                    const vk::Format format = key.vertex_buffer_formats[i];
                    const u32 vertex_format = VkToMtl::VertexFormat(format);
                    if (!vertex_format || attribute.location >= 31) {
                        error = fmt::format("vertex attribute {} (location {}) format {} has no "
                                            "Metal vertex format",
                                            i, attribute.location, vk::to_string(format));
                        break;
                    }
                    const u32 slot = kVertexBufferBase - i;
                    const bool constant = attribute.per_instance && attribute.divisor == 0;
                    MTLVertexAttributeDescriptor* a = vd.attributes[attribute.location];
                    a.format = static_cast<MTLVertexFormat>(vertex_format);
                    a.offset = 0;
                    a.bufferIndex = slot;
                    MTLVertexBufferLayoutDescriptor* layout = vd.layouts[slot];
                    if (constant) {
                        layout.stepFunction = MTLVertexStepFunctionConstant;
                        layout.stepRate = 0;
                        layout.stride = MTLBufferLayoutStrideDynamic; // Metal rejects stride 0
                    } else {
                        layout.stepFunction = attribute.per_instance
                                                  ? MTLVertexStepFunctionPerInstance
                                                  : MTLVertexStepFunctionPerVertex;
                        layout.stepRate = attribute.per_instance ? attribute.divisor : 1;
                        layout.stride = MTLBufferLayoutStrideDynamic;
                    }
                    pipeline->vertex_slots[i] = {attribute.location, slot, constant};
                }
                if (!error.empty()) {
                    break;
                }
                pipeline->num_vertex_slots = static_cast<u32>(attributes.size());
                desc.vertexDescriptor = vd;
            }

            // ---- create ----
            static std::atomic<bool> logic_op_warned{};
            if (key.logic_op != AmdGpu::ColorControl::LogicOp::Copy &&
                !logic_op_warned.exchange(true)) {
                LOG_WARNING(Render, "Metal has no logic ops; the pipelines ignore them");
            }
            const auto start = Clock::now();
            id state = CreateWithArchive(
                *impl, archive_loaded, desc,
                [&](MTLPipelineOption options, NSError** err) {
                    return [impl->mtl_device newRenderPipelineStateWithDescriptor:desc
                                                                          options:options
                                                                       reflection:nil
                                                                            error:err];
                },
                record, from_archive, error);
            elapsed_us = UsSince(start);
            if (!state) {
                break;
            }
            pipeline->state = (__bridge_retained void*)state;
            pipeline->vertex_function = (__bridge_retained void*)desc.vertexFunction;
            if (desc.fragmentFunction) {
                pipeline->fragment_function = (__bridge_retained void*)desc.fragmentFunction;
            }
        } while (false);
    }

    if (!pipeline->state) {
        LOG_ERROR(Render, "Metal render pipeline ({}): {}", StageHashes(key), error);
        // The retained function pointers must exist for the identity check; a failed entry has
        // none and is found by its (never reused while we hold the shaders) key only. Keep the
        // functions alive by retaining them here.
        pipeline->vertex_function = (__bridge_retained void*)(__bridge id)vs->function;
        if (fs) {
            pipeline->fragment_function = (__bridge_retained void*)(__bridge id)fs->function;
        }
        ++counters.failures;
    }
    ++counters.render_pipelines;
    counters.render_us += elapsed_us;
    counters.archive_hits += from_archive;
    counters.compiled += !from_archive && pipeline->state;
    if (!from_archive && pipeline->state && impl->archive) {
        ++counters.archive_adds;
    }

    std::unique_lock lock{impl->mutex};
    const auto [it, inserted] = impl->render.try_emplace(map_key, std::move(pipeline));
    if (inserted && it->second->state) {
        RecordPipeline(*impl, record, !from_archive);
    }
    if (!inserted) {
        --counters.render_pipelines; // another thread built it first; this one is dropped
        if (!it->second->state) {
            --counters.failures;
        }
    }
    return it->second->state ? it->second.get() : nullptr;
}

const ComputePipeline* PsoCache::GetComputePipeline(const Vulkan::ComputePipelineKey& key,
                                                         const CompiledShader* cs) {
    ASSERT(cs && cs->function);
    const ComputeMapKey map_key{key.value, cs->function};
    {
        std::shared_lock lock{impl->mutex};
        const auto it = impl->compute.find(map_key);
        if (it != impl->compute.end()) {
            return it->second->state ? it->second.get() : nullptr;
        }
    }

    auto pipeline = std::make_unique<ComputePipeline>();
    pipeline->shader = cs;
    pipeline->threads_per_group = cs->local_size;
    pipeline->function = (__bridge_retained void*)(__bridge id)cs->function;
    std::string error;
    bool from_archive = false;
    u64 elapsed_us = 0;
    NSMutableArray* record = [NSMutableArray array]; ///< the descriptors for the archive
    @autoreleasepool {
        do {
            const u32 total = cs->local_size[0] * cs->local_size[1] * cs->local_size[2];
            if (total == 0) {
                error = "the shader declares no LocalSize";
                break;
            }
            MTLComputePipelineDescriptor* desc = [MTLComputePipelineDescriptor new];
            desc.label = [NSString stringWithFormat:@"cs %#llx", (unsigned long long)key.value];
            desc.computeFunction = (__bridge id<MTLFunction>)cs->function;
            if (total <= 1024) {
                desc.maxTotalThreadsPerThreadgroup = total;
            }
            const auto start = Clock::now();
            id state = CreateWithArchive(
                *impl, archive_loaded, desc,
                [&](MTLPipelineOption options, NSError** err) {
                    return [impl->mtl_device newComputePipelineStateWithDescriptor:desc
                                                                           options:options
                                                                        reflection:nil
                                                                             error:err];
                },
                record, from_archive, error);
            elapsed_us = UsSince(start);
            if (!state) {
                break;
            }
            id<MTLComputePipelineState> compute_state = state;
            pipeline->max_total_threads = static_cast<u32>(compute_state.maxTotalThreadsPerThreadgroup);
            pipeline->thread_execution_width = static_cast<u32>(compute_state.threadExecutionWidth);
            if (total > pipeline->max_total_threads) {
                error = fmt::format("LocalSize {}x{}x{} exceeds the pipeline's limit of {} threads",
                                    cs->local_size[0], cs->local_size[1], cs->local_size[2],
                                    pipeline->max_total_threads);
                break;
            }
            pipeline->state = (__bridge_retained void*)state;
        } while (false);
    }

    if (!pipeline->state) {
        LOG_ERROR(Render, "Metal compute pipeline cs {:#x}: {}", key.value, error);
        ++counters.failures;
    }
    ++counters.compute_pipelines;
    counters.compute_us += elapsed_us;
    counters.archive_hits += from_archive;
    counters.compiled += !from_archive && pipeline->state;
    if (!from_archive && pipeline->state && impl->archive) {
        ++counters.archive_adds;
    }

    std::unique_lock lock{impl->mutex};
    const auto [it, inserted] = impl->compute.try_emplace(map_key, std::move(pipeline));
    if (inserted && it->second->state) {
        RecordPipeline(*impl, record, !from_archive);
    }
    if (!inserted) {
        --counters.compute_pipelines;
        if (!it->second->state) {
            --counters.failures;
        }
    }
    return it->second->state ? it->second.get() : nullptr;
}

namespace {

/// One of the tessellation path's compute pipelines; `stage_in` (nil: none) describes the vertex
/// kernel's attributes. False with `error` set.
bool MakeKernel(PsoCache::Impl& impl, bool archive_loaded, TessKernel& kernel,
                const CompiledShader& shader, NSString* label,
                MTLStageInputOutputDescriptor* stage_in, NSMutableArray* record,
                bool& from_archive, std::string& error) {
    kernel.shader = &shader;
    kernel.function = (__bridge_retained void*)(__bridge id)shader.function;
    MTLComputePipelineDescriptor* desc = [MTLComputePipelineDescriptor new];
    desc.label = label;
    desc.computeFunction = (__bridge id<MTLFunction>)shader.function;
    desc.stageInputDescriptor = stage_in;
    bool archived = false;
    id state = CreateWithArchive(
        impl, archive_loaded, desc,
        [&](MTLPipelineOption options, NSError** err) {
            return [impl.mtl_device newComputePipelineStateWithDescriptor:desc
                                                                  options:options
                                                               reflection:nil
                                                                    error:err];
        },
        record, archived, error);
    from_archive = from_archive && archived;
    if (!state) {
        return false;
    }
    id<MTLComputePipelineState> compute_state = state;
    kernel.max_total_threads = static_cast<u32>(compute_state.maxTotalThreadsPerThreadgroup);
    kernel.thread_execution_width = static_cast<u32>(compute_state.threadExecutionWidth);
    kernel.state = (__bridge_retained void*)state;
    return true;
}

/// The stage-input descriptors of a vertex kernel (one per index type) for the guest's
/// attributes, and the slots they use. False with `error`.
bool BuildStageInputs(const Vulkan::GraphicsPipelineKey& key,
                      std::span<const VertexAttribute> attributes,
                      std::array<MTLStageInputOutputDescriptor*, 3>& stage_in,
                      std::array<VertexBufferSlot, Vulkan::MaxVertexBufferCount>& vertex_slots,
                      u32& num_vertex_slots, std::string& error) {
    if (attributes.size() > kMaxVertexBuffers) {
        error = fmt::format("{} vertex attributes, at most {} fit", attributes.size(),
                            kMaxVertexBuffers);
        return false;
    }
    for (u32 index_type = 0; index_type < 3 && !attributes.empty(); ++index_type) {
        MTLStageInputOutputDescriptor* sid =
            [MTLStageInputOutputDescriptor stageInputOutputDescriptor];
        for (u32 i = 0; i < attributes.size(); ++i) {
            const VertexAttribute& attribute = attributes[i];
            const vk::Format format = key.vertex_buffer_formats[i];
            const u32 vertex_format = VkToMtl::VertexFormat(format);
            if (!vertex_format || attribute.location >= 31) {
                error = fmt::format("vertex attribute {} (location {}) format {} has no "
                                    "Metal vertex format",
                                    i, attribute.location, vk::to_string(format));
                return false;
            }
            const u32 slot = kVertexBufferBase - i;
            const bool constant = attribute.per_instance && attribute.divisor == 0;
            // (MTLAttributeFormat has the values of MTLVertexFormat.)
            MTLAttributeDescriptor* a = sid.attributes[attribute.location];
            a.format = static_cast<MTLAttributeFormat>(vertex_format);
            a.offset = 0;
            a.bufferIndex = slot;
            MTLBufferLayoutDescriptor* layout = sid.layouts[slot];
            layout.stride = MTLBufferLayoutStrideDynamic;
            if (constant) {
                layout.stepFunction = MTLStepFunctionConstant;
                layout.stepRate = 0;
            } else if (attribute.per_instance) {
                layout.stepFunction = index_type != 0
                                          ? MTLStepFunctionThreadPositionInGridYIndexed
                                          : MTLStepFunctionThreadPositionInGridY;
                layout.stepRate = attribute.divisor;
            } else {
                layout.stepFunction = index_type != 0
                                          ? MTLStepFunctionThreadPositionInGridXIndexed
                                          : MTLStepFunctionThreadPositionInGridX;
                layout.stepRate = 1;
            }
            vertex_slots[i] = {attribute.location, slot, constant};
        }
        if (index_type != 0) {
            sid.indexType = index_type == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
            sid.indexBufferIndex = kTessIndexBuffer;
        }
        stage_in[index_type] = sid;
    }
    num_vertex_slots = static_cast<u32>(attributes.size());
    return true;
}

} // namespace

const TessellationPipeline* PsoCache::GetTessellationPipeline(
    const Vulkan::GraphicsPipelineKey& key, const TessellationInput& input,
    const CompiledShader* fs) {
    ASSERT(input.vertex[0] && input.control && input.eval);
    const TessMapKey map_key{
        .key = key,
        .vertex = input.vertex[0]->function,
        .control = input.control->function,
        .eval = input.eval->function,
        .fs = fs ? fs->function : nullptr,
        .attributes_hash = XXH3_64bits(input.vertex_attributes.data(),
                                       input.vertex_attributes.size_bytes()),
    };
    {
        std::shared_lock lock{impl->mutex};
        const auto it = impl->tessellation.find(map_key);
        if (it != impl->tessellation.end()) {
            return it->second->render ? it->second.get() : nullptr;
        }
    }

    auto pipeline = std::make_unique<TessellationPipeline>();
    std::string error;
    bool from_archive = true;
    u64 elapsed_us = 0;
    NSMutableArray* record = [NSMutableArray array]; ///< the kernels' descriptors for the archive
    @autoreleasepool {
        do {
            const CompiledShader& tcs = *input.control;
            const CompiledShader& tes = *input.eval;
            // The buffers between the stages are indexed with each stage's own struct size: the
            // interfaces must agree (the recompiler's stages exchange one array of vec4s).
            const auto& vs_layout = input.vertex[0]->tess_layout;
            if (tcs.tess_layout.input != 0 && tcs.tess_layout.input != vs_layout.output) {
                error = fmt::format("the control shader reads {} bytes per vertex, the vertex shader "
                                    "writes {}",
                                    tcs.tess_layout.input, vs_layout.output);
                break;
            }
            if (tes.tess_layout.input != 0 && tes.tess_layout.input != tcs.tess_layout.output) {
                error = fmt::format("the evaluation shader reads {} bytes per control point, the "
                                    "control shader writes {}",
                                    tes.tess_layout.input, tcs.tess_layout.output);
                break;
            }
            if (tes.tess_layout.patch_input != 0 &&
                tes.tess_layout.patch_input != tcs.tess_layout.patch_output) {
                error = fmt::format("the evaluation shader reads {} bytes of patch constants, the "
                                    "control shader writes {}",
                                    tes.tess_layout.patch_input, tcs.tess_layout.patch_output);
                break;
            }
            if (tcs.tess.output_vertices == 0 || tcs.tess.output_vertices != tes.variant.output_vertices ||
                key.patch_control_points == 0) {
                error = "the control shader declares no OutputVertices (or the patch size is 0)";
                break;
            }
            pipeline->input_control_points = key.patch_control_points;
            pipeline->output_control_points = tcs.tess.output_vertices;
            pipeline->triangles = tes.tess.domain == TessDomain::Triangles;
            pipeline->vertex_stride = vs_layout.output;
            pipeline->control_point_stride = tcs.tess_layout.output;
            pipeline->patch_stride = tcs.tess_layout.patch_output;
            pipeline->factor_stride = Tessellation::FactorStride(pipeline->triangles);

            // The vertex kernel's stage input: the attributes of the guest's fetch shader, in the
            // slots of the render path's vertex descriptor (data is read by thread position, or
            // through the index buffer for indexed draws; strides are set at each draw).
            std::array<MTLStageInputOutputDescriptor*, 3> stage_in{};
            if (!BuildStageInputs(key, input.vertex_attributes, stage_in, pipeline->vertex_slots,
                                  pipeline->num_vertex_slots, error)) {
                break;
            }

            // The render pipeline (the evaluation shader is its vertex function) and the kernels.
            const RenderPipelineInput render_input{.tessellation_eval = true};
            pipeline->render = GetRenderPipeline(key, input.eval, fs, render_input);
            if (!pipeline->render) {
                error = "the render pipeline of the evaluation shader failed (see above)";
                break;
            }
            const auto start = Clock::now();
            for (u32 index_type = 0; index_type < 3; ++index_type) {
                if (!input.vertex[index_type] ||
                    input.vertex[index_type]->variant.index_type != index_type) {
                    error = "a vertex kernel variant is missing";
                    break;
                }
                NSString* label = [NSString
                    stringWithFormat:@"tess vs %#llx idx%u",
                                     (unsigned long long)key.stage_hashes[u32(Shader::SwStage::Vertex)],
                                     index_type];
                if (!MakeKernel(*impl, archive_loaded, pipeline->vertex[index_type],
                                *input.vertex[index_type], label, stage_in[index_type],
                                record, from_archive, error)) {
                    break;
                }
            }
            if (!error.empty()) {
                break;
            }
            NSString* label = [NSString
                stringWithFormat:@"tess hs %#llx",
                                 (unsigned long long)key.stage_hashes[u32(Shader::SwStage::TessellationControl)]];
            if (!MakeKernel(*impl, archive_loaded, pipeline->control, tcs, label, nil, record,
                            from_archive, error)) {
                break;
            }
            elapsed_us = UsSince(start);
        } while (false);
    }

    if (!error.empty()) {
        LOG_ERROR(Render, "Metal tessellation pipeline ({}): {}", StageHashes(key), error);
        pipeline->render = nullptr; // failed entries are remembered by their key
        ++counters.failures;
    }
    ++counters.tessellation_pipelines;
    counters.compute_us += elapsed_us;
    counters.archive_hits += from_archive && error.empty();
    counters.compiled += !from_archive && error.empty();

    std::unique_lock lock{impl->mutex};
    const auto [it, inserted] = impl->tessellation.try_emplace(map_key, std::move(pipeline));
    if (inserted && it->second->render) {
        RecordPipeline(*impl, record, !from_archive); // the kernels; the render part records itself
    }
    if (!inserted) {
        --counters.tessellation_pipelines; // another thread built it first; this one is dropped
        if (!it->second->render) {
            --counters.failures;
        }
    }
    return it->second->render ? it->second.get() : nullptr;
}

bool GeometryDebugEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("BB_GS_DEBUG");
        return value && value[0] == '1';
    }();
    return enabled;
}

namespace {

/// BB_GS_DEBUG (P7.1c): the key with blending off and every colour attachment but the first
/// float one masked off, and a fragment function that writes magenta to that one (zero to the
/// integer attachments). Null with `error` when the function does not compile.
const CompiledShader* DebugFragment(PsoCache::Impl& impl, Vulkan::GraphicsPipelineKey& key,
                                    std::string& error) {
    std::string outputs, body;
    bool magenta_done = false;
    for (u32 i = 0; i < key.num_color_attachments; ++i) {
        key.blend_controls[i].enable = false;
        if (!(key.mrt_mask & (1u << i))) {
            continue;
        }
        const auto format = key.color_buffers[i].num_format;
        const bool integer = format == AmdGpu::NumberFormat::Uint || format == AmdGpu::NumberFormat::Sint;
        const char* type = format == AmdGpu::NumberFormat::Uint   ? "uint4"
                           : format == AmdGpu::NumberFormat::Sint ? "int4"
                                                                  : "float4";
        outputs += fmt::format("    {} c{} [[color({})]];\n", type, i, i);
        if (!integer && !magenta_done) {
            magenta_done = true;
            body += fmt::format("    o.c{} = float4(1.0, 0.0, 1.0, 1.0);\n", i);
        } else {
            body += fmt::format("    o.c{} = {}(0);\n", i, type);
            key.write_masks[i] = {};
        }
    }
    const std::string source = "#include <metal_stdlib>\nusing namespace metal;\n\nstruct FOut {\n" +
                               outputs + "};\n\nfragment FOut bb_gs_debug() {\n    FOut o;\n" +
                               body + "    return o;\n}\n";
    std::scoped_lock lock{impl.debug_mutex};
    if (const auto it = impl.debug_fragments.find(source); it != impl.debug_fragments.end()) {
        return it->second.get();
    }
    @autoreleasepool {
        NSError* ns_error = nil;
        id<MTLLibrary> library =
            [impl.mtl_device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                                          options:nil
                                            error:&ns_error];
        id<MTLFunction> function = library ? [library newFunctionWithName:@"bb_gs_debug"] : nil;
        if (!function) {
            error = fmt::format("debug fragment function: {}", FirstLine(ns_error));
            return nullptr;
        }
        auto shader = std::make_unique<CompiledShader>();
        shader->stage = ShaderStage::Fragment;
        shader->entry = "bb_gs_debug";
        shader->bindings.push_back({~0u, ~0u, ShaderBinding::Kind::PushConstants,
                                    kPushConstantBuffer, 1});
        shader->num_buffers = 1;
        shader->library = (__bridge_retained void*)library;
        shader->function = (__bridge_retained void*)function;
        return impl.debug_fragments.emplace(source, std::move(shader)).first->second.get();
    }
}

} // namespace

const GeometryPipeline* PsoCache::GetGeometryPipeline(const Vulkan::GraphicsPipelineKey& key,
                                                      const GeometryPipelineInput& input,
                                                      const CompiledShader* fs) {
    ASSERT(input.es[0] && input.gs && input.pass_through);
    const GeometryMapKey map_key{
        .key = key,
        .es = input.es[0]->function,
        .gs = input.gs->function,
        .pass_through = input.pass_through->function,
        .fs = fs ? fs->function : nullptr,
        .attributes_hash = XXH3_64bits(input.vertex_attributes.data(),
                                       input.vertex_attributes.size_bytes()),
    };
    {
        std::shared_lock lock{impl->mutex};
        const auto it = impl->geometry.find(map_key);
        if (it != impl->geometry.end()) {
            return it->second->render ? it->second.get() : nullptr;
        }
    }

    auto pipeline = std::make_unique<GeometryPipeline>();
    std::string error;
    bool from_archive = true;
    u64 elapsed_us = 0;
    NSMutableArray* record = [NSMutableArray array]; ///< the kernels' descriptors for the archive
    @autoreleasepool {
        do {
            const CompiledShader& gs = *input.gs;
            pipeline->info = gs.geometry;
            pipeline->es_stride = input.es[0]->tess_layout.output;
            if (pipeline->es_stride == 0 || pipeline->info.max_vertices == 0) {
                error = "the export kernel has no output struct or the geometry shader no vertices";
                break;
            }
            std::array<MTLStageInputOutputDescriptor*, 3> stage_in{};
            if (!BuildStageInputs(key, input.vertex_attributes, stage_in, pipeline->vertex_slots,
                                  pipeline->num_vertex_slots, error)) {
                break;
            }
            const RenderPipelineInput render_input{.geometry = true,
                                                   .geometry_output = pipeline->info.output};
            Vulkan::GraphicsPipelineKey render_key = key;
            const CompiledShader* render_fs = fs;
            if (GeometryDebugEnabled() && fs) { // BB_GS_DEBUG
                render_fs = DebugFragment(*impl, render_key, error);
                if (!render_fs) {
                    break;
                }
            }
            pipeline->render =
                GetRenderPipeline(render_key, input.pass_through, render_fs, render_input);
            if (!pipeline->render) {
                error = "the render pipeline of the pass-through function failed (see above)";
                break;
            }
            const auto start = Clock::now();
            for (u32 index_type = 0; index_type < 3; ++index_type) {
                if (!input.es[index_type] ||
                    input.es[index_type]->variant.index_type != index_type) {
                    error = "an export kernel variant is missing";
                    break;
                }
                NSString* label = [NSString
                    stringWithFormat:@"gs es %#llx idx%u",
                                     (unsigned long long)key.stage_hashes[u32(Shader::SwStage::Vertex)],
                                     index_type];
                if (!MakeKernel(*impl, archive_loaded, pipeline->es[index_type],
                                *input.es[index_type], label, stage_in[index_type], record,
                                from_archive, error)) {
                    break;
                }
            }
            if (!error.empty()) {
                break;
            }
            NSString* label = [NSString
                stringWithFormat:@"gs %#llx",
                                 (unsigned long long)key.stage_hashes[u32(Shader::SwStage::Geometry)]];
            if (!MakeKernel(*impl, archive_loaded, pipeline->gs, gs, label, nil, record,
                            from_archive, error)) {
                break;
            }
            elapsed_us = UsSince(start);
        } while (false);
    }

    if (!error.empty()) {
        LOG_ERROR(Render, "Metal geometry pipeline ({}): {}", StageHashes(key), error);
        pipeline->render = nullptr; // failed entries are remembered by their key
        ++counters.failures;
    }
    ++counters.geometry_pipelines;
    counters.compute_us += elapsed_us;
    counters.archive_hits += from_archive && error.empty();
    counters.compiled += !from_archive && error.empty();

    std::unique_lock lock{impl->mutex};
    const auto [it, inserted] = impl->geometry.try_emplace(map_key, std::move(pipeline));
    if (inserted && it->second->render) {
        RecordPipeline(*impl, record, !from_archive); // the kernels; the render part records itself
    }
    if (!inserted) {
        --counters.geometry_pipelines; // another thread built it first; this one is dropped
        if (!it->second->render) {
            --counters.failures;
        }
    }
    return it->second->render ? it->second.get() : nullptr;
}

} // namespace Metal
