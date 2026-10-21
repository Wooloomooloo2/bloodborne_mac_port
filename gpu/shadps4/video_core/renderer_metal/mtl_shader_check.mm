// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the pipeline check of the Metal-native renderer: every pipeline of a stored pipeline
// cache (<user>/cache/<title>/: PipelineKey, shader metadata and SPIR-V blobs) built through the
// pipeline cache the renderer uses (Vulkan::PipelineCache -> Metal::PipelineFactory), graphics and
// compute. Reads the cache only; the MSL cache and binary archive go to a scratch directory,
// emptied first (BB_MSL_CHECK_KEEP=1 keeps it: a second run then takes the pipelines from the
// binary archive). `bbgpu-host --msl-check <cache dir> [msl-cache-dir]`.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include "shader_recompiler/profile.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_pso_cache.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace {

/// Pipelines Metal cannot build by design: their draws are skipped, with one logged error each
/// (the reasons PipelineFactory::BuildGraphics gives, mtl_pipeline_build.mm).
constexpr std::array<std::string_view, 3> kUnsupported{
    "quad lists have no Metal pipeline",
    "the clip-distance emulation fragment shader has no Metal form",
    "tessellation with a geometry shader has no Metal path", // LsHsEsGs
};

enum Kind : u32 { Compute, Plain, RectList, Tessellation, Geometry, NumKinds };
constexpr std::array<const char*, NumKinds> kKindNames{"compute", "graphics", "rect list",
                                                       "tessellation", "geometry"};

Kind KindOf(const Vulkan::Pipeline& pipeline) {
    using Shader::SwStage;
    if (pipeline.IsCompute()) {
        return Compute;
    }
    const auto& key = static_cast<const Vulkan::GraphicsPipeline&>(pipeline).GetKey();
    if (key.stage_hashes[u32(SwStage::TessellationControl)] ||
        key.stage_hashes[u32(SwStage::TessellationEval)]) {
        return Tessellation;
    }
    if (key.stage_hashes[u32(SwStage::Geometry)]) {
        return Geometry;
    }
    return key.prim_type == AmdGpu::PrimitiveType::RectList ? RectList : Plain;
}

/// The buffer cache's page shift in the stored profile (the stored SPIR-V was translated with it;
/// it is not translated again here), else the game's 16 KiB.
u32 StoredPageShift(const std::filesystem::path& dir) {
    Shader::Profile stored{};
    std::ifstream in(dir / "profile.bin", std::ios::binary | std::ios::ate);
    if (in && in.tellg() == std::streamoff(sizeof(stored))) {
        in.seekg(0);
        if (in.read(reinterpret_cast<char*>(&stored), sizeof(stored))) {
            return stored.sparse_page_shift;
        }
    }
    return 14;
}

} // namespace

extern "C" int bbgpu_msl_check(const char* cache_dir, const char* msl_cache_dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(cache_dir, ec)) {
        std::printf("MSL check: %s is not a pipeline cache directory\n", cache_dir);
        return 2;
    }
    // As many creation threads as the module check used (the game uses 4).
    const unsigned threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 16u);
    setenv("BB_PIPELINE_THREADS", std::to_string(threads).c_str(), 0);

    Metal::Device device;
    const fs::path scratch = msl_cache_dir && *msl_cache_dir
                                 ? fs::path{msl_cache_dir}
                                 : fs::temp_directory_path() / "bb-msl-check";
    const char* keep = std::getenv("BB_MSL_CHECK_KEEP");
    if (!keep || std::string_view{keep} != "1") {
        fs::remove_all(scratch, ec); // measure real compiles, not cache hits
    }
    fs::create_directories(scratch, ec);

    Storage::DataBase::Instance().OpenReadOnly(cache_dir);
    std::array<unsigned, NumKinds> built{}, failed{}, unsupported{};
    Vulkan::PipelineCache::StoredReport report{};
    unsigned printed = 0;
    bool archive_loaded = false, archive_written = false;
    u64 archive_hits = 0, archive_adds = 0, archive_write_failures = 0, archive_writes = 0,
        archive_add_failures = 0;
    fs::path archive_path;
    const auto start = std::chrono::steady_clock::now();
    {
        Vulkan::Scheduler scheduler{device, false};
        Vulkan::PipelineCache pipelines{device, scheduler, StoredPageShift(cache_dir), scratch};
        report = pipelines.LoadStored();
        pipelines.ForEachPipeline([&](const Vulkan::Pipeline& pipeline) {
            const Kind kind = KindOf(pipeline);
            if (!pipeline.Failed()) {
                ++built[kind];
                return;
            }
            const std::string& why = pipeline.FailureReason();
            if (std::ranges::find(kUnsupported, std::string_view{why}) != kUnsupported.end()) {
                ++unsupported[kind];
                return;
            }
            ++failed[kind];
            if (printed++ < 20) {
                std::printf("MSL check: FAILED %s pipeline %s: %s\n", kKindNames[kind],
                            pipeline.GetDebugString().c_str(), why.c_str());
            }
        });
        Metal::PsoCache& psos = pipelines.Psos();
        archive_written = psos.Flush();
        archive_loaded = psos.ArchiveLoaded();
        archive_hits = psos.GetCounters().archive_hits;
        archive_adds = psos.GetCounters().archive_adds;
        archive_write_failures = psos.GetCounters().archive_write_failures;
        archive_writes = psos.GetCounters().archive_writes;
        archive_add_failures = psos.GetCounters().archive_add_failures;
        archive_path = psos.ArchivePath();
    } // the factory writes its binary archive into the scratch directory
    Storage::DataBase::Instance().Close();
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    std::printf("MSL check on %s: %u pipeline keys in %s: %u loaded (%u in 0.4.0's format), "
                "%u not loadable (%u permutation conflicts), %u of an older format, "
                "%u duplicates; %.1f s on %u threads\n",
                device.Name().c_str(), report.keys, cache_dir, report.loaded, report.legacy,
                report.not_loaded, report.conflicts, report.old_format, report.duplicates, seconds,
                threads);
    unsigned total_built = 0, total_failed = 0, total_unsupported = 0;
    for (u32 k = 0; k < NumKinds; ++k) {
        if (built[k] + failed[k] + unsupported[k] == 0) {
            continue;
        }
        std::printf("  %-12s built %4u, failed %4u, unsupported in Metal (logged, skipped) %4u\n",
                    kKindNames[k], built[k], failed[k], unsupported[k]);
        total_built += built[k];
        total_failed += failed[k];
        total_unsupported += unsupported[k];
    }
    const bool archive_exists = fs::exists(archive_path, ec);
    // "unchanged": nothing was compiled, so the file was not rewritten.
    const char* file_state = !archive_written || !archive_exists ? "NOT WRITTEN"
                             : archive_writes ? "written"
                                              : "unchanged";
    std::printf("  binary archive %s: %s, %llu pipelines from it, %llu compiled, %s (%llu "
                "writes, %llu failed, %llu pipelines not addable)\n",
                archive_path.string().c_str(), archive_loaded ? "loaded" : "new",
                static_cast<unsigned long long>(archive_hits),
                static_cast<unsigned long long>(archive_adds), file_state,
                static_cast<unsigned long long>(archive_writes),
                static_cast<unsigned long long>(archive_write_failures),
                static_cast<unsigned long long>(archive_add_failures));
    // At least 90% of the stored keys load; permutation conflicts (left to compile at runtime),
    // duplicates (loaded once, as their twin) and older formats (a game session migrates them)
    // are not counted against it.
    const u32 loadable = report.keys - report.conflicts - report.duplicates - report.old_format;
    const bool enough_loaded = report.loaded > 0 && u64{report.loaded} * 10 >= u64{loadable} * 9;
    if (!enough_loaded) {
        std::printf("MSL check: FAILED: %u of %u stored keys loaded (90%% needed; permutation "
                    "conflicts, duplicates and older formats excluded)\n",
                    report.loaded, loadable);
    }
    // The pipelines must reach the binary archive, or every start compiles them again; every
    // write (the factory flushes every few pipelines) must succeed.
    const bool archive_ok = archive_written && archive_exists && archive_write_failures == 0 &&
                            archive_add_failures == 0;
    if (!archive_ok) {
        std::printf("MSL check: FAILED: the binary archive could not be written completely (%llu "
                    "failed writes, %llu pipelines not addable; see the warnings above)\n",
                    static_cast<unsigned long long>(archive_write_failures),
                    static_cast<unsigned long long>(archive_add_failures));
    }
    const bool pass = total_failed == 0 && enough_loaded && archive_ok;
    std::printf("%s: MSL check, %u pipelines built, %u failed, %u unsupported in Metal (logged)\n",
                pass ? "PASS" : "FAIL", total_built, total_failed, total_unsupported);
    return pass ? 0 : 1;
}
