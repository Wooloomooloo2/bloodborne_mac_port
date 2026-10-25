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

The image fixtures also check the retained timed-condition-wait body, original
camera defaults/parameter stores and wind-load opcode. They compile the actual 60/90/Uncap FPS++
and Enemy Control XML sets and require the installed Adaptive guard to reject
them, while accepting an unrelated cheat. At 30 there is no installed timing
adapter and these sets remain outside its guard. These are installation checks;
they do not establish moving-lift collision or live camera-speed equivalence.

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

## Upstream refresh, 25 October 2026

Merged upstream `master` (`af0fae2`), retaining the mouse controls, keyboard fixes,
launcher Stop behavior, benchmark, session logs and 0.5.7 release notes. The build's
test runner keeps both the Adaptive timing checks and upstream's Stop lifecycle tests.
The settings fixture also verifies that repeated overlay saves preserve Adaptive
timing alongside mouse enablement, sensitivity and inversion. Timing implementation,
patch guards and restart-selection code are unchanged from `3a37beb`.

Fresh arm64 and x86-64 GPU libraries, the full game runtime including the Adaptive
adapter, both native GPU-host variants and the launcher build successfully. GPU
builds used CMake/Unix Makefiles with LTO disabled. The full Ninja-based
`build_macos.sh --test` command was not run; checks were built and run individually.

Passing checks: 20 limiter cases, 93 numerical checks, 21 wind-instruction cases and
all 20 negative-control mutations; unsupported-image refusal; 19 Python patch tests,
six preparation tests, seven restart tests, both launcher Stop tests, launcher model
tests, settings persistence, launch settings, runtime, semaphore, pad and upscaler
support. Cross-architecture rings verify two million records, 500,000 events and
20,000 requests without corruption. The offscreen Metal overlay passes normally
and with `MTL_DEBUG_LAYER=1`.

No gameplay, packaged-app session or optional real-1.09-image installation test was
repeated for this update. The live gameplay results above describe the earlier build;
the existing experimental limitations still apply.
