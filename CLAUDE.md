# CLAUDE.md — Bloodborne macOS port (session handoff)

Read this first, then `docs/MACOS_PORT.md` (plan, porting table, progress checklist).
Test history and results: `docs/FINDINGS.md` (keep it updated with new measurements).
Deferred work lives in `docs/MACOS_BACKLOG.md`: when taking a shortcut to get the game running,
log the proper fix there under **1) Stability, 2) Performance, 3) Mac GUI** (priority order).

## What this repo is

A work-in-progress **macOS (Apple Silicon) port** of the native Bloodborne PC port
(`bbport` by deadinside28 → Windows port by Mrsuss60). It is NOT game source code: it is a
loader + PS4 HLE runtime (`src/`) + shadPS4-derived Vulkan GPU core (`gpu/`) that runs the
original PS4 `eboot.bin` (x86-64 machine code) directly on the CPU.

- GitHub: https://github.com/Wooloomooloo2/bloodborne_mac_port (public)
- Remotes: `origin` (this repo), `windows-fork` (Wooloomooloo2/bloodborne_pc_windows_port_arc),
  `upstream` (Mrsuss60/bloodborne_pc_windows_port)
- License GPL-2.0-or-later (shadPS4 code).

## Decisions already made (don't re-litigate)

1. **No recompiling the game.** There is no game source; static x86→ARM recompilation is out
   of scope. Everything (game code + our code) runs as **one x86-64 process under Rosetta 2**.
   Our code is recompiled for `x86_64-apple-macos`, deployment target **15.0** (Rosetta AVX2).
2. **Graphics = staged hybrid:** (1) MoltenVK with the existing Vulkan renderer;
   (2) Metal features from inside it via `VK_EXT_metal_objects` (MetalFX upscaling) and
   `VK_EXT_external_memory_host` (zero-copy guest memory); (3) native Metal only if
   Instruments/Metal System Trace profiling shows MoltenVK overhead is the bottleneck.
3. The Linux code path (`build.sh`, `#ifndef _WIN32` branches) is the starting point for
   macOS, not the Windows one — but borrow Windows' tricks where Linux uses Linux-only APIs.

## Machine & paths

- Host: Apple M4 Max, macOS 26.6, arm64. Rosetta installed. Xcode clang 21. arm64 Homebrew
  at `/opt/homebrew` (has cmake, ninja, nasm, glslang, pkgconf, boost, cryptopp).
- **Game (decrypted, 1.09 merged, verified):** `/Users/markhall/Bloodborne/CUSA03173`
  (`eboot.bin`, `sce_sys/`, `sce_module/`, `dvdroot_ps4/`). `scripts/game_check.py` → OK.
  Original archives/pkgs are in `/Users/markhall/Bloodborne/Bloodborne/` — leave untouched.
  Unpacked update (redundant, kept): `/Users/markhall/Bloodborne/update/`.
- **Never commit game files** (repo is public). `.gitignore` covers `/game/`, `/deps/`, `out/`.

## Done so far (2026-10-11)

- Repo created, plan doc `docs/MACOS_PORT.md`, README header.
- `tools/pkg_extract/` — native fpkg unpacker (shadPS4 PKG/PFS code from before upstream
  removed it in be22674f; 64-bit size fixes; parallel). Built at `out/pkg_extract/pkg_extract`.
- `scripts/macos/bootstrap_deps.sh` — builds pinned **x86-64** deps into `deps/x86_64`
  (SDL3, Vulkan headers+loader 1.4.363, fmt 11, xxHash, miniz, Zydis, FFmpeg 7.1, magic_enum,
  robin-map, VMA, xbyak; boost headers symlinked) + universal MoltenVK 1.4.2 dylib & ICD json.
  Already run successfully; re-running is a no-op (stamps in `deps/x86_64/.built-*`).
- Verified: x86-64 test binary under Rosetta sees "Apple M4 Max, Vulkan 1.3" via MoltenVK.
  MoltenVK lacks `geometryShader` and `logicOp`.
- **`build_macos.sh`** builds everything x86-64 (`--gpu-only`, `--test`): `out/gpu/libbbgpu.dylib`
  (`gpu/CMakeLists.txt` APPLE branch: no X11, `-undefined dynamic_lookup`, Metal surface via
  `SDL_Metal_CreateView`), `out/bb-probe`, `out/bb-gpu-capabilities`. All 5 test suites pass
  under Rosetta; bb-gpu-capabilities finds the M4 Max. PGO is off on macOS (GCC .gcda profiles).
- **Runtime port compiled and unit-tested** (not yet booted with the game):
  - Guest TLS: loader rewrites `fs:[0]` → `gs:[0xc00]` (`--gs-slot`/`BBPORT_GS_SLOT`, default
    384 on darwin); `runtime_thread.c` constructor reserves pthread key **384** (first free key is
    258 under Rosetta) and `set_tls_base` stores the TCB there and verifies the read-back.
  - Memory (`runtime_memory.c`): **[0xfc0000000, 0x7000000000) is a permanent kernel hole** on
    macOS, so guest `USER_MIN` = 0x7000000000 there; whole guest range reserved PROT_NONE up front
    (probe.c `main`), backing = one anonymous region aliased with `mach_vm_remap`, hole-punch =
    fixed anonymous remap. Low maps in [0x800000000, 0xfc0000000). Page size under Rosetta = 4K.
  - Write faults arrive as SIGBUS on macOS (handled). Thread dumps via `task_threads`.
  - No missing `runtime_low_alloc` routing found: guest-visible objects already use it.

## Boot status (2026-10-11)

- `--cpu-only`: guest code runs (TLS, memory, ~30 threads, FMOD audio via CoreAudio) up to
  graphics init, then faults on NULL objects from unbound `sceGnm*` imports (expected on any
  platform: cpu-only skips `bbgpu_init`, which registers the GPU HLE symbols).
- GPU boot (`run.sh`, no flag): window + Vulkan instance OK; stops in `Instance::CreateDevice`
  (`vk_instance.cpp` ~272): MoltenVK lacks **`robustBufferAccess2`** and **`nullDescriptor`**
  (has `robustBufferAccess`, `robustImageAccess2`).
- 2026-10-11 later: device created (Option A: no robustBufferAccess2/nullDescriptor, zero
  stand-in buffer); buffer cache uses one ordinary 16 GiB arena at 0x7000000000 on macOS (no
  sparse binding in MoltenVK; Metal commits memory lazily, measured). Fault handler prints
  rax/rbx/rdi/rsi/rsp; `BB_TRACE=1` adds import logs and thread snapshots.
- **The "job-pool crash" (SIGBUS at guest 0x53b3260, loop 0x263b246) is NOT a game crash:** it
  only happens after `STOP: watchdog timeout` — the `BB_TIMEOUT` stack dump derails a worker.
  Normal play (no BB_TIMEOUT) never hits it. Backlog (Stability). Don't chase it as a game bug.
- **Milestone (2026-10-11):** with BC images created without Storage usage on macOS (Metal can't
  write compressed textures; this also removed ~520 MoltenVK "command buffer not begun" errors)
  the user created a character, saved, and loaded into the first level — playable, a little
  sluggish. Open issues: dark ghosting halo around the character (grows when zooming, never
  shrinks: likely an uncleared/accumulating target — suspects: null image bindings without
  stand-ins, non-zeroed OOB reads, compute clears). Ghosting seen only in character
  creation, not in the level. Shader compilation stutter at start, then smooth-ish.
- **2026-10-11 cleanup:** Windows support purged from our code (files, `_WIN32` branches via
  unifdef, CMake WIN32 blocks, CMake bbport target); vendored `gpu/shadps4/` untouched. Docs,
  launcher and overlay are English-only; README credits upstream. Overlay opens with ⌘, on macOS
  (not F1: F1 is the game's Options key) and shows FPS + render → output resolution + upscaler.
- A stale shader cache from a crashed run fails `serdes.h:109`; delete it. Test runs without
  `<data>/user` existing put GPU caches in `~/Library/Application Support/shadPS4` (path_util
  reads `BB_GPU_USER_DIR` at static init, before bbgpu_init sets it) — `mkdir out/macdata/user`.
- macOS main thread: Cocoa needs the real main thread, so `probe.c` `main()` runs the game on a
  64 MiB-stack thread and runs `CFRunLoopRun()`; `bbgpu_init` posts the window loop to the main
  dispatch queue. (`dispatch_main()` does NOT work: SDL then reports "No available video device".)
- Run: `BB_DATA_DIR=$PWD/out/macdata BB_GAME_DIR=/Users/markhall/Bloodborne/CUSA03173
  BB_TIMEOUT=60 bash run.sh [--cpu-only]`; `BB_TRACE=1` logs unbound import calls.

## Performance status (2026-10-11, same scene, user's M4 Max)

Native 1080p 22→25 FPS; FSR 3.1 Quality (720p→1080p) 28→31; MetalFX Quality 27→30
(sharper, more distant shimmer). At 720p the GPU is no longer the limit: the draw-recording
CPU thread (`bb:DrawRec`, ~35–40 ms/frame: MoltenVK encoding in vkQueueSubmit ~47%, renderer
per-draw work ~21%, host-copy spin ~12%) caps it. `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0` gave
nothing measurable. On MoltenVK (0.4.0) MetalFX needed synchronous submits (commit order on
MoltenVK's queue); the native Metal renderer encodes MetalFX into its own frame command buffer,
so that no longer applies there.
Benchmark settings: `out/macdata/bench-{native,fsr3-quality,metalfx-quality}.ini` via BB_CONFIG.

**2026-10-13:** fragment/compute shaders no longer use SPIR-V `NoContraction` (MoltenVK made each
float op an unoptimizable `optnone` call): city respawn spot 39 → 60 FPS (vsync-capped), GPU
19.6 → 12.1 ms/frame. Shader dumps: `MVK_CONFIG_SHADER_DUMP_DIR` (see FINDINGS).
Target (user): locked 60 FPS at the highest resolution (M4 Max: 1440p native, 4K upscaled).
M4 Max now: 1080p native ~62 uncapped, 1440p 49, 4K MetalFX Quality 43; scenes above 1080p use
the live path (the startup patch breaks geometry there). Check screenshots when benchmarking.

## State as of 2026-10-15 (mac-v0.3.0 + night session)

- **Saves/settings** default to `~/Library/Application Support/Bloodborne` (run.sh; a `Saves`
  link in the repo). The user's real save lives there: **never run tests against it**; test runs
  use `BB_DATA_DIR=$PWD/out/macdata` (scratch scripts restore a snapshot there).
- **Load race fixed** (`src/runtime_hooks.c`): eboot `0xebb960` (main thread's GPU setup) and
  `0x2199af0` (GXRenderThread frame job) are hooked and run under one lock (`BB_CONTEXT_LOCK=0`
  off). `BB_CALL_COUNTS=1` (branch only) counts libc setjmp/longjmp/throw.
- **Judder:** MoltenVK prefilled command buffers (`MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2`,
  set in `bbgpu_init`) unless the upscaler at start is MetalFX: late game frames ~6x fewer.
- **Retina** drawable (`BB_HIGH_DPI=0` for points). **Bloodborne.app** via
  `scripts/macos/make_app.sh` (local; `Bloodborne.command` for a Terminal launch).
- **Release app** (2026-10-15): `scripts/macos/package_app.sh` → self-contained `dist/Bloodborne.app`
  + zip (universal dylibs, bundled standalone Python, ad-hoc signed; users clear quarantine);
  CI `.github/workflows/macos-release.yml` attaches it to `mac-v*` tag releases. run.sh now runs
  `scripts/game_check.py` (rejects modified/pre-patched eboots; `BB_SKIP_GAME_CHECK=1`).
- **Static recompilation** lives on branch `static-recompilation`
  (`docs/STATIC_RECOMPILATION_PLAN.md`): survey, test vectors, discovery, and a prototype where
  47k game functions lift to C and run natively on arm64, matching x86 bit for bit.

## State as of 2026-10-16 (mac-v0.4.0)

- **Launcher** (`launcher/App`, `launcher/Model`, Swift; `scripts/macos/build_launcher.sh --test`):
  the main executable of both apps; two-column window sized for a 13" Air. Writes `bbport.ini` and
  `launch.conf` (`BB_SHOW_LAUNCHER`, `BB_SAVE_DIR`); ⌥ at open shows it when hidden. Test runs:
  `open -n --env BB_DATA_DIR=$PWD/out/macdata --env BB_LAUNCH_CONF=$PWD/out/macdata/launch.conf Bloodborne.app`.
- **About + updates (2026-10-20):** app menu *About Bloodborne* (standard panel; Info.plist
  `CFBundleVersion` from `git describe`, `BBCommit`) and *Check for Updates…*; daily automatic
  check of GitHub releases when the launcher window shows (window off: started with the game, a
  found update shows after it instead of quitting), only after the player agreed (asked
  once at start, before the game too; `launcher/Model/Update.swift`, `launcher/App/Updater.swift`;
  launch.conf `BB_CHECK_UPDATES` absent = not asked, `BB_UPDATE_TEST_BUILDS`, `BB_UPDATE_SKIP`,
  `BB_UPDATE_CHECKED`). Test builds = GitHub pre-releases and `-test` tags; 0.5.2-test1 < 0.5.2 <
  0.5.2-5-g…. Release apps replace themselves in place; checkout apps only notify.
- **Session logs (2026-10-22):** the launcher pipes the game's output through
  `launcher/App/SessionLogWriter.swift` (`launcher/Model/SessionLog.swift`): per session
  `~/Library/Logs/Bloodborne/game-<start>.log` (ISO 8601 time + syslog severity + the raw line) and
  `.jsonl` (ECS-style fields for Loki/ELK), last 20 kept, `game.log`/`game.previous.log` are links;
  warnings/errors also to unified logging (subsystem `io.github.wooloomooloo2.bloodborne-mac-port`, category = source;
  zsh's builtin `log` shadows `/usr/bin/log`). Severity comes from the line (`Crash:` critical,
  `STOP:`/`ERROR:` error, GPU `<Warning>`...). The writer must hold the `Pipe` until the end:
  released, it closes the read end and a still-writing bbgpu-host dies of SIGPIPE.
  Limits: 64 MB per session (then the last 1,000 lines at the end), 256 MB for the folder. Write
  errors stop the files, never the reading. bb-probe and bbgpu-host ignore SIGPIPE (PR #40 review):
  the game must survive the launcher quitting.
  On a crash `launcher/App/CrashBundle.swift` collects the session's `.ips` reports
  (`launcher/Model/NativeCrash.swift`: exception + crashed thread into the report; `.ips` escapes
  slashes as `\/`, unescape before redacting) and zips report, logs, ini and conf into
  `Logs/Bloodborne/Reports`; the issue link carries the report (compact version if needed).
- **0.5.0 = the full native Metal renderer** (separate branch; `renderer=metal`).

## State as of 2026-10-21 (after 0.5.3, local master, not pushed)

- **0.5.4 candidates on master:** invariant positions by default (`BB_SPIRV_PRECISE=vertex`
  restores 0.5.3; +15% GPU-bound at 1440p; an old shader cache migrates once), three drawables
  in full screen (#25), archive writes spaced to 20x their duration. PR #20's remaining commits
  (mouse look, Cmd+W) tested on branch `test-pr20-rest` with a link fix for the unit tests.
- **Diagnostics:** `BB_SAMPLE_MS=1` (main-thread sampler for long frames, `BB_SAMPLE_DUMP`),
  `BB_ENCODER_STATS=1` (Metal encoders per frame by kind and why the previous one ended),
  `BB_PASS_STATS=1`, `BB_FRAME_STATS=1` spike lines (game/flip/present breakdown).
- Traversal hitches on an M4 Max are GPU headroom (frames-ahead waits), not compiles or the
  archive; the city's tessellated draws split 67 render passes a frame (FINDINGS 2026-10-21,
  MACOS_BACKLOG Performance: tessellation kernels out of the pass).

## State as of 2026-10-24 (mac-v0.5.5, full release, Latest)

- **0.5.5** = 0.5.4.3-0.5.4.5 + the red-flash shader fix (GCN legacy/clamped float ops, NaN/Inf
  tested on bits so Metal fast math can't fold them; shader cache meta version 11; FINDINGS
  2026-10-24). User had not yet re-checked the Cainhurst rooftop after part 2.
- Merged today: #23 (About + update check), #32 (audio diagnostics), #66 (Dock icon in memory).
  Copyright lines (`SPDX-FileCopyrightText: Copyright 2026 Mark Hall`) added to 275 files.
- **PR status:** #41 answered (updater declined; #66 replaced it). #40 changes requested (SIGPIPE
  in bb-probe/bbgpu-host, `write(contentsOf:)`, log size cap, rebase after #23). #20 asked to
  rebase (drop f0e27f0; conflict only `frame_pacing` in bbport_settings). #11 waits for a long
  co-op session. #31 needs a rebase after #23. #64 (evill33t, MetalFX frame generation, draft):
  asked to split 103a436/6e0a611; owner promised a gameplay run with the watchdog (not done yet).
  #39/#58/#59 measured (FINDINGS "PRs #39, #58, #59"), not yet answered on GitHub.
- **Online play = 0.6.0** (with other features): PR #9 merged with master on local branch
  `online-0.6` (worktree `../bb-online`, merge cb860e7). Security review findings (2 blockers:
  libbbnet not bundled by package_app.sh, GPL-3 notes missing; should-fix: https-only community
  server, no keychain/env secrets when offline, no secrets on argv, version in User-Agent, token
  only to the community host, UDP queue caps, shadNet `ExtractBlob` overflow, UPnP lease, crash
  report redaction, sign-in link checks) were being fixed on that branch by an agent when the
  session ended: check `git log online-0.6` and `git stash list` there. Ask Noel before pushing to
  his branch (maintainer edits allowed). THD must confirm `bbport-mac` matches Windows players.
- **Plans written:** `docs/DESIGN_FRAME_GENERATION.md` (macOS 26 is enough, not 27),
  `docs/DESIGN_ONLINE_COOP.md` (#9 first, Nightwatch seamless co-op later as a separate mode:
  ~105k lines, GPL-3 + Lantern section 7 terms, must run x86 in bb-probe, hooks don't collide
  with our loader by literal scan). Backlog: evaluate bloodborne-native/bloodborne-recomp as the
  post-Rosetta path after 0.6.0.
- **Open bugs:** #46/#50/#51 freeze in the very first cutscene (new game, all reporters on macOS
  27.0.1; nothing fixed yet; a new-game test on 26.6 was started 2026-10-24); #55 FPS sinking
  over long sessions (waiting for logs); #63 is the 0.5.4.1 GPU fault fixed in 0.5.4.2.
- Local branches: `zero-copy` (declined prototype, keep for reference), `online-0.6`,
  `test-pr39/58/59` + worktree `../bb-prtest` (benchmark only, can be deleted).
- **macOS 27.0.1 on this Mac (2026-10-24 evening):** 4K Dream/city −4% vs 26.6 (user was using
  the Mac; FINDINGS), no 4x drop. New-game opening movie plays through to character creation at
  60 FPS here (48 GB), so #46's freeze is not macOS 27 alone; suspect 16–18 GB memory pressure.

## State as of 2026-10-24 night (benchmark, triage)

- **Benchmark** (merged into master 2026-10-25, branch `benchmark`):
  `docs/DESIGN_BENCHMARK.md`. Launcher button + `--benchmark` (scripted); traversal = recorded
  route through Central Yharnam at fixed 60 (replay steps by host flips), throughput = 30 s
  uncapped stand; per-frame CSV from the GPU host, launcher sampler/report in
  `~/Library/Logs/Bloodborne/Benchmarks/`. Assets (not in git): `out/macdata/save-backups/
  bench-cleared-20261024/{savedata,route.pad}` (copy to `out/benchmark/`). Shipping the save in
  the release app is the owner's call (package_app.sh copies out/benchmark only if present).
  Shared block version 5 (#70 also bumps to 5: renumber whichever merges second).
- **Fixes** (merged into master 2026-10-25, branch `fixes-0.5.6`): F9 pad recording
  in split mode, arrow keys no longer press the d-pad (#61), bounded wait for a stuck GPU host
  (#71). Launcher "end the previous copy" prompt (#71) and preferred display (#60) in backlog.
- **Merged and pushed:** #40 (session logs; tested from the app). Closed as duplicates: memmove
  #37/#38/#54/#57/#62 → #34 (fix 3497dea in 0.5.4.4+), cutscene #24/#50/#51 → #46, #63 → #42.
- **Waiting on authors:** #20 (asked mouse_look default off), #31 (rebase), #58 (full key
  compare), #59 (rebase + QoS test), #64 (back to draft, FPS counter), #67 (keep plain test run,
  confirm deferred deletion). **Waiting on us:** gameplay/visual tests for #65, #68, #70, #64's
  promised run; #39 re-measured with the benchmark (ab-39 branch in ../bb-prtest).
- **Mod manager (0.6, owner-approved with cheats):** merged into master 2026-10-25 (branch
  `mod-manager`): launcher "Mods & Cheats…" window (mods,
  cheats/tweaks via bbport.ini `cheats=`, patch files), save backups before a new mod/cheat set,
  `docs/DESIGN_MOD_MANAGER.md`. Enemy Control not offered (overlaps the FPS patches). Online rule
  still open (with online-0.6). The benchmark disables mods/patch files/cheats; its passes log
  through SessionLogWriter (moved to `<pass>.log/.jsonl` next to the CSV). README has a "Coming in
  0.6" section and PR contributor credits.

## Release lines (2026-10-25, owner decision)

- **0.5.6 = everything on master as of 2026-10-25** (mod manager + cheats, benchmark, 16:10,
  #40/#23/#66/#32, fixes #61/#71/F9). 0.5.x is the stable line; **0.5.7** = fixes for 16 GB Macs.
- **0.6 = MetalFX frame generation (#64)**, pre-releases only until well tested. Online play (#9)
  is still in progress, not tied to 0.6. PR #77 (adaptive timing) is not wanted for now.

## State as of 2026-10-27 (0.5.9 Latest; master ce518e8, pushed)

- **0.5.9** is the full Latest release (CI publishes pre-releases titled with the bare tag:
  set title/notes and `--prerelease=false --latest` by hand, always `-R Wooloomooloo2/bloodborne_mac_port`).
- **Controls (merged, pushed; docs/DESIGN_INPUT.md):** `bind_<action>=` keys in bbport.ini
  (`src/runtime_bindings.c`, SDL scancode/mouse names, defaults = 0.5.9 layout + Left Alt walk),
  wheel through the shared block (version 7), `mouse_vertical`, sensitivity to 10, launcher
  Controls page (`launcher/App/ControlsView.swift`, `Model/Bindings.swift`, `Model/KeyCodes.swift`;
  tests/test_pad.c checks names/defaults against SDL and the C table). User looked at the page,
  not yet played with it. **Next:** mouse speed retune (calibrate turn rate per stick value with
  `BB_PAD_FILE rx=`, needs ~5 min on the Mac), then a ≤1-day direct-camera spike. The in-game
  hint names the game's "Camera speed" option: unverified that Bloodborne has it.
- **Preferred display (#60):** local branch `preferred-display` (8548d64, launcher Display picker,
  `BB_DISPLAY` UUID → SDL display), tested full screen on the built-in display; not merged/pushed.
- **PRs:** #64 (frame gen, evill33t): works at 120 Hz/ProMotion, real FPS drops to 40 at 144/160 Hz
  (PresentDuration on fixed-rate displays); launcher switch added (6a1149c); needs a temporal
  upscaler, so not in 16:10 (user wants MetalFX on 16:10: experiment proposed, not approved).
  #90 (MetalFX Spatial): user tested, temporal better in motion, Spatial ok as an option; asked to
  fix tests/test_mtl_overlay.mm (fixed click coordinates) and split out native motion. #87 merged.
  #78 online: told to test as an online fork (#9 closed).
- **Pending (not approved yet):** launcher/in-game hover hints from one text table; 8 GB Mac
  simulation (backlog); renderer PR benchmark batch (#39 #58 #59 #65 #67 #68); #64 pacing A/B.
- Launcher test runs: open with `--env BB_FRAME_STATS=1`, or the scratch watchdog (waits for
  "Frame stats" lines) never arms. Worktree `../bb-fg` (branches test-90/test-fg/test-master)
  is scratch. `gpu/third_party/fsr-vulkan` shows a submodule change that isn't ours: don't commit.

## Next steps (in order)

1. **Native arm64 GPU process** (docs/DESIGN_NATIVE_GPU_PROCESS.md): M0 spike passed (1.75× on
   MoltenVK-style CPU work). M1 done (guest pool = Mach named entry, `runtime_memory_pool_entry()`,
   `BB_SHARED_GUEST_MEMORY=0` fallback). M2 done (2026-10-12): `BB_DEPS_ARCH=arm64 bash scripts/macos/bootstrap_deps.sh`,
   `scripts/macos/build_gpu_arm64.sh` → `out/gpu-arm64/libbbgpu.dylib`, `out/bbgpu-host` (`host/`),
   `src/gpu_process.c` mirrors guest memory into the host; HLE split in
   `docs/HLE_CLASSIFICATION.md`. M3 implemented (2026-10-12, design "M3 plan"): portable SRT walker
   (bytecode), shared control block + rings (`host/bbgpu_shared.h`), host links the arm64 GPU
   library (`host/host_runtime.c`), game side in "remote" mode (`gpu/shim/bbport_split.*`).
   Tests pass (`tests/test_bbgpu_rings.c`, `tests/test_gpu_split.c`); the title screen renders
   through the host at 60 FPS. M4 (protect/fault relays) done: gameplay renders correctly. Split
   mode is the ONLY mode on macOS (single process and `BB_GPU_PROCESS` removed 2026-10-19: a host that
   cannot start or is lost stops the game with a "Crash:" line); text entry and Restart
   go through the host; keyboard bridged, gamepads polled in bb-probe. Same FPS as single
   process in the GPU-bound city scene (27.5), steadier worst frames. Next: render-pass merging.
2. Menu bugs: "Subpixel jitter" unticks itself; menu choices (upscaler/preset) not persisted
   as expected. MetalFX distant shimmer (jitter sign: `BB_METALFX_JITTER_SIGN=-1`).
3. Rendering: half-rendered character after reloading a save; ghosting in character creation;
   null image stand-ins (Option A).
4. Launcher / `.app` bundle.

## Gotchas learned

- **Never send signals to guest threads** (sampling, stack dumps): SIGPROF every 1 ms on the
  main thread made texture streaming fail within minutes. Suspend the thread and read its state
  instead (`src/runtime_sampler.c`).
- The test harness cannot enter full screen while nobody is at the Mac (no Space transition);
  full-screen checks need the user present.

- **Never attach lldb/debugserver to a Rosetta (x86-64) process and kill it**: on 2026-10-12 it
  wedged Rosetta (every new x86 process hung in state U until it recovered hours later).

- First launch after a rebuild is slow (~20 s to the window) and stutters: shader/pipeline caches
  rebuild. Time launches with warm caches.

- zsh: `"$VAR:src/..."` triggers zsh's `:s` history modifier — write `"${VAR}:src/..."`.
- Run Vulkan programs with `VK_DRIVER_FILES=$PWD/deps/x86_64/share/vulkan/icd.d/MoltenVK_icd.json`
  and request `VK_KHR_portability_enumeration` + `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`.
- Builds must not pick up arm64 libs from `/opt/homebrew` (bootstrap sets
  `CMAKE_IGNORE_PREFIX_PATH` and an isolated `PKG_CONFIG_LIBDIR`; do the same for bbport).
- `docs/` ROADMAP/upscaler/motion_vectors/MODS are translated upstream docs (Linux reference path).
- **Rosetta + aliased memory:** code written through one alias and executed from another keeps
  the stale translation until a protection change (RW→RX) on the executing alias.
- macOS timed mutex/rwlock waits are trylock+backoff (up to ~1 ms late); `CLOCK_MONOTONIC`
  keeps counting during system sleep.
- Several Windows-fork regressions also broke Linux builds (`runtime_file.c` `DWORD` log,
  `runtime_pad.c` missing `struct stat`, tests needing `restore_guest_fs`); fixed here.
