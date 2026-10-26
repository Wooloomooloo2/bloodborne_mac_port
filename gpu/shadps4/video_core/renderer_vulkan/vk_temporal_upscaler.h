// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: temporal upscaling of Bloodborne's scene color (docs/upscaler.md), on Metal.
//
// Native AA (output 1920x1080): Run() resolves the HDR scene color right before the game's
// post-processing combine pass, with the scene depth and camera motion vectors, and the merge
// kernel writes the result back so the game's own post, tonemap and UI continue unchanged.
// Other output sizes: RunScaled() resolves the game's tonemapped frame into an output-size UI
// image at the first UI draw; the UI draws and the display pass are redirected to output-size
// images (RedirectColor/RedirectDepth/RedirectSampled) and the presenter shows the output-size
// display buffer (DisplayOverride). Menus and loading screens (no camera) scale the background
// into the UI image instead (RunUiOnly).
//
// Guest targets stay at 1920x1080. Host scene targets use output/preset dimensions,
// resized at frame boundaries (SceneTargets). BB_RENDER_RES retains the older startup-patched
// guest-resolution path.
//
// The resolve step is pluggable (Resolve): TAA, MetalFX and FSR 3.1 plug in there.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "bbport_metalfx.h"
#include "bbport_framegen.h"
#include "ffx_metal/fsr3_metal.h"

#include "common/types.h"
#include "video_core/renderer_metal/mtl_host_kernel.h"
#include "video_core/renderer_metal/mtl_object.h"
#include "video_core/renderer_metal/mtl_scale.h"
#include "video_core/renderer_metal/vk_to_mtl.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_taa.h"
#include "video_core/texture_cache/image.h"

namespace Metal {
class Device;
}

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Scheduler;
class Runtime;
class CameraMotion;
class SceneTargets;

class TemporalUpscaler {
public:
    TemporalUpscaler(Metal::Device& device, Scheduler& scheduler,
                     VideoCore::TextureCache& texture_cache, Runtime& runtime,
                     CameraMotion& camera_motion, SceneTargets& scene_targets);
    ~TemporalUpscaler();

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

    /// A draw into a full-size RGBA16F target with the scene depth: the scene color.
    void OnSceneColor(VideoCore::ImageId color);

    /// A blended (transparent) draw into the scene color that is not a full-screen pass: the
    /// first one of a frame snapshots the opaque scene for the reactive mask.
    void OnBlendedSceneDraw();

    /// A full-screen pass into the scene color: after a snapshot, the reactive mask is taken
    /// before it (the fog composite rewrites every pixel).
    void OnSceneComposite();

    /// Before a compute dispatch: the post-processing combine shader triggers the upscale.
    void OnDispatch(u64 cs_hash);

    /// Start of a frame in the command stream (display pass).
    bool OnFrameStart();
    bool RasterScaling() const;
    /// bbport: LOD bias for scene materials, log2(render / output) while a reduced scene is
    /// upscaled (0 at native size, for TAA and without an upscaler).
    [[nodiscard]] float SceneMipBias() const;
    /// bbport: the state RedirectColor/RedirectDepth/RasterScaling depend on, for memoizing a
    /// draw's render state (Rasterizer::BeginRendering).
    [[nodiscard]] u64 RedirectState() const noexcept {
        return u64(ui_phase) | u64(display_redirect) << 1 | u64(done_this_frame) << 2 |
               u64(ui_color.index) << 8 | u64(ui_depth.index) << 36;
    }

    /// This frame's sub-pixel jitter in pixels (screen x right, y down); zero when off and after
    /// the upscale (post and UI are not jittered).
    [[nodiscard]] std::array<float, 2> Jitter() const noexcept {
        return done_this_frame ? std::array<float, 2>{} : jitter;
    }

    // Scaled presets.

    /// After identifying this draw's targets, before drawing any UI pixels. Menus use
    /// spatial background copy and native UI, without needing scene depth or temporal history.
    void OnDraw(u64 vs_hash, VideoCore::ImageId color, VideoCore::ImageId depth,
                bool native_viewport);

    /// A pass's first color target: the last render-size RGBA8 target before the UI is the
    /// game's finished frame.
    void OnColorTarget(VideoCore::ImageId color);

    struct Target {
        void* texture{}; ///< id<MTLTexture> (unretained; owned by the upscaler)
        u32 width{}, height{};
        bool native_ui = false;
    };
    /// Render target redirection (UI passes, display pass): false when not redirected.
    /// `view` is the game's view of the image: redirected targets are views in its format
    /// (sRGB).
    bool RedirectColor(VideoCore::ImageId color, const VideoCore::ImageViewInfo& view,
                       Target& target);
    bool RedirectDepth(VideoCore::ImageId depth, Target& target);
    /// A sampled image replaced by the upscaled frame (the display pass): `texture`
    /// (id<MTLTexture>) is replaced by a view in the game's format and swizzle when true.
    bool RedirectSampled(VideoCore::ImageId image, const VideoCore::ImageViewInfo& info,
                         void*& texture);
    /// Whether RedirectSampled would replace `image`.
    [[nodiscard]] bool RedirectsSampled(VideoCore::ImageId image) const {
        return display_redirect && image == ui_color;
    }

    /// Presenter: the output-size display buffer standing in for the guest one at `address`.
    struct Display {
        /// id<MTLTexture> (unretained; owned by the upscaler) in `format`, with PixelFormatView
        /// usage: the presenter views it in the video-out format (BGRA of an RGBA buffer).
        void* texture{};
        vk::Format format{};
        u32 width{}, height{};
        BbFrameGen::FramePtr framegen;
    };
    bool DisplayOverride(VAddr address, Display& display);
    BbFrameGen::Generator* FrameGenerator() const { return frame_generator.get(); }
    BbFrameGen::FramePtr FrameForDisplay() const {
        return display_redirect ? display_framegen : BbFrameGen::FramePtr{};
    }

    /// One frame of the resolve step (all textures id<MTLTexture>, unretained, alive until the
    /// GPU has run the frame). Render size: `reactive`; output size: `output`.
    /// `color`, `depth` and `motion` hold the scene in their top-left width x height: exactly
    /// that size (scene proxies) except with BB_RENDER_RES (the startup patch), where colour and
    /// depth are the game's aligned targets, a few pixels larger, and motion has the colour's
    /// size (MetalFX requires it); width x height is then MetalFX's input content size.
    struct ResolveInputs {
        void* color{};    ///< HDR RGBA16F scene color (hdr) or the tonemapped RGBA8 frame
        void* depth{};    ///< scene depth (D32S8 or D32; depth in red when sampled)
        void* motion{};   ///< RG16F, pixels, previous minus current (camera and object motion)
        void* reactive{}; ///< R8 reactive mask, or null (no mask this frame)
        void* output{};   ///< RGBA16F, ShaderWrite and RenderTarget usage
        u32 width{}, height{};         ///< render size
        u32 out_width{}, out_height{}; ///< output size
        std::array<float, 2> jitter{}; ///< this frame's jitter in pixels (x right, y down)
        bool reset{}; ///< discard history (first frame, resize, camera cut, settings change)
        bool hdr{};   ///< linear HDR input (Run) or the tonemapped frame (RunScaled)
        float frame_time_ms{16.6f}; ///< since the previous resolve (FrameTimeMs)
        float camera_near{}, camera_far{}; ///< the scene camera's planes (FSR 3.1)
        float vertical_fov{};              ///< radians
    };

private:
    void Run();
    void RunScaled();
    void RunUiOnly(VideoCore::ImageId color, VideoCore::ImageId depth);
    void EnsureUiResources(u32 width, u32 height, vk::Format color, vk::Format depth);
    void PrepareUiDepth(VideoCore::ImageId depth);
    /// Render size below the scaled-preset output size (the resolution patch is on).
    [[nodiscard]] bool Scaled() const;
    /// A target of the patched render size: the game allocates it with aligned dimensions
    /// (a 1916x1078 scene in 1916x1080 targets).
    [[nodiscard]] bool RenderTarget(u32 w, u32 h) const {
        if (!scaled_session) return w == 1920 && h == 1080;
        return w >= render_width && h >= render_height && w < render_width + 8 &&
               h < render_height + 8;
    }
    /// The scene's size inside a `w` x `h` render target: the game's viewport.
    [[nodiscard]] std::array<u32, 2> SceneSize(u32 w, u32 h) const;
    // The scene color is drawn into a reduced SceneTargets proxy (live presets).
    [[nodiscard]] bool ReducedScene(const VideoCore::Image& color) const;
    /// Available and switched on (menu setting, toggle 1 << 24).
    [[nodiscard]] bool Active() const;
    [[nodiscard]] bool ReactiveOn() const;
    /// The per-size textures; recreated (after scheduler.Finish()) when a size, the input kind
    /// or the provider changes. The motion texture is MotionSize(width, height).
    bool EnsureResources(u32 width, u32 height, u32 out_width, u32 out_height, bool hdr);
    /// The motion texture's size for a `width` x `height` scene: that size, except MetalFX in a
    /// BB_RENDER_RES session, where it is the scene targets' (the scene depth's) size: MetalFX
    /// requires motion and colour of one size. The same at every EnsureResources call, so
    /// nothing is rebuilt per frame; other providers never see the unwritten border.
    [[nodiscard]] std::array<u32, 2> MotionSize(u32 width, u32 height) const;
    /// The kernels and scale pipelines, built once at construction (not at first use on the
    /// encoder thread).
    void CreatePipelines();
    /// Records the reactive mask pass over `color` (id<MTLTexture>, render size); false when
    /// there is no snapshot this frame.
    bool RecordReactive(void* color);
    /// Records the merge of output_image into the scene color `scene` (id<MTLTexture>, output
    /// size, ShaderWrite): RGB replaced, alpha kept; the menu's debug views.
    void RecordMerge(void* scene, bool has_reactive);

    /// The resolve step: `in.color` (with depth, motion, reactive) -> `in.output` with the
    /// selected provider, recorded in order with the frame (scheduler.Record). False when it
    /// cannot run this frame: the frame stays unresolved and the caller sets `failed`.
    bool Resolve(const ResolveInputs& in);
    /// TAA (native resolution): the history ping-pong and the menu's sharpen (0..2).
    bool ResolveTaa(const ResolveInputs& in);
    /// MetalFX's temporal scaler, encoded into the frame's command buffer; recreated (after
    /// scheduler.Finish()) when a format or size changes. Then ExtraSharpen (sharpness > 1).
    bool ResolveMetalFx(const ResolveInputs& in);
    /// AMD FSR 3.1 on the FidelityFX Metal back end (fsr3, made by EnsureResources), recorded in
    /// order with the frame; its RCAS covers sharpness up to 1, ExtraSharpen the rest.
    bool ResolveFsr3(const ResolveInputs& in);
    /// Milliseconds since the previous call (the steady clock); 16.6 for the first and for
    /// values <= 0 or > 200 (a pause, a loading screen).
    float FrameTimeMs();
    /// Sharpness above 1 (the menu's 0..2) for providers whose own sharpening ends at 1: RCAS of
    /// a copy of `target` (id<MTLTexture>, `width` x `height`, ShaderWrite) back into it. `ldr`:
    /// the target is the RGBA8 UI image (sRGB or not), written through its RGBA8Unorm view.
    void ExtraSharpen(void* target, bool ldr, u32 width, u32 height);

    /// A view of `texture` (made in `texture_format`) in the game's `format` with `mapping`,
    /// kept in `views` (by format and mapping) until the texture is recreated.
    struct MirrorView {
        vk::Format format;
        vk::ComponentMapping mapping;
        Metal::Object view;
    };
    void* Mirror(void* texture, vk::Format texture_format, std::vector<MirrorView>& views,
                 vk::Format format, const vk::ComponentMapping& mapping);
    /// The provider could not run: off (logged) until the preset, output size or provider
    /// changes (OnFrameStart).
    void Fail(const char* consequence);

    Metal::Device& device;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    CameraMotion& camera_motion;
    SceneTargets& scene_targets;
    int applied_preset = -1;
    int applied_upscaler = -1;
    bool dispatched_last_frame = false;
    bool last_active = false, last_jitter = false;

    bool enabled = false;
    bool failed = false;
    u64 trigger_hash = 0x9a9cf8a9;
    VideoCore::ImageId scene_color{};
    bool done_this_frame = false;
    u32 preset_file_frames = 0; ///< BB_PRESET_FILE polling
    int applied_output = -1;
    bool snapshot_taken = false;
    bool opaque_valid = false;
    bool mask_ready = false;
    std::array<float, 2> jitter{};
    u32 jitter_index = 0;
    bool reset = true;

    u32 width = 0, height = 0;         ///< render size of the resources
    u32 out_width = 0, out_height = 0; ///< output size of the resources
    bool resources_hdr = true;
    u32 target_width = 1920, target_height = 1080; ///< output size of scaled presets
    u64 ui_trigger_vs = 0x34e8a281;
    VideoCore::ImageId ldr_target{};
    VideoCore::ImageId ui_color{}, ui_depth{};
    bool ui_phase = false;         ///< from the upscale to the next display pass
    bool display_redirect = false; ///< the display pass of an upscaled frame
    bool scaled_session = false;
    u32 ui_width = 0, ui_height = 0;
    u32 render_width = 0, render_height = 0;
    vk::Format ui_format = vk::Format::eUndefined;
    vk::Format ui_depth_format = vk::Format::eUndefined;
    Metal::Object ui_image;       ///< output size, the UI's format (PixelFormatView usage)
    Metal::Object ui_depth_image; ///< output size, the UI's depth format
    std::vector<MirrorView> ui_views;
    struct DisplayImage {
        Metal::Object image;
        std::vector<MirrorView> views;
        vk::Format format{};
        u32 width = 0, height = 0;
        bool valid = false;
        // A drawn buffer that is never flipped must not reserve a snapshot.
        // Lock the current lease in DisplayOverride; expired leases fall back.
        std::weak_ptr<BbFrameGen::Frame> framegen;
    };
    std::mutex display_mutex;
    std::unordered_map<VAddr, DisplayImage> displays;
    std::unique_ptr<BbFrameGen::Generator> frame_generator;
    BbFrameGen::FramePtr pending_framegen, display_framegen;
    u64 framegen_sequence = 0;
    std::chrono::steady_clock::time_point framegen_last{};

    bool resources_ready = false; ///< the textures below match width/height/out size
    int resources_upscaler = -1;  ///< the provider they were made for
    Metal::Object motion_image;   ///< RG16F, render size (BB_RENDER_RES: the colour's size)
    u32 motion_width = 0, motion_height = 0;
    Metal::Object output_image;   ///< output size RGBA16F
    Metal::Object opaque_image;   ///< scene color before the blended draws
    Metal::Object reactive_image; ///< R8 reactive mask
    Metal::HostKernel merge_kernel;
    Taa::Kernels taa;                         ///< built at construction
    std::array<Metal::Object, 2> taa_history; ///< output size RGBA32F (TAA only): RGB, depth
    u32 taa_next = 0;                         ///< the history written this frame
    /// MetalFX (id<MTLFXTemporalScaler>) and what it was made for. Recorded commands use it by
    /// pointer (the encoder keeps it alive once encoded); replaced only after scheduler.Finish().
    Metal::Object metalfx_scaler;
    BbMetalFx::Config metalfx_config{};
    /// FSR 3.1's context and shared textures for the current sizes and input kind (FSR 3.1 only).
    /// Recorded commands use it by pointer; re-created only after scheduler.Finish().
    FfxMetal::Fsr3Metal fsr3;
    /// Set on the recording thread when a recorded FSR dispatch failed (logged there): the next
    /// ResolveFsr3 reports the failure (Fail).
    std::atomic<bool> fsr3_dispatch_failed{false};
    std::chrono::steady_clock::time_point last_frame{};
    /// ExtraSharpen: a copy of the target (RCAS reads neighbours) in its format, and the RGBA8Unorm
    /// views of the copy and of the UI image (LDR).
    Metal::Object extra_sharpen_image;
    Metal::Object extra_sharpen_view;
    u32 extra_sharpen_format = 0;
    u32 extra_sharpen_width = 0, extra_sharpen_height = 0;
    Metal::Object ui_storage_view;
    Metal::HostKernel reactive_kernel;
    Metal::ScaleHelper scale; ///< scaled copies, UI depth resample and clear
};

} // namespace Vulkan
