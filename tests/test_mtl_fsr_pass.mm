// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: unit test for the Metal FSR 1 pass (Metal-native M6 Task 5): a 64x64 RGBA16F image with
// a vertical black/white edge at x = 32 upscaled to 128x128 by EASU, and by EASU + RCAS. Both
// outputs are 128x128, keep the flat sides black and white, and RCAS changes the edge. Links
// libbbgpu (FsrPass, the ShaderCompiler); the MSL disk cache goes to a temporary directory.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_host_kernel.h"
#include "video_core/renderer_metal/mtl_texture.h"
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"

namespace {

using namespace Metal;
using Vulkan::HostPasses::FsrPass;

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

constexpr u32 kIn = 64;
constexpr u32 kOut = 128;
constexpr u16 kHalfOne = 0x3C00;

float HalfToFloat(u16 h) {
    __fp16 v;
    std::memcpy(&v, &h, sizeof(v));
    return float(v);
}

BufferRef Ref(id<MTLBuffer> buffer) {
    return {(__bridge void*)buffer, 0};
}

/// Upscales the edge image and returns the red channel of the kOut x kOut output.
std::vector<float> Upscale(Device& device, FsrPass& pass, void* input, id<MTLBuffer> readback,
                           bool use_rcas, void** output_texture) {
    Encoder encoder{device};
    FsrPass::Settings settings;
    settings.use_rcas = use_rcas;
    void* const output = pass.Render(encoder, input, {kIn, kIn}, {kOut, kOut}, settings, false);
    Expect(output != input, "the pass applies to a smaller input");
    *output_texture = output;
    encoder.CopyTextureToBuffer(output, 0, 0, {}, {kOut, kOut, 1}, Ref(readback), kOut * 8,
                                kOut * kOut * 8);
    device.Wait(encoder.Submit());
    const u16* texels = static_cast<const u16*>(readback.contents);
    std::vector<float> red(kOut * kOut);
    for (u32 i = 0; i < kOut * kOut; ++i) {
        red[i] = HalfToFloat(texels[i * 4]);
    }
    return red;
}

void TestFsrPassUpscalesEdge(Device& device) {
    FsrPass pass{device};
    const TextureDesc in_desc{.pixel_format = MTLPixelFormatRGBA16Float, .width = kIn, .height = kIn};
    Object input = CreateTexture(device, in_desc, "fsr input", nullptr);
    Expect(bool(input), "input texture created");
    if (!input) {
        return;
    }
    std::vector<u16> texels(kIn * kIn * 4);
    for (u32 y = 0; y < kIn; ++y) {
        for (u32 x = 0; x < kIn; ++x) {
            u16* t = &texels[(y * kIn + x) * 4];
            const u16 v = x >= kIn / 2 ? kHalfOne : 0;
            t[0] = t[1] = t[2] = v;
            t[3] = kHalfOne;
        }
    }
    id<MTLDevice> mtl = (__bridge id<MTLDevice>)device.NativeDevice();
    id<MTLBuffer> staging = [mtl newBufferWithBytes:texels.data()
                                             length:texels.size() * 2
                                            options:MTLResourceStorageModeShared];
    id<MTLBuffer> readback = [mtl newBufferWithLength:kOut * kOut * 8
                                              options:MTLResourceStorageModeShared];
    {
        Encoder encoder{device};
        encoder.CopyBufferToTexture(Ref(staging), kIn * 8, kIn * kIn * 8, input.Get(), 0, 0, {},
                                    {kIn, kIn, 1});
        device.Wait(encoder.Submit());
    }

    void* easu_texture = nullptr;
    void* rcas_texture = nullptr;
    const std::vector<float> easu = Upscale(device, pass, input.Get(), readback, false, &easu_texture);
    const std::vector<float> rcas = Upscale(device, pass, input.Get(), readback, true, &rcas_texture);
    Expect(easu_texture && TextureSampleCount(easu_texture) == 1, "EASU output exists");
    Expect(easu_texture != rcas_texture, "the ring gives each frame its own output");
    for (const void* texture : {(const void*)easu_texture, (const void*)rcas_texture}) {
        id<MTLTexture> t = (__bridge id<MTLTexture>)(void*)texture;
        Expect(t.width == kOut && t.height == kOut, "output is 128x128");
        Expect(t.pixelFormat == MTLPixelFormatRGBA16Float, "output is RGBA16F");
    }

    bool sides_ok = true;
    float max_diff = 0.0f;
    for (u32 y = 0; y < kOut; ++y) {
        for (u32 x = 0; x < kOut; ++x) {
            const u32 i = y * kOut + x;
            for (const std::vector<float>* image : {&easu, &rcas}) {
                const float v = (*image)[i];
                sides_ok &= !(x <= 56 && v >= 0.05f) && !(x >= 72 && v <= 0.95f);
            }
            if (x >= 56 && x <= 72) {
                max_diff = std::max(max_diff, std::fabs(easu[i] - rcas[i]));
            }
        }
    }
    Expect(sides_ok, "black at x <= 56 and white at x >= 72, with and without RCAS");
    std::printf("  max |EASU - RCAS| near the edge: %.4f\n", max_diff);
    Expect(max_diff > 0.001f, "RCAS output differs from the EASU-only output near the edge");

    // A disabled pass, or one whose input is not smaller, hands the input back.
    Encoder encoder{device};
    FsrPass::Settings off;
    off.enable = false;
    Expect(pass.Render(encoder, input.Get(), {kIn, kIn}, {kOut, kOut}, off, false) == input.Get(),
           "a disabled pass returns its input");
    Expect(pass.Render(encoder, input.Get(), {kIn, kIn}, {kIn, kIn}, {}, false) == input.Get(),
           "an input as large as the output is returned as is");
    device.Wait(encoder.Submit());
}

} // namespace

int main() {
    @autoreleasepool {
        std::string dir_template =
            (std::filesystem::temp_directory_path() / "bb-fsr-pass-test.XXXXXX").string();
        const char* dir = mkdtemp(dir_template.data());
        if (!dir) {
            std::printf("FAIL: no temporary directory\n");
            return 1;
        }
        SetHostKernelCacheDir(dir);
        Device device;
        TestFsrPassUpscalesEdge(device);
        Expect(device.ErrorCount() == 0, "no command buffer errors on the device queue");
        std::filesystem::remove_all(dir);
    }
    if (failures) {
        std::printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    std::printf("PASS: Metal-native FSR 1 pass\n");
    return 0;
}
