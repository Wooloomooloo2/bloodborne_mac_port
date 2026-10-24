//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_metal/mtl_host_kernel.h"
#include "video_core/renderer_metal/mtl_object.h"

namespace Metal {
class Device;
class Encoder;
} // namespace Metal

namespace Vulkan {
class Scheduler;
}

namespace Vulkan::HostPasses {

/// bbport (Metal-native M6): the presenter's optional FSR 1 pass (EASU, then RCAS) as two Metal
/// compute kernels (fsr.comp), with a ring of per-size RGBA16F intermediate and output textures.
class FsrPass {
public:
    struct Settings {
        bool enable{true};
        bool use_rcas{true};
        float rcas_attenuation{0.25f};
    };
    struct Size2 {
        u32 width{};
        u32 height{};
        bool operator==(const Size2&) const = default;
    };

    FsrPass(Metal::Device& device, u32 num_images = 3);
    ~FsrPass();

    /// Records the pass into `e`: `input` (id<MTLTexture>, `input_size` texels) upscaled to
    /// `output_size`. Returns the output id<MTLTexture> (kept alive until the command buffer
    /// completes), or `input` when the pass does not apply (disabled, or the input is not
    /// smaller than the output).
    void* Render(Metal::Encoder& e, void* input, Size2 input_size, Size2 output_size,
                 const Settings& settings, bool hdr);

    /// The same on the draw scheduler's recording: returns the output texture now (the caller
    /// passes it to the next pass) while the dispatches are recorded in order. `input` and the
    /// output stay alive until the command buffer completes.
    Metal::Object Render(Scheduler& scheduler, Metal::Object input, Size2 input_size,
                         Size2 output_size, const Settings& settings, bool hdr);

private:
    struct Img {
        Metal::Object intermediary; ///< id<MTLTexture> RGBA16Float (EASU output, RCAS input)
        Metal::Object output;       ///< id<MTLTexture> RGBA16Float
    };
    /// What one pass of the ring needs, owned by the recording.
    struct Targets {
        Metal::Object intermediary;
        Metal::Object output;
    };

    [[nodiscard]] static bool Applies(const Settings& settings, Size2 input, Size2 output);
    /// The next ring entry, (re)created when the output size changed. The previous textures
    /// stay alive in the recordings that hold them (command buffers are unretained).
    Targets Acquire(Size2 output);
    void Encode(Metal::Encoder& e, void* input, Size2 input_size, Size2 output_size,
                const Settings& settings, bool hdr, void* intermediary, void* output) const;

    Metal::Device& device;
    Metal::HostKernel easu;
    Metal::HostKernel rcas;
    Metal::Object sampler;
    Size2 cur_size{};
    u32 cur_image{};
    std::vector<Img> images;
};

} // namespace Vulkan::HostPasses
