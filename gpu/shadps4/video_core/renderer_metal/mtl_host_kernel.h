// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the renderer's own compute kernels (host_shaders GLSL -> SPIR-V) as Metal compute
// pipelines (Metal-native renderer).

#pragma once

#include <array>
#include <filesystem>
#include <span>
#include <string_view>
#include "common/types.h"
#include "video_core/renderer_metal/mtl_object.h"
#include "video_core/renderer_metal/mtl_specialize.h"

namespace Metal {

class Device;
class ShaderCompiler;

struct HostKernel {
    Object pipeline;                  ///< id<MTLComputePipelineState>
    std::array<u32, 8> buffer_slot{}; ///< SPIR-V binding (set 0) -> MSL buffer index (~0u: none)
    std::array<u32, 8> texture_slot{}; ///< SPIR-V binding (set 0) -> MSL texture index (~0u: none)
    /// SPIR-V binding (set 0) -> MSL sampler index (~0u: none): the sampler half of a combined
    /// image sampler (its image half is in texture_slot) or a separate sampler.
    std::array<u32, 8> sampler_slot{};
    std::array<u32, 3> local_size{};  ///< threads per threadgroup (the module's LocalSize)
};

/// Compiles a host GLSL kernel (SPIR-V from host_shaders) with specialisation constants baked in.
/// Asserts when the kernel does not compile (a host kernel is part of the renderer).
HostKernel BuildHostKernel(Device& device, std::span<const u32> spirv,
                           std::span<const SpecConstant> constants, std::string_view name);

/// The one compiler of the process' host kernels (also AMD's FidelityFX blobs, shim/ffx_metal),
/// with the host kernel MSL disk cache. One Metal device only (asserted).
ShaderCompiler& HostKernelCompiler(Device& device);

/// The MSL disk cache directory of host kernels, instead of ShaderCompiler::DefaultCacheDir()
/// (which needs the game's ElfInfo): for tests. Call before the first BuildHostKernel.
void SetHostKernelCacheDir(std::filesystem::path dir);

} // namespace Metal
