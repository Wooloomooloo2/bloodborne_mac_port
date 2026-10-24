# Design: MetalFX frame generation

Status (2026-10-24): **plan, nothing implemented.** Research only; owner wants it soon.

## Requirements: macOS 26, not 27

- `MTLFXFrameInterpolator`, its descriptor and base class are `API_AVAILABLE(macos(26.0))` in the
  installed MacOSX26.5 SDK (`MetalFX.framework/Headers/MTLFXFrameInterpolator.h` lines 34, 130,
  310; the Metal 4 variant `MTL4FXFrameInterpolator.h:23` too). macOS 27 only adds optional
  `contentWidth/contentHeight` (dynamic resolution). macOS 27.0.1 is not needed.
- Hardware: no chip list from Apple. Check `+[MTLFXFrameInterpolatorDescriptor supportsDevice:]`
  at runtime and hide the option where it says no.
- Deployment target stays 15.0: calls go behind `@available(macOS 26.0, *)`, in the arm64
  `bbgpu-host` only (the renderer's process).
- Unrelated: `bbgpu-host-macos27` is the same host with the `__GUESTRSV` segment that keeps
  libmalloc out of the guest range on macOS 27 (`host/guest_reservation.s`, `src/gpu_process.c`).

## Inputs and where they come from

| MetalFX wants | We have |
|---|---|
| `colorTexture`, `prevColorTexture` (HUD-free) | `TemporalUpscaler::RunScaled()` resolves the tonemapped frame into the UI image at the first UI draw (`vk_temporal_upscaler.cpp` ~1048): snapshot it there; keep last frame's |
| `depthTexture`, `depthReversed` | the scene depth the upscalers already take (`BbMetalFx::Encode` uses `depthReversed=NO`) |
| `motionTexture`, `motionVectorScaleX/Y` (pixels, current → previous) | `CameraMotion` writes RG16Float pixels, previous minus current, camera + object motion |
| `nearPlane`, `farPlane`, `fieldOfView`, `aspectRatio` | the camera pass's `proj`/`prev_proj` (near `-P[3]/P[2]`, FOV from `P[1]`) |
| `jitterOffsetX/Y` | `TemporalUpscaler::Jitter()` |
| `uiTexture` / `uiTextureComposited` | the finished display buffer has the HUD in it |
| `deltaTime`, `shouldResetHistory` | flip timing; reset on cuts, loads, menus |

Optional: the descriptor's `scaler` pairs it with the MetalFX temporal scaler we already create.

Gap: native output without an upscaler goes through `Run()`, which has no pre-UI hook. First
version: frame generation only with an upscaler on (MetalFX/FSR/TAA at any scale), or add the
first-UI-draw hook to `Run()`.

## Presentation and pacing (the hard part)

- Today one `nextDrawable` per guest flip on `bb:Present`, paced by
  `presentDrawable:afterMinimumDuration:` (`vk_presenter.cpp`, `mtl_present.mm`).
- With frame generation: per game frame, encode the interpolation, then present the generated
  frame and, half a frame later, the real one (held back one frame: about +1 game frame latency).
  Drive it from a display link (`CADisplayLink`/`CAMetalDisplayLink`) rather than minimum
  durations, so both land on vsync.
- **Game timing must not change.** Generated frames never touch guest flips, vblank or the
  frame limit; with `game_timing=fixed` the 60 FPS++ step stays 1/60 s. So the settings split:
  game cap (30/60) and display rate (×2: 60/120). Sensible pairs: 60 → 120 on ProMotion, 30 → 60
  on 60 Hz displays.
- A late game frame: present the real frame alone (no interpolation across a gap), reset after
  gaps outside about 1/240–0.1 s.

## HUD

The game draws its HUD into the frame, and interpolating it smears text. Two options, A/B both:
1. `uiTexture` = finished frame, `uiTextureComposited=YES` (MetalFX removes and re-adds the UI).
2. Interpolate the HUD-free snapshot, then a small compute pass copies current-frame pixels back
   wherever the finished frame differs from the HUD-free one, plus a 2 px border
   (evill33t/bloodborne_mac's `preserve_ui`, GPL-2.0, reusable with attribution).

Menus, loading screens (`RunUiOnly`) and cutscene cuts: real frames only, reset history.

## Milestones

- **M0 probe:** a unit test in the arm64 host that creates an interpolator on this Mac
  (`supportsDevice`), then a synthetic moving edge through it; check the motion sign.
- **M1 inputs:** HUD-free colour, depth, motion snapshots and the previous colour;
  `BbMetalFx::CreateInterpolator`/`EncodeInterpolate` in `gpu/shim/bbport_metalfx.{h,mm}`.
- **M2 HUD:** both HUD options, snapshot triplets (previous / generated / current) compared.
- **M3 pacing:** display-link presentation of two drawables per game frame; `frame_generation`
  in `bbport.ini`, the launcher and the in-game menu, greyed out when unsupported.
- **M4 stats:** overlay shows rendered / displayed FPS; `BB_FRAME_STATS` fields; FINDINGS entry.

## Risks and tests

- Latency (+1 frame), ghosting on foliage and weapons, HUD edges, uneven spacing when frames are
  late, GPU cost on base M-series chips (could push a 60 cap under: measure on an M1/M2).
- Tests: motion sign and scale, reset rules, HUD pixels unchanged byte for byte, frame intervals
  at 60 → 120 Hz windowed and full screen (owner present), the lift spot with fixed steps, Metal
  HUD GPU time on and off, macOS 26.6 and 27 host variants. Watchdogs and test saves on every run.

Prior art: evill33t/bloodborne_mac `docs/MACOS_METALFX_FRAME_GENERATION_2026-10-23.md`,
`gpu/metal/frame_generation.{h,mm}` (32 → 29 rendered, 59 displayed FPS at 1080p, macOS 27).
Apple: WWDC25 session 211; developer.apple.com/documentation/metalfx/mtlfxframeinterpolatordescriptor.
