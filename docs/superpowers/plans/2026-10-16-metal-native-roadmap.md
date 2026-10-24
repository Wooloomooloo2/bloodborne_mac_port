# Metal-native renderer — roadmap

> **For agentic workers:** this is the milestone map. Each milestone gets its own task-by-task
> plan (`2026-10-16-metal-native-mN-*.md`), written when the previous milestone is done, against
> the code that exists then. Only M1's plan exists now.

**Goal:** 0.5.0 renders through Metal only; MoltenVK and the Vulkan loader are removed.

**Spec:** `docs/superpowers/specs/2026-10-16-metal-native-renderer-design.md` (approved 2026-10-16).

**Branch / worktree:** `metal-native` in `~/Developer/GitHub/bb-metal-wt` (the main checkout stays
on `master`, 0.4.0, which the user plays). The branch is allowed to be broken from M2 to M4.

## Global constraints (every milestone)

- Host code is arm64 (`scripts/macos/build_gpu_arm64.sh` → `out/gpu-arm64/libbbgpu.dylib`,
  `out/bbgpu-host`); the game stays x86-64 under Rosetta.
- Plain C++ headers never import Objective-C: Metal objects cross into C++ as `void*` or
  `Metal::Object` (M1). `.mm` files are ARC (`-fobjc-arc`, set by `gpu/CMakeLists.txt`).
- Vulkan-Headers stay as vocabulary only (enums, plain structs); no `vk*` function calls in new code.
- Limits keep MoltenVK's values for parity: uniform/storage/texel alignment 16, non-coherent atom
  16, anisotropy 16, LOD bias 16, sampler count 500000, max buffer = `maxBufferLength`.
- Never test against `~/Library/Application Support/Bloodborne` (the user's save); test data is
  `BB_DATA_DIR=$MAIN/out/macdata` with saves restored from `out/macdata/save-backups/`.
- Never attach lldb to the x86-64 game process (wedges Rosetta).
- Commits end with the `Co-Authored-By` line from the session.

## Milestones

| # | Plan | What | Main files | Gate |
|---|---|---|---|---|
| M1 | `…-m1-device-encoder.md` | `Metal::Object`, Device with ticks (`MTLSharedEvent`) and limits, `Metal::Encoder` (native-typed, from `mtl_frame`'s state machine) | `renderer_metal/mtl_object.*`, `mtl_device.*`, `mtl_encoder.*`, `tests/test_mtl_encoder.mm` | Unit test passes; old path still builds and runs — **done** (2026-10-16) |
| M2 | `…-m2-scheduler-caches.md` | Scheduler on the Encoder (`Record(lambda(Metal::Encoder&))`, recorder thread, host copies, hoisting); caches on Metal: `Buffer`/`UniqueBuffer`, guest arena (private `MTLBuffer`), staging, `Image`/`ImageView`/`Sampler`, `mtl_runtime` transfers, detile kernels, fault parser, blit helper; layouts/barriers deleted; sparse code deleted | `vk_scheduler.*`→`mtl_scheduler.*`, `buffer_cache/`, `texture_cache/`, `vk_runtime`→`mtl_runtime`, `vk_staging_buffer_pool` | Builds (library may not link the old rasterizer); staging/copy unit test; detile kernels equal the CPU detiler on sample tiles — **done** (2026-10-16; vk_runtime.cpp compiles except the M3 header vk_scene_resolution.h) |
| M3 | written after M2 | Pipelines (`mtl_pso_cache`, `mtl_shader`, binary archive, background compiles, `shader_compile` modes); rasterizer, draw prep, shader HLE, scene resolution on the Encoder; tessellation runs and geometry emulation moved from `mtl_frame`; `mtl_vk_layer`, `mtl_shadow`, `mtl_frame` deleted | `vk_pipeline_cache`, `vk_graphics_pipeline`, `vk_compute_pipeline`, `vk_pipeline_serialization`, `vk_rasterizer`, `vk_draw_prep`, `vk_shader_hle`, `vk_scene_resolution` | `bbgpu-host --msl-check` builds every cached pipeline through the new cache — **done** (2026-10-16; 644 of 645 stored pipelines built, 0 failures; the library links with undefined-symbol errors, M4–M7 files stand in through `gpu/shim/stubs_m3.*`) |
| M4 | written after M3 | Presenter on `CAMetalLayer` (2 drawables, `displaySyncEnabled`, fps cap, Retina), post-process pass; first light | `vk_presenter`, `vk_swapchain`, `host_passes/pp_pass`, `gpu/shim/bbgpu.cpp` | Title screen correct at 60 FPS; a save loads — **done** (2026-10-16; title, Central Yharnam and Hunter's Dream at a steady 60.0 FPS (one load hitch per save: 92–144 ms warm, 542 ms in the Dream with 39 compiles); warm start 195 pipelines from the binary archive, 0 compiled; window resize refits; city matches 0.4.0 (the caged figure was missing until the M5 input fix); full-screen switch checked in M5) |
| M5 | written after M4 | ImGui on Metal, MetalFX direct, TAA, camera/object motion, frame capture, GPU time stats; gameplay parity | `gpu/shim/bbport_overlay.cpp`, `bbport_metalfx.mm`, `vk_temporal_upscaler`, `vk_camera_motion`, `vk_object_motion`, `vk_frame_capture` | Three test scenes match 0.4.0; 15-min walks; `MTL_DEBUG_LAYER=1` clean — **met** (2026-10-18: ahead of 0.4.0 in all 16 scene/config pairs, +12-18% at 1440p; walks clean; API and shader validation silent. full-screen switch and the caged figure at the lamp checked by the user) |
| M6 | written after M5 | FSR 3.1 through a Metal `FfxInterface` back end (AMD's SPIR-V → MSL) | `gpu/shim/ffx_metal/`, `host_passes/fsr_pass` | FSR picture matches 0.4.0; ≥ 107 FPS at 1080p Quality — **done** (2026-10-18: bit-identical to the Vulkan path on test frames; 120.2 FPS at 1080p Quality at the headstone, ahead of 0.4.x in all 8 scene/size pairs; debug layer silent; 15-min walk clean; FSR 1 in the presenter) |
| M7 | written after M6 | Remove MoltenVK, Vulkan loader, FSR 4 / 4.1.1, single-process mode, `renderer=` and the launcher picker; x86-64 library trimmed to the remote shim; packaging and CI | `bootstrap_deps.sh`, `gpu/CMakeLists.txt`, `build_macos.sh`, `run.sh`, `package_app.sh`, `.github/workflows/macos-release.yml`, `launcher/` | Spec success criteria 1–5 — **done** (2026-10-19, shipped in 0.5.3: no MoltenVK, Vulkan loader or ICD in the app, no binary imports a `vk*` function, `package_app.sh` checks it; zip 51.3 → 46.7 MB; criteria 2, 3 and 5 met by play through 0.5.3–0.5.4.5, the user's sign-off 2026-10-24; the x86-64 library trimmed by exports, 15.5 → 13.4 MB, the further trim to 5 MB is a MACOS_BACKLOG refinement) |

## Interfaces between milestones

- **M1 → M2/M3:** `Metal::Device` (`CurrentTick`, `KnownGpuTick`, `IsFree`, `Wait`, `Finish`,
  `Commit`, `GetLimits`, `ErrorCount`, `Native`), `Metal::Encoder` (passes, state, draws,
  dispatches, blits, `KeepAlive`, `Submit`, `CommandBuffer`), `Metal::Object`, `Metal::BufferRef`.
  Exact signatures in the M1 plan.
- **M2 → M3:** the scheduler's `Record(Func)` with `Func(Metal::Encoder&)`; cache objects expose
  `Metal::BufferRef` / texture pointers instead of Vulkan handles.
- **M3 → M4:** the rasterizer hands the presenter a finished frame texture.
- **M4 → M5/M6:** the presenter's per-frame command buffer and drawable texture.

## Test scenes and protocols (from M4)

Saves in `out/macdata/save-backups/`: `loadcrash-20261014-134937` (Hunter's Dream headstone),
`cages-20261016` (Central Yharnam lamp), `livecopy-20261016` (Cathedral Ward). Scripts in the
session scratchpad drive them with `BB_PAD_FILE` (title +30 s, Cross twice, walk/turn,
screenshots); speed by the head-to-head protocol against 0.4.0.

## Open items carried from M1 (final review, 2026-10-16)

For the M2 plan:
- **Binding lifetime:** Encoder bindings are unretained and persist across Submit (also >4 KiB
  `SetBytes` temporaries). The scheduler must rebind per command buffer or reset bindings at
  Submit before caches start freeing objects at `IsFree(tick)`.
- `FillBuffer` writes only the low byte of a 32-bit pattern: add a compute fill if the caches
  use non-uniform patterns.
- `SetBytes` over 4 KiB allocates a buffer each time: use a tick-recycled ring.
- Attachments use `slice` only (3D `depthPlane`, stencil-only formats not handled yet).
- `~Encoder` should end an open clear-only pass before checking for a command buffer; `~Device`
  `Finish` has no timeout; `Wait` must not be called from a completion handler; `IsFree` assumes
  same-queue completion order; logging from handlers may outlive logging at exit.

## Open items carried from M2 (2026-10-16)

For M5 (fidelity/performance):
- RGB32 formats need an expand step (no Metal RGB32 texture); view min LOD, sampler LOD bias,
  custom border colours are not carried over; `PixelFormatView` disables lossless compression
  (bandwidth cost). 8 GB Macs: recommended working set (~5.3 GB) is above the texture cache's
  4 GB low-VRAM cutoff, so tune `BB_GC_*` there.
- `BlitHelper` is owned by `Runtime` only; `TextureCache` no longer has one.
- `Image` no longer has `aspect_mask`; buffer_cache/texture_cache derive it from `props.is_depth`.

Resolved in M3: the rasterizer, shader HLE and scene resolution consumers moved to the new API
(`Handle()`, `image_view`, `GetSampler`, `Transit` -> `AccessImage`, `IsBufferAccessed`,
`Runtime::CopyBuffer`/`FillBuffer`, `VideoCore::UniqueImage`, `RenderAttachment`,
`RasterUtil::ToMetalViewport`); `FrontAndBack` cull skips the draw (`DrawState::cull_all`);
`RenderAttachment::clear_value` is value-initialised; every rasterizer image path calls
`Runtime::AccessImage`.

Still open, for the milestones that convert these files:
- Upscaler (M5): `GetImage` x5, `Transit` x9, `usage_flags`/`supported_samples` x7, `backing` x2;
  `SceneTargets::Read(id, info)` lost its Vulkan stage/access parameters (vk_temporal_upscaler.cpp
  ~856).
- Camera motion (M5): `Transit` 290-292, usage/samples, `backing` x2. Object motion (M5)
  `image_view` 204. Post-process pass (M4) `image_view` 199; presenter (M4) `image_view` 416,
  `Transit` 418; `GetWorkSemaphore` residue in the presenter.

## Open items carried from M3 (2026-10-16)

M4 inputs (the presenter):
- **Replace the presenter stand-in**: `gpu/shim/stubs_m3.{h,cpp}` (its `BB_METAL_NATIVE_STAGE < 4`
  part), the `< 4` gate in `vk_presenter.h`, and `BB_UNCONVERTED_4` in `gpu/CMakeLists.txt`
  (vk_presenter, vk_swapchain, host_passes/pp_pass come back); then `BB_METAL_NATIVE_STAGE` 4.
  The stand-in is headless: it owns `Metal::Device`, the draw `Scheduler` (threaded), `Runtime`
  and the `Rasterizer`; `PrepareFrame` flushes the draw scheduler, `Present` does nothing.
- **`Present` contract**: `PrepareFrame` runs on the thread recording into the draw scheduler (it
  ends the frame there: flush); `Present` may run on the swap thread (`bb:Present`,
  `separate_swap`), which must not record into or flush the draw scheduler. The presenter's own
  command buffer orders after the frame through `Timeline::DeviceTickOf` +
  `Encoder::WaitForDeviceTick` (the tick committed first) or by flushing the draw scheduler.
- **`DirectEncoder`** (`Scheduler::DirectEncoder()`) for the presenter's blit/post pass on the
  recording thread; the frame texture comes from the texture cache (`RegisterVideoOutSurface`
  keeps its body in the stand-in).
- **`CAMetalLayer`**: 2 drawables, `displaySyncEnabled`, fps cap, Retina (`BB_HIGH_DPI`);
  `shim/window.cpp` already creates the SDL Metal view and hands its layer over as
  `window_info.render_surface`. `bbgpu.cpp` still sets the `MVK_CONFIG_*` variables (harmless; M7
  removes them).
- **First light**: title screen at 60 FPS, a save loads (roadmap gate). The rasterizer's draw
  path (draw helper, tessellation runs, indirect counts, prepared vertex inputs) and the
  persistent pipeline-cache warm-up run for the first time in game then.

Found by the M3 gate:
- **Binary archive never written**: `PsoCache::Flush` fails to serialize the archive of the game's
  pipelines ("expecting 'fragment' stage" / "missing 'vertex' stage in pipeline no. N", also with
  one creation thread; leaving fragment-less pipelines out did not fix it). PSOs then always
  compile from MSL (the MSL disk cache still works). Investigate in M4 (pipeline warm-up).
- **0.4.0 caches**: the stored pipeline cache is key version 6 / shader binary version 7 (MoltenVK).
  `--msl-check` reads it (vertex attributes from 0.4.0's Vulkan vertex input state); the game's
  `WarmUp` treats it as an older format and clears it on the first M4 run (one full rebuild).
- **Preloaded pipelines get no runtime info**: `LoadGraphicsPipeline` leaves
  `sel.runtime_infos` unset, so the clip-distance-emulation check is skipped on warm-up (the
  Metal profile never asks for the emulation, so nothing is lost today).
- **Both libraries compile the Metal renderer** (the x86-64 one never runs it in split mode;
  `bootstrap_deps.sh` builds SPIRV-Cross for x86-64 too). M7 trims the x86-64 library.
- Vulkan-instance tests (`taa-shader-test`, `camera-motion-test`) are gated off below stage 7;
  M5 ports them to Metal. `scene-resolution-test` and `motion-shader-test` build and pass.
- Deferred minors from the M3 tasks (SDD ledger): debug labels across encoder breaks; one clamp
  encoder per `DrawIndirectCount`; single viewport/scissor; hard-coded 16384 framebuffer and
  restart constants (move to `Metal::Limits`); SPIR-V host shaders nothing includes any more
  (`depth_resample*`, `ms_image_blit*`; `fs_tri_vert` goes with the post-process pass in M4);
  `BlitShaders::Source` compiled twice (blit helper and scene targets).

## Open items carried from M4 (2026-10-16)

For M5 (details in `docs/MACOS_BACKLOG.md` and `docs/FINDINGS.md`, 2026-10-16 first light):
- Pipeline depth/stencil formats vs the pass's attachments: the key uses `DepthValid()` /
  `StencilValid()`, the rasterizer binds the depth buffer only with depth or stencil testing on;
  the only validation message left (~600/s). Attached formats match the images.
- The figure in the cage at the Central Yharnam lamp was missing here until 2026-10-18 (fixed:
  fragment inputs the vertex shader does not write now read as zero; title and city match 0.4.0). Window resize and the full-screen switch checked
  (2026-10-18, by the user).
- Presenter parts off: HDR, display timing (`TakePastTimings`), the settings overlay, FSR 1 (M6).
- The second frame waits ~344 ms for the preloaded pipelines (behind the boot logos).
- Test tooling: `BB_SNAPSHOT_TRIGGER=<file>` writes the drawable as a PNG when the file appears
  (works with the screen locked); the frame stats print GPU memory, the collector's usage and
  trigger, and pipelines from the binary archive vs compiled every 5 s.
