// SPDX-License-Identifier: GPL-2.0-or-later
// Production Metal trace/blend readbacks over an analytic floor, wall and sky.
// Geometry inspired by lKaniel/bloodborne-server's GPL screen-lighting CPU fixture;
// these checks execute the native Metal implementation, including colour/alpha blending.
#import <Metal/Metal.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_screen_lighting.h"
#include "video_core/renderer_metal/mtl_texture.h"
#include "video_core/renderer_vulkan/screen_lighting_policy.h"

using namespace Metal;
namespace {
constexpr u32 Width = 640, Height = 360;
constexpr float Px = 1.42799f, Py = 2.53865f, Pz = 1.00002f, Pw = -0.0500679f;
Object Upload(Device& device, u32 format, u32 w, u32 h, const void* bytes, u32 stride,
              bool target = false) {
    auto native = (__bridge id<MTLDevice>)device.NativeDevice();
    auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(format)
                                                                   width:w
                                                                  height:h
                                                               mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageShaderRead | (target ? MTLTextureUsageRenderTarget : 0);
    auto image = [native newTextureWithDescriptor:desc];
    [image replaceRegion:MTLRegionMake2D(0, 0, w, h)
             mipmapLevel:0
               withBytes:bytes
             bytesPerRow:stride];
    return Object::Adopt((__bridge_retained void*)image);
}
std::vector<float> Read(Device& device, Encoder& e, void* texture, u32 w, u32 h) {
    auto native = (__bridge id<MTLDevice>)device.NativeDevice();
    auto data = [native newBufferWithLength:size_t(w) * h * 8 options:MTLResourceStorageModeShared];
    e.CopyTextureToBuffer(texture, 0, 0, {}, {w, h, 1}, {(__bridge void*)data, 0}, w * 8,
                          w * h * 8);
    device.Wait(e.Submit());
    assert(device.ErrorCount() == 0);
    const auto* p = static_cast<const __fp16*>(data.contents);
    std::vector<float> out(size_t(w) * h * 4);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = p[i];
    return out;
}
struct Scene {
    std::vector<float> depth;
    std::vector<__fp16> color;
    std::vector<u8> albedo;
    Scene(bool wall = true, float radiance = 0.3f)
        : depth(Width * Height, 1), color(Width * Height * 4), albedo(Width * Height * 4, 128) {
        for (u32 y = 0; y < Height; ++y)
            for (u32 x = 0; x < Width; ++x) {
                const float ray_y = ((y + 0.5f) / Height * 2 - 1) / Py;
                float z = ray_y > 0 ? 1.6f / ray_y : 1e9f;
                if (wall)
                    z = std::min(z, 6.0f);
                const bool sky = z > 1e8f;
                depth[y * Width + x] = sky ? 1 : Pz + Pw / z;
                const bool red = wall && std::abs(z - 6) < 1e-3f;
                const auto i = (y * Width + x) * 4;
                color[i] = red ? radiance : 0.03f;
                color[i + 1] = color[i + 2] = red ? 0.0f : 0.03f;
                color[i + 3] = -0.25f; // material data, preserved exactly by the RGB-only apply
                albedo[i + 3] = 255;
            }
    }
};
size_t FloorPixel(float z) {
    const auto y = u32(std::floor((Py * 1.6f / z + 1) * 0.5f * Height));
    assert(y < Height);
    return (y * Width + Width / 2) * 4;
}
ScreenLightingParams Params() {
    return {.projection = {Px, Py, Pz, Pw},
            .size = {Width, Height},
            .trace_size = {Width / 2, Height / 2},
            .radius = 1.5f,
            .max_radiance = 4,
            .ao = 0.5f,
            .gi = 0.7f,
            .directions = 4,
            .steps = 12};
}
void Run(Device& device) {
    ScreenLightingPasses passes(device);
    auto params = Params();
    const auto sample = [&](Scene& s, float ao, float gi) {
        auto depth =
            Upload(device, MTLPixelFormatR32Float, Width, Height, s.depth.data(), Width * 4);
        // RenderTarget/ShaderRead only: the scene must never require ShaderWrite usage.
        auto color = Upload(device, MTLPixelFormatRGBA16Float, Width, Height, s.color.data(),
                            Width * 8, true);
        auto albedo = Upload(device, MTLPixelFormatRGBA8Unorm_sRGB, Width, Height, s.albedo.data(),
                             Width * 4);
        Encoder e(device);
        auto p = params;
        p.ao = ao;
        p.gi = gi;
        assert(passes.Render(e, depth.Get(), color.Get(), albedo.Get(), p));
        return Read(device, e, color.Get(), Width, Height);
    };
    Scene scene;
    const size_t open = FloorPixel(4.4f), corner = FloorPixel(5.8f);
    const auto ao = sample(scene, 1, 0);
    std::printf("AO: open %.6f corner %.6f (source %.6f)\n", ao[open], ao[corner],
                float(scene.color[open]));
    assert(ao[open] > 0.029f && "open floor must remain open");
    assert(ao[corner] < ao[open] - 0.0005f && "the floor/wall contact is occluded");
    const auto gi = sample(scene, 0, 1);
    std::printf("GI: open %.6f corner %.6f green %.6f\n", gi[open], gi[corner], gi[corner + 1]);
    assert(gi[corner] > gi[corner + 1] + 0.001f && "red wall bounces red onto grey floor");
    assert(gi[open] < gi[corner] && "bounce weakens with distance");
    const auto both = sample(scene, 0.5f, 0.7f);
    for (size_t i = 0; i < both.size(); i += 4) {
        for (u32 c = 0; c < 3; ++c)
            assert(std::isfinite(both[i + c]) && both[i + c] >= 0);
        assert(both[i + 3] == float(scene.color[i + 3]) && "scene material alpha survives");
    }
    Scene sky(false);
    const auto no_wall = sample(sky, 1, 1);
    assert(std::abs(no_wall[corner] - float(sky.color[corner])) < 0.0001f &&
           "open plane has no false AO/GI");
    for (size_t i = 0; i < sky.depth.size(); ++i)
        if (sky.depth[i] == 1)
            for (u32 c = 0; c < 4; ++c)
                assert(no_wall[i * 4 + c] == float(sky.color[i * 4 + c]) && "sky unchanged");
    Scene lamp(true, 1000);
    const auto bright = sample(lamp, 0, 2);
    assert(bright[corner] < 4.1f && "bounded sampled radiance prevents fireflies");
    auto bad = scene;
    bad.depth[corner / 4] = std::numeric_limits<float>::quiet_NaN();
    const auto finite = sample(bad, 1, 1);
    for (size_t i = 0; i < finite.size(); i += 4)
        for (u32 c = 0; c < 3; ++c)
            assert(std::isfinite(finite[i + c]));
    {
        auto depth =
            Upload(device, MTLPixelFormatR32Float, Width, Height, scene.depth.data(), Width * 4);
        auto color = Upload(device, MTLPixelFormatRGBA16Float, Width, Height, scene.color.data(),
                            Width * 8, true);
        auto albedo = Upload(device, MTLPixelFormatRGBA8Unorm_sRGB, Width, Height,
                             scene.albedo.data(), Width * 4);
        Encoder e(device);
        params.ao = params.gi = 0;
        assert(!passes.Render(e, depth.Get(), color.Get(), albedo.Get(), params));
        auto unchanged = Read(device, e, color.Get(), Width, Height);
        for (size_t i = 0; i < unchanged.size(); ++i)
            assert(unchanged[i] == float(scene.color[i]));
        params = Params();
        params.projection[0] = std::numeric_limits<float>::infinity();
        assert(!passes.Render(e, depth.Get(), color.Get(), albedo.Get(), params));
        params = Params();
        params.size[0]--;
        assert(!passes.Render(e, depth.Get(), color.Get(), albedo.Get(), params));
    }
    {
        // Resize scratch textures, then destroy the passes and input owners before GPU completion.
        auto depth =
            Upload(device, MTLPixelFormatR32Float, Width, Height, scene.depth.data(), Width * 4);
        auto color = Upload(device, MTLPixelFormatRGBA16Float, Width, Height, scene.color.data(),
                            Width * 8, true);
        auto albedo = Upload(device, MTLPixelFormatRGBA8Unorm_sRGB, Width, Height,
                             scene.albedo.data(), Width * 4);
        Encoder e(device);
        {
            ScreenLightingPasses temporary(device);
            params = Params();
            assert(temporary.Render(e, depth.Get(), color.Get(), albedo.Get(), params));
            params.trace_size = {Width / 4, Height / 4};
            assert(temporary.Render(e, depth.Get(), color.Get(), albedo.Get(), params));
        }
        depth = {};
        albedo = {};
        auto result = Read(device, e, color.Get(), Width, Height);
        assert(std::isfinite(result[corner]));
    }
}
void Policy() {
    using namespace Vulkan::ScreenLightingPolicy;
    assert(OpaqueComposite(0x8747a367, 0x32e47788, 1, 4, 1, false));
    assert(!OpaqueComposite(0x8747a367, 0x32e47788, 1, 4, 1, true));
    assert(!OpaqueComposite(0x8747a367, 0x32e47789, 1, 4, 1, false));
    assert(!OpaqueComposite(0x8747a367, 0x32e47788, 3, 4, 1, false));
    assert(!OpaqueComposite(0x8747a367, 0x32e47788, 1, 4, 1, false, 3));
    assert((TraceSize(1920, 1080) == std::array<u32, 2>{960, 540}));
    assert((TraceSize(3840, 2160) == std::array<u32, 2>{1280, 720}));
    assert((TraceSize(3840, 1080) == std::array<u32, 2>{1280, 360}));
}
} // namespace
int main() {
    @autoreleasepool {
        Policy();
        Device device;
        Run(device);
    }
    std::puts("PASS: native Metal screen lighting GPU and placement policy");
}
