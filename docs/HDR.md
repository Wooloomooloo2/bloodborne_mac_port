# HDR presentation on Metal

Enable **HDR presentation (experimental)** in the launcher's Display section, or **HDR** in
the in-game Display page, then restart. It defaults to off. `hdr=1` and
`hdr_highlights=0.5` in `bbport.ini` select the same controls; highlight strength (0–1) is live.
Setting strength to zero retains SDR luminance on the EDR output.

The output uses `RGBA16Float`, extended linear sRGB and
`CAMetalLayer.wantsExtendedDynamicRangeContent`. The window thread samples its actual screen's
current and potential EDR headroom every 250 ms. GPU conversion and presentation clamp to the
available range, including when brightness changes or the window moves to an SDR display.
An SDR display keeps normal SDR luminance. The game's PS4 HDR mode remains disabled.

At the game's post-processing combine, the draw queue copies the floating-point scene. Before
the first recognized UI draw it copies the UI-free, tone-mapped frame in linear colour. One
64×36 exposure sample compares the pair's midtones. The final conversion expands scene
highlights, suppressing the gain where the finished frame differs from its UI-free copy.
Missing captures, menus and loading frames retain SDR luminance. Capture validity is consumed
on each flip; exposure is computed afresh so a camera cut cannot reuse an old exposure.

The game still renders and post-processes its original SDR image. This recovers highlights and
uses an inverse-tone-map expansion; it is not an authored HDR grading of Bloodborne. Fine or
translucent game HUD details can evade the image-difference mask. Menus, FPS and other host UI
render into a separate SDR layer and composite at SDR brightness in linear light. Diagnostic
PNG snapshots show a clamped SDR preview; they cannot demonstrate a display's HDR brightness.

Two frame-pool slots hold independent HDR frames and overlay targets. Scene-copy scratch images
are reused in draw-queue order, after conversion; retained resources survive command completion.
The off setting avoids HDR shaders, scene copies and extra targets. HDR adds capture, conversion,
presentation and memory costs; gameplay performance must be measured on the target Mac.

## Sources

Adapted from [lKaniel/bloodborne-server at 8d5e54f](https://github.com/lKaniel/bloodborne-server/tree/8d5e54fee50e1d7c9dd25ba0a32f224b1d76b6f1):

- `bbport/gpu/shadps4/video_core/host_shaders/hdr_exposure.comp`: matching scene/SDR midtone exposure.
- `bbport/gpu/shadps4/video_core/host_shaders/hdr_gain.comp`: UI suppression and scene-highlight gain.
- `bbport/gpu/shadps4/video_core/host_shaders/hdr_highlight.glsl`: sRGB transfer and highlight knee.

These GPL-2.0-or-later sources are adapted in `mtl_hdr.mm`. Metal kernels, resource retention,
per-frame exposure, EDR display adaptation and separate UI composition are local work. No game
assets or the source project's Vulkan presentation/experimental tracing code are imported.

Apple's [EDR tone-mapping guidance](https://developer.apple.com/documentation/metal/performing-your-own-tone-mapping)
describes the output format, linear colour space and dynamic display headroom.

## Validation

`mtl-hdr-test` runs the production shaders and reads GPU output: recovered highlights above 1,
game HUD suppression, SDR fallback, absent captures, zero strength, non-finite inputs, headroom
reduction, separate SDR UI, letterboxing, blank frames, EDR layer configuration and diagnostic
snapshot resource lifetime. It is included in `build_macos.sh --test`, normally and with
`MTL_DEBUG_LAYER=1`. Launcher model tests cover settings defaults and persistence.

Fixture success does not prove HDR appearance on a physical screen, full campaign coverage,
all HUD masking cases, external-display transitions or interaction with separate experimental
frame-generation branches.

On 2026-10-26 the arm64 and x86-64 GPU libraries, native launcher and split runtime/host built.
The HDR GPU fixture passed normally and under Metal validation; the existing SDR presentation
fixture passed under validation, and launcher/settings persistence tests passed. A bounded
120-second base-game smoke loaded a copied save into gameplay with Metal validation, logged
matching scene/UI-free captures and wrote inspected SDR-preview screenshots. The seed save's
hashes were unchanged. This is functional coverage, not an HDR display calibration or an A/B
performance result. Earlier menu-only attempts used a DLC save incompatible with upstream's
base-game content profile; those attempts do not count as gameplay validation.
