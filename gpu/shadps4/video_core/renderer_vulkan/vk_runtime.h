// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>
#include <deque>
#include <memory>
#include <span>
#include <unordered_map>

#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_copy.h"
#include "video_core/renderer_metal/mtl_buffer_barriers.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/types.h"

namespace VideoCore {
class BlitHelper;
} // namespace VideoCore

namespace Vulkan {

class GpuProfiler;
class SceneTargets;

class Scheduler;

class Runtime {
public:
    explicit Runtime(Metal::Device& device, Scheduler& scheduler);
    ~Runtime() = default;

    /// bbport: upload hoisting. A copy issued inside a render pass may run ahead of the whole
    /// submission (Scheduler::RecordHoistedCopy) when nothing in this submission has used its
    /// destination yet; then the pass stays open. Tracked per 1 KiB page of registered buffers
    /// (the buffer cache arenas), with the submission's tick.
    /// The buffer is also ordered by this Runtime's barriers (mtl_buffer_barriers.h; untracked:
    /// Device::OrderByCaller), unless BB_METAL_HAZARD_TRACKING=1.
    void RegisterHoistTarget(const VideoCore::Buffer* buffer, u64 size);
    void NoteSubmissionUse(const VideoCore::Buffer* buffer, u64 offset, u64 size);
    /// Records the copies ahead of the submission and returns true, or false (nothing done)
    /// when that is not safe or not useful; the caller then uses CopyBuffer.
    /// `shader_copy`: the game's copy shader, between guest ranges. A source in a registered
    /// buffer must be unused in this submission too, and the copy runs after the copies hoisted
    /// before it (they may have uploaded its ranges). The caller marks both ranges used.
    bool HoistCopy(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                   std::span<const VideoCore::BufferCopy> copies, bool shader_copy = false);

    Scheduler& GetScheduler() {
        return scheduler;
    }

    Metal::Device& GetDevice() {
        return device;
    }

    StagingBufferPool& GetStagingPool() {
        return staging_pool;
    }

    void TickFrame();
    SceneTargets* scene_targets = nullptr;

    /// bbport: `where` names the caller in BB_PASS_STATS.
    void CopyBuffer(const VideoCore::Buffer& src, const VideoCore::Buffer& dst,
                    std::span<const VideoCore::BufferCopy> copies,
                    std::source_location where = std::source_location::current());

    /// Fills `size` bytes at `offset` with the 32-bit `value` (offset and size multiples of 4).
    void FillBuffer(const VideoCore::Buffer& dst, u64 offset, u64 size, u32 value);

    /// Runs before commands that read or write `image` (replaces Transit: Metal tracks hazards
    /// and has no layouts): the image access hook, then the scene-resolution proxy sync.
    void AccessImage(VideoCore::Image* image, bool write);

    /// BB_GPU_PROFILE: a profiler segment for a transfer of `image` while it lives; the
    /// segment it interrupted continues afterwards.
    class TransferMark {
    public:
        TransferMark(Runtime& runtime, const char* what, const VideoCore::Image& image);
        ~TransferMark();
        TransferMark(const TransferMark&) = delete;
        TransferMark& operator=(const TransferMark&) = delete;

    private:
        GpuProfiler* profiler = nullptr;
        u64 resume = 0;
    };
    void UploadImage(VideoCore::Image* dst, const VideoCore::Buffer* src,
                     std::span<const vk::BufferImageCopy> upload_copies);
    void DownloadImage(VideoCore::Image* src, const VideoCore::Buffer* dst,
                       std::span<const vk::BufferImageCopy> download_copies);

    void CopyImage(VideoCore::Image* src, VideoCore::Image* dst);
    void CopyImageWithBuffer(VideoCore::Image* src, VideoCore::Image* dst,
                             const VideoCore::Buffer* buffer, u64 offset);
    void CopyMip(VideoCore::Image* src, VideoCore::Image* dst, u32 mip, u32 slice);

    void CopyColorAndDepth(VideoCore::Image* src, VideoCore::Image* dst);

    void CopyDepthStencil(VideoCore::Image* src, VideoCore::Image* dst,
                          const VideoCore::SubresourceRange& sub_range);

    void ResolveImage(VideoCore::Image* src, VideoCore::Image* dst,
                      const VideoCore::SubresourceRange& src_range,
                      const VideoCore::SubresourceRange& dst_range);
    void ClearImage(VideoCore::Image* dst, const VideoCore::SubresourceRange& range,
                    const vk::ClearValue& clear_value);

    void SetBackingSamples(VideoCore::Image* image, u32 num_samples, bool copy_backing = true);
    /// bbport: gives an image both lazy usages (VideoCore::Image::allowed_shader_write), `needed`
    /// (Usage*) being the ones a binding asked for: a new backing with them, the current one's
    /// contents copied (all levels and layers); the old texture and its views are released once
    /// that copy has run.
    void UpgradeUsage(VideoCore::Image* image, u32 needed);

    /// bbport: ordering of the untracked arena, per command (mtl_buffer_barriers.h): right
    /// before a command is recorded, IsBufferAccessed for each range it reads or writes, then
    /// FlushBarriers with the requests they give (it numbers the command); after recording it,
    /// AccessBuffer marks its ranges (and the hoisting bookkeeping). Unregistered buffers are not
    /// tracked here: tracked ones Metal orders, untracked ones the Encoder (an isolated write,
    /// mtl_order.h).
    /// The earlier commands the access must wait for (RAW, WAR, WAW; empty: none).
    [[nodiscard]] Metal::OrderRequest IsBufferAccessed(const VideoCore::Buffer* handle,
                                                       u64 offset, u64 size,
                                                       bool write = false) const;
    /// The access is made by `encoders` (Metal::EncoderKinds: copies EncoderCopy, dispatches
    /// EncoderCompute, draws EncoderRender), reads in render passes at `stage`.
    void AccessBuffer(const VideoCore::Buffer* handle, u64 offset, u64 size, bool write = false,
                      u8 encoders = Metal::EncoderCopy,
                      Metal::ReadStage stage = Metal::ReadStage::Late);
    /// The ordering half of AccessBuffer (no hoisting bookkeeping).
    void MarkAccess(const VideoCore::Buffer* handle, u64 offset, u64 size, bool write,
                    u8 encoders = Metal::EncoderCopy,
                    Metal::ReadStage stage = Metal::ReadStage::Late) {
        if (ordering) {
            barriers.Mark(handle, offset, size, write, encoders, stage);
        }
    }
    /// Orders `buffer` (untracked) with this Runtime's barriers (Device::OrderByCaller), until
    /// UnregisterOrdered (before it is released).
    void RegisterOrdered(const VideoCore::Buffer* buffer, u64 size);
    void UnregisterOrdered(const VideoCore::Buffer* buffer);
    /// Whether this Runtime orders `buffer` (else, untracked, the Encoder does).
    [[nodiscard]] bool IsOrdered(const VideoCore::Buffer* buffer) const {
        return ordering && barriers.Registered(buffer);
    }
    /// A recorded command that downloads textures into a staging range and uploads from it:
    /// the barrier before it, and its marks after it (the command orders its two halves).
    void BeginStagingRoundTrip(const VideoCore::Buffer* staging, u64 offset, u64 size);
    void EndStagingRoundTrip(const VideoCore::Buffer* staging, u64 offset, u64 size);
    /// Numbers the command recorded next (its marks and the Encoder's encoders), and records
    /// its wait for what `need` names (Metal::Encoder::OrderWait), when anything.
    void FlushBarriers(const Metal::OrderRequest& need);

    /// bbport: runs before this thread records commands on an image or changes its backing:
    /// the rasterizer joins its texture binding helper there (BindHelper).
    void SetImageAccessHook(void (*hook)(void*), void* context) {
        image_access_hook = hook;
        image_access_context = context;
    }
    void BeforeImageAccess() {
        if (image_access_hook) {
            image_access_hook(image_access_context);
        }
    }

private:
    /// One texture-to-texture copy; the size is in source texels (CopyRegions clamps it to both
    /// subresources). `*_z` is the depth plane of 3D textures.
    struct TextureRegion {
        u32 src_level, src_slice, src_z;
        u32 dst_level, dst_slice, dst_z;
        u32 width, height, depth;
    };
    /// Copies between the images' current backings: a blit when the formats match or one can be
    /// viewed as the other, else through a staging buffer when their blocks have the same size.
    void CopyRegions(VideoCore::Image* src, VideoCore::Image* dst,
                     std::span<const TextureRegion> regions);
    /// Buffer <-> image copies with Vulkan's region semantics (row length and image height in
    /// texels, 0 = tight), one Metal copy per layer.
    void CopyBufferImage(VideoCore::Image& image, const VideoCore::Buffer& buffer,
                         std::span<const vk::BufferImageCopy> copies, bool upload);

    /// bbport: calloc'd tag arrays: zero pages the OS commits only where a page is tagged (the
    /// arena's CPU-only parts never are); make_unique<u16[]> wrote every byte.
    struct FreeDeleter {
        void operator()(void* p) const noexcept {
            std::free(p);
        }
    };
    using TagArray = std::unique_ptr<u16[], FreeDeleter>;
    struct HoistPages {
        TagArray tick; ///< low 16 bits of the submission tick that used the page
        u64 pages;
        mutable u16 whole_tick{}; ///< ... that used the whole buffer (direct memory access)
    };
    std::unordered_map<const VideoCore::Buffer*, HoistPages> hoist_targets;
    /// Whether a hoisted copy's ranges were used by a submission still running (the upload then
    /// waits for all work before it).
    bool UsedInFlight(const VideoCore::Buffer* buffer,
                      std::span<const VideoCore::BufferCopy> copies, bool source) const;
    /// Numbers the next command (BufferBarriers::BeginCommand, Scheduler::SetOrderCommand) and
    /// retires the commands of submissions known complete.
    u64 BeginCommand();
    Metal::BufferBarriers barriers; ///< the arena's accesses, per command
    bool ordering;                  ///< the arena is untracked (no BB_METAL_HAZARD_TRACKING)
    /// The last command numbered in each submission not known complete (tick, command).
    std::deque<std::pair<u64, u64>> submitted_commands;
    u64 command_tick{}; ///< the scheduler tick of the commands numbered last

    void (*image_access_hook)(void*) = nullptr;
    void* image_access_context = nullptr;
    Metal::Device& device;
    Scheduler& scheduler;
    std::unique_ptr<VideoCore::BlitHelper> blit_helper;
    StagingBufferPool staging_pool;
};

} // namespace Vulkan
