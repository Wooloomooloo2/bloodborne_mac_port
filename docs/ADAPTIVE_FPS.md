# Adaptive FPS timing (experimental)

In the launcher, select **Game timing → Adaptive (experimental)** and a frame cap,
then start the game. Changes apply on the next start. This writes
`game_timing=adaptive` in `bbport.ini`. Fixed remains the default, and existing
fixed/variable settings continue to work. The renderer's settings overlay preserves
the launcher's timing choice when saving its own controls.

The adapter lives in the x86-64 game process. The ARM64 GPU process and its
presentation limiter continue to handle rendering and display. Adaptive replaces
the legacy FPS++ XML preset for that run; it does not combine both timing systems.
At 30 FPS it installs no patches or replacement limiter.

For an explicit command-line choice:

```sh
BB_FPS=adaptive BB_FPS_LIMIT=120 bash run.sh
```

The existing launcher offers 30, 40, 60, 90 and 120 FPS. For experiments, an explicit
`BB_FPS_LIMIT` also accepts 144, 165, 240 or another integer from 30 through 240.
`BB_FPS_LIMIT=0` removes the presenter cap; Adaptive still bounds its simulation
reference at 240 FPS. These are targets, not promised achieved frame rates.
Explicit `BB_FPS` overrides the ini choice. `BB_FPS_PATCH=0` disables timing
patches, including Adaptive. A prebuilt binary must include this adapter; an older
binary rejects `--adaptive-fps` rather than running without the required hook.

The step is measured elapsed time clamped to `[1/cap, 1/30]`. Long frames are
capped at 33.3 ms, so it does not fully catch up after stalls or sustain real-time
simulation below 30 rendered FPS. Per-frame momentum, sprint, camera constants,
physics/AI and cloth values use the source timing tables. Wind phase uses half the
task step while wind-vector amplitude retains its original authored scale. The
play clock carries fractional milliseconds instead of truncating them every frame.
The original camera constructor remains; no decompiled camera is installed.

The port checks original 1.09 instructions before changing the private image.
Admission failures stop startup. The loader checks a complete external/built-in
patch set for overlaps with installed timing edits before applying any of it.
Select a legacy timing mode if you need another FPS patch. Original game files and
saves are not migrated or rewritten by this adapter.

Run the CPU checks without starting the game:

```sh
bash scripts/test_fps_timing.sh
BB_TIMING_ELF=/path/to/eboot.elf bash scripts/test_fps_timing.sh
```

The first command checks 20 limiter scenarios against an independent reference,
93 numerical checks including 40 FPS, wind instruction execution, and a required
20-case negative control. The optional ELF command admits only the supported
loaded-image SHA-256, exercises private installation at 30/40/60/90/120/144/165/240
and uncapped, updates 1,001 variable frames per converted rate, executes the
installed jump into the host replacement, checks patch conflicts and rejects
deliberately corrupted timing sites without changing any image bytes.

Live gameplay checks on 2026-10-25 also passed with 60 and 120 FPS targets on an
M4 Max, macOS 27.0, using the current rebuilt x86-64 runtime and native ARM64 GPU
host. A fresh base-game character exercised walking, camera movement, attacks,
dodging, lock-on, enemy AI/damage, death and loading into Hunter's Dream at the
60 FPS target. A copy of that save reloaded at the 120 FPS target and exercised
sprinting upstairs, stamina, a gift dialog/menu, heavy attack, dodging and camera
movement. The user also visually checked the game and closed the second session.
The original saves and source executable remained unchanged.

These are bounded gameplay checks, not a fixed/variable A/B comparison or a
presentation benchmark. The 120 target did not sustain 120 FPS in Hunter's Dream;
the adapter followed the measured frame time above and below 60 FPS. Moving-lift
collision, exact animation/camera-speed equivalence and wider-area stability
remain unverified. Adaptive is not a claim that the legacy variable-mode lift
problem is fixed. See [live findings](FINDINGS.md#adaptive-fps-live-gameplay-checks-2026-10-25)
and [source credits and exact adaptation boundaries](../src/fps/PROVENANCE.md).
