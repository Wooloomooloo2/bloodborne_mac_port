// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/renderer_metal/mtl_host_kernel.h"

namespace VideoCore {

class BufferCache;

class FaultManager {
    static constexpr size_t MaxPendingFaults = 8;

public:
    explicit FaultManager(Metal::Device& device, Vulkan::Scheduler& scheduler,
                          BufferCache& buffer_cache, u32 sparse_pagebits, u64 sparse_num_pages);

    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return &fault_buffer;
    }

    /// The fault buffer's size: one bit per page of the page table.
    static constexpr u64 BufferSize(u64 sparse_num_pages) noexcept {
        return sparse_num_pages / 8;
    }

    void ProcessFaultBuffer();

private:
    Vulkan::Scheduler& scheduler;
    BufferCache& buffer_cache;
    RangeSet fault_ranges;
    u64 sparse_pagesize;
    u64 sparse_num_pages;
    u64 fault_buffer_size;
    Buffer fault_buffer;
    Buffer download_buffer;
    std::array<u64, MaxPendingFaults> fault_areas{};
    u32 current_area{};
    Metal::HostKernel fault_process; ///< fault_buffer_process.comp
};

} // namespace VideoCore
