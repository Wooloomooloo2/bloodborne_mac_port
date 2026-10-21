# Upstream (Linux) roadmap, 2026-09

> **macOS:** this is the upstream Linux port's roadmap from before the Mac port, kept as
> technical background. The Mac roadmap is [ROADMAP.md](ROADMAP.md). Measurements below were
> taken on Linux (RX 7800 XT, Steam Deck) unless stated otherwise.

The main text reflects the state as of 2026-09-26; the first section below covers what changed
by 09-30. The dated status snapshots and session reports it used to link to have been removed
from the tree (see git history). Topic details:
[parallel_gpu.md](parallel_gpu.md) (GPU multithreading, stutter during streaming),
[upscaler.md](upscaler.md) (frame analysis of the game, scene constants).

## Update 09-30

- Done after 09-26: FSR 4 INT8 (v07), object motion vectors, preset changes without a
  restart (downscaled scene targets; D32S8 depth copied by a shader), LTO and PGO,
  draw-preparation scaling with the thread count (Steam Deck included).
- Issues from "Known issues" below: item 3 (restart on preset change) is solved; item 1
  (object vectors) is implemented, FSR 4 still ghosts on the character at Ultra Performance;
  item 2 (blurry UI) — the UI is drawn at 1920×1080.
- Limitation: downscaled scene rendering (`SceneTargets::Eligible`) requires a 1920×1080
  target, i.e. it only works with 1080p output.
- Parallelizing the GPU command thread (step 2 of [parallel_gpu.md](parallel_gpu.md)): textures
  on a separate thread in parallel with buffers work correctly but give no speed-up — each
  draw's data migrates between cores. The bottleneck is the GPU command thread: 91% on CPU,
  ~9% waiting on small guest memory copies before a fence. Next: a two-stage pipeline (a second
  thread owns the texture cache and draw recording, with a register copy built from the
  scanner's deltas) or memoizing texture bindings across frames; see "What would scale" in
  parallel_gpu.md.
- **Two-stage draw pipeline** (`vk_draw_pipe.h`): the GPU command thread decodes PM4 and selects
  the pipeline; a second thread (`bb:DrawRec`) with its own register copy binds resources and
  records the draw. +19% FPS on 16 threads, +18% on 8 (like the Steam Deck). On by default with
  8+ threads (`BB_DRAW_PIPE=0/1`). The bottleneck is now the second stage; details and plan in
  parallel_gpu.md.
- Presets and GPU load: the scene is cheap on an RX 7800 XT, so the preset changes GPU time by
  only ~0.5 ms, while FSR 4 itself at 1080p output costs ~1.4 ms (FSR 4 Ultra Performance is
  more expensive than native rendering without an upscaler). Fixed: lighting passes stayed at
  1920×1080 for every preset (the check looked at the wrong target slots); downscaled targets
  are now sampled directly, without resampling to native (the shader cache is rebuilt once).
  Next for the Steam Deck: the guest memory copy of render targets `3d5ebf4e` (see
  parallel_gpu.md).
- **Fixed a crash with the pipeline** (guest heap corruption, guest offset `0x263b8e7`, after
  13–17 minutes): the "GPU idle" interrupt fired before EOP labels deferred to the Vulkan
  recording thread, and the game freed objects carrying those labels. The pipeline and deferred
  labels are now drained first; 2 runs of 30 minutes without crashes (before the fix, 7 of 7
  runs crashed).
- **1440p/2160p output resolution** ("Output resolution" menu, bbport.ini `output_res`, after a
  restart): the scene renders at the preset's fraction of the output, the upscaler (FSR 3 or
  FSR 4) fills the frame, and the UI is rasterized at the output resolution. Game effects
  (chromatic aberration, DoF, blur, SSAO, the game's AA, dynamic shadows, SSR, cutscenes, LOD)
  run at the same resolution.
- **Launcher** (`launcher/bb-launcher.sh`, GTK4/libadwaita) and an **AppImage** for the Steam
  Deck (`bash packaging/appimage.sh` → `dist/`; data in `~/.local/share/bbport`, `--play`
  launches without the launcher window). Both are Linux-only.
- Tools: `BB_PAD_FILE` (scripted gamepad input), `ab.sh` (A/B of `BB_TOGGLE_FILE` bits in the
  running game), GPU thread idle and stall time in `Frame stats`.

## How the port works

- The original eboot (CUSA03173 v01.09) executes directly as x86-64 code. PS4 system libraries
  are replaced by our own HLE runtime in C (`runtime_*.c`, `probe.c`).
- Graphics: the shadPS4 video core and shader recompiler (GPL), built as `libbbgpu`
  (`gpu/`), with our own changes. The full shadPS4 emulator is not used.
- Game patches (FPS, resolution, etc.) are community XML (`patches/Bloodborne.xml`), compiled
  by `patches.py` and applied by the loader at startup.

## Done

### Performance and smoothness

| What | Result |
|---|---|
| Vulkan command recording on a separate thread, texture/buffer caches, rwlock instead of a mutex | 26 → 64 FPS in one scene |
| Draw preparation on 4 threads (`bb:DrawPrep`), validation on the GPU thread | +11.5% |
| 256 KiB unprotect window when the game writes memory (fewer page faults) | +22% |
| Texture descriptor cache, fast path for the same render targets | up to ~94 FPS outdoors |
| Guest memory copies on a thread pool, staging buffer pool | streaming stutters 60–170 ms → 40–50 ms |
| Waiting for copies before game-visible fences | UI flicker removed |
| "Readbacks Relaxed" mode | no "exploding vertices" (FaceGen) |
| 480 Hz vblank + frame limit up to min(display, 120), present right at flip | even frame pacing; running does not slow down above 120 FPS |

Everything can be toggled in game through `BB_TOGGLE_FILE` (a bitmask) for A/B comparisons.
Statistics: `BB_FRAME_STATS=1`.

### Audio, input

- Audio output at the correct tempo (previously clicks every 10.7 ms).
- Gamepad via SDL3, fallback keyboard layout.

### Frame analysis

- `BB_CAPTURE_TRIGGER=<file>` records one frame: passes, targets, shaders, textures,
  constants, blend modes.
- Worked out the structure of Bloodborne's frame (G-buffer, lighting, RGBA16F scene color, fog,
  post-processing, tonemapping, the game's AA, UI) and the 864-byte scene constants
  (view and projection matrices).

### Temporal upscaler (FSR 3.1)

- **Camera motion vectors**: from scene depth and the current and previous frame matrices
  (`vk_camera_motion.cpp`, `camera_motion.comp`). Verified by reprojecting the previous frame:
  static geometry matches.
- **Jitter**: sub-pixel offset of scene geometry through the viewport, Halton(2,3) sequence;
  phase count 8·(output/render)². UI and post-processing are not offset.
- **FSR 3.1** (FireBurn/FSR-Vulkan, native Vulkan, submodule `gpu/third_party/fsr-vulkan`):
  - *Native AA*: on the HDR scene color before post-processing, 1:1.
  - *Quality/Balanced/Performance/Ultra Performance presets* (x1.5/1.7/2/3): the game renders
    at a lower resolution (a resolution patch that `patches.py` derives from the community
    720p patch), and FSR upscales the finished frame to 1920x1080 before the first UI pass.
    The UI and output pass are redirected into full-size images, which the presenter shows
    instead of the game's buffer.
- **Reactive mask** (a snapshot of the scene before transparent draws, compared before fog is
  composited): works, but causes shimmer on Bloodborne's haze — off by default.
- **In-game menu** (Dear ImGui, Insert or L3+R3): upscaler, preset, sharpness, jitter, mask,
  FPS counter. Settings in `bbport.ini`. Preset changes use the "Apply and restart game" button.

## Known issues

State of the last commit at the time. The working tree contained a move of presets to
downscaled render targets while keeping guest buffers, post-processing and UI at 1920×1080.
Transitions between presets and color/depth/stencil copies passed a Vulkan test on Lavapipe.
The RX 7800 XT does not support D32S8 blits, so the new path left FSR with a 1920×1080 input
and lowered FPS. For such GPUs the launch now automatically selects the proven path with the
game resolution patch and a 1920×1080 UI; preset changes again require a restart. GPUs with the
required support keep restart-free switching. Items 2–3 below refer to the last commit, before
these working-tree changes.

The shared object motion vector pass was too expensive: Performance gave about 49 FPS with it
and 77–78 FPS without. The extra target and shaders are now enabled only for G-buffer draws
with a pose/bone buffer; all others use camera vectors. In a trial run the selective path held
about 81–87 FPS, with no skips due to history capacity. Object vectors are on by default;
`BB_OBJECT_MOTION=0` disables them, and `BB_OBJECT_MOTION_ALL=1` restores the old shared path
for comparison. The selection by buffer size is still heuristic: the quality of weapons and
clothing in motion needs in-game verification.

1. **Moving objects "fall apart"** (weapon on the back, the character, cloth, enemies): only
   camera motion vectors exist. The third-person camera follows the character, so camera
   vectors are wrong for it, and the upscaler takes history from the wrong place. This affects
   any temporal upscaler (FSR 3/4, DLSS, XeSS).
2. **The UI is blurry in presets**: with the resolution patch the game believes it is a 540p
   game and apparently prepares fonts and UI elements for that resolution. Menus (title, pause)
   do not go through the upscaler at all.
3. **Preset changes require a restart**: the resolution is set by a code patch at startup, and
   the game allocates its buffers once.

## Plan

### 1. Object motion vectors

Needed by every upscaler and by frame generation. The game does not compute them, so we do.

How:
- For each G-buffer draw (passes with 5+ targets) the recompiler makes a second variant of the
  vertex shader that outputs the position twice: with the current constants and with the
  previous frame's constants. The difference of the projections is the motion vector, written
  to an auxiliary RG16F target as an extra pixel shader output.
- Previous-frame constants: for each draw, remember the contents of the constant buffers
  (object matrix, skeleton bones) keyed by "shader + draw index in the pass" or by data
  address. To find out: how the game stores skeleton matrices — in buffers overwritten every
  frame (then copy the previous version) or in a ring buffer (then the previous frame's address
  suffices).
- Where the object is not found (first frame, new object) — camera vectors, as now.
- Verification: a debug view of the vectors (`BB_DEBUG_MOTION`), reprojection of the previous
  frame with the full vectors, and visually — the saw on the back while running.

### 2. Scaling in the renderer (instead of the resolution patch)

The game runs at 1920x1080 (or the display resolution), and only the 3D scene passes render at
the lower resolution. UI and menus stay sharp, and presets change on the fly.

How:
- The texture cache allocates a downscaled copy for scene targets (G-buffer, depth, lighting,
  scene color, effects); guest memory and the game's descriptors are unchanged.
- Viewport and scissor of passes into these targets are scaled (as already done for the UI,
  including passes with clipping disabled).
- Shaders that compute in pixel coordinates (`gl_FragCoord`, `texelFetch`, per-pixel compute
  shaders) receive a scale factor through the recompiler's push constants. Sampling with
  normalized coordinates works unchanged.
- FSR goes where Native AA is: scene color before post-processing (HDR, best quality);
  post-processing and UI run at full resolution.
- Risks: passes that read scene targets in pixel coordinates during post-processing, and reads
  of these targets from guest memory (they must be forbidden or scaled back).

### 3. Other upscalers

All use the same inputs (color, depth, motion vectors, jitter, masks).

- **FSR 4 (INT8)**: FSR-Vulkan has an experimental FSR 4 v07 INT8 provider — add it as a menu
  option and compare with FSR 3.1 after step 1.
- **DLSS**: native Linux Vulkan SDK (NGX). Does not work on an RX 7800 XT, but needed for
  NVIDIA users.
- **XeSS / XeFG and OptiScaler**: Intel ships them only as Windows DLLs. Options: an in-process
  Windows DLL (PE) loader, or running the whole build under Proton with OptiScaler, feeding it
  inputs through the DLSS interface.
  Checked 09-29: libxess.dll (identical in DOOM: The Dark Ages, PRAGMATA, etc.) has a Vulkan
  path (`xessVKCreateContext`, `xessVKExecute`) but imports ~300 functions: MSVCP140
  (iostream), VCRUNTIME140 (C++ exceptions), UCRT, kernel32, setupapi, dxgi.
  Postponed; the most realistic path is a helper under Wine with shared Vulkan memory.

(On macOS the planned upscaler path is FSR 3.1 followed by MetalFX; see MACOS_PORT.md.)

### 4. Frame generation

- FSR 3.1 Frame Generation (available in FSR-Vulkan): an intermediate frame between two
  finished ones, using the same motion vectors; the UI must be drawn on top separately (UI
  redirection exists — it can be rendered into a separate layer).
- XeFG — after the Windows DLL question is solved (see above).
- Requirement: frame submission through the presenter (queueing and VRR already work).

### 5. Other

- Steam Deck: a portable build (patchelf, no Nix), preset testing at 1280x800.
- Further offloading of the GPU thread (step 2 of [parallel_gpu.md](parallel_gpu.md)).
