# bloodborne_mac_port: Bloodborne on macOS (Apple Silicon)

A **work-in-progress macOS port** of the native Bloodborne PC port. It runs the original PS4
executable of *Bloodborne* (CUSA03173, version 1.09) on Apple Silicon Macs: the game's own
x86-64 code executes directly under **Rosetta 2**, PS4 system libraries are replaced by a
lightweight HLE runtime written in C, and GPU command buffers are translated by a
**shadPS4-derived video core** into **native Metal** (since 0.5.0; MoltenVK before).

This is not an emulator and contains no game code: the loader and runtime run your own copy of
the game as an x86-64 process under Rosetta 2, and rendering happens in a separate native arm64
process.

> **No game files or copyrighted assets are included, and none will ever be distributed.**
> You must provide your own legally dumped and decrypted copy of Bloodborne (CUSA03173 v1.09).
> This project is not affiliated with or endorsed by Sony Interactive Entertainment,
> FromSoftware, or AMD.

## Status (2026-10-26)

**Current version: [mac-v0.5.9](https://github.com/Wooloomooloo2/bloodborne_mac_port/releases/tag/mac-v0.5.9)** (download the app, or build it as below).

**Work in progress, playable.** On an Apple M4 Max the game boots, character creation works,
saves load, and the game runs at a **locked 60 FPS** at 1080p and 1440p native, and now at
**4K native too** (70 FPS uncapped in Hemwick), or upscaled with MetalFX, FSR 3.1 or TAA,
through its own **native Metal renderer**. Expect occasional rendering glitches (see
[Known issues](#known-issues)).

The target is a locked 60 FPS (the game's physics run at 30 Hz), with even frame pacing, at the
highest resolution each Mac allows; 90 and 120 FPS caps are there for ProMotion displays.

**Releases:** 0.5.x is the stable line. **0.6** will be **MetalFX frame generation** (#64, from
@evill33t): it comes out as pre-releases first and only becomes the default once it's well tested.
The opening-movie freeze and crash are fixed in 0.5.8, and the slowdowns on 16-18 GB Macs (#69)
in 0.5.9. Online play (#9) is still in progress.

### New in 0.5.9: 16-18 GB Macs run at full speed

- **16-18 GB Macs no longer slow down to 10-20 FPS** (#69, #83). The cause was memory, not the
  GPU: the port kept 6.5 GB of GPU memory pinned (it can never be swapped out), and on a 16 or 18
  GB Mac that left too little for the game and macOS, so the Mac swapped and the frame rate
  collapsed. On Macs with 24 GB or less, the game's memory is now mapped to the GPU only where the
  game actually uses it (3.5 GB pinned instead of 6.5 GB). This is
  [@MojahedAbushawish](https://github.com/MojahedAbushawish)'s work in #85 and #86.
  - On a Mac limited to behave like an 18 GB Mac: 15-17 FPS before, 58-60 after (1080p, 60 cap).
  - Players: an M4 MacBook Air 16 GB went from 11-16 FPS to 57-60 (#83), and the M3 Pro 18 GB
    from #69 plays smoothly.
  - Macs with more memory keep the previous layout, unchanged. `BB_ARENA_LAYOUT=legacy` in
    `launch.conf` brings it back on a smaller Mac, if ever needed.
- If you added `BB_ARENA_GIB` or `BB_GC_BUDGET_MB` to `launch.conf` while testing, remove them.
- Thank you to @lifeos-creator, @danielsirbubusiness and @khhan0703 for testing the test build on
  their Macs.

### New in 0.5.8: the opening-movie freeze and crash fixed

- **New games no longer freeze or crash in the opening movie** (#46, #50, #51, #24, #84). Both
  showed up on slower or busier Macs, which is why they were hard to reproduce on a fast one.
  - **The freeze** ("no new frame for 30 seconds"): when a movie ended and the game closed the movie
    player at the same moment, the stop request could miss a decoder thread that was waiting for a
    free frame buffer, and the game waited for it forever.
  - **The crash** (signal 11 at guest offset 0x26c01c3): during the movie, the game's main thread
    drew into the render thread's GPU context from a path the port didn't lock. It now runs under
    the same lock as the others.
  - On a Mac slowed down on purpose, 5 of 5 new games froze or crashed before the fix; 4 of 4
    reached character creation after it. Normal play and the benchmark are unchanged (60 FPS).
- **A hang report:** if the game ever stops responding, the log now lists where every game thread
  is waiting, so freeze reports can be read from `game.log` alone.
- **Co-op stability** (from [@noelmiller](https://github.com/noelmiller), #11): two renderer stops
  seen in co-op (a render-pass submit and a recording-chunk race) are now survivable.
- **Safety fixes in the PS4 runtime** (from [@evill33t](https://github.com/evill33t), #80, #81,
  #82): semaphore handles kept at full width, system-module IDs checked in full before lookup, and
  guest thread-local storage checked before the runtime is entered.

### New in 0.5.7: mouse look, a Stop button, keyboard fixes

- **Mouse look** (from [@MojahedAbushawish](https://github.com/MojahedAbushawish), #20). Turn on
  **Mouse camera** in the launcher (General → **Controls**) or in the in-game menu (⌘,).
  - Click once in the game window to turn the camera with the mouse; ⌘ gives the cursor back.
  - Left click R1, right click L2, middle click R3 (lock on), side buttons R2 and L1.
  - While Mouse camera is on, the **arrow keys are the d-pad**: they scroll menus, and in game they
    switch weapons, tools and quick items.
  - The launcher's **Controls** section lists every keyboard and mouse binding.
- **A Stop button** (from [@DiAvisoo](https://github.com/DiAvisoo), #31). While a game started with
  **Play** runs, the launcher stays behind it with a **Stop** button; quitting the launcher stops
  the game too. Exit Game in the game's own menu is still the safe way out, because it finishes
  saving.
- **The keyboard works with a controller connected.** Before, a connected controller switched the
  keyboard off, so Enter or Space could not press Play Offline.
- **⌘ shortcuts no longer reach the game:** ⌘Q drank a Blood Vial. ⌘W no longer quits the game
  (from #20).
- **A safety fix in the frame rate patches.** The community 30/60/90/Uncap FPS++ patches wrote a
  return instruction into the middle of the game's condition wait. Any timed wait reaching it
  would have crashed; it never showed in play, but now the game's own code runs there. Found by an
  assessment comparing the port with [Paleblood](https://github.com/Yharnam-Hunters/Paleblood),
  verified and fixed independently.
- **Enemy Control** (patch files only) is refused together with a frame rate patch, because the two
  change the same code.

### New in 0.5.6: mod manager and cheats, benchmark, 16:10, better crash reports

- **Mods & Cheats** (asked for in #16 by @noelmiller): a **Mods & Cheats…** button in the
  launcher opens its own window. See [Mods and cheats](#mods-and-cheats).
  - **Mods:** your loose-file mods in load order, with on/off switches. You can reorder them, add
    one from a folder or a .zip, or remove one (it goes to the Trash). A mod that can't load says
    why, and a conflicts list shows which mod wins each file. Game files are never changed.
  - **Cheats & Tweaks:** 12 switches for patches the game already had: can't die, enemies don't
    see or hear you, rally, physics, analog sensitivity, camera distance and auto-rotation, text
    size, all game languages.
  - **Patch Files:** on/off switches for your own shadPS4/GoldHEN `.xml` patch files.
  - Before the first launch with a new set of mods or cheats, the launcher **backs up your save**.
- **Benchmark** (in the launcher): a recorded walk through Central Yharnam at 60 FPS, then 30
  seconds uncapped, with a results window. It measures average and best FPS, 1% and 0.1% lows,
  hitches during traversal and their likely cause (shader compiles, streaming, GPU, game CPU),
  plus CPU, GPU, memory, swap and thermal state. It's meant for comparing Macs and for reports
  about slowdowns. See [Benchmark](#benchmark).
- **Dated session logs and fuller crash reports** (from [@satperm](https://github.com/satperm), #40):
  one timestamped log per session, plus a JSON version for log tools. Warnings go to the macOS
  system log. Crash reports include macOS's own crash files and a diagnostics zip. See [Logs](#logs).
- **About panel and update check** (from @satperm, #23), and the **game's own icon in the Dock**
  (from [@soulripper13](https://github.com/soulripper13), #66; made in memory from your game
  folder, never saved).
- **16:10 resolutions** (#75, asked for by @Frosty2804): 1280×800, 1920×1200 and 2560×1600 under
  **Ultrawide or 16:10** in the launcher, using Kyo's patches (correct HUD and lock-on positions).
  That is closer to the shape of MacBook displays, so full screen has thinner black bars.
- **Audio dropout diagnostics** (from [@DiAvisoo](https://github.com/DiAvisoo), #32): opt-in
  logging of the audio signal, queue and decoder, for reports of crackling or gaps.
- **Fixes:**
  - The arrow keys no longer also press the d-pad. Turning left swapped the left-hand item and
    broke a sprint (#61, reported by @IlyaDylko). IJKL are still the d-pad.
  - A GPU process stuck on hung GPU work no longer leaves the game hanging after it's killed, so
    the launcher can start the game again (#71, reported by @wgorecodes).
  - F9 pad recording (`BB_PAD_RECORD`) works again.

### New in 0.5.5: lifts, crash and upscaling fixes, no more red flashes

0.5.5 is a full release of the 0.5.4.3 to 0.5.4.5 pre-releases, plus one new fix. The first launch
rebuilds the shader cache once (a minute or so of stutter).

- **Red flashes on breath and other effects** (new in 0.5.5). In cold areas such as Cainhurst,
  the hunter's breath could flash as a bright red ball every few seconds. The PS4's GPU
  has float instructions that turn infinities and invalid values into 0 or the largest normal
  number. The shader translator turned several of them into ordinary maths, and Metal's fast maths
  removed the checks for invalid values, so one bad pixel bloomed into a red ball. Those
  instructions now follow the PS4's rules: in testing, flashes went from about 1 frame in 60 to
  fewer than 1 in 500 with the first half of the fix, and the second half targets the rest. If you
  still see one, please report where.
- **Fixed game timing by default, so lifts no longer drop you through the floor.** The port always
  advanced the game by each frame's measured time, so on uneven frames the character could sink
  through a moving lift and die. At the 60 and 90 FPS caps the game now runs in fixed steps (the
  60/90 FPS++ patches); 30 runs the game unpatched, and 40 and 120 stay variable. If a scene can't
  hold the cap, the game slows down a little instead. **Game timing** under the launcher's frame
  rate cap (`game_timing` in `bbport.ini`) switches back to variable steps.
- **The rare "memmove" crash is fixed** (#34, #37, #38, #54, #57). The game's render thread builds
  each frame's commands in a fixed 256 KB pool. It normally uses about 3% of it, but on rare frames
  it needs more, and the game then copied into a null address and crashed. When the pool runs out,
  the port now hands the game memory from a 16 MB spare pool and logs a "Frame pool: … full" line
  in `game.log`.
- **MetalFX and FSR 3.1 no longer turn hazy while moving** (#48, found and measured by
  @MojahedAbushawish). The camera motion vectors pointed the wrong way vertically on the Metal
  renderer, so the upscalers blended in history from the mirrored position.
- **Native AA works again with texture compression on**: since 0.5.4 it, and upscaling above
  1080p, silently ran without the upscaler.
- **The launcher no longer freezes on macOS 26.0.1** when switching from Advanced back to General
  (#47): General and Advanced are now a switch at the top instead of tabs, which hit a SwiftUI
  bug in that macOS version.

### New in 0.5.4.2: GPU stall crashes fixed

- **"The GPU stopped making progress" crashes, and the frame rate sinking over a session, are
  fixed** (#42, #43, #44, #45, #49, #55). The extra Metal views of GPU memory added in 0.5.4 cost
  no memory, but macOS's GPU driver counted each one as memory it had to keep in place, until it
  paged on every frame and the GPU stalled. They are gone; meshes high in GPU memory are copied to
  a small buffer instead (@DiAvisoo's original fix). Found and measured by @noelmiller.

### New in 0.5.4.1: missing trees

- **Trees and other objects that never drew are back** (#35). The Metal renderer skipped every
  tessellated draw with more than one instance, which some areas use for whole trees: in the
  Forbidden Woods, about 1,900 draws every 5 seconds were dropped. Each instance is now drawn on
  its own; 60 FPS holds there on an M4 Max.

### New in 0.5.4: the biggest speed-up since native Metal

The same spot in Hemwick Charnel Lane, M4 Max, full screen, upscaler off, frame rate uncapped,
read from Apple's Metal Performance HUD:

| Output | 0.5.3 | 0.5.4 | |
|---|---|---|---|
| 4K native (3840×2160) | 51.3 FPS, GPU 20.1 ms | **70.1 FPS, GPU 14.9 ms** | **+37%** |
| 4K native, another view | 80.7 FPS, GPU 12.6 ms | **109.1 FPS, GPU 9.3 ms** | **+35%** |

And in alternating test runs (M4 Max, uncapped; the presenter stops at 120 FPS):

| Scene | Before | After |
|---|---|---|
| Hunter's Dream, 1440p native, texture compression off → on | 101.7–102.1 | **119.6–119.8** (at the 120 limit, so at least +18%) |
| Hunter's Dream, 1440p native, invariant positions (below) | 85.7 | **98.9** (+15%) |
| M3 Pro 18 GB, compression off → on ([@MojahedAbushawish](https://github.com/MojahedAbushawish)) | 37.6 / 49.7 / 28.5 | **44.3 / 62.2 / 40.2** (+18% / +25% / +41%) |

GPU time is what decides whether a slower Mac holds 60 FPS (16.7 ms per frame), so these gains
matter most on base and Pro chips. What changed:

- **Lossless texture compression** (from [@MojahedAbushawish](https://github.com/MojahedAbushawish), #33).
  Apple GPUs compress textures and render targets without any loss, but only when they are
  created without two usage flags. Every texture had both, so nothing was compressed and memory
  bandwidth was the largest cost of a frame. Textures now get those flags only when something
  needs them. Launcher: Advanced → Performance → **Texture compression** (on; turn it off if you
  see wrong colours or broken textures).
- **Invariant positions instead of precise math in vertex shaders** (from Mojahed's #20). Positions
  stay identical between passes through Metal's invariance, without the precise-math mode that
  blocked the shader compiler's optimisations. The shader cache rebuilds once after updating.
- **Vanishing floors, walls and props fixed** (found by [@DiAvisoo](https://github.com/DiAvisoo), #36; #35, #18).
  Metal's indexed and indirect draws use only the low 32 bits of their buffer offset, on every
  Apple GPU tested (M2 Max, M4 Max): when streaming placed a large mesh more than 4 GiB into the
  game's GPU memory, the draw read the wrong data and the mesh disappeared until a reload. Such
  draws now go through a second Metal buffer over the same memory, at a low offset, with no copy.
- **Smoother full screen** (#25): three frame buffers instead of two in full screen, where macOS
  shows the game's frames directly. Late frames per test run: 10–18 → 0–2.
- **Native ultrawide** (from [@DiAvisoo](https://github.com/DiAvisoo), #30, building on Kyo's
  resolution patches): 2560×1080 and 3440×1440 (21:9), 2560×720, 3840×1080 and 5120×1440 (32:9)
  in the launcher, with the camera and HUD adjusted. 3840×1080 holds 60 FPS on an M4 Max; the
  1440-tall modes need a lot of GPU memory, and the launcher warns on Macs with less than 24 GB.
- **Optional frame pacing** (in-game menu: Display → Frame pacing): each frame stays on screen for
  one period of the FPS cap, for even motion on ProMotion displays, at the cost of about one
  frame of input latency. Off by default.
- The pipeline archive is written less often while many new shaders appear (less competition with
  the game on Macs with fewer cores).

### New in 0.5.3: fixes, crash reports

- **The black loading screen** (#18, found by Mojahed in #20) and **very low frame rates on 16 GB
  Macs** with an upscaler or at 720p (#22, #19) are fixed.
- **Crash reports:** the launcher shows what happened and opens a GitHub issue with the log.
- **Advanced tab** in the launcher (#15); +5–15% FPS from deferred copies and 10–22% less CPU
  (Mojahed's #20); MoltenVK and the Vulkan loader are gone from the app.

### Benchmarks: 0.4.0 → 0.5.0

Apple M4 Max (48 GB), frame rate uncapped, FPS while standing still (30 s), 2026-10-17/18. Each
cell is mac-v0.4.0 (MoltenVK) → mac-v0.5.0 (native Metal); "Quality" upscales 1.5× (for example
1280×720 → 1920×1080).

| Scene | 1080p native | 1440p native | 1440p MetalFX Quality | 1440p TAA | 1080p FSR 3.1 Quality | 1440p FSR 3.1 Quality |
|---|---|---|---|---|---|---|
| Central Yharnam, first lamp | 97.5 → **97.7** | 66.2 → **75.1** | 95.1 → **95.7** | 61.9 → **69.9** | 117.0 → **119.5** | 95.6 → **98.0** |
| Cathedral Ward | 103.2 → **104.8** | 72.0 → **82.7** | 99.5 → **102.3** | 67.0 → **78.3** | 122.0 → **126.6** | 98.8 → **108.6** |
| Upper Cathedral Ward | 78.5 → **83.2** | 57.2 → **65.2** | 76.7 → **81.0** | 54.4 → **61.7** | 92.0 → **97.7** | 75.7 → **84.8** |
| Hunter's Dream, headstone | 98.0 → **104.3** | 69.5 → **82.0** | 92.6 → **100.9** | 64.9 → **76.1** | 108.0 → **120.2** | 92.6 → **99.0** |

0.5.0 is ahead in every case, by **12–18% at 1440p**. With the 60 FPS cap both hold a steady 60
(every frame 16.7 ms). Uncapped, 0.5.0 occasionally has a single longer frame (20–40 ms) that
0.4.0 does not; it does not occur with a frame cap (backlog).

### From 22 to 120 FPS

The first playable build (2026-10-11) ran the first level at **~22 FPS** at native 1080p. What
got it to 60 and beyond, in order:

| Version | Change | Effect |
|---|---|---|
| 0.3.0 | **MetalFX temporal upscaling** (via `VK_EXT_metal_objects`) alongside FSR 3.1 | Render at 720p, upscale to 1080p: 22 → ~30 FPS at the time |
| 0.3.0 | **Native arm64 GPU process.** The game still runs as x86-64 under Rosetta 2, but all rendering moved into a separate native `bbgpu-host` process sharing the game's memory (Mach memory entry, ring buffers, cross-process page-protection relays) | MoltenVK's Vulkan→Metal encoding costs **1.75× less CPU** natively; now the default |
| 0.3.0 | **Tiny render area for passes with no output.** ~20 vertex-only passes per frame were tiled over a 16384×16384 area | 27.5 → 33.9 FPS in the city |
| 0.3.0 | **Upload hoisting.** Buffer uploads recorded ahead of the submission instead of splitting render passes | 33.9 → 34.8 FPS, fewer G-buffer pass splits |
| 0.3.0 | **Pipeline cache kept between sessions.** It was wiped every launch (~500 shader compiles each session; the "stutter when attacking") | 2 compiles per session instead of ~500 |
| 0.3.0 | **No "precise" float math in pixel/compute shaders.** SPIR-V `NoContraction` became `[[clang::optnone]]` helper calls for every float operation in Metal (223 of 308 shaders) | City respawn spot **39 → 60 FPS**, GPU 19.6 → 12.1 ms per frame |
| 0.3.0 | Shader permutation fix (Metal rejected ~6 pipelines per session, their draws vanished); unnamed pooled command buffers (MoltenVK labelled every encoder, ~4% CPU) | Correct image; less CPU per frame |
| 0.3.0 | **Shaders compile on background threads** (4 low-priority threads). Option `shader_compile=wait` (brief hitch, complete image), `skip` (no hitch, an object may pop in) or `balanced` (default: waits up to 6 ms per frame, then skips) in `bbport.ini` and the in-game menu | Level load with an empty cache: render thread compile time 440 → 130 ms; worst frame 248 → 76 ms with `skip`. Walking into new areas with slow (cold) compiles: 15 frames frozen 120–190 ms with `wait`, one 41 ms frame with `balanced` |
| 0.3.1 | **MoltenVK encodes while we record** (prefilled Metal command buffers) instead of encoding the whole frame inside the flip's submit (~13 ms) | Judder: late game frames **~6× fewer**; the "solid 60" feel |
| 0.3.3 | **No Metal argument buffers in MoltenVK** (push descriptors bound directly; the tip came from [Huax-1839/bloodborne-macos](https://github.com/Huax-1839/bloodborne-macos)) | **+13–16%** everywhere: 1080p native 82 → 94, 1440p native 59 → 66 (above 60 for the first time), FSR/MetalFX 92 → 107 |
| 0.5.0 | **Native Metal renderer.** The video core encodes Metal directly (no Vulkan, no MoltenVK): untracked guest buffers with our own ordering, a pipeline archive kept between sessions, MetalFX on our own textures, FSR 3.1 through a Metal back end for AMD's FidelityFX interface | **1440p native 69.5 → 82.0**, FSR 3.1 Quality 1080p **108 → 120** (Hunter's Dream; table above) |
| 0.5.3 | **Copies inside a render pass wait until it ends** (Mojahed, #20) | +5–7% at 1440p, up to +15% at 1080p |
| 0.5.4 | **Invariant positions** instead of precise vertex math (Mojahed, #20) | Hunter's Dream 1440p **85.7 → 98.9** |
| 0.5.4 | **Lossless texture compression** (Mojahed, #33) | Hunter's Dream 1440p **102 → 120** (cap); 4K native in Hemwick **51 → 70** together with the line above |

Also fixed along the way: a MoltenVK crash from two threads sharing one command pool, and
geometry corruption at 1440p (scenes above 1080p now use live resolution changes).

### New in 0.5.2: 16 GB Macs, controller menu

- **Less GPU memory, for 16 GB Macs** (#19). The game's GPU memory block is now 6 GB on every
  Mac, sized to what the game actually uses (it was 8 GB on 16 GB Macs and up to 16 GB on larger
  ones), and textures are budgeted around it. On 16 GB Macs the game sat right at macOS's GPU
  memory limit, which can make it very slow. With a 16 GB Mac's limits simulated on an M4 Max,
  peak GPU memory drops from ~11.5 GB to 9.6 GB at a steady 60 FPS.
- **A possible fix for the black loading screen** (#18): the order of the game's compute
  submissions could leave the GPU waiting on work that was never queued.
- **The in-game menu works with a controller again.** L3+R3 opens it, the D-pad and
  Cross/Circle move around it, and the D-pad moves sliders such as Sharpness.
- **The launcher can be turned back on from the game**: the in-game menu's Display tab has
  "Show the launcher when Bloodborne opens".

### New in 0.5.1: fixes

- **macOS 27: the GPU process works again** (thanks to [Noel Miller](https://github.com/noelmiller)).
  On macOS 27 the system's memory allocator takes addresses in the range the GPU process needs
  before the program starts, so it fell back to running the game in one process. A second build
  of the GPU process (`bbgpu-host-macos27`) reserves the range when macOS loads it; the game
  starts it on macOS 27 and later only (earlier versions keep the existing one).
- **Typing your character's name works in full screen** (thanks to Noel Miller): a box over the
  game shows the prompt and what you type. Backspace deletes, Enter confirms, Esc cancels; the
  name is exactly what you typed (an empty or cancelled entry no longer becomes "Hunter").
  Password fields show dots.
- **A game folder without the game's data is refused with an explanation** instead of crashing
  (exit code 139): the 1.09 update on its own is not enough, the folder needs the whole base game
  with the update copied over it.
- The log's `GPU arena:` line says what decided the arena's size.

### New in 0.5.0: native Metal

- **A native Metal renderer replaces MoltenVK.** Faster in every scene and setting measured
  (above), most at 1440p. MetalFX, FSR 3.1 (AMD's FidelityFX code on a Metal back end; its output
  matches the Vulkan version bit for bit on test frames), TAA, FSR 1 (`BB_FSR1=1`), the settings
  overlay and frame capture all run on it.
- **Memory leak fixed (game process).** The game process grew about **4.5 GB per hour** of play
  (2.5 → 6.2 GB in a 45-minute walk): freed small blocks of the runtime's allocator were filed
  under the wrong size class. It now stays flat at about 2.5 GB.
- **Macs with less memory** (thanks to [Noel Miller](https://github.com/noelmiller)): the GPU
  memory arena is sized to the Mac (a quarter of its memory, 8 GiB minimum, 16 GiB at most);
  a 16 GiB arena left the GPU waiting on a 36 GB Mac (~7 FPS). A rare freeze after long
  sessions (two threads recording GPU commands at once) is fixed, touchpad touches behave like a
  DualShock 4's, and a missing GPU hint no longer stops the game.
- **Rendering fixes:** three full-screen passes that Metal refused are drawn (the light behind
  the cage bars at the first lamp; the figure in the cage is back).
- **Fallback:** if the GPU process cannot start, the game runs in one process on Metal (60 FPS
  here).

### New in 0.4.0: the launcher

`Bloodborne.app` now opens a launcher window (native SwiftUI) before the game, like a Steam
launcher, with every setting on one screen (no scrolling, even on a 13" MacBook Air):

- **Display:** output resolution, windowed or full screen, frame rate cap (30, 40, 60, 90, 120).
- **Upscaler:** off, MetalFX or FSR 3.1, its quality level, and the resolution the game
  actually renders at (for example 1280 × 720 → 1920 × 1080); FSR sharpening.
- **Effects:** depth of field, motion blur, chromatic aberration, ambient occlusion, shadows
  from dynamic lights, screen-space reflections, the game's own anti-aliasing, the FPS counter,
  and **skip intro videos** (straight to the title screen, no Sony and FromSoftware logos).
- **Locations:** the game folder (checked before Play is enabled) and the save folder (it
  offers to copy your saves to a new folder; the originals stay where they were).
- **Show launcher every time:** untick it to start the game directly next time; hold **⌥
  (Option)** while opening the app to get the window back.

Settings are shared with the in-game overlay (`⌘,`) and stored in `bbport.ini`. When the game
quits, the launcher comes back; if it stopped with an error, the launcher says so and offers
the log.

### Earlier changes (0.3.x, 2026-10-14)

- **Load crash fixed.** The intermittent crash while loading an area (the GPU window closing
  during a loading screen) was a race in the game itself: the main thread records
  cloth/texture compute jobs into the render thread's command stream without a lock, and the
  render thread's next loading-screen frame could start in the middle of them. On a PS4 the
  main thread is done before the next vsync; under Rosetta it can take ~7 ms longer, more when
  the Mac is busy. The port now hooks both game functions and makes them take turns (the main
  thread waits well under a millisecond); in 15 test loads the race was caught 10 times and
  never corrupted anything. As a safety net, corrupted GPU commands are logged and skipped
  instead of ending the game.
- **Sharper on Retina displays:** the window uses the display's real pixels (full screen on a
  MacBook was rendered at half resolution and stretched).
- **Full screen works** (⌃⌘F or the green window button). It used to freeze the picture.
- **Lower display latency:** two swapchain images instead of three, so the time from the
  game presenting a frame to it reaching the screen went from 48.5 to 31.7 ms.
- **Game file reads no longer fail** when they land in memory the GPU is watching. macOS
  failed such reads outright, so the game could lose an asset load; a possible cause of the
  missing-geometry reports below.
- **Shader compile mode `balanced`** is the default: new shaders compile in the background
  and a frame waits at most 6 ms for them, so new areas no longer freeze for 100+ ms.
- **Less judder:** late game frames (the cause of judder at a steady 60) dropped about 6x with
  the upscaler off or FSR: MoltenVK now encodes each frame's Metal commands while they are
  recorded instead of all at once at the flip, which the game was waiting behind.
- **Diagnostics:** `BB_FRAME_STATS=1` prints per-stage timings of every late frame ("Spike"
  lines, with each game thread's CPU time), and corrupted GPU command streams are reported
  with the game threads and code involved.

### Known issues

- **Missing geometry:** floors, walls and props vanishing until a reload (Metal's 32-bit index
  offsets) are fixed in 0.5.4, and trees and other instanced tessellated objects that never drew
  (Forbidden Woods, #35) in 0.5.4.1. If something still goes missing, reports with a screenshot
  and `game.log` are welcome on #35.
- **Occasional judder at a steady 60 FPS** (some game frames take 20–45 ms): much rarer since
  2026-10-15 (about 6x fewer late frames).
- **Uncapped, an occasional single long frame** (20–40 ms) on the Metal renderer; capped play
  (60/90/120) is not affected.
- **Above 1080p, the selected item's icon in the HUD is missing** (an empty frame; #14). It is
  there at 1080p.
- **At 16:10, full-screen menus keep their 16:9 artwork** (grey bars at the sides), and the first
  moments of loading a level can glitch. The HUD itself is placed correctly. A UI mod such as
  "16x10 and 4x3 UI Fixes" (Nexus Mods) fixes the menus and loads through **Mods & Cheats…**.

### What's next

- **Fewer Metal render passes.** In Central Yharnam, tessellated draws split the render pass
  67 times a frame (372 Metal encoders per frame); skipping tessellation altogether as an upper
  bound saves 1–2 ms of GPU time per frame (6.6–7.5 → 5.6 ms). Keeping those draws inside the pass is next
  (Mojahed's #39 starts on it).
- **0.6:** MetalFX frame generation (#64), as pre-releases first. Online co-op (#9) after that.
- Rebindable keys (an `input.ini`, from #21's ideas) and the keyboard/mouse list in the launcher made editable.
- Confirm the missing-geometry fix on more Macs; the frame-time judder.
- Signing and notarisation of the downloadable app (today it must be released from quarantine).

Details:
- Roadmap (released, next, later): [docs/ROADMAP.md](docs/ROADMAP.md)
- Plan, design decisions and porting checklist: [docs/MACOS_PORT.md](docs/MACOS_PORT.md)
- Known issues and planned refinements: [docs/MACOS_BACKLOG.md](docs/MACOS_BACKLOG.md)
- Every test and measurement: [docs/FINDINGS.md](docs/FINDINGS.md)
- The two-process design: [docs/DESIGN_NATIVE_GPU_PROCESS.md](docs/DESIGN_NATIVE_GPU_PROCESS.md)

## How it works

- **Native execution under Rosetta 2.** The eboot is converted offline into a flat memory
  image; PS4 libc and libSceFios2 functions are linked directly into it. There is no CPU
  emulation layer of our own — Rosetta 2 translates the x86-64 code (game + runtime) once.
  The build targets `x86_64-apple-macos` with deployment target 15.0 (Rosetta AVX2 support).
- **HLE runtime** (`src/`): kernel, memory, threads, audio, input (SDL3), save data and other
  PS4 libraries. macOS specifics: guest TLS via a reserved pthread TSD slot, guest-visible
  memory kept below 1 TiB, Mach-based thread inspection.
- **GPU** (`gpu/`): the shadPS4 video core and shader recompiler, built as
  `libbbgpu.dylib`, rendering through native Metal (`renderer_metal/`). It runs in
  `bbgpu-host`, a **native arm64 process** that maps the game's memory at the same addresses
  (`host/`, `docs/DESIGN_NATIVE_GPU_PROCESS.md`). If it cannot start, the game stops with a
  "Crash:" line that the launcher shows in a crash report.
- **Upscalers:** MetalFX temporal upscaling, FSR 3.1 (AMD FidelityFX on a Metal back end,
  `gpu/shim/ffx_metal/`) and TAA. Planned: zero-copy guest memory on unified memory.
- Inherited from upstream: high-frame-rate community patches, synthetic motion vectors and
  temporal upscaling (FSR 3.1), a two-stage multi-threaded draw pipeline, and an in-game
  settings overlay (`⌘,` or `L3 + R3`).

## Download (no building needed)

Ready-made builds are attached to the [releases](https://github.com/Wooloomooloo2/bloodborne_mac_port/releases)
as `Bloodborne-macOS-<version>.zip` (about 50 MB). It needs an Apple Silicon Mac with macOS 15
or newer and **at least 16 GB of memory**. You still need **your own game dump** (see below): no
game files are included.

1. Unzip it and move **`Bloodborne.app`** to Applications (or anywhere).
2. **Release it from quarantine.** The app is not signed with an Apple developer certificate,
   so macOS blocks downloaded copies ("Apple could not verify…"). Either run, in Terminal:

   ```sh
   xattr -dr com.apple.quarantine /Applications/Bloodborne.app
   ```

   or open it once, then go to **System Settings → Privacy & Security** and click
   **Open Anyway** next to the Bloodborne message.
3. Open it. The launcher window opens: under **Locations**, choose your game folder (the
   `CUSA03173` folder containing `eboot.bin`); it is checked, and **Play** is enabled once it
   passes. Pick your settings and press Play. If Rosetta 2 is missing, it offers to install it.

The app contains everything else (the game process, the native GPU process, the libraries and
a small Python for the launch scripts). Saves and settings go to
`~/Library/Application Support/Bloodborne` (another save folder can be chosen in the launcher),
the logs to `~/Library/Logs/Bloodborne` (see [Logs](#logs)). To change the game folder, use the launcher's
**Locations** (hold ⌥ while opening the app if the launcher is set not to show).

**Updates.** The first time it opens, the app asks whether to check for updates automatically
(also when the launcher window is turned off, before the game starts). If you agree, the launcher
checks the GitHub releases once a day and offers a newer version with its release notes: when its
window opens, or, with the window turned off, in the background while you play, showing the update
after the game ends. **Install
Update** downloads it, checks it (size, SHA-256 from GitHub, bundle identifier, version, code
signature), replaces `Bloodborne.app` where it is and opens it again; saves and settings are not
touched. **Bloodborne → Check for Updates…** checks at once; the update window has the
*Check for updates automatically* and *Include test builds* (pre-releases and `mac-v…-test1`
tags) options. The
app must be somewhere you can write (Applications is fine) and not run from the Downloads quarantine
location. **Bloodborne → About Bloodborne** shows the version, build and commit.

**Preparing the game folder from PKG files.** The app needs an unpacked folder, not `.pkg`
files. If you already play on shadPS4 you have one: use that folder, but first copy the
contents of the update folder next to it (`CUSA03173-patch` or similar) into it, replacing
files. To unpack decrypted (fake-signed) PKGs, download `pkg_extract-macOS.zip` from the same
release, unzip it and, in Terminal from that folder:

```sh
xattr -d com.apple.quarantine pkg_extract
./pkg_extract /path/to/base-game.pkg ~/Bloodborne        # creates ~/Bloodborne/CUSA03173
./pkg_extract /path/to/update-1.09.pkg ~/Bloodborne-update
ditto ~/Bloodborne-update/CUSA03173 ~/Bloodborne/CUSA03173   # merge the update over the game
```

Retail PKGs (from the PlayStation Store or a disc install) are encrypted with console keys and
must be dumped on a jailbroken PS4 first; no Mac tool can unpack them.

**The game files must be the original 1.09 executable.** A pre-patched `eboot.bin` (for
example a 60 FPS patch made for jailbroken consoles) is rejected: this port applies its own
60 FPS patches in memory at start, so use the unmodified `eboot.bin` from the 1.09 update.

## Requirements (building from source)

- A Mac with **Apple Silicon** running **macOS 15 or newer**, with at least **16 GB** of memory
- **Rosetta 2** (`softwareupdate --install-rosetta`)
- **Xcode Command Line Tools** (`xcode-select --install`)
- **Homebrew** (arm64) with build tools:
  `brew install cmake ninja nasm glslang pkgconf boost cryptopp`
- **Python 3** (patch compiler and eboot preparation scripts)
- **Your own decrypted game dump**: a `CUSA03173` folder (v1.09) containing `eboot.bin`,
  `sce_sys/`, `sce_module/` and `dvdroot_ps4/`. `python3 scripts/game_check.py` verifies it.

## Building

```sh
git clone --recursive https://github.com/Wooloomooloo2/bloodborne_mac_port.git
cd bloodborne_mac_port

# 1. Build pinned dependencies (SDL3, FFmpeg, SPIRV-Cross, Vulkan-Headers, ...) into deps/:
#    x86-64 for the game process, arm64 for the native GPU host (once)
bash scripts/macos/bootstrap_deps.sh
BB_DEPS_ARCH=arm64 bash scripts/macos/bootstrap_deps.sh

# 2. Build the loader, the GPU library and the GPU host
#    (out/bb-probe, out/gpu/libbbgpu.dylib, out/bbgpu-host)
bash build_macos.sh
```

Launching rebuilds whatever changed, so after a `git pull` you can simply launch again.

## Launching

### As a Mac app (no Terminal window)

Once, in Terminal from the repository folder:

```sh
bash scripts/macos/make_app.sh
```

This creates **`Bloodborne.app`** in the repository folder, with the game's own icon (taken from
your game folder, so the app is made on your Mac and never uploaded). Double-click it, or drag
it to the Dock. It opens the same launcher window as the downloadable app (settings, game and
save folders; see "New in 0.4.0" above), runs the game from this checkout without a Terminal
window, and writes the game's logs to `~/Library/Logs/Bloodborne` (see [Logs](#logs)). If the
game stops with an error, the launcher offers a crash report and the log. Run the
script again if you move the repository folder. Its update check only tells you about new releases: update a
checkout with `git pull` and build again.

### From Finder with a Terminal window

Double-click **`Bloodborne.command`** in the repository folder. It opens a Terminal window and
starts the game, with its log in that window.

- **First launch:** it asks for your game folder (the `CUSA03173` folder containing
  `eboot.bin`) and remembers it in `launch.conf` next to it. If the game folder sits next to the
  repository (`../CUSA03173`), it is found without asking.
- **Keep the Terminal window open** while you play: closing it ends the game. The game's log
  appears there, and if the game exits with an error the window stays open so you can read it.
- macOS may ask you to allow Terminal to access the folder the game is in (Desktop, Documents,
  an external drive): allow it.
- To put it in the Dock or on the Desktop, ⌥⌘-drag `Bloodborne.command` there to make an alias.
- If macOS refuses to open it ("cannot be opened because it is from an unidentified
  developer"; this happens if the repository was downloaded as a zip rather than cloned),
  right-click it, choose **Open**, then **Open** again. If it says you lack permission, run
  `chmod +x Bloodborne.command` once.

`launch.conf` holds shell assignments and is not committed. Besides the game folder it can set
any other `run.sh` variable, such as `BB_DATA_DIR` (below):

```sh
BB_GAME_DIR=/Users/you/Bloodborne/CUSA03173
```

### Saves and settings

Saves, settings (`bbport.ini`) and shader caches live in
**`~/Library/Application Support/Bloodborne`**. The **`Saves`** shortcut in the repository folder
opens the save folder in Finder; your character is in `1/CUSA00207/SPRJ0005/` (the PS4's own
save layout, so saves can be copied to and from the Linux and Windows ports). Quit the game
before copying saves.

To keep them elsewhere, set `BB_DATA_DIR=/some/folder` in `launch.conf` (or the environment).
A checkout that already has saves in its own folder (`user/`, from before this default) keeps
using them; move `user/` and `bbport.ini` to the folder above to switch.

### Mods and cheats

**Mods & Cheats…** at the bottom left of the launcher opens the mod manager. The text next to the
button shows what's on ("No mods or cheats", "2 mods · 1 cheat on").

- **Mods** are folders in `~/Library/Application Support/Bloodborne/mods` (**Open Mods Folder**).
  A mod can hold `dvdroot_ps4/`, or the game's own top-level folders (`chr/`, `parts/`, ...),
  optionally inside one wrapper folder. **Add Mod…** takes a folder or a .zip and checks its layout
  straight away.
  - Mods later in the list win when two replace the same file. The **Conflicts** list shows each
    case.
  - Each launch gets a private merged view of the game made of links. Your game folder and the
    mod folders are never written to.
  - Replacements for `eboot.bin`, `sce_module` and `sce_sys` are refused.
  - The order and switches are saved in `mods.json`. [docs/MODS.md](docs/MODS.md) has the folder
    rules.
- **Cheats & Tweaks** are patches from the community patch file (`patches/Bloodborne.xml`), each
  with its own note. Pairs that do opposite things switch each other off: rally decay or no rally,
  lighter or heavier corpse physics. The choices are stored as `cheats=` in `bbport.ini`. *Enemy
  Control* is not offered: it rewrites the same code as the frame-rate patches.
- **Patch Files** lists `.xml` patch files you put in `~/Library/Application
  Support/Bloodborne/patches`, each with an on/off switch.
- **Save backups:** before the first launch with a set of mods or cheats it hasn't seen,
  **Play** copies your save folder to `save-backups/<date and time>/`, keeping the newest five.
  Other folders in `save-backups` are never touched.

### Benchmark

**Benchmark…** in the launcher starts the game twice with its own save and settings, about 6
minutes in all. Your save and settings are not used or changed, and mods, patch files and cheats
are off, so results from different Macs compare like for like.

1. **Traversal:** a recorded walk through Central Yharnam at 1080p, capped at 60 FPS. New areas
   stream in and new shaders compile on the way, so this pass shows traversal stutter.
2. **Throughput:** 30 seconds standing still, uncapped, for the highest frame rate the Mac
   reaches.

The results window shows, for each pass:

- average and best FPS, 1% and 0.1% lows, the longest frame;
- frames over 33, 50 and 100 ms, and milliseconds lost to hitches per minute;
- the worst hitches with their time on the route and likely cause;
- GPU busy time, shader compiles, the game's CPU use and memory, swap, memory pressure and thermal
  state.

Each run is kept in `~/Library/Logs/Bloodborne/Benchmarks/<date and time>/`:

- `report.md` (the same text, ready to paste into an issue) and `summary.json`;
- a per-frame CSV for each pass, `system.csv`, and each pass's game log.

Please run it when you are reporting a performance problem. The benchmark's own save (a character
in Central Yharnam with the route cleared of enemies) and the recorded route are in `benchmark/`
and inside the app; your own saves are never used.
[docs/DESIGN_BENCHMARK.md](docs/DESIGN_BENCHMARK.md) has the details.

### Logs

Each game session started from the launcher writes two logs to **`~/Library/Logs/Bloodborne`**,
named by the time it started. The last 20 sessions are kept, at most 256 MB in all, and one
session's logs stop at 64 MB (a normal session is well under 1 MB): past that only its last
1,000 lines are written, when the game ends, after a line saying how many were left out.

- `game-2026-10-22_14-03-05.log`: the game's output, each line with its time and a syslog
  severity (`CRIT`, `ERROR`, `WARN`, `NOTICE`, `INFO`, `DEBUG`):

  ```text
  2026-10-22T14:03:05.869+03:00 NOTICE launcher: Bloodborne for Mac 0.5.4 on Mac15,8 (Apple M3 Max), 64 GB, ...
  2026-10-22T14:03:06.284+03:00 WARN   GPU [Render.Vulkan] <Warning> vk_pipeline_serialization.cpp:369 ...
  2026-10-22T14:09:41.604+03:00 CRIT   Crash: the GPU process crashed (signal 11 in ...)
  ```

- `game-2026-10-22_14-03-05.jsonl`: the same lines as JSON, one object per line, for Grafana Loki
  (Alloy/Promtail), Elasticsearch (Filebeat), Vector and similar: `@timestamp` (UTC),
  `log.level`, `syslog.severity.code`, `log.logger` (who wrote the line: `runtime`, `bbgpu-host`,
  `gpu.render.vulkan`, ...), `message`, `session.id` and `service.version`.

`game.log` and `game.previous.log` link to the last two sessions' `.log` files. Warnings and
errors also go to the macOS system log (unified logging), with the line's source as its category:
open **Console.app** and search for `bloodborne-mac-port`, or in Terminal:

```sh
/usr/bin/log stream --predicate 'subsystem == "io.github.wooloomooloo2.bloodborne-mac-port"'
```

The game never depends on the logs: if the launcher quits or a log can't be written (a full
disk), the game keeps running and only those lines are lost.

The severity is read from the line itself: `Crash:` lines are critical, `STOP:` and `ERROR:` lines
errors, the GPU library's own levels (`<Warning>`, `<Error>`) are kept, and lines saying something
failed are warnings. Your home folder appears as `~` in the system log and in crash reports.

**Crash reports.** When the game stops with an error, the launcher shows what happened and, by
itself, puts a diagnostics zip in `~/Library/Logs/Bloodborne/Reports` (the last 10 are kept): the
crash report, the session's `.log` and `.jsonl`, `bbport.ini`, `launch.conf`, and macOS's own
crash reports (`.ips`) of the game's processes from that session. Their exception and crashed
thread also go into the report. Your home folder is shown as `~`, and the identifiers macOS puts
in `.ips` files (crash reporter key, sleep/wake and boot session ids) are removed. Other paths
stay as they are: a game folder on an external drive shows as `/Volumes/<drive name>/...`, so
check the report if a drive or folder name is private. **Report on
GitHub** opens a new issue with the report already filled in (copied to the clipboard as well,
for reports too long for the link) and shows the zip in Finder to drag onto the issue. Nothing is
sent until you submit the issue.

### Typing text

Text the game asks for (the character's name, the network password under *Network Settings*)
is typed on the keyboard: a box over the frame shows the prompt and the text (as dots for the
password), Backspace deletes, Enter confirms, Esc cancels and leaves the field as it was. The
field gets exactly what was typed; the game's own length limit applies (the log line
`ImeDialog opened (..., up to N chars)` shows it).

### From Terminal

```sh
cd bloodborne_mac_port
BB_GAME_DIR=/path/to/CUSA03173 bash run.sh
```

`run.sh` prepares the game image and rebuilds anything whose sources changed. It uses the same
saves and settings as the Finder launcher.

### In the game

- **Settings overlay:** `⌘,` (or L3 + R3 on a controller): FPS, resolution, upscaler (MetalFX,
  FSR 3.1 or off), shader compile mode. Some changes need a restart. The launcher shows the
  same settings before the game starts.
- **Full screen:** ⌃⌘F or the green window button.
- **Input:** game controllers (through SDL), the keyboard, and the mouse for the camera (turn on
  **Mouse camera**). The launcher's **Controls** section lists the bindings.
- **First launch and new areas** compile shaders, so the first minutes stutter a little; later
  sessions load them from the cache at start. The first launch after a rebuild takes ~20 s to
  open the window.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | Loader (`probe.c`) and PS4 HLE runtime |
| `gpu/` | shadPS4-derived video core with the native Metal renderer, draw pipeline, upscalers |
| `host/` | Native arm64 GPU host process (`bbgpu-host`) and the shared rings |
| `scripts/` | Eboot preparation, linking, patch compiler (`patches.py`); `scripts/macos/` dependency bootstrap |
| `patches/` | Community XML patches for Bloodborne 1.09 |
| `Bloodborne.command` | Finder launcher (runs `run.sh` in Terminal) |
| `scripts/macos/make_app.sh` | Builds `Bloodborne.app`, a launcher without a Terminal window (local, not committed) |
| `scripts/macos/package_app.sh` | Builds the self-contained `dist/Bloodborne.app` and its zip for releases (CI: `.github/workflows/macos-release.yml`) |
| `Saves` | Shortcut to the save folder (created on first launch, not committed) |
| `launcher/` | The macOS launcher window (Swift: `Model/` with tests, `App/`; built by `scripts/macos/build_launcher.sh`) |
| `tools/` | Developer utilities, including `pkg_extract` (fpkg unpacker) |
| `docs/` | macOS plan and backlog, plus technical notes inherited from upstream |

## Credits

This port stands entirely on the work of others:

- **deadinside28** — author of `bbport`, the original native Linux port of Bloodborne: the flat memory image approach, the HLE
  runtime, the two-stage Vulkan draw pipeline, synthetic motion vectors and the upscaler
  integration.
- **Mrsuss60** — [bloodborne_pc_windows_port](https://github.com/Mrsuss60/bloodborne_pc_windows_port),
  the Windows port this repository was forked from, including its stability, audio, memory
  and frame-pacing fixes (documented in that repository's `CHANGES.md`).
- **Paleblood contributors** — system-module argument validation evidence from
  [Paleblood, branch `codex/runtime-sysmodule-wire`](https://github.com/evill33t/Paleblood/tree/codex/runtime-sysmodule-wire),
  commit `f9c5e5556db987d9fb4fa60b837d8a54817a0718`; provenance and limits in
  [`docs/SYSMODULE_WIRE.md`](docs/SYSMODULE_WIRE.md).
- **[shadPS4](https://github.com/shadps4-emu/shadPS4)** project — the Vulkan video core and
  shader recompiler (`gpu/shadps4/`, GPL-2.0-or-later).
- **FireBurn** — [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan), the native Vulkan
  FSR 3.1 / FSR 4 runtime the port used until 0.5.0; FSR 3.1 is still built from its AMD
  FidelityFX SDK sources (MIT).
- **Omar Cornut** — [Dear ImGui](https://github.com/ocornut/imgui), used for the in-game overlay (MIT).
- **Alex Barney (Thealexbarney)** — [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9),
  AT9 audio decoding (MIT).
- **sirit** — SPIR-V emitter used by the shader recompiler (BSD-3-Clause), with Khronos
  **SPIRV-Headers** (MIT-style Khronos license).
- **Christian Rau** — [half](https://half.sourceforge.net/), IEEE 754 half-precision
  library (MIT).
- **AMD** — GCN PM4/register headers in `gpu/third_party/gcn/` (MIT-style AMD license).
- **DejaVu fonts** (Bitstream Vera license) — overlay font.
- **[MoltenVK](https://github.com/KhronosGroup/MoltenVK)** (Apache-2.0), which ran the renderer
  until 0.5.0, and the dependencies built by `scripts/macos/bootstrap_deps.sh`: SDL3,
  Vulkan-Headers, SPIRV-Cross, fmt, xxHash, miniz, FFmpeg, magic_enum, robin-map, Boost.
- **Community patch authors** — Kyo, Lance McDonald, illusion, emoose, auser1337 and others
  for the 60 FPS, camera and engine patches in `patches/Bloodborne.xml`.
- **zackcage6** — testing and bug reports for the Windows port.

### Contributors to this port

Thank you to everyone who sent pull requests:

- **[Noel Miller](https://github.com/noelmiller)**
  - macOS 27 support for the GPU process (#7).
  - A survivable GPU setup warning (#10).
  - Touchpad gestures (#8).
  - Buffer arena sizing for Macs with less memory (#13).
  - The HUD item icon above 1080p (#17).
  - Online play (#9, in progress).
  - Survivable co-op renderer stops (#11).
- **[Mojahed Abushawish](https://github.com/MojahedAbushawish)**
  - Lossless texture compression (#33).
  - The upscaler haze fix (#48).
  - Invariant vertex positions, copies held to the end of a render pass, and the loading-screen fix
    (from #20).
  - Mouse look (#20).
  - Guest memory mapped in runs for 16-18 GB Macs (#85, #86).
  - Benchmarks on an M3 Pro.
- **[DiAvisoo](https://github.com/DiAvisoo)**
  - Native ultrawide (#30).
  - Duplicate-launch and game-folder checks (#28).
  - The disappearing-geometry finding (#36).
  - Audio dropout diagnostics (#32).
  - The launcher's Stop button (#31).
- **[satperm](https://github.com/satperm)**
  - The About panel and update check (#23).
  - Dated session logs and crash reports (#40).
- **[soulripper13](https://github.com/soulripper13)**: the game's icon in the Dock (#66).
- **[evill33t](https://github.com/evill33t)**: PS4 runtime safety fixes (#80, #81, #82) and MetalFX
  frame generation (#64, in progress).

Thanks also to everyone who reports issues and tests builds on their Macs. Reports, logs and
measurements that shaped recent releases came from @noelmiller, @MojahedAbushawish, @DiAvisoo,
@satperm, @IlyaDylko (keyboard sprint #61, long-session FPS #55), @wgorecodes (stuck game #71,
preferred display #60), @Frosty2804 (16:10, #75), @lifeos-creator (M3 Pro, #53/#69, tested 0.5.9), @Sah-D (#47),
@ulhaas, @Ohaq2108 and @rozenstein94 (the opening-movie freeze, #46), @khhan0703 (the opening-movie crash, #84, tested 0.5.9), @Didjus (#18),
@PensionPlanning (#19), @muharremsedona6034gm-hub (#22), @Walltor (#25), @AmberUpraity,
@VanDung-dev, @douglasrlee, @jefer9011 and @mariuszparowa666 (GPU stalls, fixed in 0.5.4.2),
@zhouhaoyaomei-ship-it, @SimonKabb and @juliotwo (the memmove crash, fixed in 0.5.5), @ELLDev
(#74), @danielsirbubusiness (#83, tested 0.5.9), @g3rro333333333, @rwv7p56n9s-cmyk, @LICHTYYYVVV and @vtsilva77.

Each third-party component keeps its own license file in its directory.

## License

GNU General Public License v2.0 or later (GPL-2.0-or-later), because of the shadPS4
components. See [LICENSE](LICENSE).
