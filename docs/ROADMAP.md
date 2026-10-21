# Roadmap: Bloodborne on macOS

State as of 2026-10-19 (mac-v0.5.2). Plans change with testers' reports: the detailed, prioritised
list of known issues and refinements is [MACOS_BACKLOG.md](MACOS_BACKLOG.md), and every
measurement behind the numbers here is in [FINDINGS.md](FINDINGS.md). The upstream Linux port's
roadmap, from before the Mac port, is kept in [ROADMAP_UPSTREAM.md](ROADMAP_UPSTREAM.md).

## Goals

1. **Even frame pacing** at a locked 60 FPS, with 90 and 120 FPS caps for ProMotion displays.
   Steady frames matter more than peak FPS, and 60 is also the sweet spot for battery life.
2. **The best resolution each Mac allows:** 1440p native or upscaled on Pro/Max chips (4K with
   MetalFX on an M4 Max), 1080p native on a base M5, upscaled 1080p on older and 16 GB Macs.
3. **Native Mac technology**, not translation layers: a native Metal renderer, MetalFX, and an
   arm64 GPU process next to the game, which runs under Rosetta 2.
4. **Easy to use:** a downloadable app with a native launcher; no building, no Terminal.

## Where we are (0.5.2)

- **Playable on Apple Silicon** with your own game dump (CUSA03173 with update
  1.09). The game's x86-64 code runs under Rosetta 2. Rendering runs in a native arm64 GPU process
  with its own Metal renderer.
- **Performance on an M4 Max, uncapped:** 1080p native 83–105 FPS; 1440p native 65–83; 1440p
  MetalFX Quality 81–102; FSR 3.1 Quality at 1080p ~98–127. With the 60 FPS cap every frame
  takes 16.7 ms. An M3 Pro reports 50–55 FPS at 720p with FSR 3.1.
- **Upscalers:** MetalFX, FSR 3.1 (AMD's code on a Metal back end), TAA, FSR 1.
- **Launcher:** output resolution, upscaler, effects, frame cap, game and save folders, and (from
  0.5.3) an Advanced tab with the in-game menu's options. **In-game menu:** ⌘, or L3+R3, usable
  with a controller.
- **Memory:** the GPU's memory block is sized to the game (6 GB) since 0.5.2, so 16 GB Macs stay
  under macOS's GPU memory limit. Waiting on results from real 16 GB Macs (#19).

## Released

| Version | Date | Headline |
|---|---|---|
| 0.3.0 | 2026-10-14 | Stability round: load race fixed, native arm64 GPU process, MetalFX |
| 0.3.1–0.3.2 | 2026-10-15 | Downloadable app; launch fix |
| 0.3.3 | 2026-10-15 | 13–16% faster (no Metal argument buffers in MoltenVK) |
| 0.4.0 | 2026-10-16 | Native Mac launcher |
| 0.5.0 | 2026-10-18 | **Native Metal renderer**, replacing MoltenVK: 12–18% faster at 1440p |
| 0.5.1 | 2026-10-18 | Fixes: macOS 27 GPU process, name entry in full screen, clearer errors for incomplete game folders |
| 0.5.2 | 2026-10-19 | 16 GB Macs (less GPU memory), controller menu, the launcher can be turned back on in the game |

## Next: 0.5.3

**Already on master**
- **Black loading screen fixed** (#18): the render thread's draw-list job now takes turns with the
  main thread's loading work (from PR #20, MojahedAbushawish).
- **Launcher Advanced tab** with the in-game menu's options (#15). The unused Renderer picker is
  gone.
- **Cleanup:** MoltenVK, the Vulkan loader, FSR 4 and the single-process mode are gone from the
  app and the build: a 4.5 MB smaller download (milestone M7 of the Metal renderer plan).

**From contributors, once reviewed and tested**
- **Mouse look** (PR #20), then **rebindable keyboard and mouse controls** with a Controls tab
  (PR #21, rebased on #20's mouse).
- **From PR #20, after testing on an M4 Max:**
  - Copy-shader copies deferred to the end of the render pass: +12.8% on a GPU-bound M3 Pro.
  - Renderer threads sleep instead of spinning: about 24% less renderer CPU.
  - A per-frame texture hash skipped.
  - ⌘W no longer quits while W is held.
- **Invariant positions** (PR #20, +12% on an M3 Pro), only as an opt-in: the same change broke
  geometry here on 2026-10-13.

**Planned**
- **Hide the mouse cursor** during play (#18; PR #20's capture on click covers most of it).
- **Fewer render passes:** first re-measure encoders and passes per frame on the Metal renderer,
  then merge the avoidable splits. Expected 10–20% where the GPU is the limit, mostly at 1440p
  and 4K. PR #20's deferred copies are the first step.
- **Faster pipeline-archive saves:** write only the new pipelines, and less often.

**Open bugs, fixed if solved in time**
- Floors and objects that disappear after playing for a while, back after a reload (#18).
- The HUD's quick-item icon missing above 1080p, and at 720p with FSR (#14, #17).
- A tessellation pipeline Metal refuses, so its draws are skipped.
- Occasional exit-code-139 crashes, about one every hour or two on an M5 Pro (#12).

## 0.6: online play

Co-op and online play through The Hunter's Dream server pool, with Windows players (PR #9,
noelmiller). Online release builds are GPL-3, because of the server code they include. Waiting on:
- the server operators' agreement to how the Mac client identifies itself;
- a rebase onto master.

## Later

Roughly in priority order; none of it is scheduled yet.

- **Zero-copy memory** (milestone M7 of the GPU process design): the GPU reads the game's memory
  directly instead of copying buffers into its own block. Less memory, fewer copies, and less
  write tracking. This is the next big step for 16 GB Macs if 0.5.2 isn't enough. A smaller
  alternative is a sparse block built on Metal placement heaps.
- **Frame pacing:** remove the remaining traversal hitches and the occasional long frame when
  uncapped. Make 90 and 120 FPS hold everywhere they can.
- **Performance ideas from other forks** (DiAvisoo/bbport-mac-optimized): skip draws that produce
  nothing, and cache memory-region lookups per thread.
- **Signed and notarised app,** so players no longer have to release it from quarantine.
- **Launcher extras:** ultrawide 21:9 and 32:9 with the UI-mod installer, quicksave and quickload,
  cheats (#15), easier mods (#16). Room for other games later.
- **Static recompilation** (research, branch `static-recompilation`): the game's 47,000 functions
  lifted to C and run natively on arm64, without Rosetta. A working prototype matches the x86 code
  bit for bit; it isn't on the release path.
- **The Metal renderer for shadPS4:** offering the renderer upstream as a Metal back end. Parked
  until the 0.5.3 cleanup is done.

## How decisions are made

- **Measure first.** Each performance change is A/B tested on the same scene with frame pacing
  reported before FPS (FINDINGS.md), and checked with screenshots.
- **Priorities:** stability first, then performance, then Mac GUI (the order of MACOS_BACKLOG.md).
- **No game files in the repository:** players bring their own dump.
