// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// Exercises the production target/resolve path on Metal (no game/window).
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "common/slot_vector.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"

int main() {
    using namespace Vulkan;
    Metal::Device device;
    Scheduler scheduler(device, false);
    Runtime runtime(device, scheduler);
    Common::SlotVector<VideoCore::ImageView> views;
    Common::SlotVector<VideoCore::Image> images;
    VideoCore::ImageInfo ci;
    ci.size = {1920,1080,1};
    ci.resources = {1,1};
    ci.type = AmdGpu::ImageType::Color2D;
    ci.pixel_format = vk::Format::eR8G8B8A8Unorm;
    ci.num_bits = 32;
    const auto color_id = images.insert(device, runtime, views, ci);
    auto di = ci;
    di.pixel_format = vk::Format::eD32SfloatS8Uint;
    di.props.is_depth = di.props.has_stencil = true;
    const auto depth_id = images.insert(device, runtime, views, di);
    SceneTargets targets(device, scheduler, runtime, [&](VideoCore::ImageId id, u64 uid) {
        if (!images.is_allocated(id)) return static_cast<VideoCore::Image*>(nullptr);
        return !uid || images[id].image_uid == uid ? &images[id] : nullptr;
    });
    const bool color_eligible = targets.Eligible(images[color_id]);
    const bool depth_eligible = targets.Eligible(images[depth_id]);
    std::fprintf(stderr, "Scene target eligibility: color=%d depth=%d\n",
                 color_eligible, depth_eligible);
    assert(color_eligible && depth_eligible);

    VideoCore::Buffer readback(device, VideoCore::MemoryType::HostCached, 0, 16, "readback");
    const auto pixel = [&](void* texture, Metal::BlitAspect aspect, u32 level, u32 x, u32 y) {
        std::memset(readback.mapped_data.data(), 0, 4);
        scheduler.EndRendering();
        scheduler.Record([=, ref = readback.Ref()](Metal::Encoder& e) {
            e.CopyTextureToBuffer(texture, level, 0, {x, y, 0}, {1, 1, 1}, ref, 4, 4, aspect);
        });
        scheduler.Finish();
        u32 result;
        std::memcpy(&result, readback.mapped_data.data(), 4);
        return result;
    };
    const auto render = [&](const Metal::RenderPassDesc& pass) {
        scheduler.EndRendering();
        scheduler.Record([pass](Metal::Encoder& e) {
            e.BeginRenderPass(pass);
            e.EndRenderPass();
        });
    };
    VideoCore::ImageViewInfo cv;
    cv.format = ci.pixel_format;
    auto dv = cv;
    dv.format = di.pixel_format;
    // A guest write of the native images (clears): the proxies become stale.
    const auto clear_native = [&](u32 rgba, u32 stencil = 7) {
        runtime.AccessImage(&images[color_id], true);
        runtime.AccessImage(&images[depth_id], true);
        Metal::RenderPassDesc pass{};
        pass.colors[0].texture = images[color_id].GetImage();
        pass.colors[0].clear = true;
        pass.colors[0].clear_color = {double(rgba & 255) / 255, double((rgba >> 8) & 255) / 255,
                                      double((rgba >> 16) & 255) / 255, 1.0};
        pass.depth.texture = images[depth_id].GetImage();
        pass.depth.has_stencil = true;
        pass.depth.clear_depth = pass.depth.clear_stencil = true;
        pass.depth.depth = 0.25;
        pass.depth.stencil = stencil;
        pass.width = 1920;
        pass.height = 1080;
        render(pass);
    };
    using Metal::BlitAspect;
    constexpr u32 stencil_values[] = {0, 255, 128, 85, 170, 7, 13, 255, 0, 170};
    u32 stencil_index = 0;
    for (int preset : {3,1,4,2,0,3,0,1,0,3}) {
        const u32 copied_stencil = stencil_values[stencil_index++];
        const u32 resolved_stencil = copied_stencil ^ 255;
        const auto output = stencil_index == 7 ? SceneResolution::Size{3840,1080}
                          : stencil_index == 9 ? SceneResolution::Size{5120,1440}
                          : stencil_index > 7 ? SceneResolution::Size{3840,2160}
                                              : SceneResolution::Size{};
        const auto size = SceneResolution::ForPreset(preset, output);
        targets.SetSize(size);
        assert(SceneResolution::Unpack(SceneResolution::Pack(size)) == size);
        clear_native(0xffff0000, copied_stencil); // blue
        const u32 x = size.width - 1, y = size.height - 1;
        auto c = targets.Read(color_id,cv);
        assert(pixel(c.texture,BlitAspect::All,0,x,y)==0xffff0000);
        auto d = targets.Read(depth_id,dv);
        assert(std::bit_cast<float>(pixel(d.texture,BlitAspect::Depth,0,x,y))==0.25f);
        assert((pixel(d.texture,BlitAspect::Stencil,0,x,y)&255)==copied_stencil);

        c = targets.Attachment(color_id,cv);
        d = targets.Attachment(depth_id,dv);
        Metal::RenderPassDesc pass{};
        pass.colors[0].texture = c.texture;
        pass.colors[0].clear = true;
        pass.colors[0].clear_color = {1.0, 0.0, 0.0, 1.0};
        pass.depth.texture = d.texture;
        pass.depth.has_stencil = true;
        pass.depth.clear_depth = pass.depth.clear_stencil = true;
        pass.depth.depth = 0.75;
        pass.depth.stencil = resolved_stencil;
        pass.width = size.width;
        pass.height = size.height;
        render(pass);
        // A native read resolves the proxies.
        runtime.AccessImage(&images[color_id], false);
        assert(pixel(images[color_id].GetImage(),BlitAspect::All,0,1919,1079)==0xff0000ff);
        runtime.AccessImage(&images[depth_id], false);
        assert(std::bit_cast<float>(pixel(images[depth_id].GetImage(),BlitAspect::Depth,0,1919,
                                          1079))==0.75f);
        assert((pixel(images[depth_id].GetImage(),BlitAspect::Stencil,0,1919,1079)&255)==
               resolved_stencil);
        clear_native(0xff00ff00); // overwrite after reduced render: proxy must be invalidated
        c=targets.Read(color_id,cv);
        assert(pixel(c.texture,BlitAspect::All,0,x,y)==0xff00ff00);
        std::printf("Scene targets: %ux%u color/depth/stencil roundtrip PASS\n",size.width,size.height);
    }

    // Half-resolution mip chain (the game's bloom pyramid): one proxy per level, scaled by the
    // scene's factor, resolved into its own native level.
    auto hi = ci;
    hi.size = {960, 540, 1};
    hi.resources = {4, 1};
    const auto half_id = images.insert(device, runtime, views, hi);
    const auto scene = SceneResolution::ForPreset(4); // 640x360 (x3 of 1080p)
    targets.SetSize(scene);
    auto& half = images[half_id];
    assert(targets.Eligible(half) && !targets.EligibleScene(half));
    assert((targets.ProxySize(half, 0) == SceneResolution::Size{320, 180}));
    assert((targets.ProxySize(half, 2) == SceneResolution::Size{80, 45}));
    auto lv = cv;
    lv.range.base.level = 2;
    const auto proxy = targets.Attachment(half_id, lv);
    {
        Metal::RenderPassDesc pass{};
        pass.colors[0].texture = proxy.texture;
        pass.colors[0].clear = true;
        pass.colors[0].clear_color = {0.0, 1.0, 0.0, 1.0};
        pass.width = 80;
        pass.height = 45;
        render(pass);
    }
    assert(targets.ProxyCurrent(half, 2, 1) && !targets.ProxyCurrent(half, 0, 4));
    // A native access resolves the level: its last texel (239, 134) is the proxy's colour.
    runtime.AccessImage(&half, false);
    assert(pixel(half.GetImage(), BlitAspect::All, 2, 239, 134) == 0xff00ff00);
    std::puts("Scene targets: half-resolution mip level proxy roundtrip PASS");
    scheduler.Finish();
}
