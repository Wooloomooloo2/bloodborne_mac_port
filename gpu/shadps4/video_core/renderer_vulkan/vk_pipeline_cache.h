// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <unordered_map>
#include <shared_mutex>
#include <boost/container/static_vector.hpp>
#include <tsl/robin_map.h>
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_metal/mtl_pipeline_build.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"

namespace AmdGpu {
class Liverpool;
}

namespace Serialization {
struct Archive;
}

namespace Shader {
struct Info;
}

namespace Vulkan {

class Scheduler;

struct Program {
    struct Module {
        /// The permutation's SPIR-V; the Metal forms compile when a pipeline first needs them.
        std::unique_ptr<Metal::ShaderModule> module;
        Shader::StageSpecialization spec;
    };
    static constexpr size_t MaxPermutations = 8;
    using ModuleList = boost::container::small_vector<Module, MaxPermutations>;

    Shader::Info info;
    ModuleList modules{};
    size_t last_used = 0; ///< bbport: permutation of the previous lookup, compared first
    /// bbport: `info` as translated, for draw-preparation workers (they must not read `info`,
    /// whose user data the GPU thread rewrites every draw). Guarded by programs_mutex.
    std::unique_ptr<Shader::Info> info_template;

    Program() = default;
    Program(Shader::HwStage stage, Shader::SwStage l_stage, Shader::ShaderParams params)
        : info{stage, l_stage, params} {}

    void InsertPermut(std::unique_ptr<Metal::ShaderModule> module,
                      Shader::StageSpecialization&& spec, size_t perm_idx) {
        modules.resize(std::max(modules.size(), perm_idx + 1)); // <-- beware of realloc
        modules[perm_idx] = {std::move(module), std::move(spec)};
    }
};

struct DrawIndirectParams {
    u16 vertex_sgpr_offset;
    u32 instance_sgpr_offset;
};

} // namespace Vulkan

namespace AmdGpu {
union Regs;
}

namespace Vulkan {

/// bbport: state of one graphics/compute pipeline selection. The GPU thread owns one
/// (PipelineCache::sel); draw-preparation workers use their own with their register copies.
struct PipelineSelection {
    const AmdGpu::Regs* regs{};
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::array<const Shader::Info*, MaxShaderStages> infos{};
    std::array<const Metal::ShaderModule*, MaxShaderStages> modules{};
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
    GraphicsPipelineKey graphics_key{};
    bool motion = false;
    DrawIndirectParams draw_indirect_params{};
    struct PrepWorker* worker{}; ///< set: read-only selection for a draw-preparation worker
};

/// bbport: a draw-preparation worker's own program state (see vk_draw_prep.h).
struct PrepWorker {
    struct Stage {
        const Program* program;
        u64 hash;
        Shader::HwStage hw_stage;
        VAddr pgm_base;
        const std::vector<u32>* flat;
    };
    // Node-based: stages keep pointers to these Infos while later stages are inserted.
    std::unordered_map<const Program*, Shader::Info> infos;
    boost::container::static_vector<Stage, MaxShaderStages> stages;
    bool failed = false;
};

struct PreparedDraw;

class PipelineCache {
public:
    /// `sparse_page_shift`: the buffer cache's page size (the shaders' page table lookups).
    explicit PipelineCache(Metal::Device& device, Scheduler& scheduler,
                           AmdGpu::Liverpool* liverpool, u32 sparse_page_shift);
    /// bbport (`bbgpu-host --msl-check`): no guest GPU and nothing loaded at construction;
    /// LoadStored loads the stored pipelines. `metal_cache_dir`: the MSL disk cache and the binary
    /// archive (empty: Metal::ShaderCompiler::DefaultCacheDir()).
    PipelineCache(Metal::Device& device, Scheduler& scheduler, u32 sparse_page_shift,
                  std::filesystem::path metal_cache_dir);
    ~PipelineCache();

    void WarmUp();
    void Sync();

    /// bbport: what LoadStored found.
    struct StoredReport {
        u32 keys = 0;       ///< PipelineKey blobs
        u32 loaded = 0;     ///< pipelines created (they build on the creation pool)
        u32 legacy = 0;     ///< of those, from the previous key version (0.4.0, MoltenVK)
        u32 old_format = 0; ///< blobs of an older key version, not read
        u32 duplicates = 0; ///< keys stored more than once (loaded once)
        u32 not_loaded = 0; ///< metadata or SPIR-V missing, or a binding/permutation conflict
        u32 conflicts = 0;  ///< of not_loaded: binding/permutation conflicts (left to runtime)
    };
    /// bbport (`--msl-check`): loads every pipeline stored in the open cache database as WarmUp
    /// does, without the profile check and without writing or clearing anything. Also reads the
    /// previous key version: its Vulkan vertex input state gives the vertex attributes.
    StoredReport LoadStored();
    /// Every pipeline of the cache, compute ones first, once its build has finished.
    void ForEachPipeline(const std::function<void(const Pipeline&)>& func);

    bool LoadComputePipeline(Serialization::Archive& ar);
    bool LoadGraphicsPipeline(Serialization::Archive& ar);
    bool LoadPipelineStage(Serialization::Archive& ar, size_t stage,
                           Shader::Backend::Bindings& binding);
    size_t FreePermIndex(u64 pgm_hash, size_t from) const;
    const GraphicsPipeline* Drawable(const GraphicsPipeline* pipeline);

    const GraphicsPipeline* GetGraphicsPipeline(const DrawIndirectParams params = {},
                                                const PreparedDraw* prepared = nullptr);

    /// bbport: worker side of draw preparation: selects the pipeline key for `sel.regs` without
    /// creating anything. False when a program or permutation does not exist yet.
    bool PrepareGraphicsPipeline(PipelineSelection& sel);

    /// bbport: GPU-thread side: the pipeline for a prepared draw after checking that registers
    /// and flattened user data match; null to take the regular path.
    const GraphicsPipeline* TryPreparedPipeline(const PreparedDraw& prepared);

    /// bbport: the prepared draw the last GetGraphicsPipeline used, or null (regular path).
    [[nodiscard]] const PreparedDraw* UsedPrepared() const noexcept {
        return used_prepared;
    }

    const ComputePipeline* GetComputePipeline();
    const GraphicsPipeline* HdrUiPipeline(const GraphicsPipeline& source);

    using Result = std::tuple<const Shader::Info*, const Metal::ShaderModule*,
                              std::optional<Shader::Gcn::FetchShaderData>, u64>;
    Result GetProgram(PipelineSelection& sel, Shader::HwStage stage, Shader::SwStage l_stage,
                      const Shader::ShaderParams& params, Shader::Backend::Bindings& binding);

    static std::string GetShaderName(Shader::HwStage stage, u64 hash,
                                     std::optional<size_t> perm = {});

    auto& GetProfile() const {
        return profile;
    }

    /// The Metal shader compiler and PSO cache of the pipelines (one per device; their disk
    /// caches are in ShaderCompiler::DefaultCacheDir()).
    Metal::ShaderCompiler& Compiler();
    Metal::PsoCache& Psos();

private:
    bool RefreshGraphicsKey(PipelineSelection& sel);
    bool RefreshGraphicsStages(PipelineSelection& sel);
    bool RefreshComputeKey();

    void DumpShader(std::span<const u32> code, u64 hash, Shader::HwStage stage, size_t perm_idx,
                    std::string_view ext);
    std::optional<std::vector<u32>> GetShaderPatch(u64 hash, Shader::HwStage stage, size_t perm_idx,
                                                   std::string_view ext);
    std::unique_ptr<Metal::ShaderModule> CompileModule(Shader::Info& info,
                                                       Shader::RuntimeInfo& runtime_info,
                                                       const std::span<const u32>& code,
                                                       size_t perm_idx,
                                                       Shader::Backend::Bindings& binding);
    const Shader::RuntimeInfo& BuildRuntimeInfo(PipelineSelection& sel, Shader::HwStage stage,
                                                Shader::SwStage l_stage);

    [[nodiscard]] bool IsPipelineCacheDirty() const {
        return num_new_pipelines > 0;
    }

private:
    Scheduler& scheduler;
    AmdGpu::Liverpool* liverpool;
    Shader::Profile profile{};
    /// Before the pipelines: their builds use it until they are destroyed.
    std::unique_ptr<Metal::PipelineFactory> factory;
    Shader::Pools pools;
    tsl::robin_map<size_t, std::unique_ptr<Program>> program_cache;
    /// bbport: exclusive for program/permutation insertions, shared for worker lookups.
    std::shared_mutex programs_mutex;
    u64 prepared_hits = 0, prepared_misses = 0;
    const PreparedDraw* used_prepared = nullptr;
    tsl::robin_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>> compute_pipelines;
    tsl::robin_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>> graphics_pipelines;
    std::unordered_map<const GraphicsPipeline*, std::unique_ptr<GraphicsPipeline>> hdr_ui_pipelines;
    PipelineSelection sel{}; ///< GPU thread selection state
    ComputePipelineKey compute_key{};
    u32 num_new_pipelines{}; // new pipelines added to the cache since the game start
    /// bbport: LoadStored is reading a blob of the previous key version (0.4.0's vertex input
    /// state and shader binary version).
    bool loading_legacy = false;
    /// bbport: the last LoadPipelineStage refused a stored permutation that conflicts with this
    /// session's bindings or permutation indices (LoadStored counts them apart).
    bool load_conflict = false;
    /// bbport: the last LoadPipelineStage refused a stored shader of an older meta version
    /// (another BB_SPIRV_PRECISE mode or an earlier build): an older cache format, migrated by
    /// clearing once like old pipeline keys.
    bool load_old_meta = false;
};

} // namespace Vulkan
