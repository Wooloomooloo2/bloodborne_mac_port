// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/tile_manager.h"

#include "video_core/host_shaders/tiling_macro_128_comp.h"
#include "video_core/host_shaders/tiling_macro_16_comp.h"
#include "video_core/host_shaders/tiling_macro_32_comp.h"
#include "video_core/host_shaders/tiling_macro_64_comp.h"
#include "video_core/host_shaders/tiling_macro_8_comp.h"
#include "video_core/host_shaders/tiling_macro_96_comp.h"
#include "video_core/host_shaders/tiling_micro_128_comp.h"
#include "video_core/host_shaders/tiling_micro_16_comp.h"
#include "video_core/host_shaders/tiling_micro_32_comp.h"
#include "video_core/host_shaders/tiling_micro_64_comp.h"
#include "video_core/host_shaders/tiling_micro_8_comp.h"
#include "video_core/host_shaders/tiling_micro_96_comp.h"

#include <cstring>
#include <magic_enum/magic_enum.hpp>

namespace VideoCore {

struct TilingInfo {
    u32 bank_swizzle;
    u32 num_slices;
    u32 num_mips;
    std::array<ImageInfo::MipInfo, 16> mips;
};

TileManager::TileManager(Metal::Device& device_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_)
    : device{device_}, scheduler{scheduler_}, runtime{runtime_} {}

TileManager::~TileManager() = default;

const Metal::HostKernel& TileManager::GetTilingKernel(const ImageInfo& info, bool is_tiler) {
    const TilingKey key{
        .tile_mode = info.tile_mode,
        .num_bits = info.num_bits,
        .num_samples = info.num_samples,
        .is_tiler = is_tiler,
    };
    std::scoped_lock lock{kernels_mutex};
    if (const auto it = tiling_kernels.find(key); it != tiling_kernels.end()) {
        return it->second;
    }

    const auto micro_tile_mode = AmdGpu::GetMicroTileMode(info.tile_mode);
    const bool is_macro = AmdGpu::IsMacroTiled(info.array_mode);
    std::array<u32, 12> spec_data{
        info.num_samples,
        u32(micro_tile_mode),
        AmdGpu::GetMicroTileThickness(info.array_mode),
        u32(is_tiler),
    };
    if (is_macro) {
        const auto macro_tile_mode =
            AmdGpu::CalculateMacrotileMode(info.tile_mode, info.num_bits, info.num_samples);
        spec_data[4] = u32(info.array_mode);
        spec_data[5] = u32(AmdGpu::GetPipeConfig(info.tile_mode));
        spec_data[6] = AmdGpu::GetBankWidth(macro_tile_mode);
        spec_data[7] = AmdGpu::GetBankHeight(macro_tile_mode);
        spec_data[8] = AmdGpu::GetNumBanks(macro_tile_mode);
        spec_data[9] = std::bit_width(spec_data[8]) - 1;
        spec_data[10] = AmdGpu::CalculateTileSplit(info.tile_mode, info.array_mode, micro_tile_mode,
                                                   info.num_bits);
        spec_data[11] = AmdGpu::GetMacrotileAspect(macro_tile_mode);
    }

    std::span<const u32> code;
    switch (info.num_bits) {
    case 8:
        code = is_macro ? std::span<const u32>{TILING_MACRO_8_COMP}
                        : std::span<const u32>{TILING_MICRO_8_COMP};
        break;
    case 16:
        code = is_macro ? std::span<const u32>{TILING_MACRO_16_COMP}
                        : std::span<const u32>{TILING_MICRO_16_COMP};
        break;
    case 32:
        code = is_macro ? std::span<const u32>{TILING_MACRO_32_COMP}
                        : std::span<const u32>{TILING_MICRO_32_COMP};
        break;
    case 64:
        code = is_macro ? std::span<const u32>{TILING_MACRO_64_COMP}
                        : std::span<const u32>{TILING_MICRO_64_COMP};
        break;
    case 96:
        code = is_macro ? std::span<const u32>{TILING_MACRO_96_COMP}
                        : std::span<const u32>{TILING_MICRO_96_COMP};
        break;
    case 128:
        code = is_macro ? std::span<const u32>{TILING_MACRO_128_COMP}
                        : std::span<const u32>{TILING_MICRO_128_COMP};
        break;
    default:
        UNREACHABLE_MSG("Unsupported tiling pixel width {}", info.num_bits);
    }

    const u32 spec_count = is_macro ? 12 : 4;
    std::array<Metal::SpecConstant, 12> constants{};
    for (u32 i = 0; i < spec_count; ++i) {
        constants[i] = {i, spec_data[i]};
    }

    const auto module_name = fmt::format("{}_{} {}", magic_enum::enum_name(info.tile_mode),
                                         info.num_bits, is_tiler ? "tiler" : "detiler");
    LOG_INFO(Render_Vulkan, "Creating tiling pipeline {}", module_name);

    auto kernel = Metal::BuildHostKernel(device, code,
                                         std::span{constants.data(), spec_count}, module_name);
    return tiling_kernels.emplace(key, std::move(kernel)).first->second;
}

void TileManager::RecordTilingDispatch(const Metal::HostKernel& kernel, const Buffer* tiled,
                                       u64 tiled_offset, const Buffer* linear, u64 linear_offset,
                                       const void* params, u32 params_size, u32 dim_x,
                                       bool tiled_written, u64 size) {
    // Both buffers are the arena or device-local staging, which the Runtime orders: a plain
    // access then (else an isolated write, ordered by the Encoder).
    runtime.FlushBarriers(runtime.IsBufferAccessed(tiled, tiled_offset, size, tiled_written) |
                          runtime.IsBufferAccessed(linear, linear_offset, size, !tiled_written));
    const Metal::Access access = runtime.IsOrdered(tiled) && runtime.IsOrdered(linear)
                                     ? Metal::Access::Read
                                     : Metal::Access::Write;
    // The parameters are small (268 bytes): SetBytes copies them when the encoder runs, so the
    // recorded closure owns its own copy.
    std::array<u8, sizeof(TilingInfo)> params_copy;
    ASSERT(params_size == params_copy.size());
    std::memcpy(params_copy.data(), params, params_size);
    scheduler.Record([pipeline = kernel.pipeline.Get(), tiled_slot = kernel.buffer_slot[0],
                      linear_slot = kernel.buffer_slot[1], params_slot = kernel.buffer_slot[2],
                      tiled_ref = tiled->Ref(tiled_offset), linear_ref = linear->Ref(linear_offset),
                      params_copy, dim_x, access](Metal::Encoder& e) {
        e.SetComputePipeline(pipeline, access, Metal::Lane::Copy);
        e.SetBuffer(Metal::Stage::Compute, tiled_slot, tiled_ref);
        e.SetBuffer(Metal::Stage::Compute, linear_slot, linear_ref);
        e.SetBytes(Metal::Stage::Compute, params_slot, params_copy.data(),
                   static_cast<u32>(params_copy.size()));
        e.Dispatch({dim_x, 1, 1}, {64, 1, 1});
    });
}

std::pair<const Buffer*, u64> TileManager::DetileImage(const VideoCore::Buffer* in_buffer,
                                                       u64 in_offset, const ImageInfo& info) {
    if (!info.props.is_tiled) {
        return {in_buffer, in_offset};
    }

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.num_slices = info.props.is_volume ? info.size.depth : info.resources.layers;
    params.num_mips = info.resources.levels;
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const auto staging = runtime.GetStagingPool().Request(info.guest_size, MemoryType::DeviceLocal);

    scheduler.EndRendering();

    const auto dim_x = static_cast<u32>((info.guest_size / (info.num_bits / 8)) / 64);
    {
        const Vulkan::Runtime::TransferMark mark{runtime, "detiling (guest to staging)", info};
        RecordTilingDispatch(GetTilingKernel(info, false), in_buffer, in_offset, staging.buffer,
                             staging.offset, &params, sizeof(params), dim_x, false,
                             info.guest_size);
    }
    // bbport: the source may be the arena (BB_PREUPLOAD), which later uploads write: a copy into
    // it must wait for this read.
    runtime.AccessBuffer(in_buffer, in_offset, info.guest_size);
    runtime.AccessBuffer(staging.buffer, staging.offset, info.guest_size, true);

    return {staging.buffer, staging.offset};
}

void TileManager::TileImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                            const VideoCore::Buffer* out_buffer, u64 out_offset) {
    const auto& info = in_image.info;
    if (!info.props.is_tiled) {
        for (auto& copy : buffer_copies) {
            copy.bufferOffset += out_offset;
        }
        runtime.DownloadImage(&in_image, out_buffer, buffer_copies);
        return;
    }

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.num_slices = info.props.is_volume ? info.size.depth : info.resources.layers;
    params.num_mips = static_cast<u32>(buffer_copies.size());
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const auto staging = runtime.GetStagingPool().Request(info.guest_size, MemoryType::DeviceLocal);
    for (auto& copy : buffer_copies) {
        copy.bufferOffset += staging.offset;
    }

    runtime.DownloadImage(&in_image, staging.buffer, buffer_copies);

    const auto dim_x = static_cast<u32>((info.guest_size / (info.num_bits / 8)) / 64);
    {
        const Vulkan::Runtime::TransferMark mark{runtime, "tiling (staging to guest)", info};
        RecordTilingDispatch(GetTilingKernel(info, true), out_buffer, out_offset, staging.buffer,
                             staging.offset, &params, sizeof(params), dim_x, true,
                             info.guest_size);
    }

    runtime.AccessBuffer(staging.buffer, staging.offset, info.guest_size);
    runtime.AccessBuffer(out_buffer, out_offset, info.guest_size, true);
}

} // namespace VideoCore
