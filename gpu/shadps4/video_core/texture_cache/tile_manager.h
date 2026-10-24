// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <unordered_map>
#include <utility>
#include <boost/container_hash/hash.hpp>

#include "common/types.h"
#include "video_core/amdgpu/tiling.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_metal/mtl_host_kernel.h"

namespace Vulkan {
class Runtime;
class Scheduler;
}

namespace VideoCore {

struct ImageInfo;
struct Image;

class TileManager {
    struct TilingKey {
        AmdGpu::TileMode tile_mode;
        u32 num_bits;
        u32 num_samples;
        bool is_tiler;

        bool operator==(const TilingKey&) const = default;

        struct Hash {
            size_t operator()(const TilingKey& key) const {
                size_t hash = 0;
                boost::hash_combine(hash, key.tile_mode);
                boost::hash_combine(hash, key.num_bits);
                boost::hash_combine(hash, key.num_samples);
                boost::hash_combine(hash, key.is_tiler);
                return hash;
            }
        };
    };

public:
    explicit TileManager(Metal::Device& device, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime);
    ~TileManager();

    void TileImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                   const VideoCore::Buffer* out_buffer, u64 out_offset);

    std::pair<const Buffer*, u64> DetileImage(const VideoCore::Buffer* in_buffer, u64 in_offset,
                                              const ImageInfo& info);

private:
    /// `tiled_written`: the dispatch writes the tiled buffer (tiling) and reads the linear one,
    /// else the reverse (detiling); `size` bytes each. Orders both with the Runtime's barriers.
    void RecordTilingDispatch(const Metal::HostKernel& kernel, const Buffer* tiled, u64 tiled_offset,
                              const Buffer* linear, u64 linear_offset, const void* params,
                              u32 params_size, u32 dim_x, bool tiled_written, u64 size);

    const Metal::HostKernel& GetTilingKernel(const ImageInfo& info, bool is_tiler);

private:
    Metal::Device& device;
    Vulkan::Scheduler& scheduler;
    Vulkan::Runtime& runtime;
    std::mutex kernels_mutex;
    // Node-based: kernels keep their address while the map grows.
    std::unordered_map<TilingKey, Metal::HostKernel, TilingKey::Hash> tiling_kernels;
};

} // namespace VideoCore
