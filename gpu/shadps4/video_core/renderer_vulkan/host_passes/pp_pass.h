//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include "common/types.h"
#include "video_core/renderer_metal/mtl_object.h"
#include "video_core/renderer_metal/mtl_present.h"

namespace Metal {
class Device;
}

namespace Vulkan {
class Scheduler;
}

namespace Vulkan::HostPasses {

/// bbport (Metal-native M4): the presenter's post-process pass (user gamma, sRGB decode of 10-bit
/// input) on the draw scheduler, over Metal::PresentPasses, which also holds the present blit.
class PostProcessingPass {
public:
    /// post_process.frag's push constants: {float gamma; u32 hdr; u32 srgb_input;}.
    using Settings = Metal::PostProcessSettings;

    PostProcessingPass(Metal::Device& device, u32 frame_format /*MTLPixelFormat*/,
                       u32 drawable_format /*MTLPixelFormat*/);
    ~PostProcessingPass();

    /// Records the pass into `scheduler`: `input` (id<MTLTexture>) resampled over all of
    /// `output` (a render target of frame_format, width x height). Both stay alive until the
    /// command buffer completes.
    void Render(Scheduler& scheduler, Metal::Object input, Metal::Object output, u32 width,
                u32 height, const Settings& settings);

    /// The pipelines, for the present blit (Metal::PresentPasses::Present).
    [[nodiscard]] Metal::PresentPasses& Passes() noexcept {
        return *passes;
    }

private:
    std::unique_ptr<Metal::PresentPasses> passes;
};

} // namespace Vulkan::HostPasses
