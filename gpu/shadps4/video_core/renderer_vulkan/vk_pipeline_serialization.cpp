// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include "common/hash.h"
#include "common/serdes.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_metal/mtl_pso_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

namespace Shader::Backend::SPIRV {
int PreciseMode();
}

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
static constexpr u32 ShaderBinaryVersion = 8u; // bbport: Metal shader profile (native Metal)
// bbport: 9 no NoContraction in FS/CS; 10 GCN legacy/clamped float ops (0 * Inf = 0, Inf
// clamped to FLT_MAX) and NaN/Inf tests on the bit pattern, so a stored cache is rebuilt once.
static constexpr u32 ShaderMetaVersionBase = 11u;
// bbport: cached SPIR-V depends on BB_SPIRV_PRECISE, which the keys do not record (mode 0,
// "vertex", keeps the base version; the invariant default since 0.5.4 adds the mode).
static u32 ShaderMetaVersion() {
    return ShaderMetaVersionBase | (u32(Shader::Backend::SPIRV::PreciseMode()) << 8);
}
// bbport: 7 Metal vertex attributes; 8 depth/stencil formats only while the pass binds the
// depth buffer (Metal-native M5; a game's stored cache is rebuilt once).
static constexpr u32 PipelineKeyVersion = 8u;
// bbport: the previous versions (0.4.0, MoltenVK), read by PipelineCache::LoadStored only.
static constexpr u32 LegacyShaderBinaryVersion = 7u;
static constexpr u32 LegacyPipelineKeyVersion = 6u;
} // namespace Serialization

namespace Vulkan {

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash,
                        size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    meta.Write(Serialization::ShaderMetaVersion());
    meta.Write(Serialization::ShaderBinaryVersion);

    meta.Write(perm_hash);
    meta.Write(perm_idx);

    spec.Serialize(ar);
    info.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", perm_hash), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", pgm_hash, perm_idx),
                                       std::move(spv));
}

bool LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                    std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                    Shader::StageSpecialization& spec, size_t& perm_idx, bool legacy,
                    bool& old_meta) {
    Serialization::Reader meta{ar};

    u32 meta_version{};
    meta.Read(meta_version);
    if (meta_version != Serialization::ShaderMetaVersion()) {
        old_meta = true;
        return false;
    }

    u32 binary_version{};
    meta.Read(binary_version);
    if (binary_version != (legacy ? Serialization::LegacyShaderBinaryVersion
                                  : Serialization::ShaderBinaryVersion)) {
        return false;
    }

    u64 perm_hash_ar{};
    meta.Read(perm_hash_ar);
    meta.Read(perm_idx);

    spec.Deserialize(ar);
    info.Deserialize(ar);

    // Motion vertex shaders embed session-local buffer device addresses. They must be
    // recompiled for the current allocation, never loaded from a previous process.
    if (info.hw_stage == Shader::HwStage::Vertex && spec.runtime_info.hw.vs.motion_vectors) {
        return false;
    }
    // bbport: direct memory access shaders read the page table and the fault buffer, created
    // only with BB_DIRECT_MEMORY_ACCESS=1 (BufferCache): an entry compiled with it on compiles
    // again without it. (A cache version bump would clear every cache at each switch.)
    if (info.uses_dma && !EmulatorSettings.IsDirectMemoryAccessEnabled()) {
        return false;
    }

    fetch_shader_data = spec.fetch_shader_data;
    return true;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    compute_key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    Shader::Backend::Bindings binding{};
    if (!LoadPipelineStage(meta_ar, 0, binding)) {
        return false;
    }

    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    ASSERT(is_new);

    it.value() = std::make_unique<ComputePipeline>(*factory, profile, compute_key, *sel.infos[0],
                                                   sel.modules[0], sdata, true);

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);

    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(static_cast<u32>(vertex_attributes.size()));
    for (const auto& attribute : vertex_attributes) {
        sdata.Write(attribute.location);
        sdata.Write(static_cast<u32>(attribute.per_instance));
        sdata.Write(attribute.divisor);
    }
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    u32 count{};
    sdata.Read(count);
    if (count > MaxVertexBufferCount) {
        return false;
    }
    vertex_attributes.clear();
    for (u32 i = 0; i < count; ++i) {
        VertexAttribute attribute{};
        u32 per_instance{};
        sdata.Read(attribute.location);
        sdata.Read(per_instance);
        sdata.Read(attribute.divisor);
        attribute.per_instance = per_instance != 0;
        vertex_attributes.push_back(attribute);
    }
    return true;
}

namespace {
/// bbport: the vertex attributes of a previous-version blob: 0.4.0 stored its Vulkan vertex input
/// state raw (GetVertexInputs: one binding per attribute, binding = location, in fetch order).
bool ReadLegacyVertexAttributes(Serialization::Archive& ar,
                                VertexInputs<VertexAttribute>& out_attributes) {
    VertexInputs<vk::VertexInputAttributeDescription> attributes;
    VertexInputs<vk::VertexInputBindingDescription> bindings;
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    Serialization::Reader sdata{ar};
    sdata.Read(&attributes, sizeof(attributes));
    sdata.Read(&bindings, sizeof(bindings));
    sdata.Read(&divisors, sizeof(divisors));
    // The multisample state and auxiliary tessellation shaders that follow are not needed.
    if (attributes.size() > MaxVertexBufferCount || bindings.size() > MaxVertexBufferCount ||
        divisors.size() > MaxVertexBufferCount) {
        return false;
    }
    out_attributes.clear();
    for (const auto& attribute : attributes) {
        const auto binding = std::ranges::find(bindings, attribute.binding,
                                               &vk::VertexInputBindingDescription::binding);
        const auto divisor =
            std::ranges::find(divisors, attribute.binding,
                              &vk::VertexInputBindingDivisorDescriptionEXT::binding);
        out_attributes.push_back(VertexAttribute{
            .location = attribute.location,
            .per_instance = binding != bindings.end() &&
                            binding->inputRate == vk::VertexInputRate::eInstance,
            .divisor = divisor != divisors.end() ? divisor->divisor : 1,
        });
    }
    return true;
}
} // namespace

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar) {
    sel.graphics_key.Deserialize(ar);

    GraphicsPipeline::SerializationSupport sdata{};
    if (loading_legacy ? !ReadLegacyVertexAttributes(ar, sdata.vertex_attributes)
                       : !sdata.Deserialize(ar)) {
        return false;
    }

    // Stages in the order the runtime binds them (RefreshGraphicsStages: fragment first), so
    // each one's first binding can be checked against the stages before it.
    Shader::Backend::Bindings binding{};
    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = sel.graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx, binding)) {
            return false;
        }
    }

    const auto [it, is_new] = graphics_pipelines.try_emplace(sel.graphics_key);
    ASSERT(is_new);

    it.value() = std::make_unique<GraphicsPipeline>(*factory, profile, sel.graphics_key, sel.infos,
                                                    sel.runtime_infos, sel.fetch_shader,
                                                    sel.modules, sdata, true);

    sel.infos.fill(nullptr);
    sel.modules.fill(nullptr);
    sel.fetch_shader.reset();

    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage,
                                      Shader::Backend::Bindings& binding) {
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    size_t perm_idx{};
    if (!LoadShaderMeta(ar, program->info, sel.fetch_shader, spec, perm_idx, loading_legacy,
                        load_old_meta)) {
        return false;
    }
    // bbport: a permutation's SPIR-V numbers its resources from the bindings of the stages before
    // it. One stored for another pipeline (a later session reused its index) gives this pipeline
    // a layout its shader does not match: Metal rejects it ("cannot reserve 'buffer' resource
    // location") and its draws are skipped. Such a pipeline is left to the runtime.
    const bool has_bindings = !program->info.buffers.empty() || !program->info.images.empty() ||
                              !program->info.samplers.empty();
    if (has_bindings && spec.start != binding) {
        LOG_WARNING(Render_Vulkan,
                    "Cached permutation {} of {:#x} starts at binding {} (pipeline needs {}), "
                    "skipping preload",
                    perm_idx, program->info.pgm_hash, spec.start.unified, binding.unified);
        load_conflict = true;
        return false;
    }
    program->info.AddBindings(binding);

    std::vector<u32> spv{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderBinary,
                                       fmt::format("{:#018x}_{}", program->info.pgm_hash, perm_idx),
                                       spv);
    if (spv.empty()) {
        return false;
    }

    // Permutation hash depends on shader variation index. To prevent collisions, we need insert it
    // at the exact position rather than append

    // The SPIR-V is kept; its Metal forms compile when the pipeline builds (on the creation pool).
    const u64 perm_hash = HashCombine(program->info.pgm_hash, perm_idx);
    const Metal::ShaderModule* module{};

    auto [it_pgm, new_program] = program_cache.try_emplace(program->info.pgm_hash);
    if (new_program) {
        it_pgm.value() = std::move(program);
    } else {
        const auto& it = std::ranges::find(it_pgm.value()->modules, spec, &Program::Module::spec);
        if (it != it_pgm.value()->modules.end()) {
            // A matching permutation is valid only at its original index. A different index means
            // the store holds entries from more than one cache generation, so this pipeline is
            // left to compile at runtime.
            const auto idx = std::distance(it_pgm.value()->modules.begin(), it);
            if (perm_idx != idx) {
                LOG_WARNING(Render_Vulkan,
                            "Cached permutation {} of {}_{:x} conflicts with index {}, skipping "
                            "preload",
                            perm_idx, program->info.hw_stage, program->info.pgm_hash, idx);
                load_conflict = true;
                return false;
            }
            module = it->module.get();
        }
    }
    if (!module) {
        auto new_module = std::make_unique<Metal::ShaderModule>(std::move(spv), perm_hash);
        module = new_module.get();
        it_pgm.value()->InsertPermut(std::move(new_module), std::move(spec), perm_idx);
    }

    sel.infos[stage] = &it_pgm.value()->info;
    sel.modules[stage] = module;

    return true;
}

void PipelineCache::WarmUp() {
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        return;
    }

    Storage::DataBase::Instance().Open();

    // Check if cache is compatible
    std::vector<u8> profile_data{};
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    if (profile_data.empty()) {
        Storage::DataBase::Instance().FinishPreload();

        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        return;
    }
    if (profile_data.size() != sizeof(Shader::Profile)) {
        LOG_WARNING(Render, "Pipeline cache profile has unexpected size ({} != {})",
                    profile_data.size(), sizeof(Shader::Profile));
    }
    Shader::Profile cached_profile{};
    if (profile_data.size() == sizeof(Shader::Profile)) {
        std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
    }
    if (profile_data.size() != sizeof(Shader::Profile) || cached_profile != profile) {
        // bbport: upstream closed the cache for the session here, so it was never rewritten
        // and every later session compiled every shader again (stutters on each new area).
        // Start a fresh cache for this build and GPU instead.
        LOG_WARNING(Render, "Pipeline cache isn't compatible with current system: rebuilding it");
        Storage::DataBase::Instance().Clear();
        Storage::DataBase::Instance().FinishPreload();
        profile_data.resize(sizeof(profile));
        std::memcpy(profile_data.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_data));
        return;
    }

    u32 num_pipelines{};
    u32 num_total_pipelines{};
    u32 num_old_format{};
    const auto warm_start = std::chrono::steady_clock::now();

    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
            ++num_total_pipelines;

            Serialization::Archive ar{std::move(data)};
            Serialization::Reader pldata{ar};

            u32 version{};
            pldata.Read(version);
            if (version != Serialization::PipelineKeyVersion) {
                ++num_old_format;
                return;
            }

            u32 is_compute{};
            pldata.Read(is_compute);

            bool result{};
            load_old_meta = false;
            if (is_compute) {
                result = LoadComputePipeline(ar);
            } else {
                result = LoadGraphicsPipeline(ar);
            }

            if (result) {
                ++num_pipelines;
            } else if (load_old_meta) {
                // A shader from another BB_SPIRV_PRECISE mode or build: it could never preload
                // again, so the cache migrates like one with old pipeline keys.
                ++num_old_format;
            }
        });

    std::printf("Pipeline cache: preloaded %u of %u pipelines in %.0f ms\n", num_pipelines,
                num_total_pipelines,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                          warm_start)
                    .count());
    // bbport: clear only for entries of an older format (a one-time migration). A pipeline that
    // fails to preload (e.g. a permutation index from another session) compiles when first
    // used; clearing for it wiped the whole cache every session (one such entry was always
    // present), so every pipeline compiled again in every session: in-game stutter.
    if (num_old_format) {
        LOG_WARNING(Render, "{} pipelines of an older cache format: clearing the cache",
                    num_old_format);
        Storage::DataBase::Instance().Clear();
        // Clear removes the profile too: without it the next session would not preload.
        std::vector<u8> profile_blob(sizeof(profile));
        std::memcpy(profile_blob.data(), &profile, sizeof(profile));
        Storage::DataBase::Instance().FinishPreload();
        Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, "profile",
                                           std::move(profile_blob));
        return;
    } else if (num_total_pipelines > num_pipelines) {
        LOG_WARNING(Render, "{} cached pipelines could not be preloaded; they compile when used",
                    num_total_pipelines - num_pipelines);
    }

    Storage::DataBase::Instance().FinishPreload();
}

PipelineCache::StoredReport PipelineCache::LoadStored() {
    StoredReport report{};
    Storage::DataBase::Instance().ForEachBlob(
        Storage::BlobType::PipelineKey, [&](std::vector<u8>&& data) {
            ++report.keys;
            Serialization::Archive ar{std::move(data)};
            Serialization::Reader pldata{ar};
            u32 version{};
            pldata.Read(version);
            if (version != Serialization::PipelineKeyVersion &&
                version != Serialization::LegacyPipelineKeyVersion) {
                ++report.old_format;
                return;
            }
            loading_legacy = version == Serialization::LegacyPipelineKeyVersion;
            u32 is_compute{};
            pldata.Read(is_compute);
            // A key stored twice (a blob of each version) is loaded once.
            Serialization::Archive peek = ar;
            bool duplicate;
            if (is_compute) {
                ComputePipelineKey key{};
                key.Deserialize(peek);
                duplicate = compute_pipelines.contains(key);
            } else {
                GraphicsPipelineKey key{};
                key.Deserialize(peek);
                duplicate = graphics_pipelines.contains(key);
            }
            if (duplicate) {
                ++report.duplicates;
                return;
            }
            load_conflict = false;
            load_old_meta = false;
            if (is_compute ? LoadComputePipeline(ar) : LoadGraphicsPipeline(ar)) {
                ++report.loaded;
                report.legacy += loading_legacy ? 1 : 0;
            } else if (load_old_meta) {
                ++report.old_format;
            } else {
                ++report.not_loaded;
                report.conflicts += load_conflict ? 1 : 0;
            }
            // A failed load leaves stages selected: the next pipeline starts clean.
            sel.infos.fill(nullptr);
            sel.modules.fill(nullptr);
            sel.fetch_shader.reset();
        });
    loading_legacy = false;
    return report;
}

void PipelineCache::ForEachPipeline(const std::function<void(const Pipeline&)>& func) {
    for (const auto& [key, pipeline] : compute_pipelines) {
        pipeline->WaitReady();
        func(*pipeline);
    }
    for (const auto& [key, pipeline] : graphics_pipelines) {
        pipeline->WaitReady();
        func(*pipeline);
    }
}

void PipelineCache::Sync() {
    factory->Psos().Flush();
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        // bbport: the walker is bytecode; loads zeroed after faults this session are stored
        // live again (bit 31 of each opcode word).
        std::vector<u32> code(walker_func, walker_func + walker_func_size / sizeof(u32));
        for (size_t i = 1; i < code.size(); i += 3) {
            code[i] &= ~(1u << 31);
        }
        srt.Write(code.data(), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    if (walker_func_size) {
        walker_func = RegisterWalkerCode(ar.CurrPtr(), walker_func_size);
        ar.Advance(walker_func_size);
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (fetch_shader_data) {
        spec.Write(sizeof(*fetch_shader_data));
        fetch_shader_data->Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return true;
}

} // namespace Shader
