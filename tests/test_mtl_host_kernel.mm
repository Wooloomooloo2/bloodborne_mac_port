// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: unit test for Metal::BuildHostKernel's sampler slots (Metal-native M5 Task 2): a host
// GLSL kernel with a combined image sampler (tests/shaders/host_kernel_sampler.comp, SPIR-V from
// add_host_shader) compiles through the renderer's ShaderCompiler; HostKernel::sampler_slot maps
// its SPIR-V binding to the MSL sampler slot, and a dispatch that samples a 4x4 texture with a
// linear sampler bound there returns the bilinear values. Links libbbgpu (the ShaderCompiler);
// the MSL disk cache goes to a temporary directory (SetHostKernelCacheDir).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include "video_core/host_shaders/host_kernel_sampler_comp.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_host_kernel.h"
#include "video_core/renderer_metal/mtl_shader.h"
#include "video_core/renderer_metal/mtl_texture.h"

namespace {

using namespace Metal;

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

id<MTLBuffer> SharedBuffer(Device& device, NSUInteger length) {
    id<MTLBuffer> buffer = [(__bridge id<MTLDevice>)device.NativeDevice()
        newBufferWithLength:length
                    options:MTLResourceStorageModeShared];
    std::memset(buffer.contents, 0, length);
    return buffer;
}

BufferRef Ref(id<MTLBuffer> buffer) {
    return {(__bridge void*)buffer, 0};
}

void TestSamplerKernel(Device& device) {
    const HostKernel kernel = BuildHostKernel(device, HOST_KERNEL_SAMPLER_COMP, {},
                                              "host_kernel_sampler");
    Expect(kernel.texture_slot[0] != ~0u, "binding 0 (storage image) has a texture slot");
    Expect(kernel.texture_slot[1] != ~0u, "binding 1 (combined image sampler) has a texture slot");
    Expect(kernel.sampler_slot[1] != ~0u, "binding 1 (combined image sampler) has a sampler slot");
    Expect(kernel.sampler_slot[0] == ~0u, "binding 0 (storage image) has no sampler slot");
    bool unbound = true;
    for (u32 b = 2; b < kernel.sampler_slot.size(); ++b) {
        unbound &= kernel.sampler_slot[b] == ~0u;
    }
    Expect(unbound, "bindings 2-7 (absent) have no sampler slot");
    if (kernel.texture_slot[0] == ~0u || kernel.texture_slot[1] == ~0u ||
        kernel.sampler_slot[1] == ~0u) {
        return;
    }

    // 4x4 RGBA8Unorm: red = 60 * x, green = 60 * y, blue 0, alpha 255.
    constexpr u32 kSize = 4;
    u8 texels[kSize * kSize * 4];
    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            u8* t = &texels[(y * kSize + x) * 4];
            t[0] = u8(60 * x);
            t[1] = u8(60 * y);
            t[2] = 0;
            t[3] = 255;
        }
    }
    Object source = CreateTexture(
        device, TextureDesc{.pixel_format = MTLPixelFormatRGBA8Unorm, .width = kSize, .height = kSize},
        "sampled", nullptr);
    Object result = CreateTexture(device,
                                  TextureDesc{.pixel_format = MTLPixelFormatRGBA32Float,
                                              .width = 4,
                                              .height = 1,
                                              .shader_write = true},
                                  "result", nullptr);
    Object sampler = CreateSampler(device, SamplerDesc{.min_filter = MTLSamplerMinMagFilterLinear,
                                                       .mag_filter = MTLSamplerMinMagFilterLinear,
                                                       .mip_filter = MTLSamplerMipFilterNotMipmapped,
                                                       .address_u = MTLSamplerAddressModeClampToEdge,
                                                       .address_v = MTLSamplerAddressModeClampToEdge,
                                                       .address_w = MTLSamplerAddressModeClampToEdge,
                                                       .max_lod = 1000.0f});
    Expect(source && result && sampler, "test objects created");

    // Texel coordinates (uv * 4 - 0.5): a texel centre, a midpoint, an uneven point, the
    // clamped corner.
    const float uv[4][2] = {{1.5f / 4, 2.5f / 4}, {1.0f / 4, 1.0f / 4}, {1.25f / 4, 2.75f / 4},
                            {0.0f, 1.0f}};
    const float expect[4][2] = {{60, 120}, {30, 30}, {45, 135}, {0, 180}};

    id<MTLBuffer> staging = SharedBuffer(device, sizeof(texels));
    std::memcpy(staging.contents, texels, sizeof(texels));
    id<MTLBuffer> readback = SharedBuffer(device, 4 * 16);
    Encoder encoder{device};
    encoder.CopyBufferToTexture(Ref(staging), kSize * 4, sizeof(texels), source.Get(), 0, 0, {},
                                {kSize, kSize, 1});
    encoder.SetComputePipeline(kernel.pipeline.Get());
    encoder.SetTexture(Stage::Compute, kernel.texture_slot[0], result.Get());
    encoder.SetTexture(Stage::Compute, kernel.texture_slot[1], source.Get());
    encoder.SetSampler(Stage::Compute, kernel.sampler_slot[1], sampler.Get());
    encoder.SetBytes(Stage::Compute, kPushConstantBuffer, uv, sizeof(uv));
    encoder.Dispatch({1, 1, 1}, {kernel.local_size[0], kernel.local_size[1], kernel.local_size[2]});
    encoder.CopyTextureToBuffer(result.Get(), 0, 0, {}, {4, 1, 1}, Ref(readback), 64, 64);
    device.Wait(encoder.Submit());

    const float* out = static_cast<const float*>(readback.contents);
    bool ok = true;
    for (u32 i = 0; i < 4; ++i) {
        const float* p = &out[i * 4];
        const bool texel_ok = std::fabs(p[0] * 255.0f - expect[i][0]) <= 1.0f &&
                              std::fabs(p[1] * 255.0f - expect[i][1]) <= 1.0f && p[2] == 0.0f &&
                              p[3] == 1.0f;
        if (!texel_ok) {
            std::printf("  sample %u: (%.2f, %.2f, %.3f, %.3f), want (%.0f, %.0f, 0, 1)\n", i,
                        p[0] * 255.0f, p[1] * 255.0f, p[2], p[3], expect[i][0], expect[i][1]);
        }
        ok &= texel_ok;
    }
    Expect(ok, "linear samples through the kernel's sampler slot: bilinear values");
    Expect(kernel.local_size[0] == 4 && kernel.local_size[1] == 1, "local size 4x1x1");
}

} // namespace

int main() {
    @autoreleasepool {
        std::string dir_template =
            (std::filesystem::temp_directory_path() / "bb-host-kernel-test.XXXXXX").string();
        const char* dir = mkdtemp(dir_template.data());
        if (!dir) {
            std::printf("FAIL: no temporary directory\n");
            return 1;
        }
        SetHostKernelCacheDir(dir);
        Device device;
        TestSamplerKernel(device);
        Expect(device.ErrorCount() == 0, "no command buffer errors on the device queue");
        std::filesystem::remove_all(dir);
    }
    if (failures) {
        std::printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    std::printf("PASS: Metal-native host kernel samplers\n");
    return 0;
}
