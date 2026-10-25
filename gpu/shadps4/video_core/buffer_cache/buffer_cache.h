// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "bbport_copy.h"

#include <deque>
#include <mutex>
#include <vector>
#include <boost/container/small_vector.hpp>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_copy.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Metal {
class Device;
}

namespace Vulkan {
class Runtime;
class Scheduler;
class StagingBufferPool;
} // namespace Vulkan

namespace VideoCore {

class TextureCache;
class MemoryTracker;
class PageManager;

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    static constexpr u64 ARENA_PAGE_BITS = 32;
    static constexpr u64 ARENA_PAGE_SIZE = u64{1} << ARENA_PAGE_BITS;
    static constexpr u64 NUM_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - ARENA_PAGE_BITS);
    /// Granularity of the page table (what MoltenVK's sparse alignment gave: 16 KiB, shift 14).
    static constexpr u64 MIN_BLOCK_SIZE = 16_KB;
    static constexpr u64 MacArenaBase = 0x7000000000; ///< runtime_memory.c USER_MIN (macOS)
    u64 mac_arena_end = 0; ///< end of the arena (smaller where Metal buffers are limited)
    /// Highest arena offset requested so far, in 256 MiB steps (logged when it grows: how big
    /// the arena needs to be; it is committed in full, ~1 GB of memory per GiB).
    std::atomic<u64> arena_high_steps{0};

public:
    /// Read-only bindings up to this size are copied into a stream buffer (bbport: public for
    /// the draw pipeline's constant ring).
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

    explicit BufferCache(Metal::Device& device, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return bda_pagetable_buffer.get();
    }

    /// bbport: the arenas (guest memory), for the ordering of address-reached reads.
    [[nodiscard]] const std::deque<Buffer>& GetArenas() const noexcept {
        return arenas;
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer.
    StreamBuffer& GetStreamBuffer() noexcept {
        return stream_buffer;
    }

    /// Returns the page table granularity (log2 of the block size).
    u32 GetSparsePageShift() const noexcept {
        return block_shift;
    }

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false);

    /// bbport: a guest write fault at `device_addr` was handled; unprotect its neighbourhood.
    void ExtendWriteFault(VAddr device_addr);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool assume_locks = false);

    /// Finds a buffer for the specified region.
    /// bbport: `note_use` = false leaves marking the range used in this submission (upload
    /// hoisting) to the caller, which may first want to hoist a copy of it.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false,
                                                             bool note_use = true);

    /// Metal's indexed draw path truncates offsets above 4 GiB on M2 (observed).
    /// Rebase those ranges into fence-managed staging; preserve GPU-written indices.
    /// bbport: only for an arena without CPU contents; otherwise the encoder draws through a
    /// window of the arena (Metal::LowOffset) and the arena range is returned.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainIndexBuffer(VAddr device_addr, u32 size);

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// Processes the fault buffer.
    void ProcessFaultBuffer();

    /// Synchronizes all buffers needed for DMA.
    void SynchronizeDmaBuffers();

private:
    const Buffer* GetArena(u64 first_block, u64 last_block);

    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block);

    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);

public:
    /// bbport: a guest write fault: InvalidateMemory and ExtendWriteFault with one protect relay
    /// when the faulting range holds no GPU data (stutter plan phase 2; second toggle word bit 5
    /// off). Returns false when the caller must still call ExtendWriteFault.
    bool InvalidateWriteFault(VAddr device_addr, u64 size);

private:

    /// bbport: a guest write fault's read-back done by the calling (fault) thread, straight from
    /// the lazy arena's CPU view, when the command buffers that wrote the faulting page have
    /// completed (bbport_stutter.h, stutter plan phase 1). False: nothing changed, take the old
    /// path. `verify`: decide and snapshot the bytes, change nothing, return false.
    /// `pending_writer`: when the faulting page's writer has not completed, receives its tick and
    /// the fallback is left to the caller to count (it may wait for that writer and try again).
    struct VerifyEntry {
        VAddr address;
        u64 stamp;
        std::vector<u8> bytes;
    };
    bool TryDirectReadBack(VAddr device_addr, u64 size, bool is_write,
                           std::vector<VerifyEntry>* verify, u64* pending_writer = nullptr);
    /// bbport: blocks the fault thread until the command buffer of `tick` completed; one still
    /// being recorded is committed first by the GPU command thread (stutter plan phase 1b). False:
    /// not waited (switched off, not the fault thread, or still not committed), take the old path.
    bool WaitForWriter(u64 tick);
    /// After the old path ran in verify mode: compares guest memory with the snapshot.
    void CheckDirectReadBack(const std::vector<VerifyEntry>& snapshot);

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    /// bbport: batched small copies on the copy threads (BbCopy::QueueCopy).
    static void RunGuestCopy(const BbCopy::Item& item);
    void SmallGuestCopy(const BbCopy::Item& item);

    const Buffer* UploadCopies(const Buffer* arena, std::span<BufferCopy> copies,
                               size_t total_size_bytes);

    bool SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size);

    Metal::Device& device;
    Vulkan::Scheduler& scheduler;
    // bbport: texel buffer ranges known not to alias an image, per image registry generation.
    struct ImageMiss {
        VAddr address = 0;
        u32 size = 0;
        u64 generation = ~0ULL;
    };
    std::array<ImageMiss, 256> image_miss_cache{};
    Vulkan::Runtime& runtime;
    Vulkan::StagingBufferPool& staging_pool;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    Buffer gds_buffer;
    RangeSet gpu_modified_ranges;
    /// bbport: every read-back (DownloadMemory, TryDirectReadBack) holds readback_mutex, so the
    /// fault thread's direct path never runs in the middle of the GPU command thread's download.
    /// gpu_ranges_mutex guards gpu_modified_ranges, which the recording thread adds to. Lock
    /// order: readback_mutex, region lock, gpu_ranges_mutex.
    std::mutex readback_mutex;
    std::mutex gpu_ranges_mutex;
    /// bbport: small guest copies of the current submission (SmallGuestCopy), on the threads that
    /// own the scheduler.
    u64 small_copy_tick = 0;
    u64 small_copy_bytes = 0;

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;

    std::array<const Buffer*, NUM_ARENA_PAGES> address_space{};
    std::deque<Buffer> arenas;

    /// Blocks whose page table entries are filled (the arena itself is fully backed).
    IntervalList<Interval> resident_ranges;

    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
};

} // namespace VideoCore
