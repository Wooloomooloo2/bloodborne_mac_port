// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "common/assert.h"
#include "common/types.h"
#include "video_core/renderer_metal/mtl_memory_type.h"
#include "video_core/renderer_metal/mtl_object.h"

namespace Metal {
class Device;
} // namespace Metal

namespace Vulkan {
class Scheduler;
} // namespace Vulkan

namespace VideoCore {

/// One MTLBuffer. Metal tracks hazards and shared storage is coherent, so there is nothing to
/// flush or invalidate.
class UniqueBuffer {
public:
    /// Creates the MTLBuffer (asserts on failure). Replaces any previous one.
    void Create(Metal::Device& device, u64 size, MemoryType type, std::string_view label = "",
                bool guest_memory = false);
    void Destroy();

    /// id<MTLBuffer>, unretained.
    [[nodiscard]] void* Handle() const noexcept {
        return buffer.Get();
    }
    [[nodiscard]] u64 GpuAddress() const noexcept {
        return gpu_address;
    }
    /// The CPU pointer of a shared buffer; null for DeviceLocal (private storage).
    [[nodiscard]] u8* Contents() const noexcept {
        return contents;
    }
    /// bbport: the CPU pointer of a guest memory buffer (Metal::CreateBuffer's host_view), else
    /// null. Only for reading GPU-written data whose command buffer has completed.
    [[nodiscard]] u8* HostView() const noexcept {
        return host_view;
    }

private:
    Metal::Object buffer;
    u64 gpu_address{};
    u8* contents{};
    u8* host_view{};
};

struct Buffer {
    /// bbport: `guest_memory`: a buffer holding guest memory (BufferCache chunks): lazily committed
    /// and readable by the CPU at any size (mtl_buffer.mm).
    explicit Buffer(Metal::Device& device, MemoryType mem_type, VAddr cpu_addr_, u64 size_bytes_,
                    std::string_view debug_name = "", bool guest_memory = false);

    Buffer& operator=(const Buffer&) = delete;
    Buffer(const Buffer&) = delete;

    Buffer& operator=(Buffer&&) = default;
    Buffer(Buffer&&) = default;

    [[nodiscard]] bool IsInBounds(VAddr addr, u64 size) const noexcept {
        return addr >= cpu_addr && addr + size <= cpu_addr + SizeBytes();
    }

    [[nodiscard]] VAddr CpuAddr() const noexcept {
        return cpu_addr;
    }

    [[nodiscard]] u64 Offset(VAddr other_cpu_addr) const noexcept {
        return other_cpu_addr - cpu_addr;
    }

    size_t SizeBytes() const {
        return size_bytes;
    }

    /// id<MTLBuffer>, unretained.
    void* Handle() const noexcept {
        return buffer.Handle();
    }

    Metal::BufferRef Ref(u64 offset = 0) const noexcept {
        return {Handle(), offset};
    }

    /// The buffer's GPU virtual address (for shaders that read it by address).
    u64 GpuAddress() const noexcept {
        return buffer.GpuAddress();
    }

    VAddr cpu_addr = 0;
    size_t size_bytes = 0;
    std::span<u8> mapped_data{}; ///< shared storage only
    MemoryType mem_type{MemoryType::DeviceLocal};
    UniqueBuffer buffer;
};

struct StreamBuffer : public Buffer {
    explicit StreamBuffer(Metal::Device& device, Vulkan::Scheduler& scheduler, MemoryType mem_type,
                          u64 size_bytes);

    /// Reserves a region of memory from the stream buffer. Must be followed by Commit().
    std::pair<u8*, u64> Map(u64 size, u64 alignment = 0, bool allow_wait = true);

    /// Marks the reserved bytes as used (shared storage needs no flush).
    void Commit();

    /// Reserves and commits a region in one step.
    std::optional<u64> Reserve(u64 size, u64 alignment = 0, bool allow_wait = true);

    /// Tick of the most recent commit into this buffer, 0 if never used.
    [[nodiscard]] u64 LastTick() const noexcept {
        return last_tick;
    }

    /// Maps and commits a memory region with user provided data
    u64 Copy(auto src, size_t size, size_t alignment = 0) {
        const auto [data, offset] = Map(size, alignment);
        ASSERT_MSG(data, "Failed to copy {:#x} bytes", size);
        std::memcpy(data, reinterpret_cast<const void*>(src), size);
        Commit();
        return offset;
    }

private:
    struct Watch {
        u64 tick{};
        u64 upper_bound{};
    };

    /// Aligns, wraps if needed and waits for the GPU. On success mapped_size is set and the
    /// region starts at offset.
    bool PrepareMap(u64 size, u64 alignment, bool allow_wait);

    /// Advances offset past the mapped region and records the current tick for it.
    void AdvanceAndWatch();

    /// Increases the amount of watches available.
    void ReserveWatches(std::vector<Watch>& watches, std::size_t grow_size);

    /// Waits pending watches until requested upper bound.
    bool WaitPendingOperations(u64 requested_upper_bound, bool allow_wait);

private:
    Vulkan::Scheduler& scheduler;
    u64 offset{};
    u64 mapped_size{};
    u64 last_tick{};
    std::vector<Watch> current_watches;
    std::size_t current_watch_cursor{};
    std::optional<size_t> invalidation_mark;
    std::vector<Watch> previous_watches;
    std::size_t wait_cursor{};
    u64 wait_bound{};
};

} // namespace VideoCore
