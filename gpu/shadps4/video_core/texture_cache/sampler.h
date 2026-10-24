// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/amdgpu/regs_texture.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_metal/mtl_object.h"

namespace Metal {
class Device;
}

namespace VideoCore {

class Sampler {
public:
    explicit Sampler(Metal::Device& device, const AmdGpu::Sampler& sampler,
                     const AmdGpu::BorderColorBuffer border_color_base, bool is_depth,
                     float extra_lod_bias = 0.0f);
    ~Sampler();

    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    Sampler(Sampler&&) = default;
    Sampler& operator=(Sampler&&) = default;

    /// The id<MTLSamplerState> (unretained).
    void* Handle() const noexcept {
        return handle.Get();
    }

    size_t lru_id{};

private:
    Metal::Object handle;
};

} // namespace VideoCore
