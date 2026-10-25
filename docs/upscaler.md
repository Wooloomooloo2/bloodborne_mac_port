# Temporal upscaling and frame generation — frame analysis and plan

## Experimental MetalFX frame generation on macOS (2026-10-24)

Open the in-game menu with **L3+R3** (or **Cmd+,**) and select **Display → Frame
generation → MetalFX 2x (experimental)** to enable an intermediate image between
each pair of real frames. The choice is saved as `framegen=0|1` in `bbport.ini` and
is **off by default**. Use **Apply and Restart Game** after changing it; toggling
back to the running choice clears the restart notice. The current session keeps
its existing presentation resources until restart.

It requires macOS 26+, a build with the macOS 26 SDK or newer, a
device accepted by `MTLFXFrameInterpolatorDescriptor.supportsDevice`, an active
temporal upscaler (`metalfx`, `fsr3`, or `taa`), and a known window-display refresh
rate at least twice the configured real FPS cap. A 60 Hz display permits a 30 FPS
cap, while a 60 FPS cap needs at least 120 Hz; 120 FPS needs at least 240 Hz.
An uncapped launch or unknown refresh rate disables the switch with a short note.
The window thread reads SDL's current mode for the window's own display and updates
it on display/mode/fullscreen changes. Moving to a slower display suspends snapshot
capture and paired presentation; returning to a suitable display resumes them.
The control explains when generation is unavailable.
`BB_METAL_FRAMEGEN=0|1` still overrides the setting at launch and
disables the menu switch for that session; remove it to use the menu. It never
overwrites the file's `framegen` preference: unrelated menu saves preserve the
pre-override value (including the default off when no key existed). For example,
with your existing game/data configuration:

```sh
BB_METAL_FRAMEGEN=1 BB_UPSCALER=metalfx bash run.sh
```

Keep the existing game FPS cap at the desired **real** frame rate. The presenter
attempts two images per real frame; simulation, physics, input polling and guest
completion signals keep their existing rates. A 120 Hz display can show a 60 FPS
real-frame stream plus intermediates. Generating images adds GPU work and latency;
it does not make the game simulate faster or guarantee a higher delivered rate.
The FPS counter and settings footer show **Real** and **Displayed** rates separately
only while frame generation is enabled for the running session on a supported path.
With frame generation off or unavailable, they retain the original single FPS and
frame-time readout: the exponential present-interval average (95% previous / 5%
new interval), including reused and blank frames. Pending changes to the
frame-generation switch take effect after restart.
Real FPS counts new game frames supplied to the presenter, excluding reused and
blank frames. Displayed FPS counts distinct, positive Metal drawable presentation
timestamps for real and generated images, rather than assuming a 2x multiplier or
counting GPU completions. Missing generated frames, refresh limits and real-only
fallback therefore affect the displayed reading. Both rates use bounded one-second
cadence windows and expire after a pause; the milliseconds are frame intervals,
not GPU execution times. The generated/real pair replays the same counter snapshot.
Presentation callbacks are requested only while frame generation is active and
the FPS counter or settings menu is visible. The counter reserves a fixed width
with extra room when frame generation is active, and right-aligns the FPS and
millisecond columns so changing digit counts
cannot resize the box or shift the columns.

The renderer snapshots depth, current-to-previous motion, and pre-HUD color at the
tonemapped scene/UI boundary. A second pass identifies changes made by the HUD.
Interpolation reads through the actual video-out view after the game's gamma LUT,
so its channel swizzle and sRGB decode match the real frame. Source coordinates
clamp to the actual video-out texture's width and height, including native 16:10
and ultrawide paths with smaller video-out images. A dilated HUD mask
restores current real-frame colors after interpolation. At native 1080p this
opt-in mode uses the existing output-size UI path and reconstructs the tonemapped
scene, rather than the default HDR reconstruction before game post-processing.

Snapshots belong to individual frames and travel with the display buffer to the
presenter. Three reusable snapshot sets bound allocation (25% fewer sets than the
original four-set pool); busy slots skip generation without waiting. Deferred
closures, GPU completion handlers, interpolation history,
and the final real-frame present pin the relevant sets until all reads complete.
The real present's completion releases its snapshot before returning the frame
to the pool; skipped presents release it too. Display-buffer entries hold weak
references to a distinct lease for each use of a snapshot allocation, so buffers
that were drawn but never flipped cannot reserve a set or acquire it after reuse.
Queued recording, history, and GPU completion handlers keep strong leases until
their reads finish.
The two presents wait on the same render tick. A three-drawable layer admits the
pair; a missing second drawable presents the real frame alone. The same overlay
draw data is rendered over both images without advancing UI/input/FPS timing twice.
The minimum display interval comes only from the configured real FPS cap (half its
period, with scheduling slack). Measured frame cadence includes drawable waits;
using it to extend the next display hold creates a feedback loop that can collapse
real FPS. The duration helper returns zero for uncapped output; the display policy
disables paired generation for uncapped launches.

Warmup, camera cuts, projection/resolution changes, missing scene frames, invalid
timing, and gaps in real-frame sequence reset history. Menus/loading screens with
no matching scene use real frames. Unsupported devices/SDKs retain normal
presentation. The legacy FSR 1 presenter pass and 10-bit video-out conversion use
real frames alone. The MetalFX effect is encoded on the existing render queue;
there is no extra interpolation queue or CPU wait per generated frame.

This remains experimental: interpolation currently sees the composited display
image, and the mask restores the current HUD footprint rather than reconstructing
the scene hidden by the previous HUD. Moving/fading HUD elements, transparency and
disocclusions can still leave artifacts. Extended gameplay, input latency, GPU cost,
and long-session behavior still require QA.

The counter, wider layout and presentation path have also been checked in the
DLC-enabled city-gate scene. The timed comparison below includes actual real and
displayed cadence; generation still adds GPU work and can lower real FPS.

Offscreen checks (no game/window):

```sh
cmake --build out/gpu-arm64 --target framegen-test
out/gpu-arm64/framegen-test
MTL_DEBUG_LAYER=1 out/gpu-arm64/framegen-test
cmake --build out/gpu-arm64 --target overlay-test
out/gpu-arm64/overlay-test
MTL_DEBUG_LAYER=1 out/gpu-arm64/overlay-test
cmake --build out/gpu-arm64 --target frame-rates-test
out/gpu-arm64/frame-rates-test
```

The overlay fixture covers L3+R3 opening/closing, D-pad/Cross activation of the new
row, saved preferences, reverting a pending restart, and unavailable/overridden
controls, refresh-limited/uncapped launches, and re-enabling after a display change.
The settings round-trip fixture checks temporary launch overrides in both
directions, the absent-key default, unrelated saves, and preservation
of the launcher's other keys. These are offscreen input events, not a physical
controller gameplay test.

The frame-rate fixture checks independent real/displayed rates, 2x delivery,
real-only fallback, refresh limits, invalid/duplicate/out-of-order callbacks,
pause/resume, the original off-path interval average, and concurrent updates.
The overlay fixture verifies the single-rate
counter and settings footer with frame generation off or unavailable, restart-dependent
display changes, independent displayed FPS with frame generation on, identical replay over a
generated/real pair, and stable counter bounds across changing digit counts and
unavailable readings in both modes.

The framegen fixture checks sRGB/BGRA interpretation, HUD restoration, snapshot retention
through GPU completion, three-set bounded-pool fallback, unpresented-buffer lease
expiry and safe slot reuse, smaller video-out width/height
clamping, refresh/cap gating, warmup/cut/gap resets and moving
geometry. In the synthetic motion case, a rectangle at real-frame center 51.5
pixels interpolated to the expected midpoint 49.5. These are fixture results,
not gameplay or FPS measurements. The existing present and overlay fixtures also
cover paired drawable retirement and identical overlay pixels.

### Gameplay benchmark and pacing regression (2026-10-24)

An M4 Max with 48 GiB memory and macOS 27.0 (26A428), on battery power, ran the
same stationary city-gate scene at 1920x1080 MetalFX Native AA. Each launch used
private copies of the same DLC-dependent save and warmed GPU cache, with neutral
scripted input. All three add-ons, including `SPEXPANSIONDLC03`, were recognized.
This is a base-game scene with DLC loaded, not a DLC-area benchmark. The common
benchmark runtime included the existing DLC-folder discovery provider from
`bloodborne_mac`; that helper is separate from this renderer PR.

The original presentation policy fed measured frame cadence back into minimum
display holds. Two 60-second framegen-on samples at a 120 FPS cap averaged
17.51–21.01 real / 34.83–41.91 displayed FPS. Changing only the hold policy to use
the configured cap recovered 58.11 real / 116.22 displayed FPS, with the same
capture, interpolation and HUD work. Regression tests now ensure that observed
slow frames cannot lengthen future display holds. The display-link loop in the
original port releases its producer independently of those presentation updates.

The corrected 60 FPS-cap comparison used the **same binary**, changing only
`framegen=0|1`. Both modes had at least 30 seconds of gameplay warmup followed by
a 60-second sample, with snapshots outside the timing window. Thermal state was
fair throughout both samples. GPU builds used arm64 RelWithDebInfo, LTO off;
Metal validation was off for timing and the FPS overlay was on.

| Mode | Stream | FPS | Mean ms | P95 ms | P99 ms |
| --- | --- | ---: | ---: | ---: | ---: |
| Off | Real | 60.00 | 16.67 | 16.69 | 16.70 |
| Off | Displayed | 60.00 | 16.67 | 16.67 | 16.67 |
| On, corrected | Real | 59.06 | 16.93 | 16.99 | 25.02 |
| On, corrected | Displayed | 118.12 | 8.47 | 8.33 | 16.67 |

Real throughput was 1.6% lower; displayed throughput was 96.9% higher in this
pair. Real intervals count new game frames at the presenter; displayed intervals
use distinct positive drawable presentation timestamps. These are cadence
intervals, not GPU durations or input latency. Included windows had no logged
occlusion, render errors or render stalls; an interrupted control was excluded.
All original source-save hashes remained unchanged.

These measurements predate the refresh-rate guard: a 120 FPS real cap now
requires at least 240 Hz and is disabled on this 120 Hz panel. The review fixes
have offscreen regression coverage; the gameplay benchmark was not rerun.

At a 120 FPS real cap, valid off controls varied from 63.28 to 77.04 FPS. Battery
and thermal conditions do not establish constant clocks. Showing two images per
real frame on this 120 Hz panel also bounds the paired path near 60 real FPS, so
that uncapped comparison does not isolate interpolation's GPU cost. The observed
60-cap result is specific to this scene and target; traversal, latency, AC-power
performance and long-session behavior remain untested.

API reference: [Apple's frame interpolator documentation](https://developer.apple.com/documentation/metalfx/mtlfxframeinterpolatorbase).
Snapshot/presentation ideas were adapted from the direct-Metal `bloodborne_mac`
port's `gpu/metal/frame_generation.mm` at `b395cf4`; the implementation here uses
this project's scheduler, camera/object motion, and independent present queue.

## Native TAA and missing FSR 4 assets (2026-10-01)

`upscaler=taa` / `BB_UPSCALER=taa` is a separate native-resolution temporal AA
mode, available live in the launcher and overlay. It uses the existing jitter and
camera/object motion vectors, reprojection, depth rejection and 3×3 neighborhood
clipping in one compute pass with two ping-pong RGBA16F histories. History stores
depth in alpha; the scene's original alpha is preserved by the HDR merge. HUD is
composed afterwards and never enters history. Output changes and AA/provider
changes reset history. TAA uses Native AA dimensions while preserving the saved
FSR preset, and does not allocate an FSR context or execute an FSR model/RCAS.
It still adds GPU work compared with disabling temporal AA; native 4K raster cost
also remains. The explicit `BB_RENDER_RES` compatibility path must be unset.

FSR Native AA is not a fast pass-through: it renders at the output resolution and
also executes the temporal reconstruction (including the model for FSR 4) and
optional sharpening. It can therefore be slower than no AA at the same output.

FSR 4.1.1 assets have separate 1080p/2160p tiers and standard/Ultra models:
`t1080_m0`, `t1080_m1`, `t2160_m0`, `t2160_m1`. Outputs above 1080p need the 2160
tier, irrespective of the preset. A missing `t2160_m0/spd.spv` means missing
assets, not a GPU feature failure. These assets are not bundled: build the full
set with `tools/fsr4cap/build_assets.sh` and put it in `BB_FSR411_DIR` or the
packaged data directory's `fsr4_411`. Fatal asset failures now select FSR 3.1 in
the live settings, so the menu shows the active provider, retains the error, and
allows retrying FSR 4 after changing the output/installing assets.

`taa-shader-test` checks real SPIR-V output for accumulation, clipping, depth
disocclusion, invalid history and out-of-frame motion on Vulkan (Lavapipe works).
`out/taa-appimage-build.log` records these checks, settings round trips and 64
Python tests. `out/taa-live-validation.log` and `out/taa-appimage-validation.log`
check TAA at 1080p/720p/1440p/4K, TAA ↔ FSR 3, and a missing 2160-tier FSR 4.1.1
model in one gameplay process, including output captures and camera movement.
`out/taa-launcher-validation.log` checks actual GTK controls and saved settings.
TAA has not yet been tested on the tester's GTX 1060.

## Startup patch is the default again for outputs other than 1080p (2026-10-02)

The live path below made the Steam Deck and a GTX 1060 + 4-core Haswell drop to 7–8 FPS
(release 0.1 AppImage: 40–50 FPS on the Deck). With guest allocations at 1920×1080 the game's
post-processing stays at 1080p even for 720p output, guest compute passes resolve scene proxies
back to 1080p every frame (RX 7800 XT, 720p FSR 3 Performance on 4 cores: 826 vs 576
draws/frame, 185 vs 210 FPS), and on GPUs without shader stencil export every depth/stencil
copy took nine draws.

- `run.sh` again patches the game's render size for outputs other than 1080p (as in 0.1) and
  marks it with `BB_AUTO_RENDER_RES=1`, so an in-game restart recomputes it. Preset and output
  changes then need a restart (the menu offers it). `BB_LIVE_RES=1` selects the live path.
  1080p output and TAA keep the live path.
- Depth/stencil scene proxies need `VK_EXT_shader_stencil_export` again; without it these
  targets stay native (as in 0.1). `BB_SCENE_STENCIL_BITS=1` still forces the portable
  resampler for tests.

## Live resolution changes (2026-10-01)

The in-game output selector now resizes 720p/1080p/1440p/2160p host targets at a
display-pass boundary. Presets use `output / scale`, including raster sizes above
1080p (4K Quality: 2560×1440; Native AA: 3840×2160). Guest allocations stay at
1920×1080; `run.sh` no longer inserts a resolution patch for normal output choices.
The explicit `BB_RENDER_RES` compatibility override is retained.

`SceneTargets` resolves and retires old proxies before changing size. FSR resources
and history are rebuilt for the new input/output, with completion waits for both
FSR 3 and FSR 4. The 1080p path keeps HDR reconstruction before game post;
other outputs reconstruct tonemapped scene proxies at the first Scaleform pass,
then draw HUD/menu geometry into the output-size target. The display buffer is
resized too. Movie shader identification prevents a native-size fullscreen post
pass from being mistaken for UI.

Guest compute passes retain their native dispatch sizes and can resolve proxies
back to 1080p. This change does not scale every post-processing pass; performance
and intermediate detail can differ from the old startup-patched render sizes.

Verification: `out/dev/live-resolution-summary.log` switches output, preset and FSR
off/on in one PID. `BB_PRESENT_DUMP_TRIGGER` captures the completed host display
buffer (including HUD) into `BB_DUMP_DIR`. Captures confirm actual 720p/1440p/2160p
buffers. `scene-resolution-test` covers color/depth/stencil round trips through
proxies both smaller and larger than the guest allocation. `BB_PRESET_FILE` accepts
`preset [output-index [upscaler-index]]` for scripted menu-equivalent changes.

Frame analyzer: `BB_CAPTURE_TRIGGER=<file> BB_CAPTURE_DIR=<dir>`; creating the file records
the next frame (boundary: the pass writing a display buffer) — passes, targets, shaders,
sampled textures, and the first 1 KiB of bound constants for small passes and first draws.

## Bloodborne's frame (1920x1080, Hunter's Dream / Nightmare)

| Passes | What |
|---|---|
| first pass of a frame | copy of the previous UI target into the display buffer (sRGB) |
| shadow | 4096x4096 D32 depth |
| G-buffer | 6 targets 1920x1080 (RGBA8 x3, sRGB albedo, B10G11R11, RGBA16F) + D32S8 depth |
| lighting | light volumes into two B10G11R11 targets |
| scene color | RGBA16F 1920x1080, also forward/transparent draws and effects |
| volumetric fog | compute, reads linear depth (R32F 1920x1080) and composites into scene color |
| half-res effects | RGBA8 960x540 with half-res depth |
| post | combine/bloom pyramid in RGBA16F / B10G11R11 |
| tonemap | into RGBA8 1920x1080 (the UI target) |
| game AA | ping-pong RGBA8 1920x1080 after tonemap (to be skipped when upscaling) |
| UI | stencil-masked draws over the same RGBA8 target |

There is no velocity buffer, also with the camera moving: motion vectors are computed.

## Scene constants (864 bytes, bound by most passes)

Signature: `[0]=3000 (far) [1]=1/3000 [4]=1920 [5]=1080 [6]=1/1920 [7]=1/1080`.

| Floats | Meaning |
|---|---|
| 8–19 | view matrix, 3x4 rows (rotation + translation); the only block that changes with the camera |
| 36–51 | inverse projection (0.700285 = 1/1.42799, 0.39391 = 1/2.53865) |
| 52, 57, 62, 63 | projection (rows 52–67), D3D depth 0..1: x 1.42799, y 2.53865, z 1.00002 / -0.0500679 (near 0.05, far 3000) |
| 112–175 | shadow cascade matrices |
| 180–191 | inverse view (camera to world), 3x4 (176–179: 2, 8, 15, 0) |

The previous frame's matrices are not there; the port keeps them itself.

## Plan

1. Find the scene constants every frame (signature), keep the previous view/projection.
2. Camera motion vectors: compute pass from depth and current/previous matrices into an
   RG16F target; debug view to check them. Object motion later (vertex shader replay with the
   previous frame's constants through the recompiler).
3. Jitter: sub-pixel offset of clip-space positions in the scene passes (recompiler).
4. Upscaler at the scene color stage (before post and UI): FSR 3.1 first (open, native Vulkan,
   also frame generation), then DLSS (native on Linux), XeSS/XeFG and OptiScaler through a
   loader for their Windows DLLs.

## Status

- Camera motion vectors: verified by reprojecting the previous frame (`BB_DEBUG_MOTION=1`,
  toggles 1<<22 reprojected frame, 1<<23 error map): static geometry matches.
- FSR 3.1 (`BB_UPSCALER=fsr3`, FireBurn/FSR-Vulkan submodule): scene color before the
  post-processing combine (compute shader 9a9cf8a9), 1:1, RGB written back (the game keeps data
  in the alpha). Toggle 1<<24 switches it off at run time.
- Jitter: viewport offset of scene geometry (drawn with the scene depth, not full-screen
  quads), Halton(2,3) 8 phases; sign checked by sharpness (correct 255, flipped 208, off 271 —
  1:1 jitter trades high-frequency aliasing for a slightly softer image). Toggle 1<<25 off.
- Frame-to-frame difference while standing still: ~33% lower with FSR.
- Missing: motion of animated objects (characters, cloth, foliage), reactive/transparency
  masks for particles and fog, render-resolution scaling, frame generation.

## 2026-10-01: FSR 4 at 1440p/2160p output did not work (flicker and shimmer)

Video `video_2026-10-01_03-58-59.mp4` (3840x2160 output, FSR 4 Performance): every object
flickered and shimmered. Cause: in this mode the upscaler never ran at all while jitter stayed
on, so the screen showed the stretched scene frame, shifted to a different Halton phase each
frame.

1. The UI pass was identified by the exact `BB_RENDER_RES` target size (1916x1078), but the
   game allocates targets with an aligned height (1916x1080; the scene constants also say
   1916x1080). `RunScaled` was never called (no `UI: native composition` in the log). Alignment
   up to 8 pixels is now accepted (`RenderTarget`), and the scene size for FSR comes from the
   scene constants (`CameraMotion::RenderSize`, `SceneSize`).
2. After that FSR 4 failed with `external image registration failed (-1000069000)`:
   `RunScaled` created new image views every frame, and the FSR 4 registry holds eight. It now
   uses the same `CachedView` as the Native AA path.

Checked with a dump (`BB_DUMP_TRIGGER=<file> BB_DUMP_DIR=<dir>`, `BB_DUMP_FRAMES`, default 8:
FSR input, motion vectors and output, raw): with a still camera the PSNR between consecutive
output frames is ~45 dB versus ~28 dB for the jittered input; no trails when turning the camera
or walking. FPS in this mode is ~107 instead of ~220 — previously FSR 4 simply did not run.

Remaining: sprites (4 indices each) drawn into scene color with scene depth — glows, small
lights — are still not jittered (the rule "≤ 6 indices = full-screen pass").

## 2026-10-01: faster FSR 4 — the post pass (2.9 → 0.8 ms at 4K)

Measurements: `BB_FSR4_PROFILE=1` (GPU time of each FSR 4 pass; a provider patch in the
submodule, see `gpu/patches/fsr-vulkan`), the out-of-game benchmark `out/gpu/fsr4-bench`
(`ninja -C out/gpu fsr4-bench`; `--stats` prints registers and instructions from RADV). 4K
Balanced (2260x1272 → 3840x2160), RX 7800 XT: all of FSR 4 takes 5.8 ms, of which **post is
2.5–2.9 ms** (not the network: the last layers, 2x2 pixel shuffle and history blending), the
12 model passes 2.4 ms, pre 0.55.

Cause: each thread computes a 2x2 block of output pixels and writes them one at a time into
three images (recurrent state, history, output), so each wave store instruction writes every
other pixel. Removing any one of the three stores made the pass 1.3–4x faster with otherwise
identical code.

Fix: `tools/fsr4_optimize.sh` decompiles post (spirv-cross), `tools/fsr4_post_lds.pl` gathers
the workgroup's 16x16 block in shared memory and writes contiguous rows, glslang compiles it
back into `fsr4_shaders/opt/`; `vk_fsr4.cpp` loads it from there (`BB_FSR4_OPT=0` uses the
original). Two subtleties found by comparing outputs:
- spirv-cross translates signed int8 unpacking (`OpBitcast` to `i8vec4`) as `unpack8(uint)`
  (unsigned) — without a fix the result is completely different (PSNR 17 dB);
- values in shared memory must stay float: the compiler turns half values from shared memory
  into a 16-bit store, which rounds to unorm8 differently.

`tools/fsr4_verify.sh` compares the original and optimized post for all presets at
1080p/1440p/4K output on pseudo-random inputs: 1440p and 4K are bit-identical; post at 4K
2.1–3.5 → 0.81–0.87 ms, 1440p 1.2–1.5 → 0.36–0.38 ms, 1080p 0.37–0.69 → 0.20–0.22 ms. In game
(4K Balanced): FSR 4 5.8 → 4.0 ms, GPU frame 13.1 → ~11.6 ms; beyond that FPS is CPU-bound (the
GPU command thread waits on guest memory copies, "host copies" ~20%).

Found along the way: **the original FSR 4 at the 1080 tier (1920x1080 output) is
non-deterministic** — from run to run on identical inputs a band at the left edge (columns
0–132) changes, i.e. there is a race or a read of uninitialized memory somewhere in the
model/provider. At 1080p output this can cause flicker along the left edge. Not investigated.

The reactive mask is no longer computed with FSR 4 (FSR 4 does not accept it).

### Also checked (same setup, 4K Balanced)

- **A/B in one process** (`ab.sh`, bit 24 — FSR off, UI copy without upscaling): 86.5 FPS with
  FSR 4 versus 87.7 without. After optimizing post the frame is bound by the CPU (the GPU
  command thread waiting on guest memory copies), not the GPU. Async compute for FSR
  (overlapping it with the start of the next frame) would gain at most that ~1% — postponed
  until the CPU side is faster; it would also require reordering the UI and present commands
  of frame N after the work of frame N+1.
- **WMMA (`VK_KHR_cooperative_matrix`, supported by RADV on RDNA3)**: a prototype of model
  pass 1 (16-channel residual block: 3x3 16→16, 1x1 16→32 ReLU, 1x1 32→16) — 0.85 ms in the
  first version, 0.49 ms with a word layout in shared memory, versus 0.30 ms for the original
  dot4. The arithmetic on WMMA would take ~0.1 ms, but at 16 channels the tile layout,
  epilogues through shared memory and occupancy (6 waves/SIMD, LDS-limited) eat the gain. The
  ceiling is about 1 ms for all 12 passes with careful manual rework of each; not started.
- `RADV_PERFTEST=cswave32` (wave32 for compute): FSR 4 slower, 4.2 → 5.4 ms.
- The pre pass (0.54 ms) is not store-bound (0.50 ms without stores).

## 2026-10-01: FSR 4 flicker at the left edge at 1080p output — a race in model pass 11

The 1080-tier non-determinism (see above) is a bug in the v07 model shader, pass 11 (decoder,
1/4 → 1/2 resolution). Each thread handles one 1/4-resolution input pixel and writes a 2x2
block of the 1/2-resolution output. The dispatch rounds the width up to 64 threads, and threads
beyond the input width do not exit: at the 1080 tier (input 480 wide) threads 480..511 write
output pixels 960..1023, i.e. the first pixels of the next row (a row of any tensor is 15360
bytes), racing with their real writers. At 4K output the width 960 is a multiple of 64, so
there is no bug.

`tools/fsr4_pass11_guard.pl` adds an early exit for out-of-bounds threads (the size comes from
the pass's own neighbor bounds check); `tools/fsr4_optimize.sh` builds it into
`fsr4_shaders/opt` for all presets together with post; `vk_fsr4.cpp` and the benchmark load any
pass from `opt/`. `tools/fsr4_verify.sh`: 1440p/4K are bit-identical to the original (the guard
never triggers there); 1080p gives the same result from run to run, differing from the original
after the first frame only in the former left-edge band.

Found in the process: spirv-cross translates all signed int8 unpacks (`OpBitcast` to
`v4char`) as unsigned `unpack8(uint)`; pass 11 has nine different forms of them. The fix is
shared by the scripts — `tools/Fsr4SpirvCrossFixes.pm` (all `unpack8` go through a signed
helper).

## 2026-10-01: FSR 4.1.1 — what is inside and how to run it on Vulkan (investigation)

Source: `amd_fidelityfx_upscaler_dx12.dll` 4.1.1.2740 from OptiScaler (`FSR4_LATEST`).
- The DLL contains the provider `ffxProvider_FSR4_Int8` / `Fsr4Int8UpscalerModel` (version
  "FSR4-i8 4.1.1"). 1028 DXIL containers (25.7 MB): model passes
  `fsr4_model_v07_fp8_no_scale_pass1..12` (+ `_post`), 270 `prepass` variants, 144 `postpass`,
  `fsr_rcas_pass` and the FSR 3.1/2 fallback. No compressed data.
- Despite "fp8" in the name, the model passes are plain DXIL with `dot4AddPacked` (int8 dot4),
  without AMD extensions (WMMA). The architecture is the same v07 (12 passes), but weights and
  biases are not baked into the shader; they are read from `InitializerBuffer` (t18, typed
  buffer), tensor sizes from cbuffer `CsTensorSizes`, scratch is `u11`. Each pass has 9
  variants: the grid is baked in (tiers 1080 / 4K / 8K: 480x270, 960x540, 1920x1080 for pass 5)
  and, apparently, presets.
- `amdxcffx64.dll` (loader 2.3.0.2913, loaded by Proton) contains the same FP8 FSR 4 passes and
  ML frame generation (`mlfi_*`), but no separate INT8 model. `FSR4_INT8` 4.0.2 is `v07_i8`,
  i.e. what the port already uses.
- dxil-spirv (HansKristian-Work/dxil-spirv, built from source) translates the passes into
  valid SPIR-V: `dxil-spirv <pass> --enable-shader-i8-dot --ssbo-uav --use-reflection-names`.

Missing for our own 4.1.1 provider: the mapping of containers to (preset, tier, pass), the
contents of `InitializerBuffer`, the `CsTensorSizes` values, the prepass/postpass permutations,
constants and dispatch sizes. The most reliable path is not to reverse the provider's x86 code
but to record one frame: a small test program on the FFX API (DX12, MinGW) under Wine/Proton
with vkd3d-proton, explicitly selecting the "FSR4-i8" provider through a version override, plus
a Vulkan layer that records pipelines (SPIR-V from dxil-spirv), buffer contents and dispatch
sizes. Then replay it with our own provider through the same benchmark (`fsr4-bench`),
checking against the recorded output.

## 2026-10-01: FSR 4.1.1 in the port (upscaler=fsr411)

AMD's 4.1.1 model (INT8) runs natively on Vulkan and matches the DLL bit for bit.

**How it was obtained.** `tools/fsr4cap/fsr4cap.exe` (C, MinGW) calls FidelityFX API 2.3 from
`amd_fidelityfx_loader_dx12.dll` with an explicit version 4.1.1 and, by hooking D3D12 method
tables, records everything the DLL does under vkd3d-proton: pipelines (DXIL), root signatures,
descriptors, uploads, constants, dispatches. A frame is 29 dispatches: SPD auto-exposure,
prepass, pass0_post, model passes 1..12 (each followed by `_post`, clearing the tensor border),
postpass, RCAS.
- There are two models: m0 (Native..Performance) and m1 (Ultra Performance, ratio 3.0); weights
  are 128 KB (`InitializerBuffer`). Tiers: t1080 (output up to 1920x1080), t2160 (larger,
  including 2560x1080).
- Tensors are derived from the output aligned to 8; the `CsTensorSizes` table (17 sizes), group
  counts and constants (MLSR as in v07, but width/height not aligned and
  `inv_scale = 1 / scale`; SPD; RCAS) are checked by `extract.py` against 20 recordings.
- dxil-spirv translates DXIL the same way vkd3d-proton does: `--mixed-float-dot-product`
  (`VK_VALVE_shader_mixed_float_dot_product`, half dot2 accumulating into float) — without it
  PSNR is 40 dB instead of an exact match. `--class-bindings` (a patch) lays out SRV/UAV/CBV/
  sampler registers at bindings +0/+32/+64/+96; the runtime binds resources by name.
- `fsr411/fsr411.cpp` — an independent Vulkan core (in game and `fsr4-bench --fsr411`).
- `verify.sh`: DLL and replay on identical inputs, 8 frames — bit-identical for 1080p (both
  models), 1440p, 2560x1080, 4K (both models). 1600x900 differs only at the right edge, where
  the DLL itself is non-deterministic (a race in its shaders when the tensor width is not a
  multiple of 64).

**Speed-up.** postpass was rewritten to use workgroup memory at the SPIR-V assembly level
(`postpass_lds.py`; going through GLSL is inexact: `DenormPreserve` and the exact
`RelaxedPrecision` placement are lost, and something else changes the result too): 2.18 →
0.95 ms at 4K, bit-identical to the DLL. All of FSR 4.1.1: 1.09 ms at 1080p (1280x720), 4.2 ms
at 4K Balanced. In game (4K Balanced) the upscaler takes 4.12 ms versus 4.32 ms for the
accelerated v07.

**Building the assets** (by the user, from their own DLLs; the repository contains nothing from
AMD):

    bash tools/fsr4cap/build_assets.sh <amd_fidelityfx_upscaler_dx12.dll 4.1.x> <amd_fidelityfx_loader_dx12.dll 2.3.x>

Requires nix-shell (or MinGW, cmake, ninja, python3, SPIRV-Tools, umu-run), network access for
dxil-spirv and the FidelityFX SDK headers, and Proton (GE-Proton). The DLLs ship, for example,
with OptiScaler (`FSR4_LATEST/`) and with games using FSR 3.1/4. A build from scratch produces
the same files; `VERIFY=1` compares against the DLL at the end. (This is the Linux reference
path; it has not been tried on macOS, where FSR 4 is not expected to work.)

**Correction to the A/B above ("the frame is CPU-bound").** Those measurements ran while 12
hung shadPS4 processes each occupied a core. On an idle system: "host copies" 1.6%, GPU wait
~41% — the frame is GPU-bound, so async compute for FSR makes sense again.
