// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_host_kernel.h"
#include "video_core/renderer_metal/mtl_shader.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/host_shaders/buffer_multi_copy_comp.h"
#include "bbport_toggles.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"

extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace Vulkan {

static constexpr u64 COPY_SHADER_HASH = 0xfefebf9f;

// bbport: the copy shader runs ~57 times per frame with ~1024 small ranges each. As one
// vkCmdCopyBuffer with that many regions it cost ~37 ns of GPU time per range (~2.1 ms per
// frame) plus the recording; buffer_multi_copy.comp copies all ranges in one dispatch.
// Prepared (checks, region table in the stream buffer) where the copy is issued, recorded then
// or, deferred to the end of the render pass, later in the same submission.
struct MultiCopyPlan {
    u64 src_min = 0, dst_min = 0, src_size = 0, dst_size = 0;
    Metal::BufferRef table{};
    u32 count = 0;
};

static const Metal::HostKernel& MultiCopyKernel(Metal::Device& device) {
    // The kernel indexes its runtime arrays without .length(): no buffer-size table is needed.
    static const Metal::HostKernel kernel =
        Metal::BuildHostKernel(device, BUFFER_MULTI_COPY_COMP, {}, "buffer_multi_copy");
    return kernel;
}

static bool PrepareMultiCopy(Rasterizer& rasterizer, std::span<const VideoCore::BufferCopy> copies,
                             MultiCopyPlan& plan) {
    if (copies.size() < 8 || BbToggle::Disabled(BbToggle::MultiCopyShader)) {
        return false;
    }
    u64 src_min = ~0ull, src_max = 0, dst_min = ~0ull, dst_max = 0;
    for (const auto& copy : copies) {
        if ((copy.src_offset | copy.dst_offset | copy.size) & 3) {
            return false; // dword copies only
        }
        src_min = std::min(src_min, copy.src_offset);
        src_max = std::max(src_max, copy.src_offset + copy.size);
        dst_min = std::min(dst_min, copy.dst_offset);
        dst_max = std::max(dst_max, copy.dst_offset + copy.size);
    }
    auto& device = rasterizer.GetRuntime().GetDevice();
    const u64 align = device.GetLimits().storage_alignment;
    src_min = Common::AlignDown(src_min, align);
    dst_min = Common::AlignDown(dst_min, align);
    if (!MultiCopyKernel(device).pipeline) {
        return false; // no kernel: CopyBuffer instead of a dispatch without a pipeline
    }

    // The region table goes into the stream buffer (host writes, visible at submission).
    thread_local std::vector<u32> regions;
    regions.resize(copies.size() * 4);
    for (size_t i = 0; i < copies.size(); ++i) {
        regions[i * 4 + 0] = static_cast<u32>((copies[i].src_offset - src_min) / 4);
        regions[i * 4 + 1] = static_cast<u32>((copies[i].dst_offset - dst_min) / 4);
        regions[i * 4 + 2] = static_cast<u32>(copies[i].size / 4);
        regions[i * 4 + 3] = 0;
    }
    auto& stream = rasterizer.GetBufferCache().GetStreamBuffer();
    const u64 table_size = regions.size() * sizeof(u32);
    const u64 table_offset = stream.Copy(regions.data(), table_size, align);
    plan = {src_min, dst_min, src_max - src_min, dst_max - dst_min, stream.Ref(table_offset),
            static_cast<u32>(copies.size())};
    return true;
}

static void RecordMultiCopy(Rasterizer& rasterizer, const VideoCore::Buffer* src,
                            const VideoCore::Buffer* dst,
                            std::span<const VideoCore::BufferCopy> copies,
                            const MultiCopyPlan& plan) {
    auto& runtime = rasterizer.GetRuntime();
    auto& scheduler = runtime.GetScheduler();
    const auto& kernel = MultiCopyKernel(runtime.GetDevice());
    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0xC0B1ull, [] { return std::string{"copy shader HLE: dispatch"}; });
    }
    scheduler.EndRendering();
    // The arena's ordering (Runtime::IsBufferAccessed): waits for the commands that used the
    // ranges. (Exact ranges: the batch spans up to 64 KiB of scattered table entries.)
    Metal::OrderRequest need;
    for (const auto& copy : copies) {
        need |= runtime.IsBufferAccessed(src, copy.src_offset, copy.size) |
                runtime.IsBufferAccessed(dst, copy.dst_offset, copy.size, true);
    }
    runtime.FlushBarriers(need);
    scheduler.Record([pipeline = kernel.pipeline.Get(), src_slot = kernel.buffer_slot[0],
                      dst_slot = kernel.buffer_slot[1], table_slot = kernel.buffer_slot[2],
                      group_size = kernel.local_size, src = src->Ref(plan.src_min),
                      dst = dst->Ref(plan.dst_min), table = plan.table,
                      count = plan.count](Metal::Encoder& e) {
        // Guest memory only (the arena, ordered by the Runtime) and the stream buffer.
        e.SetComputePipeline(pipeline, Metal::Access::Read, Metal::Lane::Copy);
        e.SetBuffer(Metal::Stage::Compute, src_slot, src);
        e.SetBuffer(Metal::Stage::Compute, dst_slot, dst);
        e.SetBuffer(Metal::Stage::Compute, table_slot, table);
        e.SetBytes(Metal::Stage::Compute, Metal::kPushConstantBuffer, &count, sizeof(count));
        e.Dispatch({count, 1, 1}, {group_size[0], group_size[1], group_size[2]});
    });
    runtime.NoteSubmissionUse(src, plan.src_min, plan.src_size);
    runtime.NoteSubmissionUse(dst, plan.dst_min, plan.dst_size);
    for (const auto& copy : copies) {
        runtime.MarkAccess(src, copy.src_offset, copy.size, false);
        runtime.MarkAccess(dst, copy.dst_offset, copy.size, true);
    }
}

static bool MultiCopy(Rasterizer& rasterizer, const VideoCore::Buffer* src,
                      const VideoCore::Buffer* dst, std::span<const VideoCore::BufferCopy> copies) {
    MultiCopyPlan plan;
    if (!PrepareMultiCopy(rasterizer, copies, plan)) {
        return false;
    }
    RecordMultiCopy(rasterizer, src, dst, copies, plan);
    return true;
}

// bbport: copy-shader copies issued inside a render pass wait for the end of the pass instead
// of ending it (~30 passes per frame were split by them, reopened on the same attachments: on
// a tile-based GPU each split stores and reloads every attachment). In Bloodborne they rewrite
// the same entries of one constants table between draws, and nothing in the pass reads those
// entries afterwards (frame traces, docs/FINDINGS.md), so the copies can run after the pass.
// They run in order, right after the pass ends (Scheduler pass-end hook: before the command
// that ended it, and before the submission), or earlier when a command of the pass touches
// their ranges: a read or write of a destination, a write of a source (OrderAccesses). Their
// ranges are marked used in the submission where they were issued, so no later copy or upload
// is hoisted ahead of them. BB_DEFER_COPIES=0: the copies end the pass as before.
namespace {
struct DeferredCopy {
    const VideoCore::Buffer* src;
    const VideoCore::Buffer* dst;
    std::vector<VideoCore::BufferCopy> copies; ///< arena offsets
    bool multi;
    MultiCopyPlan plan;
};
struct Interval {
    const VideoCore::Buffer* buffer;
    u64 begin, end;
};
std::mutex deferred_mutex;
std::vector<DeferredCopy> deferred;
std::vector<Interval> deferred_dst, deferred_src; ///< the pending copies' ranges, merged
std::atomic<bool> any_deferred{false};

bool DeferCopies() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_DEFER_COPIES");
        return !(env && env[0] == '0');
    }();
    return enabled;
}

void AddIntervals(std::vector<Interval>& set, const VideoCore::Buffer* buffer,
                  std::span<const VideoCore::BufferCopy> copies, bool source) {
    if (copies.empty()) {
        return;
    }
    for (const auto& copy : copies) {
        const u64 begin = source ? copy.src_offset : copy.dst_offset;
        set.push_back({buffer, begin, begin + copy.size});
    }
    std::ranges::sort(set, [](const Interval& a, const Interval& b) {
        return a.buffer != b.buffer ? a.buffer < b.buffer : a.begin < b.begin;
    });
    size_t out = 0;
    for (size_t i = 1; i < set.size(); ++i) {
        if (set[i].buffer == set[out].buffer && set[i].begin <= set[out].end) {
            set[out].end = std::max(set[out].end, set[i].end);
        } else {
            set[++out] = set[i];
        }
    }
    set.resize(out + 1);
}

bool Overlaps(const std::vector<Interval>& set, const VideoCore::Buffer* buffer, u64 begin,
              u64 end) {
    for (const auto& interval : set) {
        if (interval.buffer == buffer && begin < interval.end && interval.begin < end) {
            return true;
        }
    }
    return false;
}
} // namespace

void FlushDeferredCopies(Rasterizer& rasterizer) {
    if (!any_deferred.load(std::memory_order_acquire)) {
        return;
    }
    std::vector<DeferredCopy> list;
    {
        std::scoped_lock lk{deferred_mutex};
        list.swap(deferred);
        deferred_dst.clear();
        deferred_src.clear();
        any_deferred.store(false, std::memory_order_release);
    }
    auto& runtime = rasterizer.GetRuntime();
    for (const auto& d : list) {
        if (d.multi) {
            RecordMultiCopy(rasterizer, d.src, d.dst, d.copies, d.plan);
        } else {
            runtime.CopyBuffer(*d.src, *d.dst, d.copies);
        }
    }
    BbStats::deferred_shader_copies.fetch_add(list.size(), std::memory_order_relaxed);
}

bool DeferredCopiesConflict(const VideoCore::Buffer* buffer, u64 offset, u64 size, bool write) {
    if (!any_deferred.load(std::memory_order_acquire) || size == 0) {
        return false;
    }
    std::scoped_lock lk{deferred_mutex};
    return Overlaps(deferred_dst, buffer, offset, offset + size) ||
           (write && Overlaps(deferred_src, buffer, offset, offset + size));
}

static void Defer(Rasterizer& rasterizer, const VideoCore::Buffer* src,
                  const VideoCore::Buffer* dst, std::span<const VideoCore::BufferCopy> copies) {
    DeferredCopy d{src, dst, {copies.begin(), copies.end()}, false, {}};
    d.multi = PrepareMultiCopy(rasterizer, copies, d.plan);
    auto& runtime = rasterizer.GetRuntime();
    if (!runtime.GetScheduler().IsRendering()) {
        // The stream buffer's wait for space submitted and ended the pass: run it now.
        if (d.multi) {
            RecordMultiCopy(rasterizer, src, dst, copies, d.plan);
        } else {
            runtime.CopyBuffer(*src, *dst, copies);
        }
        return;
    }
    std::scoped_lock lk{deferred_mutex};
    AddIntervals(deferred_dst, dst, copies, false);
    AddIntervals(deferred_src, src, copies, true);
    deferred.push_back(std::move(d));
    any_deferred.store(true, std::memory_order_release);
}

static bool ExecuteCopyShaderHLE(const Shader::Info& info, const AmdGpu::ComputeProgram& cs_program,
                                 Rasterizer& rasterizer) {
    auto& runtime = rasterizer.GetRuntime();
    auto& buffer_cache = rasterizer.GetBufferCache();

    // Copy shader defines three formatted buffers as inputs: control, source, and destination.
    const auto ctl_buf_sharp = info.buffers[0].GetSharp(info);
    const auto src_buf_sharp = info.buffers[1].GetSharp(info);
    const auto dst_buf_sharp = info.buffers[2].GetSharp(info);
    const auto buf_stride = src_buf_sharp.GetStride();

    struct CopyShaderControl {
        u32 dst_idx;
        u32 src_idx;
        u32 end;
    };
    static_assert(sizeof(CopyShaderControl) == 12);
    // bbport: only a corrupted command buffer (load-time overlap, MACOS_BACKLOG) has these:
    // skip the copy rather than end the GPU host.
    if (buf_stride != dst_buf_sharp.GetStride() ||
        ctl_buf_sharp.GetStride() != sizeof(CopyShaderControl)) {
        static std::atomic<u32> reports{0};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 4) {
            std::fprintf(stderr, "GPU: copy shader with inconsistent strides (source %u, "
                                 "destination %u, control %u) skipped (corrupted state?)\n",
                         u32(buf_stride), u32(dst_buf_sharp.GetStride()),
                         u32(ctl_buf_sharp.GetStride()));
        }
        return true;
    }
    const auto ctl_buf = reinterpret_cast<const CopyShaderControl*>(ctl_buf_sharp.base_address);

    static std::vector<VideoCore::BufferCopy> copies;
    copies.clear();
    copies.reserve(cs_program.dim_x);

    for (u32 i = 0; i < cs_program.dim_x; i++) {
        const auto& [dst_idx, src_idx, end] = ctl_buf[i];
        const u32 local_dst_offset = dst_idx * buf_stride;
        const u32 local_src_offset = src_idx * buf_stride;
        const u32 local_size = (end + 1) * buf_stride;
        copies.emplace_back(local_src_offset, local_dst_offset, local_size);
    }

    // bbport: 64 KiB instead of 64 MiB. The copies are a few KiB spread over up to 57 MiB, and
    // each batch synchronizes (uploads, marks GPU-modified) its whole range: GPU time of the copy
    // shader 1.5 -> 0.6 ms/frame, GPU busy 85% -> 77%, frame rate no lower.
    // BB_COPY_MERGE_KB overrides it.
    static const u64 MaxDistanceForMerge = [] {
        const char* env = std::getenv("BB_COPY_MERGE_KB");
        return env ? u64(std::strtoull(env, nullptr, 10)) * 1024 : u64(64_KB);
    }();
    if (FrameCapture::Tracing()) {
        FrameCapture::Trace(fmt::format("S {} stride {} src {:#x} dst {:#x} ctl {:#x}",
                                        cs_program.dim_x, buf_stride, src_buf_sharp.base_address,
                                        dst_buf_sharp.base_address, ctl_buf_sharp.base_address));
    }
    u32 batch_start = 0;
    u32 batch_end = 0;

    while (batch_end < copies.size()) {
        // Place first copy into the current batch
        const auto& copy = copies[batch_start];
        auto src_offset_min = copy.src_offset;
        auto src_offset_max = copy.src_offset + copy.size;
        auto dst_offset_min = copy.dst_offset;
        auto dst_offset_max = copy.dst_offset + copy.size;

        for (++batch_end; batch_end < copies.size(); batch_end++) {
            // Compute new src and dst bounds if we were to batch this copy
            const auto& [src_offset, dst_offset, size] = copies[batch_end];
            auto new_src_offset_min = std::min(src_offset_min, src_offset);
            auto new_src_offset_max = std::max(src_offset_max, src_offset + size);
            if (new_src_offset_max - new_src_offset_min > MaxDistanceForMerge) {
                break;
            }

            auto new_dst_offset_min = std::min(dst_offset_min, dst_offset);
            auto new_dst_offset_max = std::max(dst_offset_max, dst_offset + size);
            if (new_dst_offset_max - new_dst_offset_min > MaxDistanceForMerge) {
                break;
            }

            // We can batch this copy
            src_offset_min = new_src_offset_min;
            src_offset_max = new_src_offset_max;
            dst_offset_min = new_dst_offset_min;
            dst_offset_max = new_dst_offset_max;
        }

        // Obtain buffers for the total source and destination ranges.
        // bbport: the ranges are marked used in the submission after the copy (hoisting).
        const auto [src_buf, src_buf_offset] = buffer_cache.ObtainBuffer(
            src_buf_sharp.base_address + src_offset_min, src_offset_max - src_offset_min, false,
            false, false, BbArenaRuns::Caller::Hle);
        const auto [dst_buf, dst_buf_offset] = buffer_cache.ObtainBuffer(
            dst_buf_sharp.base_address + dst_offset_min, dst_offset_max - dst_offset_min, true,
            false, false, BbArenaRuns::Caller::Hle);

        // Apply found buffer base.
        const auto vk_copies = std::span{copies}.subspan(batch_start, batch_end - batch_start);
        for (auto& copy : vk_copies) {
            copy.src_offset = copy.src_offset - src_offset_min + src_buf_offset;
            copy.dst_offset = copy.dst_offset - dst_offset_min + dst_buf_offset;
        }

        // Execute buffer copies.
        LOG_TRACE(Render_Vulkan, "HLE buffer copy: src_size = {}, dst_size = {}",
                  src_offset_max - src_offset_min, dst_offset_max - dst_offset_min);
        // bbport: a copy inside a render pass ended it (~30 per frame at 1080p, reopening the
        // same attachments). Unless its ranges were used earlier in the submission, a small one
        // runs ahead of the submission (hoisted); the others wait for the end of the pass
        // (Defer above). BB_HOIST_UNSAFE=1 hoists them anyway: wrong image, a bound only.
        const bool tracing = FrameCapture::Tracing();
        const bool was_rendering = runtime.GetScheduler().IsRendering();
        const bool src_gpu = tracing && buffer_cache.IsRegionGpuModified(
                                            src_buf_sharp.base_address + src_offset_min,
                                            src_offset_max - src_offset_min);
        const char* path = "inline";
        static const bool unsafe_hoist = std::getenv("BB_HOIST_UNSAFE") != nullptr;
        if (was_rendering && unsafe_hoist) {
            runtime.GetScheduler().RecordHoistedCopy(src_buf->Ref(), dst_buf->Ref(),
                                                     vk_copies, true);
            BbStats::hoisted_shader_copies.fetch_add(1, std::memory_order_relaxed);
            path = "unsafe";
        } else if (was_rendering && vk_copies.size() < 8 &&
                   runtime.HoistCopy(src_buf, dst_buf, vk_copies, true)) {
            BbStats::hoisted_shader_copies.fetch_add(1, std::memory_order_relaxed);
            path = "hoist";
        } else if (was_rendering && DeferCopies() &&
                   !BbToggle::Disabled(BbToggle::DeferredShaderCopies)) {
            Defer(rasterizer, src_buf, dst_buf, vk_copies);
            path = "defer";
        } else if (MultiCopy(rasterizer, src_buf, dst_buf, vk_copies)) {
            path = "multi";
        } else {
            BbStats::inline_shader_copies.fetch_add(1, std::memory_order_relaxed);
            runtime.CopyBuffer(*src_buf, *dst_buf, vk_copies);
        }
        if (tracing) {
            // Absolute guest ranges: the batch's, then each destination range (offset+size from
            // the batch's destination start).
            std::string line = fmt::format(
                "C {} rend {} srcgpu {} src {:#x} {} dst {:#x} {} n {} :",
                path, was_rendering ? 1 : 0, src_gpu ? 1 : 0,
                src_buf_sharp.base_address + src_offset_min, src_offset_max - src_offset_min,
                dst_buf_sharp.base_address + dst_offset_min, dst_offset_max - dst_offset_min,
                vk_copies.size());
            for (const auto& copy : vk_copies) {
                line += fmt::format(" {}+{}", copy.dst_offset - dst_buf_offset, copy.size);
            }
            FrameCapture::Trace(std::move(line));
        }
        runtime.NoteSubmissionUse(src_buf, src_buf_offset, src_offset_max - src_offset_min);
        runtime.NoteSubmissionUse(dst_buf, dst_buf_offset, dst_offset_max - dst_offset_min);
        batch_start = batch_end;
    }

    return true;
}

bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer) {
    switch (info.pgm_hash) {
    case COPY_SHADER_HASH:
        return ExecuteCopyShaderHLE(info, cs_program, rasterizer);
    default:
        return false;
    }
}

} // namespace Vulkan
