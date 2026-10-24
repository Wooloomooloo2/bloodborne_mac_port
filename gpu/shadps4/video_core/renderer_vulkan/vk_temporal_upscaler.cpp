// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/motion_history.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/renderer_vulkan/ui_composition.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "bbport_settings.h"
#include "bbport_toggles.h"
#include "common/assert.h"
#include "core/emulator_settings.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/host_shaders/upscale_merge_comp.h"
#include "video_core/host_shaders/upscale_reactive_comp.h"
#include "video_core/renderer_metal/mtl_device.h"
#include "video_core/renderer_metal/mtl_encoder.h"
#include "video_core/renderer_metal/mtl_shader.h"
#include "video_core/renderer_metal/mtl_texture.h"
#include "video_core/renderer_vulkan/vk_camera_motion.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

namespace Vulkan {

namespace {

using Metal::VkToMtl::PixelFormat;

/// bbport: BB_DUMP_TRIGGER=<file> BB_DUMP_DIR=<dir> (default out/dump): creating the file dumps
/// the upscaler's images of the next BB_DUMP_FRAMES (8) frames as raw files
/// <dir>/fNNN_<name>_<w>x<h>_<format>.raw, for checking temporal stability offline.
/// Returns the frame number to dump, or -1.
int DumpFrame() {
    static const char* trigger = std::getenv("BB_DUMP_TRIGGER");
    static int remaining = 0, index = 0, polls = 0;
    if (!trigger) {
        return -1;
    }
    if (remaining == 0) {
        if (++polls % 30 != 0 || std::remove(trigger) != 0) {
            return -1;
        }
        const char* frames = std::getenv("BB_DUMP_FRAMES");
        remaining = frames ? std::max(1, std::atoi(frames)) : 8;
        index = 0;
    }
    --remaining;
    return index++;
}

struct DumpImage {
    void* texture; ///< id<MTLTexture>, level 0
    u32 width, height, bytes_per_pixel;
    const char* name;
    const char* format;
    Metal::BlitAspect aspect = Metal::BlitAspect::All;
};

/// Copies `images` into host buffers after the commands recorded so far and writes them to
/// files once the GPU is done.
void DumpImages(Metal::Device& device, Scheduler& scheduler, int frame,
                std::initializer_list<DumpImage> images) {
    static const std::string dir = [] {
        const char* env = std::getenv("BB_DUMP_DIR");
        return std::string{env && env[0] ? env : "out/dump"};
    }();
    for (const auto& image : images) {
        const u32 row = image.width * image.bytes_per_pixel;
        const u64 size = u64(row) * image.height;
        auto buffer = std::make_shared<VideoCore::Buffer>(device, VideoCore::MemoryType::HostCached,
                                                          0, size, "Upscaler dump");
        scheduler.Record([texture = image.texture, width = image.width, height = image.height,
                          aspect = image.aspect, dst = buffer->Ref(), row,
                          size](Metal::Encoder& e) {
            e.CopyTextureToBuffer(texture, 0, 0, {}, {width, height, 1}, dst, row, u32(size),
                                  aspect);
        });
        char path[512];
        std::snprintf(path, sizeof(path), "%s/f%03d_%s_%ux%u_%s.raw", dir.c_str(), frame,
                      image.name, image.width, image.height, image.format);
        scheduler.DeferPriorityOperation([buffer, size, file = std::string{path}] {
            if (FILE* f = std::fopen(file.c_str(), "wb")) {
                std::fwrite(buffer->mapped_data.data(), 1, size, f);
                std::fclose(f);
            }
        });
    }
    std::printf("Dump: frame %d -> %s\n", frame, dir.c_str());
}

/// A host kernel's threadgroups over `width` x `height` pixels.
void Dispatch2D(Metal::Encoder& e, const Metal::HostKernel& kernel, u32 width, u32 height) {
    e.Dispatch({(width + kernel.local_size[0] - 1) / kernel.local_size[0],
                (height + kernel.local_size[1] - 1) / kernel.local_size[1], 1},
               {kernel.local_size[0], kernel.local_size[1], kernel.local_size[2]});
}

Metal::Object CreateTexture(Metal::Device& device, vk::Format format, u32 width, u32 height,
                            bool render_target, bool shader_write, bool format_views,
                            const char* label) {
    Metal::Object texture = Metal::CreateTexture(device,
                                                 Metal::TextureDesc{
                                                     .pixel_format = PixelFormat(format),
                                                     .width = width,
                                                     .height = height,
                                                     .render_target = render_target,
                                                     .shader_write = shader_write,
                                                     .format_views = format_views,
                                                 },
                                                 label, nullptr);
    ASSERT_MSG(texture, "Upscaler: no {} texture {}x{} ({})", label, width, height,
               vk::to_string(format));
    return texture;
}

} // namespace

TemporalUpscaler::TemporalUpscaler(Metal::Device& device_, Scheduler& scheduler_,
                                   VideoCore::TextureCache& texture_cache_, Runtime& runtime_,
                                   CameraMotion& camera_motion_, SceneTargets& scene_targets_)
    : device{device_}, scheduler{scheduler_}, texture_cache{texture_cache_}, runtime{runtime_},
      camera_motion{camera_motion_}, scene_targets{scene_targets_}, scale{device_} {
    // Available unless BB_UPSCALER=none; on/off and the parameters are the menu's settings.
    const char* env = std::getenv("BB_UPSCALER");
    enabled = !(env && std::strcmp(env, "none") == 0);
    BbSettings::Get().framegen_supported =
        enabled && !EmulatorSettings.IsFsrEnabled() && BbFrameGen::Supported(device);
    if (enabled && BbFrameGen::Requested()) {
        if (EmulatorSettings.IsFsrEnabled()) {
            std::printf("Frame generation: legacy FSR 1 presentation is not supported\n");
        } else if (BbFrameGen::Supported(device)) {
            frame_generator = std::make_unique<BbFrameGen::Generator>(device);
            std::printf("Frame generation: experimental 2x MetalFX enabled; temporal upscaler required\n");
        } else {
            std::printf("Frame generation: unavailable (requires macOS 26 and a supported Metal device)\n");
        }
    }
    if (const char* hash = std::getenv("BB_UPSCALE_BEFORE_CS")) {
        trigger_hash = std::strtoull(hash, nullptr, 16);
    }
    if (const char* hash = std::getenv("BB_UI_TRIGGER_VS")) {
        ui_trigger_vs = std::strtoull(hash, nullptr, 16);
    }
    if (const char* res = std::getenv("BB_OUTPUT_RES")) {
        u32 w = 0, h = 0;
        if (std::sscanf(res, "%ux%u", &w, &h) == 2 && w && h) {
            target_width = w;
            target_height = h;
        }
    }
    // Explicit resolution overrides retain the old compatibility path. Normal presets
    // keep guest allocations/UI native and change only host raster targets at frame boundaries.
    scaled_session = std::getenv("BB_RENDER_RES") && std::getenv("BB_RENDER_RES")[0];
    render_width = 1920;
    render_height = 1080;
    if (const char* res = std::getenv("BB_RENDER_RES")) {
        u32 w = 0, h = 0;
        if (std::sscanf(res, "%ux%u", &w, &h) == 2 && w && h) {
            render_width = w;
            render_height = h;
        }
    }
    if (enabled) {
        CreatePipelines();
        std::printf("Upscaler: available (%s) on scene color before compute shader %016llx\n",
                    BbSettings::UpscalerName(BbSettings::Get().upscaler),
                    static_cast<unsigned long long>(trigger_hash));
    }
}

TemporalUpscaler::~TemporalUpscaler() {
    // Recorded commands may still use the textures (and FSR 3.1's context, destroyed after).
    if (resources_ready || ui_image || fsr3.MemoryBytes() != 0) {
        scheduler.Finish();
    }
}

void TemporalUpscaler::Fail(const char* consequence) {
    failed = true;
    std::printf("Upscaler: %s could not run, %s; off until the preset, output size or upscaler "
                "changes\n",
                BbSettings::UpscalerName(BbSettings::Get().upscaler), consequence);
}

bool TemporalUpscaler::Active() const {
    // Toggle 1 << 24 switches it off at run time (A/B); history restarts after.
    return enabled && !failed &&
           (BbSettings::Get().upscaler == BbSettings::UpscalerFsr3 ||
            BbSettings::Get().upscaler == BbSettings::UpscalerTaa ||
            BbSettings::Get().upscaler == BbSettings::UpscalerMetalFx) &&
           !BbToggle::Disabled(1u << 24);
}

bool TemporalUpscaler::ReactiveOn() const {
    return BbSettings::Get().reactive && !BbToggle::Disabled(1u << 27) &&
           BbSettings::Get().upscaler != BbSettings::UpscalerTaa &&
           BbSettings::Get().upscaler != BbSettings::UpscalerMetalFx; // MetalFX: no mask input
}

void TemporalUpscaler::OnSceneColor(VideoCore::ImageId color) {
    scene_color = color;
}

namespace {
float Halton(u32 index, u32 base) {
    float f = 1.0f, result = 0.0f;
    for (u32 i = index; i > 0; i /= base) {
        f /= float(base);
        result += f * float(i % base);
    }
    return result;
}
} // namespace

bool TemporalUpscaler::ReducedScene(const VideoCore::Image& color) const {
    return !scaled_session && scene_targets.Reduced() && scene_targets.EligibleScene(color);
}

bool TemporalUpscaler::RasterScaling() const {
    return !scaled_session && scene_targets.Reduced() && !done_this_frame;
}

bool TemporalUpscaler::OnFrameStart() {
    // Finish the current HUD before resetting the UI state for the next scene.
    // The packet travels with this exact display buffer, never just a texture address.
    display_framegen = std::exchange(pending_framegen, {});
    if (display_framegen) {
        scheduler.EndRendering();
        scheduler.Record([generator = frame_generator.get(), frame = display_framegen,
                          ui = ui_image.Get()](Metal::Encoder& e) {
            generator->FinishUi(e, frame, ui);
        });
        scheduler.GetDynamicState().Invalidate();
    }
    ++framegen_sequence;
    // bbport: BB_PRESET_FILE=<file> holding a preset number, read about once a second: switches
    // the preset like the menu does (scripted tests of live preset changes).
    static const char* preset_file = std::getenv("BB_PRESET_FILE");
    if (preset_file && ++preset_file_frames % 64 == 0) {
        if (FILE* f = std::fopen(preset_file, "r")) {
            int value = 0;
            int output = -1;
            int provider = -1;
            const int fields = std::fscanf(f, "%d %d %d", &value, &output, &provider);
            if (fields >= 1 && value >= 0 &&
                value < BbSettings::PresetCount) {
                BbSettings::Get().preset.store(value);
            }
            if (fields >= 2 && output >= 0 && output < BbSettings::OutputCount) {
                BbSettings::Get().output_res.store(output);
            }
            if (fields == 3 && provider >= 0 && provider < BbSettings::UpscalerCount) {
                // What runs (an unavailable provider falls back); the saved choice is kept.
                const auto& v = BbSettings::Get();
                BbSettings::Get().upscaler.store(BbSettings::FallbackUpscaler(
                    provider, v.metalfx_supported, v.fsr3_supported, v.preset));
            }
            std::fclose(f);
        }
    }
    const auto& settings = BbSettings::Get();
    // The guest's startup resolution patch remains in effect until restart. Keep the
    // upscaler at the applied preset while the menu saves the requested one for run.sh.
    const int preset = BbSettings::RenderPreset();
    const int upscaler = settings.upscaler.load();
    // A failed provider (a size TAA refuses, an FSR 3.1 context or dispatch FSR refused) stays
    // off until the preset, the output size or the provider changes.
    if (applied_preset != preset || applied_upscaler != upscaler ||
        upscaler == BbSettings::UpscalerOff) {
        failed = false;
    }
    const bool active = Active();
    const bool jitter_on = active && settings.jitter && !BbToggle::Disabled(1u << 25);
    const int output = settings.output_res.load();
    const bool output_changed = !scaled_session && applied_output != output;
    if (output_changed) {
        target_width = BbSettings::OutputWidths[output];
        target_height = BbSettings::OutputHeights[output];
        std::printf("Output resolution: %ux%u (live)\n", target_width, target_height);
        failed = false;
    }
    const bool changed = output_changed || applied_preset != preset || active != last_active ||
                         jitter_on != last_jitter || applied_upscaler != upscaler;
    // Dynamic scene resolution scaling (live preset switching).
    if (!scaled_session) {
        scene_targets.SetSize(SceneResolution::ForPreset(active ? preset : 0,
                                                        {target_width, target_height}));
        render_width = scene_targets.Size().width;
        render_height = scene_targets.Size().height;
    }
    BbSettings::Get().active_render_width = Scaled() ? render_width : scene_targets.Size().width;
    BbSettings::Get().active_render_height = Scaled() ? render_height : scene_targets.Size().height;
    if (changed || !dispatched_last_frame) reset = true;
    if (changed) jitter_index = 0;
    applied_preset = preset;
    applied_output = output;
    applied_upscaler = upscaler;
    last_active = active;
    last_jitter = jitter_on;
    dispatched_last_frame = false;

    // The display pass of an upscaled frame reads the upscaled UI image.
    display_redirect = ui_phase;
    ui_phase = false;
    done_this_frame = false;
    ldr_target = {};
    scene_color = {};
    snapshot_taken = false;
    opaque_valid = false;
    mask_ready = false;
    // Halton(2, 3); the menu or toggle 1 << 25 disables it.
    if (!jitter_on) {
        jitter = {};
        return changed;
    }
    // CameraMotion::OnDisplayPass has already cleared Depth(). Use the render size, which
    // survives the frame boundary (Performance 960 -> 1920 needs 32 phases).
    const u32 phases = Scaled() ? Motion::JitterPhases(render_width, target_width)
        : Motion::JitterPhases(scene_targets.Size().width, 1920);
    jitter_index = jitter_index % phases + 1;
    jitter = {Halton(jitter_index, 2) - 0.5f, Halton(jitter_index, 3) - 0.5f};
    return changed;
}

void TemporalUpscaler::OnDispatch(u64 cs_hash) {
    if (cs_hash != trigger_hash || done_this_frame || failed || Scaled()) {
        return;
    }
    done_this_frame = true;
    if (!scene_color || !camera_motion.Ready() || !Active()) {
        reset = true;
        return;
    }
    Run();
}

std::array<u32, 2> TemporalUpscaler::MotionSize(u32 w, u32 h) const {
    if (!scaled_session || BbSettings::Get().upscaler != BbSettings::UpscalerMetalFx ||
        !camera_motion.Depth()) {
        return {w, h};
    }
    // RunScaled requires the colour target to have the depth's size.
    const auto& size = texture_cache.GetImage(camera_motion.Depth()).info.size;
    return {std::max(w, size.width), std::max(h, size.height)};
}

bool TemporalUpscaler::EnsureResources(u32 w, u32 h, u32 ow, u32 oh, bool hdr) {
    const int upscaler = BbSettings::Get().upscaler.load();
    if (upscaler == BbSettings::UpscalerTaa && (w != ow || h != oh)) {
        std::printf("TAA: render size %ux%u is not the output size %ux%u (native-resolution "
                    "TAA only; remove BB_RENDER_RES)\n", w, h, ow, oh);
        std::fflush(stdout);
        return false;
    }
    const auto [mw, mh] = MotionSize(w, h);
    if (resources_ready && w == width && h == height && ow == out_width && oh == out_height &&
        mw == motion_width && mh == motion_height && hdr == resources_hdr &&
        upscaler == resources_upscaler) {
        return true;
    }
    // Recorded commands of earlier frames can still use the textures (a live output-size or
    // preset change): wait for them before they are released.
    scheduler.Finish();
    resources_ready = false;
    // A failure recorded for the previous context is not this one's.
    fsr3_dispatch_failed.store(false);
    width = w;
    height = h;
    out_width = ow;
    out_height = oh;
    motion_width = mw;
    motion_height = mh;
    resources_hdr = hdr;
    // Motion, opaque and reactive: render size; the output: output size, written by kernels
    // and (MetalFX) as a render target.
    motion_image = CreateTexture(device, vk::Format::eR16G16Sfloat, mw, mh, false, true, false,
                                 "Upscaler motion");
    output_image = CreateTexture(device, vk::Format::eR16G16B16A16Sfloat, ow, oh, true, true,
                                 false, "Upscaler output");
    // Black until a provider writes it: a dispatch that fails on the recording thread (FSR 3.1)
    // shows black for that frame, not uninitialised memory.
    scheduler.EndRendering();
    scheduler.Record([output = output_image.Get()](Metal::Encoder& e) {
        Metal::RenderPassDesc pass;
        pass.colors[0].texture = output;
        pass.colors[0].clear = true;
        pass.colors[0].clear_color = {0.0, 0.0, 0.0, 1.0};
        e.BeginRenderPass(pass);
        e.EndRenderPass();
    });
    scheduler.GetDynamicState().Invalidate();
    // Opaque: also TAA's diagnostic output (BB_TAA_DIAGNOSTICS; TAA runs at render = output
    // size), which the kernel binds as a storage texture every frame.
    opaque_image = CreateTexture(device, vk::Format::eR16G16B16A16Sfloat, w, h, false, true,
                                 false, "Upscaler opaque scene");
    reactive_image = CreateTexture(device, vk::Format::eR8Unorm, w, h, false, true, false,
                                   "Upscaler reactive mask");
    // TAA's history: RGB and depth, written one frame and read the next (taa_next).
    for (auto& history : taa_history) {
        history = upscaler == BbSettings::UpscalerTaa
                      ? CreateTexture(device, vk::Format::eR32G32B32A32Sfloat, ow, oh, false, true,
                                      false, "Upscaler TAA history")
                      : Metal::Object{};
    }
    taa_next = 0;
    if (upscaler != BbSettings::UpscalerMetalFx) {
        metalfx_scaler = {}; // its memory, while another provider runs
    }
    if (upscaler == BbSettings::UpscalerFsr3) {
        // The context is made for the exact sizes; its pipelines come from the back end's cache,
        // so a preset or output size change compiles nothing.
        const FfxMetal::Fsr3Metal::CreateInfo info{w, h, ow, oh, hdr};
        if (!fsr3.Matches(info)) {
            std::string error;
            if (!fsr3.Create(device, info, &error)) {
                std::printf("Upscaler: no FSR 3.1 context for %ux%u -> %ux%u: %s\n", w, h, ow, oh,
                            error.c_str());
                std::fflush(stdout);
                return false;
            }
            std::printf("Upscaler: FSR 3.1 context %ux%u -> %ux%u (%.1f MiB)\n", w, h, ow, oh,
                        double(fsr3.MemoryBytes()) / (1024.0 * 1024.0));
        }
    } else {
        fsr3.Destroy(); // its memory, while another provider runs
    }
    opaque_valid = false;
    mask_ready = false; // the new mask texture holds nothing yet
    reset = true;
    resources_ready = true;
    resources_upscaler = upscaler;
    std::printf("Upscaler: %s %ux%u -> %ux%u (%s)\n", BbSettings::UpscalerName(upscaler), w, h,
                ow, oh, hdr ? "HDR scene color" : "tonemapped frame");
    return true;
}

void TemporalUpscaler::CreatePipelines() {
    merge_kernel = Metal::BuildHostKernel(device, UPSCALE_MERGE_COMP, {}, "upscale_merge");
    reactive_kernel = Metal::BuildHostKernel(device, UPSCALE_REACTIVE_COMP, {}, "upscale_reactive");
    // TAA, its sharpen and the extra sharpen of the other providers (RGBA16F and LDR).
    taa = Taa::Build(device);
    // The scale pipelines of every copy below: the output into the UI image and the menu
    // background (RGBA8, linear or sRGB), the UI depth.
    for (const auto format : {vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb}) {
        scale.PrepareColor(PixelFormat(format), Metal::ScaleFilter::Linear);
        scale.PrepareColor(PixelFormat(format), Metal::ScaleFilter::Nearest);
    }
    for (const auto format : {vk::Format::eD32SfloatS8Uint, vk::Format::eD32Sfloat}) {
        scale.PrepareDepth(PixelFormat(format), Metal::StencilCopy::None);
    }
}

void TemporalUpscaler::OnBlendedSceneDraw() {
    // The mask is opt-in (menu, BB_REACTIVE=1): on Bloodborne's thin mist it trades trails for
    // jitter shimmer, which looked worse. Toggle 1 << 27 switches it off.
    if (snapshot_taken || !scene_color || !Active() || !ReactiveOn()) {
        return;
    }
    snapshot_taken = true;
    auto& color = texture_cache.GetImage(scene_color);
    // With a live preset the scene is in the reduced proxy: the mask and the upscaler must use
    // that size, or each frame would recreate the resources (native <-> reduced).
    const bool reduced = ReducedScene(color);
    const u32 ow = color.info.size.width, oh = color.info.size.height;
    // Scaled presets: the scene fills the top-left of its aligned targets (see RunScaled).
    const auto scene = Scaled() ? SceneSize(ow, oh) : std::array<u32, 2>{ow, oh};
    const u32 w = reduced ? scene_targets.Size().width : scene[0];
    const u32 h = reduced ? scene_targets.Size().height : scene[1];
    if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat) {
        return;
    }
    if (!EnsureResources(w, h, Scaled() ? target_width : ow, Scaled() ? target_height : oh,
                         !Scaled())) {
        Fail("no resources at this size");
        return;
    }
    scheduler.EndRendering();
    void* source = nullptr;
    if (reduced) {
        VideoCore::ImageViewInfo ci;
        ci.format = color.info.pixel_format;
        source = scene_targets.Read(scene_color, ci).texture;
    } else {
        runtime.AccessImage(&color, false);
        source = color.GetImage();
    }
    // On the recording thread, in order with the scene (Metal orders it after the draws that
    // wrote the source and before the mask pass that reads the copy).
    scheduler.Record([source, opaque = opaque_image.Get(), w, h](Metal::Encoder& e) {
        e.CopyTexture(source, 0, 0, {}, opaque, 0, 0, {}, {w, h, 1});
    });
    opaque_valid = true;
}

void TemporalUpscaler::OnSceneComposite() {
    if (!opaque_valid || mask_ready || failed || !ReactiveOn()) {
        return;
    }
    auto& color = texture_cache.GetImage(scene_color);
    const bool reduced = ReducedScene(color);
    const auto scene = Scaled() ? SceneSize(color.info.size.width, color.info.size.height)
                                : std::array<u32, 2>{color.info.size.width, color.info.size.height};
    const u32 w = reduced ? scene_targets.Size().width : scene[0];
    const u32 h = reduced ? scene_targets.Size().height : scene[1];
    if (w != width || h != height) {
        return;
    }
    scheduler.EndRendering();
    void* source = nullptr;
    if (reduced) {
        VideoCore::ImageViewInfo ci;
        ci.format = color.info.pixel_format;
        source = scene_targets.Read(scene_color, ci).texture;
    } else {
        runtime.AccessImage(&color, false);
        source = color.GetImage();
    }
    mask_ready = RecordReactive(source);
}

bool TemporalUpscaler::RecordReactive(void* color) {
    if (!opaque_valid || !ReactiveOn()) {
        return false;
    }
    // Relative color change times the scale (default 1), zero below the threshold (0.2), at
    // most the maximum (0.9, never fully reactive) — the defaults of AMD's mask generator.
    const auto& settings = BbSettings::Get();
    const std::array<float, 3> params{settings.reactive_scale, settings.reactive_max,
                                      settings.reactive_threshold};
    scheduler.EndRendering();
    scheduler.Record([kernel = &reactive_kernel, opaque = opaque_image.Get(), color,
                      mask = reactive_image.Get(), params, w = width,
                      h = height](Metal::Encoder& e) {
        e.SetComputePipeline(kernel->pipeline.Get(), Metal::Access::None); // textures only
        e.SetTexture(Metal::Stage::Compute, kernel->texture_slot[0], opaque);
        e.SetTexture(Metal::Stage::Compute, kernel->texture_slot[1], color);
        e.SetTexture(Metal::Stage::Compute, kernel->texture_slot[2], mask);
        e.SetBytes(Metal::Stage::Compute, Metal::kPushConstantBuffer, params.data(),
                   sizeof(params));
        Dispatch2D(e, *kernel, w, h);
    });
    return true;
}

void TemporalUpscaler::RecordMerge(void* scene, bool has_reactive) {
    // Debug (menu or toggle 1 << 28): the reactive mask in red over a darkened frame; or the
    // motion vectors (menu). The object motion texture is sampled for the motion view only; the
    // motion texture stands in for it when object motion is off.
    const int debug_view = BbSettings::Get().debug_view;
    const u32 mode = has_reactive && (debug_view == BbSettings::DebugReactive ||
                                      BbToggle::Disabled(1u << 28))
                         ? 1
                     : debug_view == BbSettings::DebugMotion ? 2
                                                             : 0;
    void* const motion = motion_image.Get();
    void* objects = camera_motion.ObjectMotionView();
    if (!objects) {
        objects = motion;
    }
    scheduler.EndRendering();
    scheduler.Record([kernel = &merge_kernel, output = output_image.Get(), scene,
                      mask = reactive_image.Get(), motion, objects, mode, w = out_width,
                      h = out_height](Metal::Encoder& e) {
        e.SetComputePipeline(kernel->pipeline.Get(), Metal::Access::None); // textures only
        e.SetTexture(Metal::Stage::Compute, kernel->texture_slot[0], output);
        e.SetTexture(Metal::Stage::Compute, kernel->texture_slot[1], scene);
        e.SetTexture(Metal::Stage::Compute, kernel->texture_slot[2], mask);
        e.SetTexture(Metal::Stage::Compute, kernel->texture_slot[3], motion);
        e.SetTexture(Metal::Stage::Compute, kernel->texture_slot[4], objects);
        e.SetBytes(Metal::Stage::Compute, Metal::kPushConstantBuffer, &mode, sizeof(mode));
        Dispatch2D(e, *kernel, w, h);
    });
}

bool TemporalUpscaler::Resolve(const ResolveInputs& in) {
    switch (BbSettings::Get().upscaler.load()) {
    case BbSettings::UpscalerTaa:
        // TAA's own sharpen covers the menu's whole 0..2.
        return ResolveTaa(in);
    case BbSettings::UpscalerMetalFx:
        if (!ResolveMetalFx(in)) {
            return false;
        }
        ExtraSharpen(in.output, false, in.out_width, in.out_height);
        return true;
    case BbSettings::UpscalerFsr3:
        if (!ResolveFsr3(in)) {
            return false;
        }
        ExtraSharpen(in.output, false, in.out_width, in.out_height);
        return true;
    default:
        return false;
    }
}

bool TemporalUpscaler::ResolveTaa(const ResolveInputs& in) {
    // Native resolution only (EnsureResources refuses other sizes and makes the history).
    if (!taa_history[0] || in.width != in.out_width || in.height != in.out_height) {
        return false;
    }
    static const u32 diagnostics = [] {
        const char* env = std::getenv("BB_TAA_DIAGNOSTICS");
        return env ? u32(std::clamp(std::atoi(env), 1, 3)) : 0u;
    }();
    Taa::Params params{in.jitter, in.reset ? 1u : 0u, diagnostics,
                       camera_motion.TaaDepthParameters()};
    // Optional techniques for A/B in one run (BB_TOGGLE_FILE bits 51-55): tonemapped blending,
    // YCoCg clipping, variance clipping, 3x3 reconstruction, keeping nearer history. Measured in
    // game on 2026-10-02 (static and panning camera, interleaved captures): none improved
    // stability beyond run noise; YCoCg clipping and the reconstruction made it worse. Default:
    // all off.
    params.mode |= (BbToggle::Disabled(BbToggle::TaaTonemapBlend) ? 1u << 8 : 0u) |
                   (BbToggle::Disabled(BbToggle::TaaClip) ? 2u << 8 : 0u) |
                   (BbToggle::Disabled(BbToggle::TaaVariance) ? 4u << 8 : 0u) |
                   (BbToggle::Disabled(BbToggle::TaaFilter) ? 8u << 8 : 0u) |
                   (BbToggle::Disabled(BbToggle::TaaKeepNearerHistory) ? 16u << 8 : 0u);
    if (params.mode & 0xffu) {
        static u32 diagnostic_frames = 0;
        if (diagnostic_frames++ < 16) {
            std::printf("TAA diagnostic: reset=%u jitter=%.6f,%.6f mode=%u\n", params.reset,
                        in.jitter[0], in.jitter[1], params.mode);
        }
    }
    // RCAS of the new history into the output (the history itself stays unsharpened); not over
    // a diagnostic view.
    const auto& settings = BbSettings::Get();
    const float strength = std::clamp(settings.sharpness.load(), 0.0f, 2.0f);
    const bool sharpen = settings.sharpen && strength > 0.0f && !(params.mode & 0xffu);
    const Taa::Textures textures{
        .color = in.color,
        .depth = in.depth,
        .motion = in.motion,
        .history = taa_history[1 - taa_next].Get(),
        .result = in.output,
        .next_history = taa_history[taa_next].Get(),
        .diagnostic = opaque_image.Get(),
    };
    scheduler.EndRendering();
    // Metal orders the history's write in one frame before its read in the next (tracked
    // textures, one queue); with reset the kernel ignores the undefined previous history.
    scheduler.Record([kernels = &taa, textures, params, sharpen, strength, w = in.out_width,
                      h = in.out_height](Metal::Encoder& e) {
        Taa::Record(e, *kernels, textures, params, w, h);
        if (sharpen) {
            Taa::RecordSharpen(e, kernels->sharpen, textures.next_history, textures.result,
                               strength, w, h);
        }
    });
    taa_next = 1 - taa_next;
    return true;
}

void TemporalUpscaler::ExtraSharpen(void* target, bool ldr, u32 w, u32 h) {
    const auto& settings = BbSettings::Get();
    const float extra = std::clamp(settings.sharpness.load(), 0.0f, 2.0f) - 1.0f;
    if (!settings.sharpen || extra <= 0.0f) {
        return;
    }
    ASSERT_MSG(!ldr || target == ui_image.Get(), "ExtraSharpen: the LDR target is the UI image");
    const u32 format = Metal::TexturePixelFormat(target);
    if (!extra_sharpen_image || extra_sharpen_width != w || extra_sharpen_height != h ||
        extra_sharpen_format != format) {
        // Commands of earlier frames may still read the old copy and its view.
        scheduler.Finish();
        extra_sharpen_view = {};
        // The target's format: Metal copies between equal formats (the kernel reads any float
        // format). PixelFormatView usage for the RGBA8Unorm view of an sRGB copy.
        extra_sharpen_image = Metal::CreateTexture(device,
                                                   Metal::TextureDesc{
                                                       .pixel_format = format,
                                                       .width = w,
                                                       .height = h,
                                                       .format_views = true,
                                                   },
                                                   "Upscaler extra sharpen", nullptr);
        ASSERT_MSG(extra_sharpen_image, "Upscaler: no extra sharpen texture {}x{}", w, h);
        extra_sharpen_width = w;
        extra_sharpen_height = h;
        extra_sharpen_format = format;
    }
    void* input = extra_sharpen_image.Get();
    void* output = target;
    if (ldr) {
        // The UI image's format may be sRGB: read and write its bytes unencoded, as FSR 3 writes
        // it through an RGBA8Unorm view too.
        const u32 unorm = PixelFormat(vk::Format::eR8G8B8A8Unorm);
        constexpr std::array<u8, 4> identity{2, 3, 4, 5}; // MTLTextureSwizzleRed..Alpha
        if (!extra_sharpen_view) {
            extra_sharpen_view = Metal::CreateTextureView(input, unorm, Metal::TextureType::Tex2D,
                                                          0, 1, 0, 1, identity);
        }
        if (!ui_storage_view) {
            ui_storage_view = Metal::CreateTextureView(target, unorm, Metal::TextureType::Tex2D, 0,
                                                       1, 0, 1, identity);
        }
        ASSERT_MSG(extra_sharpen_view && ui_storage_view, "ExtraSharpen: no RGBA8Unorm views");
        input = extra_sharpen_view.Get();
        output = ui_storage_view.Get();
    }
    scheduler.EndRendering();
    scheduler.Record([kernel = ldr ? &taa.sharpen_ldr : &taa.sharpen, target,
                      copy = extra_sharpen_image.Get(), input, output, extra, w,
                      h](Metal::Encoder& e) {
        e.CopyTexture(target, 0, 0, {}, copy, 0, 0, {}, {w, h, 1});
        Taa::RecordSharpen(e, *kernel, input, output, extra, w, h);
    });
}

bool TemporalUpscaler::ResolveMetalFx(const ResolveInputs& in) {
    // BB_METALFX_JITTER="sx,sy" and BB_METALFX_JITTER_SIGN: the jitter's signs per axis (the
    // distant-shimmer investigation), the same for Run and RunScaled.
    static const std::array<float, 2> signs = [] {
        const auto s = BbMetalFx::JitterSigns(std::getenv("BB_METALFX_JITTER"),
                                              std::getenv("BB_METALFX_JITTER_SIGN"));
        if (s[0] != 1.0f || s[1] != 1.0f) {
            std::printf("MetalFX: jitter signs %g,%g\n", s[0], s[1]);
        }
        return s;
    }();
    const BbMetalFx::Config config{
        .color_format = Metal::TexturePixelFormat(in.color),
        .depth_format = Metal::TexturePixelFormat(in.depth),
        .motion_format = Metal::TexturePixelFormat(in.motion),
        .output_format = Metal::TexturePixelFormat(in.output),
        .width = in.width,
        .height = in.height,
        .out_width = in.out_width,
        .out_height = in.out_height,
        .hdr = in.hdr,
    };
    bool fresh = false;
    if (!metalfx_scaler || config != metalfx_config) {
        // Commands recorded earlier hold the old scaler by pointer until they are encoded.
        scheduler.Finish();
        metalfx_scaler = BbMetalFx::CreateScaler(device, config);
        metalfx_config = config;
        fresh = true;
        if (!metalfx_scaler) {
            return false; // logged; the caller's Fail() keeps it off until a settings change
        }
    }
    const BbMetalFx::Frame frame{
        .color = in.color,
        .depth = in.depth,
        .motion = in.motion,
        .output = in.output,
        .content_width = in.width,
        .content_height = in.height,
        .jitter_x = signs[0] * in.jitter[0],
        .jitter_y = signs[1] * in.jitter[1],
        .reset = in.reset || fresh,
    };
    scheduler.EndRendering();
    scheduler.Record([scaler = metalfx_scaler.Get(), frame](Metal::Encoder& e) {
        BbMetalFx::Encode(e, scaler, frame);
    });
    return true;
}

bool TemporalUpscaler::ResolveFsr3(const ResolveInputs& in) {
    // An earlier frame's dispatch failed on the recording thread (logged there).
    if (fsr3_dispatch_failed.exchange(false)) {
        return false;
    }
    // EnsureResources made the context for this frame's sizes and input kind.
    if (!fsr3.Matches({in.width, in.height, in.out_width, in.out_height, in.hdr})) {
        return false;
    }
    const auto& settings = BbSettings::Get();
    // Toggle 1 << 26 (tests): the opposite sign convention for FSR.
    const float sign = BbToggle::Disabled(1u << 26) ? -1.0f : 1.0f;
    const FfxMetal::Fsr3Metal::DispatchInfo info{
        .color = in.color,
        .depth = in.depth,
        .motion = in.motion,
        .reactive = in.reactive,
        .output = in.output,
        .render_w = in.width,
        .render_h = in.height,
        .output_w = in.out_width,
        .output_h = in.out_height,
        .jitter_x = sign * in.jitter[0],
        .jitter_y = sign * in.jitter[1],
        .frame_time_ms = in.frame_time_ms,
        .camera_near = in.camera_near,
        .camera_far = in.camera_far,
        .vertical_fov = in.vertical_fov,
        // RCAS strength 0..1 (menu, BB_FSR_SHARPNESS): AMD's RCAS ends at 1; ExtraSharpen adds
        // the rest of the menu's 0..2.
        .sharpness = std::min(settings.sharpness.load(), 1.0f),
        .sharpen = settings.sharpen.load(),
        .reset = in.reset,
    };
    scheduler.EndRendering();
    // AMD's host code keeps per-frame state: dispatches run on the recording thread, in frame
    // order. The context is only re-created after scheduler.Finish().
    scheduler.Record([fsr3 = &fsr3, failed = &fsr3_dispatch_failed, info](Metal::Encoder& e) {
        if (!fsr3->Dispatch(e, info)) {
            failed->store(true);
        }
    });
    return true;
}

float TemporalUpscaler::FrameTimeMs() {
    const auto now = std::chrono::steady_clock::now();
    float frame_ms = std::chrono::duration<float, std::milli>(now - last_frame).count();
    if (frame_ms <= 0.0f || frame_ms > 200.0f) {
        frame_ms = 16.6f;
    }
    last_frame = now;
    return frame_ms;
}

void TemporalUpscaler::Run() {
    if (auto* profiler = GpuProfiler::Get()) {
        const char* label = BbSettings::Get().upscaler == BbSettings::UpscalerTaa
            ? "upscaler Run (TAA)" : "upscaler Run (FSR)";
        profiler->Mark(0xF5A0'0000ull ^ std::hash<std::string_view>{}(label),
                       [label] { return std::string{label}; });
    }
    auto& color = texture_cache.GetImage(scene_color);
    auto& depth = texture_cache.GetImage(camera_motion.Depth());
    const u32 ow = color.info.size.width, oh = color.info.size.height;
    const bool reduced = ReducedScene(color) && scene_targets.EligibleScene(depth);
    const u32 w = reduced ? scene_targets.Size().width : ow;
    const u32 h = reduced ? scene_targets.Size().height : oh;
    // The merge writes the scene color as a storage texture: a texture made without the usage
    // (lazy usages, VideoCore::LazyTextureUsage) gets it once.
    if (!color.backing->desc.shader_write && color.allowed_shader_write) {
        runtime.UpgradeUsage(&color, VideoCore::Image::UsageShaderWrite);
    }
    if (color.info.pixel_format != vk::Format::eR16G16B16A16Sfloat ||
        depth.info.size.width != ow || depth.info.size.height != oh ||
        !color.backing->desc.shader_write) {
        return;
    }
    if (!EnsureResources(w, h, ow, oh, true)) {
        Fail("no resources at this size");
        return;
    }
    scheduler.EndRendering();
    void* input_color = nullptr;
    void* input_depth = nullptr;
    if (reduced) {
        // The proxies hold the scene; the native images are not resolved for this.
        VideoCore::ImageViewInfo ci;
        ci.format = color.info.pixel_format;
        input_color = scene_targets.Read(scene_color, ci).texture;
        ci.format = depth.info.pixel_format;
        input_depth = scene_targets.Read(camera_motion.Depth(), ci).texture;
    } else {
        runtime.AccessImage(&depth, false);
        runtime.AccessImage(&color, false);
        input_color = color.GetImage();
        input_depth = depth.GetImage();
    }
    camera_motion.RecordMotion(input_depth, motion_image.Get(), w, h);
    const bool has_reactive = mask_ready || RecordReactive(input_color);
    const ResolveInputs inputs{
        .color = input_color,
        .depth = input_depth,
        .motion = motion_image.Get(),
        .reactive = has_reactive ? reactive_image.Get() : nullptr,
        .output = output_image.Get(),
        .width = w,
        .height = h,
        .out_width = ow,
        .out_height = oh,
        .jitter = jitter,
        .reset = reset,
        .hdr = true,
        .frame_time_ms = FrameTimeMs(),
        .camera_near = camera_motion.Near(),
        .camera_far = 3000.0f,
        .vertical_fov = camera_motion.VerticalFov(),
    };
    if (!Resolve(inputs)) {
        Fail("the frame stays at render size");
        return;
    }
    reset = false;
    dispatched_last_frame = true;
    // The result replaces the scene color's RGB (its alpha carries data for the post). A write
    // of the native image: dirty proxies are resolved into it first (the alpha), then stale.
    runtime.AccessImage(&color, true);
    RecordMerge(color.GetImage(), has_reactive);
}

std::array<u32, 2> TemporalUpscaler::SceneSize(u32 w, u32 h) const {
    if (!scaled_session) return {render_width, render_height};
    // The game's own render size (scene constants); BB_RENDER_RES until the first camera.
    auto size = camera_motion.RenderSize();
    if (size[0] == 0 || size[1] == 0) {
        size = {render_width, render_height};
    }
    return {std::min(size[0], w), std::min(size[1], h)};
}

float TemporalUpscaler::SceneMipBias() const {
    if (!Active() || BbSettings::Get().upscaler == BbSettings::UpscalerTaa) return 0.0f;
    const float render = float(BbSettings::Get().active_render_width.load());
    const float output = float(Scaled() ? target_width : 1920u);
    return render > 0.0f && render < output ? std::log2(render / output) : 0.0f;
}

bool TemporalUpscaler::Scaled() const {
    // Frame generation needs the finished tonemapped scene and an explicit HUD
    // boundary at native 1080p too. Reuse the established output-size UI path.
    return scaled_session || target_width != 1920 || target_height != 1080 ||
           (frame_generator && Active());
}

void TemporalUpscaler::OnColorTarget(VideoCore::ImageId color) {
    if (ui_phase || !camera_motion.Depth()) {
        return;
    }
    const auto& image = texture_cache.GetImage(color);
    const auto& depth = texture_cache.GetImage(camera_motion.Depth());
    if (image.info.pixel_format == vk::Format::eR8G8B8A8Unorm &&
        image.info.size.width == depth.info.size.width &&
        image.info.size.height == depth.info.size.height) {
        ldr_target = color;
    }
}

void TemporalUpscaler::OnDraw(u64 vs_hash, VideoCore::ImageId color,
                              VideoCore::ImageId depth, bool native_viewport) {
    // bbport: color draws only; depth-only draws (reported since #17) leave 1080p as before.
    if (!Scaled() && color && vs_hash == ui_trigger_vs) done_this_frame = true;
    if (Scaled() && ui_phase && !color && depth && depth != ui_depth &&
        (vs_hash == ui_trigger_vs || UiComposition::MovieShader(vs_hash))) {
        // bbport: a Scaleform stencil mask drawn with depth only (no color target), before any
        // UI draw bound this depth buffer. Without this the mask went into the guest buffer,
        // the first masked draw (the HUD's quick-item icon) then had the host depth prepared
        // with a cleared stencil, and failed its stencil test: a black item slot at 1440p/4K.
        const auto& image = texture_cache.GetImage(depth);
        if (RenderTarget(image.info.size.width, image.info.size.height)) {
            EnsureUiResources(ui_width, ui_height, ui_format, image.info.pixel_format);
            PrepareUiDepth(depth);
            ui_depth = depth;
        }
        return;
    }
    if (!Scaled() || !color) {
        return;
    }
    if (ui_phase) {
        // A UI movie can start without stencil and enable it for later text/masks.
        if (color == ui_color && depth && depth != ui_depth) {
            EnsureUiResources(ui_width, ui_height, ui_format,
                              texture_cache.GetImage(depth).info.pixel_format);
            PrepareUiDepth(depth);
            ui_depth = depth;
        }
        return;
    }
    const auto& image = texture_cache.GetImage(color);
    const bool movie = vs_hash == ui_trigger_vs || UiComposition::MovieShader(vs_hash) ||
                       (scaled_session && native_viewport);
    const bool ui_draw = movie &&
        (image.info.pixel_format == vk::Format::eR8G8B8A8Unorm ||
         image.info.pixel_format == vk::Format::eR8G8B8A8Srgb) &&
        RenderTarget(image.info.size.width, image.info.size.height) &&
        !FrameCapture::IsDisplayBuffer(image.info.guest_address);
    switch (UiComposition::Choose(Scaled(), ui_draw,
                                  camera_motion.Ready() && ldr_target == color,
                                  Active() && !failed)) {
    case UiComposition::Background::None:
        return;
    case UiComposition::Background::Temporal:
        RunScaled();
        if (ui_phase) {
            return;
        }
        // A failed resolve must not force UI text back to the scene resolution.
        [[fallthrough]];
    case UiComposition::Background::Copy:
        RunUiOnly(color, depth);
        return;
    }
}

void TemporalUpscaler::EnsureUiResources(u32 w, u32 h, vk::Format color, vk::Format depth) {
    const bool resized = w != ui_width || h != ui_height;
    const bool new_color = !ui_image || resized || color != ui_format;
    const bool new_depth = !ui_depth_image || resized || depth != ui_depth_format;
    if (!new_color && !new_depth) {
        return;
    }
    // Recorded commands of earlier frames can still use the textures and their views.
    scheduler.Finish();
    ui_width = w;
    ui_height = h;
    if (new_color) {
        // Views in the game's formats (Mirror); storage writes for the resolve providers.
        ui_views.clear();
        ui_storage_view = {};
        ui_image = CreateTexture(device, color, w, h, true, true, true, "Upscaler UI");
        ui_format = color;
    }
    if (new_depth) {
        ui_depth_image = CreateTexture(device, depth, w, h, true, false, false, "Upscaler UI depth");
        ui_depth_format = depth;
    }
    std::printf("UI: native composition %ux%u (scene %ux%u), independent of upscaler history\n",
                w, h, render_width, render_height);
}

void TemporalUpscaler::PrepareUiDepth(VideoCore::ImageId depth_id) {
    scheduler.EndRendering();
    // The scene depth, resampled to the output size (nearest; stencil cleared), when its format
    // matches; else cleared (UI depth tests still work against the cleared buffer).
    void* source = nullptr;
    if (depth_id) {
        auto& depth = texture_cache.GetImage(depth_id);
        if (depth.info.pixel_format == ui_depth_format) {
            runtime.AccessImage(&depth, false);
            source = depth.GetImage();
        }
    }
    scheduler.Record([scale = &scale, source, target = ui_depth_image.Get()](Metal::Encoder& e) {
        if (source) {
            scale->ResampleDepth(e, source, target, Metal::StencilCopy::None);
        } else {
            scale->ClearDepth(e, target, 1.0f, 0);
        }
    });
    scheduler.GetDynamicState().Invalidate();
}

void TemporalUpscaler::RunUiOnly(VideoCore::ImageId color_id, VideoCore::ImageId depth_id) {
    if (auto* profiler = GpuProfiler::Get()) {
        profiler->Mark(0xF5A1'0000ull, [] { return std::string{"upscaler RunUiOnly"}; });
    }
    auto& color = texture_cache.GetImage(color_id);
    const auto depth_format = depth_id ? texture_cache.GetImage(depth_id).info.pixel_format
                                      : vk::Format::eD32SfloatS8Uint;
    EnsureUiResources(target_width, target_height, color.info.pixel_format, depth_format);
    PrepareUiDepth(depth_id);
    // Copy only the background already drawn. The subsequent menu/HUD/text draws execute
    // directly at output resolution, including frames with no scene or camera at all.
    void* source = nullptr;
    if (!scaled_session && camera_motion.Depth() && scene_targets.Reduced() &&
        scene_targets.EligibleScene(color)) {
        VideoCore::ImageViewInfo view;
        view.format = color.info.pixel_format;
        source = scene_targets.Read(color_id, view).texture;
    } else {
        runtime.AccessImage(&color, false);
        source = color.GetImage();
    }
    scheduler.Record([scale = &scale, source, ui = ui_image.Get()](Metal::Encoder& e) {
        scale->ScaleColor(e, source, ui, Metal::ScaleFilter::Linear);
    });
    scheduler.GetDynamicState().Invalidate();
    reset = true; // returning to 3D must not reuse history from before a menu/loading screen
    done_this_frame = ui_phase = true;
    ui_color = color_id;
    ui_depth = depth_id;
}

void TemporalUpscaler::RunScaled() {
    if (auto* profiler = GpuProfiler::Get()) {
        const char* label = BbSettings::Get().upscaler == BbSettings::UpscalerTaa
            ? "upscaler RunScaled (TAA)" : "upscaler RunScaled (FSR)";
        profiler->Mark(0xF5A0'0000ull ^ std::hash<std::string_view>{}(label),
                       [label] { return std::string{label}; });
    }
    auto& color = texture_cache.GetImage(ldr_target);
    auto& depth = texture_cache.GetImage(camera_motion.Depth());
    // The scene fills the top-left w x h of its targets (iw x ih, aligned by the game).
    const u32 iw = color.info.size.width, ih = color.info.size.height;
    const auto [w, h] = SceneSize(iw, ih);
    const u32 ow = target_width, oh = target_height;
    if (depth.info.size.width != iw || depth.info.size.height != ih || w > ow || h > oh) {
        return;
    }
    if (!EnsureResources(w, h, ow, oh, false)) {
        Fail("no resources at this size");
        return;
    }
    EnsureUiResources(ow, oh, color.info.pixel_format, depth.info.pixel_format);
    PrepareUiDepth(camera_motion.Depth());
    void* color_texture = nullptr;
    void* depth_texture = nullptr;
    u32 source_width = iw, source_height = ih;
    if (!scaled_session) {
        VideoCore::ImageViewInfo ci, di;
        ci.format = color.info.pixel_format;
        di.format = depth.info.pixel_format;
        color_texture = scene_targets.Read(ldr_target, ci).texture;
        depth_texture = scene_targets.Read(camera_motion.Depth(), di).texture;
        source_width = w;
        source_height = h;
    } else {
        runtime.AccessImage(&depth, false);
        runtime.AccessImage(&color, false);
        color_texture = color.GetImage();
        depth_texture = depth.GetImage();
    }
    scheduler.EndRendering();
    camera_motion.RecordMotion(depth_texture, motion_image.Get(), w, h);

    const ResolveInputs inputs{
        .color = color_texture,
        .depth = depth_texture,
        .motion = motion_image.Get(),
        .reactive = mask_ready && ReactiveOn() ? reactive_image.Get() : nullptr,
        .output = output_image.Get(),
        .width = w,
        .height = h,
        .out_width = ow,
        .out_height = oh,
        .jitter = jitter,
        .reset = reset,
        .hdr = false,
        .frame_time_ms = FrameTimeMs(),
        .camera_near = camera_motion.Near(),
        .camera_far = 3000.0f,
        .vertical_fov = camera_motion.VerticalFov(),
    };
    const bool ok = Resolve(inputs);
    if (ok) {
        // The HDR-format output into the output-size UI image (same size, format conversion).
        scheduler.Record([scale = &scale, output = output_image.Get(),
                          ui = ui_image.Get()](Metal::Encoder& e) {
            scale->ScaleColor(e, output, ui, Metal::ScaleFilter::Nearest);
        });
        scheduler.GetDynamicState().Invalidate();
        if (frame_generator) {
            const auto now = std::chrono::steady_clock::now();
            const float interval = framegen_last.time_since_epoch().count()
                ? std::chrono::duration<float>(now - framegen_last).count()
                : inputs.frame_time_ms / 1000.f;
            framegen_last = now;
            pending_framegen = frame_generator->Prepare({
                .width = w, .height = h, .out_width = ow, .out_height = oh,
                .ui_format = Metal::TexturePixelFormat(ui_image.Get()),
                .sequence = framegen_sequence, .interval = interval,
                .near_plane = inputs.camera_near, .far_plane = inputs.camera_far,
                .vertical_fov = inputs.vertical_fov,
                .reset = inputs.reset || !camera_motion.Continuous(),
            });
            if (pending_framegen) {
                scheduler.Record([generator = frame_generator.get(), frame = pending_framegen,
                                  depth_texture, motion = motion_image.Get(),
                                  ui = ui_image.Get()](Metal::Encoder& e) {
                    generator->Capture(e, frame, depth_texture, motion, ui);
                });
                scheduler.GetDynamicState().Invalidate();
            }
        }
        reset = false;
        dispatched_last_frame = true;
        if (const int dump = DumpFrame(); dump >= 0) {
            DumpImages(device, scheduler, dump,
                       {{color_texture, source_width, source_height, 4, "input", "rgba"},
                        {motion_image.Get(), w, h, 4, "motion", "rg16f"},
                        {output_image.Get(), ow, oh, 8, "output", "rgba16f"},
                        {depth_texture, source_width, source_height, 4, "depth", "f32",
                         Metal::BlitAspect::Depth}});
            if (BbSettings::Get().upscaler == BbSettings::UpscalerTaa && taa_history[0] &&
                std::getenv("BB_TAA_DIAGNOSTICS")) {
                // The history read this frame and the diagnostic output.
                DumpImages(device, scheduler, dump,
                           {{taa_history[taa_next].Get(), ow, oh, 16, "previous_history",
                             "rgba32f"},
                            {opaque_image.Get(), ow, oh, 8, "history_sample", "rgba16f"}});
            }
            // ObjectMotion's attachment can be in a different resolution from the upscaler's
            // inputs: only dump it when it matches, to avoid an out-of-bounds GPU copy.
            if (void* const object = camera_motion.ObjectMotionImage(w, h)) {
                DumpImages(device, scheduler, dump, {{object, w, h, 16, "objects", "rgba32f"}});
            }
        }
    } else {
        Fail("the background is scaled instead");
    }
    done_this_frame = true; // the UI is not jittered
    if (ok) {
        ui_phase = true;
        ui_color = ldr_target;
        ui_depth = camera_motion.Depth();
    }
}

void* TemporalUpscaler::Mirror(void* texture, vk::Format texture_format,
                               std::vector<MirrorView>& views, vk::Format format,
                               const vk::ComponentMapping& mapping) {
    for (const auto& entry : views) {
        if (entry.format == format && entry.mapping == mapping) {
            return entry.view.Get();
        }
    }
    // The port's textures have PixelFormatView usage: the game's format (its sRGB twin, or
    // another of the same texel size) can view them. Where Metal cannot make that view, the
    // texture's own format is used, with that format's swizzle (as SceneTargets::View).
    const u32 requested = PixelFormat(format);
    const u32 pixel_format =
        Metal::SafeViewPixelFormat(Metal::TexturePixelFormat(texture), requested, true);
    const auto swizzle = Metal::VkToMtl::ComposeSwizzle(
        mapping, pixel_format == requested ? format : texture_format);
    Metal::Object view =
        Metal::CreateTextureView(texture, pixel_format, Metal::TextureType::Tex2D, 0, 1, 0, 1,
                                 {swizzle.r, swizzle.g, swizzle.b, swizzle.a});
    ASSERT_MSG(view, "Upscaler: no {} view of an output-size texture", vk::to_string(format));
    views.push_back({format, mapping, std::move(view)});
    return views.back().view.Get();
}

bool TemporalUpscaler::RedirectColor(VideoCore::ImageId color,
                                     const VideoCore::ImageViewInfo& view_info, Target& target) {
    if (ui_phase && color == ui_color) {
        target = {Mirror(ui_image.Get(), ui_format, ui_views, view_info.format, {}),
                  ui_width, ui_height, true};
        return true;
    }
    const auto& image = texture_cache.GetImage(color);
    const VAddr address = image.info.guest_address;
    if (!FrameCapture::IsDisplayBuffer(address)) {
        display_redirect = false;
        return false;
    }
    std::scoped_lock lock{display_mutex};
    auto& display = displays[address];
    if (!display_redirect) {
        display.valid = false; // not upscaled: the presenter shows the guest buffer
        display.framegen.reset();
        return false;
    }
    if (!display.image || display.format != image.info.pixel_format ||
        display.width != ui_width || display.height != ui_height) {
        // Recorded commands of earlier frames can still use the texture and its views.
        scheduler.Finish();
        display.views.clear();
        std::printf("Display: host buffer %ux%u (live)\n", ui_width, ui_height);
        display.format = image.info.pixel_format;
        display.width = ui_width;
        display.height = ui_height;
        display.image = CreateTexture(device, display.format, ui_width, ui_height, true, false,
                                      true, "Upscaler display");
    }
    display.valid = true;
    display.framegen = display_framegen;
    target = {Mirror(display.image.Get(), display.format, display.views, view_info.format, {}),
              ui_width, ui_height};
    return true;
}

bool TemporalUpscaler::RedirectDepth(VideoCore::ImageId depth, Target& target) {
    if (!ui_phase || depth != ui_depth) {
        return false;
    }
    target = {ui_depth_image.Get(), ui_width, ui_height, true};
    return true;
}

bool TemporalUpscaler::RedirectSampled(VideoCore::ImageId image,
                                       const VideoCore::ImageViewInfo& info, void*& texture) {
    if (!display_redirect || image != ui_color) {
        return false;
    }
    // Metal orders the read after the UI passes that drew the image (tracked texture).
    texture = Mirror(ui_image.Get(), ui_format, ui_views, info.format, info.mapping);
    return true;
}

bool TemporalUpscaler::DisplayOverride(VAddr address, Display& display) {
    std::scoped_lock lock{display_mutex};
    const auto it = displays.find(address);
    if (it == displays.end() || !it->second.valid) {
        return false;
    }
    display = {it->second.image.Get(), it->second.format, it->second.width, it->second.height,
               std::exchange(it->second.framegen, {})};
    // Diagnostic capture of the actual completed display buffer, including native UI.
    // BB_PRESENT_DUMP_COUNT=N: the trigger dumps N consecutive frames (temporal stability).
    static const char* dump = std::getenv("BB_PRESENT_DUMP_TRIGGER");
    static const int dump_count = [] {
        const char* env = std::getenv("BB_PRESENT_DUMP_COUNT");
        return env ? std::max(1, std::atoi(env)) : 1;
    }();
    static int dump_index = 0;
    static int dump_remaining = 0;
    if (dump && dump_remaining == 0 && std::remove(dump) == 0) {
        dump_remaining = dump_count;
    }
    if (dump_remaining > 0) {
        --dump_remaining;
        scheduler.EndRendering();
        const bool bgra = display.format == vk::Format::eB8G8R8A8Unorm ||
                          display.format == vk::Format::eB8G8R8A8Srgb;
        DumpImages(device, scheduler, dump_index++,
                   {{display.texture, display.width, display.height, 4, "present",
                     bgra ? "bgra" : "rgba"}});
    }
    return true;
}

} // namespace Vulkan
