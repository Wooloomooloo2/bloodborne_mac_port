// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: native-resolution TAA (host_shaders/taa.comp) and its RCAS sharpen
// (taa_sharpen.comp) on Metal. The temporal upscaler and taa-shader-test record the kernels
// through the same functions.

#pragma once

#include <array>
#include <span>

#include "common/types.h"
#include "video_core/renderer_metal/mtl_host_kernel.h"
#include "video_core/renderer_metal/mtl_object.h"

namespace Metal {
class Device;
class Encoder;
} // namespace Metal

namespace Vulkan::Taa {

/// taa.comp's push constants (std430, 64 bytes).
struct Params {
    std::array<float, 2> jitter; ///< this frame's jitter in pixels (x right, y down)
    u32 reset;                   ///< 1: the history is discarded (its contents are undefined)
    u32 mode; ///< bits 0-7: diagnostic output (BB_TAA_DIAGNOSTICS 1-3); 8-12: A/B switches
    /// Current projection, previous projection, and row Z of previous view * inverse view
    /// (CameraMotion::TaaDepthParameters).
    std::array<std::array<float, 4>, 3> depth;
};
static_assert(sizeof(Params) == 64);

struct Kernels {
    Metal::HostKernel taa;         ///< TAA_COMP (or the override)
    Metal::HostKernel sharpen;     ///< TAA_SHARPEN_COMP: RGBA16F output
    Metal::HostKernel sharpen_ldr; ///< TAA_SHARPEN_LDR_COMP: RGBA8 (Unorm) output
    Metal::Object sampler;         ///< id<MTLSamplerState>: nearest, clamp to edge
};

/// Builds the kernels (at construction, not on the encoder thread). `taa_spirv`: SPIR-V to run
/// instead of TAA_COMP (taa-shader-test's argv[1]); empty for the production kernel.
Kernels Build(Metal::Device& device, std::span<const u32> taa_spirv = {});

/// The TAA kernel's textures (id<MTLTexture>, unretained).
struct Textures {
    void* color;        ///< current frame (any float-readable format)
    void* depth;        ///< scene depth in red (D32/D32S8 or a float colour format)
    void* motion;       ///< pixels, previous minus current (RG16F)
    void* history;      ///< previous history: RGB and depth in alpha (RGBA32F)
    void* result;       ///< RGBA16F, ShaderWrite
    void* next_history; ///< RGBA32F, ShaderWrite
    void* diagnostic;   ///< RGBA16F, ShaderWrite: written when `mode & 0xff`
};

/// Encodes taa.comp over `width` x `height` (the output size, equal to the render size).
void Record(Metal::Encoder& encoder, const Kernels& kernels, const Textures& textures,
            const Params& params, u32 width, u32 height);

/// Encodes taa_sharpen.comp (`kernel`: Kernels::sharpen or sharpen_ldr): RCAS of `resolved`
/// (read only, any float-readable format) into `output` (ShaderWrite; RGBA16F, or an RGBA8Unorm
/// texture or view for sharpen_ldr) over `width` x `height`. Strength 0..2 (above 1 the RCAS
/// change is extrapolated: the extra pass after FSR and MetalFX).
void RecordSharpen(Metal::Encoder& encoder, const Metal::HostKernel& kernel, void* resolved,
                   void* output, float strength, u32 width, u32 height);

} // namespace Vulkan::Taa
