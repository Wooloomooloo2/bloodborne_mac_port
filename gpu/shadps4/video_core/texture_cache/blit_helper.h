// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <utility>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_metal/mtl_object.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Metal {
class Device;
} // namespace Metal

namespace Vulkan {
class Scheduler;
} // namespace Vulkan

namespace VideoCore {

/// bbport: copies the texture cache does with a draw (Metal-native renderer): the programs of
/// renderer_metal/mtl_blit_shaders.h as render pipelines made once per attachment formats and
/// sample count. Textures are id<MTLTexture>; sample counts and formats are taken from them.
class BlitHelper {
public:
    explicit BlitHelper(Metal::Device& device, Vulkan::Scheduler& scheduler);
    ~BlitHelper();

    /// Writes channel N of each texel of the 1-sample colour `source` into sample N of the MSAA
    /// depth `dest` (level 0, layer 0 of both).
    void ReinterpretColorAsMsDepth(u32 width, u32 height, u32 num_samples,
                                   vk::Format src_pixel_format, vk::Format dst_pixel_format,
                                   void* source, void* dest);

    /// Copies level 0, layer 0 of `source` into `dest` (same format), one of them multisampled:
    /// an MSAA source copies sample by sample, a 1-sample source fills every sample.
    void CopyBetweenMsImages(u32 width, u32 height, u32 num_samples, vk::Format pixel_format,
                             bool src_msaa, void* source, void* dest);

private:
    struct PipelineKey {
        const char* fragment; ///< function in BlitShaders::Source
        u32 color_format, depth_format, stencil_format, samples;

        bool operator==(const PipelineKey&) const noexcept = default;
    };
    /// id<MTLRenderPipelineState> for `key`, made on first use (any thread).
    void* GetPipeline(const PipelineKey& key);

    Metal::Device& device;
    Vulkan::Scheduler& scheduler;
    Metal::Object library; ///< id<MTLLibrary> of BlitShaders::Source
    // id<MTLDepthStencilState>s owned by the scheduler's DepthStencilCache.
    void* depth_write_state{}; ///< depth test Always, write on
    void* no_depth_state{};    ///< depth test and write off
    std::mutex pipeline_mutex;
    std::vector<std::pair<PipelineKey, Metal::Object>> pipelines;
};

} // namespace VideoCore
