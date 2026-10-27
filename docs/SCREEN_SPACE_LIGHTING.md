# Screen-space lighting on Metal

Select **Screen-space lighting → Low or High** in the launcher's effects page, or in the
in-game **Graphics** tab. It is off by default and changes live in-game. **Extra occlusion**
controls added contact shading (0–1, default 0.5); **Indirect light** controls one diffuse
bounce from visible surfaces (0–2, default 0.7). The game's original SSAO setting stays
independent. Reduce extra occlusion if their combined shading is too dark.

This adds light and occlusion to the floating-point opaque scene. It does not operate on the
finished SDR image. Particles, later fog, depth of field, motion blur, tone mapping and HUD
continue through the game's normal pipeline. The stage does not require an active upscaler.

## Source assessment and implementation choice

The reference is `lKaniel/bloodborne-server` at
[`8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1`](https://github.com/lKaniel/bloodborne-server/tree/8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1),
particularly `bbport/gpu/shadps4/video_core/host_shaders/screen_lighting.glsl`,
`screen_lighting.comp`, and `renderer_vulkan/vk_screen_lighting.cpp`.
The reusable part is its horizon integration: samples newly covering the visible hemisphere
contribute occlusion and bounded scene radiance, avoiding counting the same horizon twice.
That math is adapted under GPL-2.0-or-later, with source attribution in the Metal implementation.

The renderer integration is native Metal rather than a copy of the Vulkan backend:

| Reference path | This implementation |
| --- | --- |
| Vulkan descriptors, storage-image writes and explicit Vulkan barriers | Metal compute and render pipelines using the existing Encoder/Scheduler |
| Half-size Low / full-size High traces | Half-size traces for both; High increases angular/depth samples; maximum trace 1280×720 |
| Full-size compute apply with a 4×4 depth gather | A 2×2 depth/normal-aware reconstruction in a fullscreen blended draw |
| Scene colour needs storage-image usage | Scene needs only its existing render-target and sampled usage; reduced proxies stay compressible |
| RGB/alpha written by compute | RGB-only blend preserves scene material/coverage alpha exactly |
| Optional map ray queries | Screen-space samples only |

The blend evaluates `scene × visibility + albedo × indirect_light`, using the source alpha
blend factor for visibility and masking alpha writes. The trace reads the unmodified opaque
scene; the following draw applies the result. There is no scene read/write feedback inside
one dispatch. Two RGBA16Float trace/guide buffers use about 7.9 MiB at 1080p, capped at about
14.1 MiB for larger outputs. They are cached across quality changes/off periods. Every texture
and pipeline used by an unretained command buffer stays alive through GPU completion; resizing
scratch buffers needs no GPU drain.

Depth-derived geometric normals avoid depending on an unverified game normal-buffer encoding.
The guide stores these normals and linear view depth for edge-aware reconstruction. A small
world-space bias reduces self-shadowing. Sample radiance is capped and non-finite samples are
rejected. The sampling pattern is deterministic; there is no additional temporal history or
accumulation of stale light. Low uses 24 depth samples per trace pixel, High up to 64.

## Placement and fallbacks

A captured game 1.09 frame places the full-screen opaque composite (VS `8747a367`, PS
`32e47788`) ahead of transparency and post-processing. The hook runs immediately after that
admitted draw, once per frame. It requires a complete main-camera G-buffer from this frame,
matching depth and sRGB albedo, compatible target dimensions, and an actual successfully
encoded opaque draw. It reads the exact render attachments, including reduced scene proxies.

Unknown shader variants, external shader replacements, missing/current-frame camera data,
unsupported target formats or a failed opaque draw leave the game unchanged. There is no late
post-process fallback: applying AO after transparencies would darken the wrong surfaces.
There are no asset patches or changes to the HDR or frame-generation feature branches.

## Configuration and diagnostics

```ini
screen_lighting=off
screen_lighting_ao=0.50
screen_lighting_gi=0.70
```

`screen_lighting` accepts `off`, `low`, or `high`. The corresponding environment overrides are
`BB_SCREEN_LIGHTING`, `BB_SCREEN_LIGHTING_AO`, and `BB_SCREEN_LIGHTING_GI`.

For private comparisons, `BB_LIGHTING_FILE` names a text file containing
`quality [view [ao [gi]]]`: quality 0/1/2, view 0 composite / 1 visibility / 2 bounce. It is
polled at frame boundaries. Views still pass through the game's later post-processing and HUD.
`BB_LIGHTING_DUMP_TRIGGER` names a trigger file; creating it captures the next admitted stage
into `BB_LIGHTING_DUMP_DIR`: original/result RGBA16Float scene, RGBA16Float lighting and guide,
D32 depth, and projection/settings metadata. These private captures can contain game imagery.

## Validation and limits

`screen-lighting-test` executes the production Metal trace and blend on an analytic floor,
red wall and sky. It checks contact occlusion, coloured bounce, open surfaces, sky, radiance
limits, finite output, exact scene-alpha preservation, rejected inputs, disabled strengths,
resizing and resource release before GPU completion. It also checks the opaque-shader gate
and trace-size cap. The test is included normally and with Metal validation in
`build_macos.sh --test`. Settings and launcher model checks cover defaults, persistence,
environment overrides and finite bounds.

The first bounded game smoke loaded a private copied base-game save, switched Off/Low/High
and diagnostic views live, exercised camera movement and inspected the HUD/menu. A frame
capture confirmed the hook follows the opaque composite before later transparency passes.
Raw before/after output was finite and preserved all 2,073,600 scene-alpha values. Source save
seed hashes stayed unchanged. Metal validation reported no command-buffer errors.

Both arm64 and x86-64 GPU libraries, the split runtime/host, and the native launcher built.
The production lighting, camera-motion and overlay Metal fixtures passed with validation;
C++ settings and Swift launcher/model checks passed. Additional bounded game runs admitted
the stage at 1920×1080 with Native AA and without an upscaler, and at 1280×720 with MetalFX
Quality (640×360 trace). It also ran with the game's original SSAO disabled. Each run used
a private copied save and left the source seed hashes unchanged.

A warmed, fixed-camera 1080p comparison without GPU profiling alternated Off/Low/Off/High/Off.
The five retained statistics windows per phase averaged 60.00/60.00/60.00/59.78/59.98 FPS
at a 60 FPS cap. GPU busy time varied between control phases (8.00–10.31 ms averages), so
this does not establish a precise lighting overhead or uncapped performance. A separate
instrumented Low run recorded two encoders per frame and approximately 0.6 ms in the
profiler's segment column; its overlapping encoder intervals summed to about 2.7 ms.
Those profiler measurements are not additive or an isolated before/after cost. Capture
and readback introduced stalls and were excluded from the unprofiled comparison.

This is additional screen-space shading, not full global illumination. Hidden/offscreen
surfaces cannot contribute; thin surfaces, disocclusions and depth discontinuities can be
inaccurate. Low may show sampling texture in motion, and High costs more GPU time. The
existing SSAO and baked/ambient lighting can overlap with the additional shading. Gameplay
coverage was limited to the opening clinic. Broader areas, combat, 4K, combined HDR/frame
generation, other shader variants and long-session stability need further testing.
