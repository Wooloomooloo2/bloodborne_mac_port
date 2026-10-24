// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: the renderer's own compute kernels as Metal compute pipelines (Metal-native renderer).

#import <Metal/Metal.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <xxhash.h>
#include "common/assert.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_device_impl.h"
#include "video_core/renderer_metal/mtl_host_kernel.h"
#include "video_core/renderer_metal/mtl_shader.h"

namespace Metal {

namespace {

std::filesystem::path& CacheDirOverride() {
    static std::filesystem::path dir;
    return dir;
}

} // namespace

/// One compiler for every host kernel of the process, with the GPU shader cache directory
/// (<user>/cache/<title>/metal, ShaderCompiler::DefaultCacheDir; SetHostKernelCacheDir) as its
/// MSL disk cache.
ShaderCompiler& HostKernelCompiler(Device& device) {
    static std::mutex mutex;
    static std::unique_ptr<ShaderCompiler> compiler;
    static Device* owner = nullptr;
    std::scoped_lock lock{mutex};
    if (!compiler) {
        compiler = std::make_unique<ShaderCompiler>(device, CacheDirOverride());
        owner = &device;
    }
    ASSERT_MSG(owner == &device, "Host kernels are built for one Metal device only");
    return *compiler;
}

void SetHostKernelCacheDir(std::filesystem::path dir) {
    CacheDirOverride() = std::move(dir);
}

HostKernel BuildHostKernel(Device& device, std::span<const u32> spirv,
                           std::span<const SpecConstant> constants, std::string_view name) {
    const std::vector<u32> specialized = Specialize(spirv, constants);
    const u64 hash = XXH3_64bits(specialized.data(), specialized.size() * sizeof(u32));
    const CompileResult result = HostKernelCompiler(device).Compile(specialized, hash, name);
    ASSERT_MSG(result.status == CompileStatus::Ok && result.shader,
               "Host kernel {} did not compile: {}", name, result.error);
    const CompiledShader& shader = *result.shader;
    ASSERT_MSG(shader.stage == ShaderStage::Compute, "Host kernel {} is not a compute shader",
               name);

    id<MTLDevice> mtl_device = device.GetImpl().device;
    id<MTLFunction> function = (__bridge id<MTLFunction>)shader.function;
    NSError* error = nil;
    id<MTLComputePipelineState> pipeline = [mtl_device newComputePipelineStateWithFunction:function
                                                                                      error:&error];
    ASSERT_MSG(pipeline != nil, "Host kernel {}: no compute pipeline state: {}", name,
               error ? error.localizedDescription.UTF8String : "unknown error");

    HostKernel kernel;
    kernel.pipeline = Object::Adopt((__bridge_retained void*)pipeline);
    kernel.buffer_slot.fill(~0u);
    kernel.texture_slot.fill(~0u);
    kernel.sampler_slot.fill(~0u);
    for (u32 b = 0; b < kernel.buffer_slot.size(); ++b) {
        if (const auto* binding = shader.Find(0, b, ShaderBinding::Kind::Buffer)) {
            kernel.buffer_slot[b] = binding->msl_index;
        }
        for (const auto kind : {ShaderBinding::Kind::Texture, ShaderBinding::Kind::StorageTexture,
                                ShaderBinding::Kind::TexelBuffer}) {
            if (const auto* binding = shader.Find(0, b, kind)) {
                kernel.texture_slot[b] = binding->msl_index;
                break;
            }
        }
        if (const auto* binding = shader.Find(0, b, ShaderBinding::Kind::Sampler)) {
            kernel.sampler_slot[b] = binding->msl_index;
        }
    }
    kernel.local_size = shader.local_size;
    return kernel;
}

} // namespace Metal
