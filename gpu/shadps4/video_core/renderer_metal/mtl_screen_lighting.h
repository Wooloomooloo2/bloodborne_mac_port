// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <memory>
#include "common/types.h"
#include "video_core/renderer_metal/mtl_object.h"

namespace Metal {
class Device;
class Encoder;

struct alignas(16) ScreenLightingParams {
    std::array<float, 4> projection{};
    std::array<u32, 2> size{}, trace_size{};
    float radius{1.5f}, max_radiance{4.0f}, ao{0.5f}, gi{0.7f};
    u32 directions{2}, steps{6}, view{}, reserved{};
};
static_assert(sizeof(ScreenLightingParams) == 64);

/// Horizon AO and one diffuse screen-space bounce. A half-size compute trace followed
/// by a bilateral blended draw: the scene needs RenderTarget, never ShaderWrite usage.
/// All resources used by Render survive unretained command buffers through completion.
class ScreenLightingPasses {
public:
    explicit ScreenLightingPasses(Device& device);
    ~ScreenLightingPasses();
    bool Render(Encoder& encoder, void* depth, void* scene, void* albedo,
                const ScreenLightingParams& params);
    void* TraceTexture() const;
    void* GuideTexture() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Metal
