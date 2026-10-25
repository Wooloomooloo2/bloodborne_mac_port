// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

#include "common/div_ceil.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include "video_core/host_shaders/fault_buffer_process_comp.h"

namespace VideoCore {

static constexpr size_t MaxPageFaults = 1024;
static constexpr size_t PageFaultAreaSize = MaxPageFaults * sizeof(u64);

FaultManager::FaultManager(Metal::Device& device, Vulkan::Scheduler& scheduler_,
                           BufferCache& buffer_cache_, u32 sparse_pagebits, u64 sparse_num_pages_)
    : scheduler{scheduler_}, buffer_cache{buffer_cache_}, sparse_pagesize{1ULL << sparse_pagebits},
      sparse_num_pages{sparse_num_pages_}, fault_buffer_size{BufferSize(sparse_num_pages_)},
      fault_buffer{device, MemoryType::DeviceLocal, 0, fault_buffer_size, "Fault Buffer"},
      download_buffer{device, MemoryType::HostCached, 0, MaxPendingFaults * PageFaultAreaSize,
                      "Fault Download Buffer"} {
    // Shaders reach the fault buffer by address: resident, and declared on every encoder.
    device.MakeResident(fault_buffer.Handle());
    scheduler.Record([buffer = fault_buffer.Handle()](Metal::Encoder& e) {
        e.DeclareIndirect(buffer);
    });

    const std::array<Metal::SpecConstant, 2> constants{{
        {0, sparse_pagebits},
        {1, static_cast<u32>(MaxPageFaults)},
    }};
    fault_process = Metal::BuildHostKernel(device, FAULT_BUFFER_PROCESS_COMP, constants,
                                           "fault_buffer_process");
}

void FaultManager::ProcessFaultBuffer() {
    if (u64 wait_tick = fault_areas[current_area]) {
        scheduler.Wait(wait_tick);
        scheduler.PopPendingOperations();
    }

    const u32 offset = current_area * PageFaultAreaSize;
    u8* mapped = download_buffer.mapped_data.data() + offset;
    std::memset(mapped, 0, PageFaultAreaSize);

    scheduler.EndRendering();
    // 1 bit per page, 32 pages per thread
    const u32 group_size = fault_process.local_size[0];
    const u32 num_threads = sparse_num_pages / 32;
    const u32 num_workgroups = Common::DivCeil(num_threads, group_size);
    scheduler.Record([pipeline = fault_process.pipeline.Get(),
                      fault_slot = fault_process.buffer_slot[0],
                      download_slot = fault_process.buffer_slot[1], fault = fault_buffer.Ref(),
                      download = download_buffer.Ref(offset), num_workgroups,
                      group_size](Metal::Encoder& e) {
        // Takes the bits with an atomic exchange (not a write in the ordering's sense: bits set
        // by work still running are processed next time) and writes the tracked download buffer.
        e.SetComputePipeline(pipeline, Metal::Access::Read);
        e.SetBuffer(Metal::Stage::Compute, fault_slot, fault);
        e.SetBuffer(Metal::Stage::Compute, download_slot, download);
        e.Dispatch({num_workgroups, 1, 1}, {group_size, 1, 1});
    });

    scheduler.DeferOperation([this, mapped, area = current_area] {
        fault_ranges.Clear();
        const u64* fault_buf = std::bit_cast<const u64*>(mapped);
        // The kernel counts every fault but stores only MaxPageFaults - 1 of them.
        const u32 fault_count =
            std::min<u32>(static_cast<u32>(fault_buf[0]), static_cast<u32>(MaxPageFaults - 1));
        for (u32 i = 1; i <= fault_count; ++i) {
            fault_ranges.Add(fault_buf[i], sparse_pagesize);
            LOG_INFO(Render_Vulkan, "Accessed non-GPU cached memory at {:#x}", fault_buf[i]);
        }
        fault_ranges.ForEach([&](VAddr start, VAddr end) {
            ASSERT_MSG((end - start) <= std::numeric_limits<u32>::max(),
                       "Buffer size is too large");
            (void)buffer_cache.ObtainBuffer(start, end - start, false);
        });
        fault_areas[area] = 0;
    });

    fault_areas[current_area++] = scheduler.CurrentTick();
    current_area %= MaxPendingFaults;
}

} // namespace VideoCore
