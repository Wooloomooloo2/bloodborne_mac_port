// SPDX-License-Identifier: GPL-2.0-or-later
// Scene resource selection adapted from lKaniel/bloodborne-server at
// 8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1, vk_screen_lighting.cpp (GPL-2.0-or-later).
// Native Metal resources, guarded placement and the command scheduling are local work.
#include "video_core/renderer_vulkan/vk_screen_lighting.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>
#include <tuple>
#include <cmath>
#include "bbport_settings.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_vulkan/screen_lighting_policy.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {
ScreenLighting::ScreenLighting(Metal::Device& d, Scheduler& s, VideoCore::TextureCache& t,
                               Runtime& r, CameraMotion& c, SceneTargets& targets)
    : device{d}, scheduler{s}, texture_cache{t}, runtime{r}, camera{c}, scene_targets{targets} {
    control_file = std::getenv("BB_LIGHTING_FILE") != nullptr;
    if (const char* v = std::getenv("BB_LIGHTING_VIEW"))
        view = std::clamp(std::atoi(v), 0, 2);
    if (Enabled())
        passes = std::make_unique<Metal::ScreenLightingPasses>(device);
}
ScreenLighting::~ScreenLighting() {
    if (passes)
        scheduler.Finish();
}
bool ScreenLighting::Enabled() const { return BbSettings::Get().screen_lighting != 0; }
void ScreenLighting::OnFrameStart() {
    done = false;
    frame_depth = {};
    frame_albedo = {};
    depth_uid = albedo_uid = 0;
    BbSettings::Get().screen_lighting_active = false;
    ++frame;
    // Private live A/B controls; no saves or game files are modified.
    if (const char* path = std::getenv("BB_LIGHTING_FILE"); path && frame % 32 == 0) {
        if (FILE* f = std::fopen(path, "r")) {
            int quality = -1, debug = -1;
            float ao = -1, gi = -1;
            const int n = std::fscanf(f, "%d %d %f %f", &quality, &debug, &ao, &gi);
            std::fclose(f);
            auto& settings = BbSettings::Get();
            if (n >= 1 && quality >= 0 && quality < BbSettings::ScreenLightingCount &&
                settings.screen_lighting != quality) {
                settings.screen_lighting = quality;
                std::printf("Screen lighting: %s (control file)\n",
                            BbSettings::ScreenLightingNames[quality]);
            }
            if (n >= 2 && debug >= 0 && debug <= 2)
                view = debug;
            if (n >= 3 && std::isfinite(ao) && ao >= 0)
                settings.screen_lighting_ao = std::clamp(ao, 0.0f, 1.0f);
            if (n >= 4 && std::isfinite(gi) && gi >= 0)
                settings.screen_lighting_gi = std::clamp(gi, 0.0f, 2.0f);
        }
    }
}
void ScreenLighting::OnGBuffer(VideoCore::ImageId depth, VideoCore::ImageId albedo) {
    if (!Enabled() || !depth || !albedo || depth != camera.Depth())
        return;
    frame_depth = depth;
    frame_albedo = albedo;
    depth_uid = texture_cache.GetImage(depth).image_uid;
    albedo_uid = texture_cache.GetImage(albedo).image_uid;
}
void ScreenLighting::AfterOpaqueDraw(VideoCore::ImageId color_id, VideoCore::ImageId depth_id,
                                     const RenderState& state,
                                     std::shared_ptr<std::atomic<bool>> encoded) {
    if (!Enabled() || done || !color_id || !frame_albedo || !frame_depth ||
        depth_id != frame_depth || depth_id != camera.Depth() || state.num_colors != 1 ||
        !state.colors[0].texture || !state.depth.texture || state.colors[0].level ||
        state.colors[0].slice || state.depth.level || state.depth.slice)
        return;
    auto& depth = texture_cache.GetImage(depth_id);
    auto& color = texture_cache.GetImage(color_id);
    auto& albedo = texture_cache.GetImage(frame_albedo);
    if (depth.image_uid != depth_uid || albedo.image_uid != albedo_uid ||
        color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
        (albedo.info.pixel_format != vk::Format::eR8G8B8A8Srgb &&
         albedo.info.pixel_format != vk::Format::eB8G8R8A8Srgb) ||
        color.info.size.width != depth.info.size.width ||
        color.info.size.height != depth.info.size.height ||
        albedo.info.size.width != depth.info.size.width ||
        albedo.info.size.height != depth.info.size.height)
        return;
    const auto projection = camera.ProjectionForFrame();
    if (projection[0] == 0 || projection[1] == 0)
        return;
    done = true;
    if (!passes)
        passes = std::make_unique<Metal::ScreenLightingPasses>(device);
    scheduler.EndRendering();
    VideoCore::ImageViewInfo av;
    av.format = albedo.info.pixel_format;
    void* albedo_texture = nullptr;
    if (const auto proxy = scene_targets.SampleProxy(albedo, av))
        albedo_texture = proxy->texture;
    else {
        runtime.AccessImage(&albedo, false);
        albedo_texture = albedo.FindView(av).Texture();
    }
    const auto& settings = BbSettings::Get();
    const auto trace_size = ScreenLightingPolicy::TraceSize(state.width, state.height);
    const bool high = settings.screen_lighting == BbSettings::ScreenLightingHigh;
    const Metal::ScreenLightingParams p{.projection = projection,
                                        .size = {state.width, state.height},
                                        .trace_size = trace_size,
                                        .radius = 1.5f,
                                        .max_radiance = 4.0f,
                                        .ao = settings.screen_lighting_ao,
                                        .gi = settings.screen_lighting_gi,
                                        .directions = high ? 4u : 2u,
                                        .steps = high ? 8u : 6u,
                                        .view = view};
    struct Capture {
        std::string name;
        u32 w{}, h{}, bytes{};
        std::shared_ptr<VideoCore::Buffer> buffer;
    };
    auto captures = std::make_shared<std::vector<Capture>>();
    const char* trigger = std::getenv("BB_LIGHTING_DUMP_TRIGGER");
    const char* dir = std::getenv("BB_LIGHTING_DUMP_DIR");
    std::filesystem::path dump_dir;
    if (trigger && dir && std::remove(trigger) == 0) {
        dump_dir = dir;
        std::filesystem::create_directories(dump_dir);
        for (auto [name, w, h, bytes] : {std::tuple{"scene_before", p.size[0], p.size[1], 8u},
                                         {"scene_after", p.size[0], p.size[1], 8u},
                                         {"trace", trace_size[0], trace_size[1], 8u},
                                         {"guide", trace_size[0], trace_size[1], 8u},
                                         {"depth", p.size[0], p.size[1], 4u}}) {
            captures->push_back({name, w, h, bytes,
                                 std::make_shared<VideoCore::Buffer>(
                                     device, VideoCore::MemoryType::HostCached, 0,
                                     u64(w) * h * bytes, "Screen lighting readback")});
        }
        if (FILE* f = std::fopen((dump_dir / "parameters.json").c_str(), "w")) {
            std::fprintf(
                f,
                "{\"width\":%u,\"height\":%u,\"trace_width\":%u,\"trace_height\":%u,"
                "\"projection\":[%.9g,%.9g,%.9g,%.9g],\"ao\":%.6g,\"gi\":%.6g,\"view\":%u}\n",
                p.size[0], p.size[1], trace_size[0], trace_size[1], projection[0], projection[1],
                projection[2], projection[3], p.ao, p.gi, p.view);
            std::fclose(f);
        }
    }
    auto capture_complete = std::make_shared<std::atomic<bool>>(false);
    auto* profiler = GpuProfiler::Get();
    const u64 prior = profiler ? profiler->Current() : 0;
    if (profiler)
        profiler->Mark(0x5343524c49474854ull,
                       [] { return "screen-space lighting: trace + bilateral blend"; });
    scheduler.Record([pass = passes.get(), source = Metal::Object::Retain(state.colors[0].texture),
                      d = Metal::Object::Retain(state.depth.texture),
                      a = Metal::Object::Retain(albedo_texture), p, captures, encoded,
                      capture_complete, owner = this, high](Metal::Encoder& e) {
        if (!encoded->load(std::memory_order_acquire))
            return;
        const auto copy = [&](size_t i, void* image,
                              Metal::BlitAspect aspect = Metal::BlitAspect::All) {
            const auto& c = (*captures)[i];
            e.CopyTextureToBuffer(image, 0, 0, {}, {c.w, c.h, 1}, c.buffer->Ref(), c.w * c.bytes,
                                  c.w * c.h * c.bytes, aspect);
        };
        if (!captures->empty())
            copy(0, source.Get());
        const bool applied = pass->Render(e, d.Get(), source.Get(), a.Get(), p);
        BbSettings::Get().screen_lighting_active = applied;
        if (applied && !owner->logged.exchange(true))
            std::printf("Screen lighting: native Metal %s, scene %ux%u, trace %ux%u, after opaque "
                        "32e47788\n",
                        high ? "high" : "low", p.size[0], p.size[1], p.trace_size[0],
                        p.trace_size[1]);
        if (applied && !captures->empty()) {
            copy(1, source.Get());
            copy(2, pass->TraceTexture());
            copy(3, pass->GuideTexture());
            copy(4, d.Get(), Metal::BlitAspect::Depth);
            capture_complete->store(true, std::memory_order_release);
        }
    });
    if (profiler)
        profiler->Resume(prior);
    if (!captures->empty()) {
        scheduler.DeferPriorityOperation([captures, dump_dir, capture_complete] {
            if (!capture_complete->load(std::memory_order_acquire))
                return;
            for (const auto& c : *captures) {
                const auto path = dump_dir / (c.name + "_" + std::to_string(c.w) + "x" +
                                              std::to_string(c.h) + ".raw");
                if (FILE* f = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(c.buffer->mapped_data.data(), 1, size_t(c.w) * c.h * c.bytes, f);
                    std::fclose(f);
                }
            }
        });
        std::printf("Screen lighting: private opaque scene readback -> %s\n", dir);
    }
    if (FrameCapture::Active())
        FrameCapture::Note(
            "screen-space lighting after admitted opaque composite, before transparencies");
    scheduler.GetDynamicState().Invalidate();
}
} // namespace Vulkan
