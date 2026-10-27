# Scene-based HDR on Metal

Enable **HDR (experimental)** in Display and restart. It defaults to off. The HDR image
is generated from the game's exposed floating-point lighting and unclipped bloom, before the
original SDR shoulder and 8-bit targets. The former SDR highlight-gain/inverse-tone-map path
has been removed. The initial native path requires **1920×1080, Native AA, and MetalFX,
FSR 3.1 or TAA**. Other resolutions, scaled presets, disabled temporal AA, native ultrawide,
explicit `BB_RENDER_RES` and external shader replacements retain the complete SDR output.
Those paths require further HDR/upscaler integration; silently bypassing their selected
upscaler would be incorrect.

## Calibration

- `hdr=1`: request native HDR output on the next launch.
- `hdr_paper_white=1.0`: scene reference brightness relative to the desktop's SDR white;
  live, bounded to 0.5–2.0. Game and host UI keep desktop-white brightness.
- `hdr_peak=0`: automatically use current display headroom. A positive value caps the peak
  to 1–16 times desktop white and never exceeds current display headroom.

These are relative EDR units, not an unverified absolute-nit calibration. Display shows the
current and potential headroom separately, plus whether a native scene or the SDR fallback
is being presented. A potential value of 16 does not mean 16 is currently available. The first
interactive test had only 1.20 times available; a different tone mapper cannot exceed that
physical limit. Moving the window or changing screen brightness updates the available range.
The old `hdr_highlights` key is obsolete and is removed when settings are saved.

## Rendering

The game keeps rendering its complete original SDR image. For the recognised 1.09 tone-map
pixel shaders (`5c02eaf8`, `5e67d866`), an IR adapter also exports exposed scene lighting and
bloom into separate RGBA16Float attachments. It requires the complete expected, straight-line
dataflow: three exponential shoulder inputs, three unclipped bloom expressions and the
original four-channel export. A changed or unknown shader is left intact and falls back to SDR.
The additional exports are properties of the selected shader module; SDR permutations and
shared persistent shader metadata retain the original exports. HDR and SDR specializations
have different attachment formats, including when loaded from a warm cache.

Lighting comes from the tone-map inputs after the game's scene effects, including depth of
field and motion blur. The adapter exports the original exposure/modulation calculations
before exponential compression, bloom before its unit-range clamp, and the original grain
and gamma-offset term. The tone pass's colour matrix and gamma constants and the following
3D grading LUT are retained. No game shader binary or copied decompilation is distributed.

A new hue-preserving exponential shoulder maps the floating-point lighting and bloom to the
available HDR range. It preserves the dark-region curve below its knee and smoothly opens
the shoulder above it. Bloom above the old clamp remains distinct. The bounded SDR grading
LUT is applied to normalised colour above white, then the HDR excursion is restored. This is
an HDR adaptation of the game's grade, not an original HDR grade supplied by FromSoftware.
It does not derive scene luminance by multiplying the finished SDR pixels.

Game UI draws export to the original SDR target and floating-point black/white backdrops
in one render pass, preserving the original RGB blend state and separately accumulating
coverage alpha. A runtime-only SPIR-V variant duplicates the colour export; the original
module and persistent shader metadata stay unchanged. The two backdrops encode actual
per-channel transmission and additive colour, including the game's multiply/additive HUD
layers. Ordinary alpha-over layers composite in linear light using coverage. Other supported
affine RGB blends retain the game's working-space blend equation. These are coefficients from
the actual UI draw calls, not differences between scene screenshots or a guessed HUD mask.

The final additive SDR glare pass also writes the auxiliary outputs ahead of the UI, preserving its game-authored
brightness. Native temporal AA runs before the tone-map input; the old SDR-only game AA pass
is not used by the HDR companion image.

All three outputs share the original stencil/depth attachment and rasterisation. This avoids
replaying each UI draw twice, copying stencil, or switching render targets for every HUD layer.
Unsupported non-affine/dual-source blend modes and offscreen UI target changes fall back to the
original SDR frame. New UI variants compile asynchronously; a pending or failed variant keeps
the original draw and SDR fallback instead of stalling or dropping UI. Host settings/FPS overlays remain separate at SDR brightness.

At each display-copy boundary the completed lighting, grading and UI are resolved before their
scratch textures are reused. Three output slots are associated with actual guest display-buffer
addresses and consumed once per flip. GPU-completion ticks protect slot reuse. All auxiliary
textures and readbacks survive unretained command buffers through GPU completion. Native HDR
adds extra attachments, tone mapping and memory traffic; it is not a zero-cost output mode.

Presentation remains RGBA16Float in extended linear sRGB with
`CAMetalLayer.wantsExtendedDynamicRangeContent`. Its NSWindow's actual screen is sampled on the
window thread every 250 ms. The game's PS4 HDR capability remains disabled: this is a native
host rendering conversion. The off setting keeps the original SDR rendering path.

## Validation and diagnostic captures

`mtl-hdr-test` runs the production tone mapper and reads GPU output. It covers different
lighting values that quantise to the same SDR white, unclipped bloom, paper white and peak
limits, finite-value handling, dark midtones, opaque/translucent game UI, SDR fallback, host
UI, letterboxing, reduced headroom and snapshot lifetime. `native-hdr-shader-test` exercises
both IR adapters through SPIR-V and the Metal compiler, verifies original SDR exports are
unchanged, and rejects missing dataflow, other shader hashes and conflicting exports. It also
compiles the one-pass UI export variant, verifies the original module is unchanged, and rejects
conflicting MRT outputs.
Launcher model and settings tests cover defaults, persistence and calibration bounds.

Diagnostic PNGs are clamped SDR previews and cannot prove physical HDR appearance. For raw
readbacks set `BB_HDR_DUMP_TRIGGER` to a trigger-file path and `BB_HDR_DUMP_DIR` to a private
output directory, then create the trigger while gameplay is active. The next admitted completed
frame writes lighting, bloom and HDR output as RGBA16Float files with dimensions in their names.
These captures can contain game imagery; keep them private.

Physical display calibration, wider-area/post-effect coverage, external-screen transitions,
all game UI blend modes and HDR/frame-generation interaction require further validation.
Passing synthetic readbacks alone does not establish gameplay image quality or performance.

Local validation on 2026-10-27 built both GPU architectures and the runtime/host/launcher.
Production HDR, UI MRT readback, overlay and shader-admission fixtures passed under Metal
validation; launcher model and settings persistence checks passed. Bounded base-game runs
loaded private copied saves, checked native frame admission, inspected the HUD/menu previews,
and kept the source seed hashes unchanged. Raw lighting exceeded 1.0 and the resolved output
exceeded SDR white while remaining below the current 1.20 display cap (latest capture: 1.077). PNGs do not establish physical HDR appearance.

The initial UI replay prototype was discarded after profiling its encoder overhead. The final
path adds UI MRT exports to the original draw. In one warm, stationary 1080p MetalFX Native AA
comparison, both HDR-on and HDR-off windows stayed near the 60 FPS cap. Reported GPU busy time
was approximately 9–11 ms/frame with HDR and 7.4–8.0 ms/frame without it. These are limited,
sequential samples with the profiler and Metal validation disabled, not an uncapped FPS or
thermal-controlled benchmark. HDR still costs GPU time; wider scenes and resolutions need work.

## Sources

The EDR presenter began with the highlight-recovery adaptation from
[lKaniel/bloodborne-server at 8d5e54f](https://github.com/lKaniel/bloodborne-server/tree/8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1),
under GPL-2.0-or-later (`hdr_exposure.comp`, `hdr_gain.comp`, `hdr_highlight.glsl`). The gain and
estimated-exposure algorithms have now been replaced by the scene-radiance export and HDR
tone mapper. Standard sRGB transfer functions and the existing Metal presentation work remain.
The native adapter, HDR curve, grading adaptation, UI composition and frame association are local work.

Apple's [EDR tone-mapping guidance](https://developer.apple.com/documentation/metal/performing-your-own-tone-mapping)
describes the float output, colour space and dynamic display headroom. No networking or
path-tracing experiment code is included.
