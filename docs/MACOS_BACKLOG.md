# macOS Port Backlog (refinements)

Work deliberately deferred while getting the game running. When a shortcut is taken, the
proper solution goes here under one of three categories, in priority order:

1. **Stability** — correctness and robustness gaps left by shortcuts.
2. **Performance** — speed and efficiency work.
3. **Mac GUI** — making it look and feel like a polished Mac app.

Each item: what was done for now, what the refinement is, and where it lives.

**Order agreed with the user (2026-10-15).** Performance first: stay ahead of the other Mac ports
(bmy/bbport-mac reports 45–60 FPS at 1440p FSR on an M5 Max).
1. Measure (CPU and Metal traces at 1440p and in a busy scene) to size the native Metal back end.
2. Design the native Metal back end (plan doc, branch, switchable against MoltenVK).
3. Build it in stages, the costliest path first, with MoltenVK kept as the fallback.
4. Meanwhile: the M2 Max 2–3 FPS report (issue #2), an earlier shader compile start, a native
   Mac launcher built for more than one game.
5. Later: merge upstream bbport 0.5/0.6 (native game settings, online play, mouse look, aspect
   ratios).

## 1. Stability

- **Life after Rosetta: evaluate bloodborne-recomp (2026-10-24, after 0.6.0).** The game side
  (bb-probe and the HLE runtime in `src/`) runs under Rosetta 2, which Apple supports fully only
  through macOS 27. github.com/bloodborne-native/bloodborne-recomp (GPL-2.0-or-later) recompiles
  all 241,031 eligible game functions to C and runs the whole game natively on arm64 (with bbcpu,
  an interpreter and translator, as the fallback), but uses 20-30% more CPU than their translator
  and has not been compared with Rosetta. Our `static-recompilation` branch stopped at 47,125
  lifted functions. *Next:* a spike, not adoption: can our loader call their `librecomp.dylib`;
  what building `src/` for arm64 takes; recompiled vs Rosetta CPU time on the M4 Max.
  Collaborating beats duplicating.
- **Signals into guest threads break the game (2026-10-21).** SIGPROF sent to the main thread
  every 1 ms (a sampler's first version) made texture streaming fail every time within minutes
  (`CSGXTextureCreateFragment.cpp(137)` panic, then the game's deliberate null write), even with
  a handler that made no system call. The watchdog's stack dump (`probe.c` `dump_frames`,
  SIGUSR2 to every thread) is the same kind of interruption, which fits CLAUDE.md's "the
  BB_TIMEOUT stack dump derails a worker". Cause not found (an interrupted wait or syscall the
  runtime passes on as an error is the suspect). *Fix:* dump stacks by suspending threads and
  reading their state (as `runtime_sampler.c` now does), not with signals.
- **`mtl-encoder-test` fails on an M2 (#30, 2026-10-20).** Its timestamp check "the render
  encoder's fragment end is sampled, after the blit" assumes the M4's sampling points; on an M2
  Max it fails and stops `build_macos.sh --test`. Make the check depend on the counter sampling
  points the device reports, or skip it where they differ.

- **Metal-native: world geometry missing after long play (Hemwick graveyard, user's 0.4.5 test,
  2026-10-17).** After a long session a large rock wall and the terrain behind it drew as a flat
  fog-coloured hole; quit to menu and load restored them. Suspects: a missed CPU write to guest
  memory the game recycles while streaming (the GPU keeps stale vertex/index data), or GPU
  visibility culling reading stale results (`BB_READBACKS` Relaxed). Unknown whether 0.4.0 does it.
  Repro: long scripted walk from `save-backups/dense-20261017` with before/after snapshots.

- **Metal-native: "Submit inside a render pass" assertion ended a co-op session (2026-10-17,
  online-shadnet; made survivable).** The first phantom to load in on The Hunter's Dream pool
  hit `Encoder::Finish`'s `ASSERT_MSG(!in_pass)` (bbgpu-host exited with status 23). The
  scheduler ends its pass before every submission (`SubmitExecution` → `EndRendering`), so the
  encoder's `in_pass` and the scheduler's `is_rendering` had come apart; the two direct
  `BeginRenderPass` users in vk_runtime.cpp (layer resolves, clears) are balanced, so the path is
  not found yet (no backtrace from the host process). Now `Finish` closes the render encoder,
  commits, and keeps the pass: the next draw reopens it with load actions, as after compute or
  blit work inside a pass; a `Submit inside a render pass` warning (first 8) marks it in the log.
  *2026-10-18 (PR #11 review):* most likely a symptom of the recording-chunk race below:
  `Finish` is reached only from `SubmitExecution`, after `EndRendering` and `SyncRecording`,
  so `in_pass` can be true there only if another thread recorded a `BeginRenderPass` at the
  same time, and the relayed fault's download calls `scheduler.Finish()` from stage A, the
  thread that raced. The warning now carries the reporting thread's stack (which thread
  submitted); since the pass stays open until the scheduler's next pass end, a still-open pass
  at `Encoder::BeginRenderPass` (the stop would have moved there: `blit_helper.cpp`'s fullscreen
  draw, the layer resolves and clears in vk_runtime.cpp) ends first with the same warning, and
  an `EndRenderPass` without a pass warns and returns. The workaround stays as a safety net; a
  raced chunk may also carry reordered records, so a log with any of these warnings is a bug
  report, not a clean run. *Open:* only if the warning shows up again with the race fixed.
- **Metal-native (M4 → M5): render pipelines declare a depth/stencil format the pass does not
  bind.** Fixed 2026-10-16 (M5 Task 3): `BoundDepthAttachment` (vk_depth_attachment.h) is the one
  rule for the pass's depth attachment and the pipeline key's formats; `MTL_DEBUG_LAYER=1` title +
  city run 96,721 messages → 0. Pipeline key version 8: one cache rebuild.
- ~~**Metal-native (M4 → M5): the figure in the cage at the Central Yharnam lamp is missing.**~~
  Fixed 2026-10-18: the user sees the figure back at the lamp on the 0.4.5 Metal app.
  0.4.0 draws a creature behind the bars of the cage left of the lamp post (`cages-20261016`
  save, +40/+50 s after Cross); the Metal-native renderer shows the cage empty in both captures
  (scratchpad `t4-m-cages-*.png` vs `t4-w-cages-*.png`, 2026-10-16). Nothing is logged (no skipped
  draw, failed pipeline or error). Still missing with the depth-format fix above (2026-10-16), so
  not that. Start with a frame capture of that view on both.
  2026-10-18: three G-buffer pipelines were skipped in every scene (fragment inputs the
  full-screen vertex shader does not write; they are logged at startup, so "nothing is logged"
  above was wrong); fixed (inputs read as zero), which adds atmospheric light behind those bars.
  Whether the figure is back needs a 0.4.0 window capture with the screen unlocked.
- ~~**Metal-native (M4): full-screen switch not checked while presenting.**~~ Checked 2026-10-18
  by the user on the 0.4.5 Metal app (ProMotion display): full screen works and the picture looks
  right. Window resize refits with black bars and no stall (2026-10-16).
- **Metal-native (M4): minimised/occluded window.** Fixed 2026-10-16: a minimised, hidden or
  fully occluded macOS window keeps its pixel size, so the 0x0 skip never fired and
  `nextDrawable` could block ~1 s per frame. `WindowSDL` now tracks SDL's MINIMIZED/HIDDEN/
  OCCLUDED flags and `Present` frees the frame without a drawable (`ShouldAcquireDrawable`;
  a `BB_SNAPSHOT_TRIGGER` request still acquires). Measured: 20 s minimised in the city, Frame
  stats at 60 FPS throughout, 60 FPS after restore. *Left:* one ~1 s hitch as the window
  minimises (the present already inside `nextDrawable` when the Dock animation starts; SDL's
  MINIMIZED arrives after it). Fix if it matters: a `nextDrawable` timeout shorter than 1 s
  (`allowsNextDrawableTimeout` is all or nothing; acquire on a helper with a deadline).
- **Metal-native (M4): `PrepareBlankFrame(true)` blocks on the swap thread.** It does a blocking
  `FramePool::Take`. If the draw thread queued both pool frames as flips while the port is
  closing, the game frame waits in the swap queue behind the blocked blank frame: a deadlock
  (pre-existing shape). Fix: a non-blocking try-take for blank frames (skip the blank when no
  frame is free).
- **Metal-native (M4): `PipelineCache::Sync` does not drain the build pool.** Pipelines still
  building when it flushes are not in that write. Only `~Presenter` (single process) and the
  M4 exit flush (`bbgpu_flush_caches`, bounded 5 s) write at the end; a build finishing after
  them is lost until the next session compiles it again.
- **Metal-native (M4): `~Presenter` vs the swap/present threads (single-process exit).**
  `presenter` (gnmdriver.cpp) and `driver` (video_out.cpp, owns the swap and present threads)
  are destroyed in unspecified order by guest `exit()`; `present_queue.reset()` before the
  threads stop lets a concurrent `Present` use a null queue. Goes away with single-process mode
  (M7); else stop the driver threads first or null-check in `Present`.
- **Metal-native (M4): present drawable path covered only in game.** `TestAutoreleaseLoop`
  commits without a drawable; `presentDrawable` → drawable return is checked only by game runs
  (tens of thousands of frames). Optional: a test with a layer on an offscreen `NSWindow`.
- **Metal-native (M4): presenter parts off until M5/M6.** HDR (`SetHDR` is a no-op, the layer is
  BGRA8 sRGB; was a MoltenVK HDR10 swapchain), display timing (`IsDisplayTimingSupported` false,
  `TakePastTimings` empty: the spike log's present-to-display column reads 0), the settings
  overlay (not drawn; M5 ImGui on Metal) and FSR 1 (`BB_FSR1` logs a warning and presents
  without it; M6). FSR 1 and the overlay are done (M5/M6); HDR and display timing remain.
- **Metal-native (M6): FSR 3.1 back end minors** (review findings, none seen in game):
  `ffx_fsr3_host` needs the patched `fsr-vulkan` submodule (a fresh clone without the patch fails
  to build: add a CMake check, or vendor the files in M7); the FFX pipeline cache is keyed by
  `Device*` and never pruned; read paths (`GetResource`, `StorageView`) after the last context is
  destroyed allocate a new backend state that leaks; `ExecuteGpuJobs` returns only the last error;
  a failed dispatch is reported one frame late (asynchronous recording); samplers are matched by
  order (fine for FSR 3 only). Test gaps: negative inputs (null blob, bad context id), destroy
  with a dispatch in flight, a `MTL_SHADER_VALIDATION=1` run of `ffx-metal-test` in
  `build_macos.sh`, FSR 1 HDR and resize.

- **1440p (live resolution path): the item icon in the HUD is missing. Fixed 2026-10-18 (#17,
  Noel Miller; 0.5.1).** Upscaler off only (FSR/MetalFX drew it). Scaleform's stencil mask for the
  item slot is a depth-only draw; the rasterizer reported only one-color-target draws to the
  upscaler, so the mask went into the guest 1080p depth while the icon was redirected to the host
  target with a cleared stencil. Depth-only movie-shader draws in the UI phase now bind the host
  UI depth; `done_this_frame` at 1080p still needs a color draw (guard added at merge).
  *Check:* #18's screenshots (0.5.1, 1280x720 output, FSR 3.1 Native AA) show the item slot
  empty at 720p *with* FSR on: below 1080p the UI is also scaled; confirm whether #17 covers it.

- **PARKED (2026-10-13): the game window sometimes closes by itself in split mode** (grey screen,
  then the host exits with status 0 and no error). Seen 3 times: after turning subpixel jitter on
  in the menu (user), after the same via `BB_TOGGLE_FILE` (bit 25), and during a profiling run with
  an Instruments attach; NOT reproduced in 3 later runs (toggle, screenshot, plain play), so
  intermittent. Instrumented: the window loop now prints which SDL event closed it ("Window: …"),
  the host prints when its run loop returns, and SDL no longer turns SIGINT/SIGTERM into a quit
  event in the host (a signal now shows as a signal). Next occurrence: read those lines.
- **Shader/pipeline compile stutter.** *Fixed 2026-10-13 (main cause):* the pipeline cache was
  wiped at every launch (one entry that failed to preload cleared everything), so ~500 pipelines
  compiled again each session, many mid-game (attacks, effects). Now only old-format entries clear
  it; the next session preloaded 226 of 227 and compiled 2 during play (was 517). Both processes
  use `<data>/user` (`run.sh` exports `BB_GPU_USER_DIR`; the host used to fall back to
  `~/Library/Application Support/shadPS4`). *Remaining:* a pipeline never seen before still
  compiles on first sight and blocks the frame — compile in the background with a fallback.

- **"Unimplemented PM4 type 0" seen again in split mode (2026-10-13).** Once in ~8 runs, while
  loading into the level (`liverpool.cpp:820`, base reg 530), despite the command-buffer copy
  arena. Find which buffer is still read from guest memory after the submit (CCB? indirect
  buffers / `IndirectBuffer` packets pointing into game memory are not copied).
  *2026-10-13 evening, 2 more (of ~10 runs, right after "Continue" on the title):* the copied DCB
  equals the game's buffer at the fault (0 words differ), so the game submitted those bytes; not
  a late rewrite. Both times stray words sit in a compute-shader setup sequence (`SET_SH_REG`
  0x212 / 0x216–0x217 nearby); in one, `70d1b005 00000000` is a COMPUTE_PGM_LO/HI value
  (program at 0x70d1b00500) with its packet header missing. Suspect a packet writer that emits a
  different size than the game reserves (our HLE `sceGnmSetCsShader*`/dispatch functions, or a
  patch). The diagnostic's guest-buffer comparison crashed the host before (a declaration inside
  `namespace AmdGpu` resolved to a null call through `-undefined dynamic_lookup`): fixed.
  *2026-10-14, hunted:* not reproduced in **62 cold-cache save loads** since (42 with a ring of
  every `libSceGnmDriver` call, 20 without), so the recorder's timing does not hide it. Left in
  place, cheap: each DCB/CCB is walked at submit in the game process ("Gnm check: bad packet …",
  with the library calls that wrote near the bad word; `BB_GNM_TRACE=0` turns the call ring
  off), and the host records each arena copy's sequence number and hash on arrival, so the next
  "Bad PM4 packet" says whether its copy changed after it arrived (arena reused mid-parse) or
  was already bad at submit. Next occurrence: read those two lines first.
  *2026-10-14, caught at submit (load into the level from the title):* the game submitted a
  506-word DCB with only 58 words written; the rest were zeros (still zeros 50 ms later, so no
  writer was racing the submit). The last packets were a correct 25-word
  `sceGnmSetCsShader` (from another thread than the submit); the arena copy was unchanged.
  **Now:** the CP skips type-0 packets as register writes, as the hardware does (a zero tail is
  one write to register 0 per two words), and logs once per buffer ("PM4 type-0 packet …
  skipped"); tested with an injected 448-word zero tail. *Remaining:* why the game leaves the
  tail unwritten (the "Gnm check" now also prints where the writing thread's next library calls
  went: another buffer = a chunk switch the size does not reflect).
  *2026-10-14 later, 2 of 3 user loads with a warm cache* (the 62-load hunt used a cold one): a
  second form, a type-1 word (`704fcf00 704f3f00 704fcf00 000478ff 00008fff`, no header) after
  four `SET_CONTEXT_REG`s, 25 words after a `sceGnmSetCsShader` whose thread wrote its next
  `sceGnmSetCsShader` 65 words later in the same buffer. Both forms appear right after the
  game starts its `CSClothThread0..5` (cloth compute, built on other threads than the submit).
  The hardware would hang on type 1, so the game cannot do this on a PS4: suspect our HLE lets
  two game threads write one command buffer (a lock or semaphore that is not exclusive in some
  case: timed waits are trylock+backoff on macOS). **Now:** an invalid packet type skips the
  rest of that command buffer (logged) instead of stopping the game. *Next:* reproduce with a
  warm cache (`loadloop.sh` without the scratch cache) and trace the cloth threads' locks.
  *Third load, same session (skipped, the game played on):* the clearest record yet. The
  render thread wrote `sceGnmSetPsShader` (40 words) at word 153 of a DCB; the cloth thread then
  wrote `sceGnmSetCsShader` (25 words) at word **159**, inside the shader's words (its header
  replaced the PS RSRC values, leaving `70d1b005` = COMPUTE_PGM_LO as a header), and a second
  one at word 337 of the same buffer. Two threads were handed overlapping command-buffer space:
  look at how the game hands out command-buffer chunks to the cloth jobs (its "reserve failed"
  callback / chunk allocator) and which of our primitives guard it.
  **Root cause found (2026-10-14 evening; FINDINGS "Load crash: root cause"): a race in the
  game that our slower main thread exposes, not a broken primitive.** While a level loads, the
  main thread's cloth/texture setup (eboot `0xebb960`, called from `0x2276501`) records three
  compute dispatches straight into the engine's global immediate GX context (`*0x55406c8` →
  `+0x10` → `+8`, the context `GXRenderThread` records into; owner field still
  `GXRenderThread`), with no lock. `GXRenderThread` keeps drawing the loading screen at vblank
  pace into the same context. The game counts on the main thread being done before the next
  vblank frame starts: natively that work is short; under Rosetta it takes ~7 ms, and CPU load
  stretches it, so the render thread's next frame starts in the middle of it and both append
  through one `CommandBuffer`. Corrupted packets in 6 of 9 evening walk runs (no extra load);
  other overlaps leave valid packets. **Option A (now):** every corrupted-stream path
  seen so far is survivable: invalid PM4 types and unknown type-3 opcodes skip the rest of the
  buffer, an EOP with a bad data/interrupt select is skipped, an early flip IRQ flips anyway,
  a copy shader with inconsistent strides is skipped. The game played on in each test since.
  Diagnostics: "Gnm: context X used by thread A, then B" names the overlap with the guest call
  chain. **Option B:** make the two exclusive: hook the call at `0x2276501` (and the other
  direct immediate-context users, if any) so the main thread waits until `GXRenderThread` is
  between frames, and `GXRenderThread` waits for it before starting the next frame (a guest
  call-site patch to a trampoline in low memory, with a timeout against deadlock). Still open:
  the SIGSEGV in `Shader::StageSpecialization` on corrupted shader state (not seen since).
  **Option B done (2026-10-14 night, `src/runtime_hooks.c`):** `0xebb960` and the render
  thread's frame job `0x2199af0` are hooked at their entries and run under one lock (100 ms
  timeout, then the function runs anyway and says so; `BB_CONTEXT_LOCK=0` disables). 10 walks
  with 16 busy processes (9 reached the level): 0 corrupted buffers, the main thread waited
  0.3–0.7 ms for the render thread's frame in 5 of 9 loads (each a prevented overlap), no
  timeouts. Option A stays as a safety net. *Watch:* missing geometry after loads (entry below)
  should stop if it came from this race.
  **2026-10-17:** one overlap still got through the lock on a death reload (log: "Gnm: context
  ... used by thread A, then B after 44.6 us", a type-0 packet at word 283 holding a guest
  pointer) and landed on the one corrupted-stream path that was still fatal: the command
  processor's "NOP hint is missing in CB/DB setup sequence" assertions (`liverpool.cpp`,
  `SetContextReg` for `CbColorNBase`, `CbColorNCmask`, `DbZInfo`), which took the GPU host down
  (status 23). They now log a warning and leave the extent unknown (`raw = 0`), like the other
  survivable paths. Why the lock did not cover this overlap is still open: the main thread was
  in `0xebb960` (hooked) while the render thread's frame job (hooked) had used the context 44.6
  us earlier, so either a third direct user of the immediate context exists or the frame job's
  recording outlives the hooked call.
  **Closed 2026-10-19 (2cb8681, PR #20): a third direct user,** the render thread's draw-list
  task `0x219d220` (black loading screen entry below).
- **Fixed 2026-10-14: `MapNumberConversion` UNREACHABLE (data_fmt 0, SnormNz) killed the GPU
  host during a level load** (1 in ~60 loads). A descriptor with that format reads as zeros on
  the GPU; it now logs once ("SnormNz descriptor with data format …") and converts nothing.
  *Remaining:* find which descriptor carries it (stale table read, or a legitimately unused
  slot).
- **Gameplay judder with no compiles (2026-10-13).** The player sees occasional judder at a
  steady 60/61 FPS. Automated runs standing in the city show a worst frame of 20–25 ms in every
  5 s window (median 16.67, p99 17–22 ms) with zero shader compiles; the title screen has none.
  Find what those frames wait on (present timing, a periodic CPU job, host-copy waits).
  *2026-10-14, frame spike log* (`BB_FRAME_STATS=1`, "Spike #…" lines, FINDINGS): in 2 min of
  walking (user settings, 1080p MetalFX Native AA) **9 frames reached the display a vsync late**
  (33.3 ms) and **30 game frames took 21–46 ms** while the display stayed at 16.7 ms. With the
  delta-time patch a long game frame moves the world further in one step, so motion judders
  at a steady 60. Two causes seen: GPU hiccups (one GPU frame ~50 ms: frames-ahead wait 34–36 ms,
  then the game waits too) and runs of 4–5 frames where the recording thread is busy 20–25 ms
  (normal ~15) with the same draw count. *Next:* what the GPU does in the 50 ms frames (Metal
  System Trace at the spike), and why recording slows (MoltenVK encoding, CPU contention).
  *2026-10-14, later:* game-side GPU page faults ruled out (spike lines now show them: ~1–3 per
  frame, ~0.1 ms). In the Hunter's Dream gameplay has 0–3 long game frames per 5 s; most
  "game" spikes in a run are title/loading screens at 30 FPS. Re-measure in Central Yharnam.
  *2026-10-15: largely fixed.* Late game frames were the game waiting ~13 ms behind MoltenVK's
  encoding inside the flip's `vkQueueSubmit` (all game threads use ~12 ms of CPU in a 30 ms
  frame). Prefilled Metal command buffers (default unless MetalFX) cut late game frames ~6x
  (4–5 per 5 s → 0.6–0.8). *Remaining:* MetalFX keeps encode-at-submit (its command buffer
  queues behind a prefilled one); encode MetalFX into the frame's own Metal command buffer, or
  order it with an event, to give MetalFX the same gain. Re-measure in Central Yharnam.
  *Tried 2026-10-15:* encoding MetalFX right after the submit, before the next command buffer
  begins: no change (~38 late frames per 5 s with prefill). The spikes alternate: one frame
  spends ~22 ms in "frame prep" (the upscaler step on the GPU thread), the next is a late game
  frame; the presenter's submit takes exactly one vsync (16.7 ms). *Profiled (sample of
  bbgpu-host, MetalFX + prefill):* `bb:DrawRec` spends ~58% of its time in
  `Presenter::PrepareFrame` → `GetRenderFrame` → `vkWaitForFences` (no free presenter frame),
  because `bb:Present` sits in `CAMetalLayer nextDrawable`: with prefill, MoltenVK takes the
  drawable when the swapchain image is first used in recording (`vkCmdClearColorImage` in
  `Presenter::Present`), and both drawables are in flight. Three drawables: 10.9 late frames
  per 5 s (vs 3.9–4.8 without prefill) and +16 ms latency, so no. *Next idea:* with MetalFX,
  record the presenter's swapchain work in a command buffer that is not prefilled (MoltenVK
  decides per command buffer? check `VkCommandBufferUsageFlags`/one-time-submit handling), or
  make the presenter frame pool deeper than the drawable count. *Tried:* presenter command
  buffers begun without ONE_TIME_SUBMIT: 46 late frames per 5 s, no help.
- **Fixed 2026-10-14: full screen froze the window** (⌃⌘F / green button: the picture
  stopped, the window stayed off screen at its old size while the game kept running). The
  window's event loop ran forever inside one main-queue block, starving AppKit's own main-queue
  work for the transition. Events are now polled from a 2 ms run loop timer (common modes), and
  the host and bb-probe restart the main run loop when SDL stops it (it does on entering full
  screen; before, the process then exited). *Fixed 2026-10-14 night:* the window is created
  with high pixel density, so the swapchain has the display's pixels (full screen 2992x1876
  instead of 1496x938 stretched; `BB_HIGH_DPI=0` for the old behaviour; "Window: WxH pixels"
  in the log).
- **Display latency (2026-10-14).** *Done:* two drawables on macOS (`BB_SWAPCHAIN_IMAGES`
  overrides): `vkQueuePresent` to the screen 48.5 -> 31.7 ms mean, same FPS and late frames
  (FINDINGS "Display latency"). *Remaining:* it still jumps to ~48 ms when the frame's GPU work
  ends after the vsync (GPU ~13–14.5 ms of 16.7 in the Dream); less GPU time per frame is the
  lever. Display-clock flip pacing was tried and made late frames worse (dropped). Check on
  slower Macs that are GPU-bound below 60 that two drawables do not cost FPS.
- **Fixed 2026-10-14: frame timeline counters on M1–M3** printed `CpuCycles()` deltas as ns
  (41.7x too small on 24 MHz counters); now converted with `bbport::CyclesToNs` (CNTFRQ_EL0 on
  arm64, calibrated once under Rosetta).
- **Fixed 2026-10-13: pipelines Metal rejected after a cached session** ("cannot reserve
  'buffer' resource location"): shader permutation indices reused across sessions. *Remaining:*
  one pixel shader (`fs_fbe7c4b6`) has two cached permutations that compare equal ("conflicts
  with index 1"), so one pipeline compiles each session.
- **Startup resolution patch above 1080p breaks geometry** (2560x1440). *Now:* `run.sh` uses the
  live path above 1080p. *Proper fix:* find which game buffer the patch outgrows (the live path
  keeps guest targets at 1080p and costs proxy copies).
- **Shader-side buffer bounds checks (MoltenVK has no `robustBufferAccess2`).**
  *Now:* on macOS the device is created without `robustBufferAccess2`/`nullDescriptor`
  (`vk_instance.cpp`), and null buffer bindings use a small zero-filled dummy buffer
  (`vk_rasterizer.cpp`). Out-of-bounds buffer reads are then only guaranteed not to crash, not
  to return 0 as on the PS4 GPU (reads past `num_records` return 0) — possible visual glitches.
  Shader writes through a null binding also land in the stand-in buffer instead of being
  discarded.
  *Refinement:* emit bounds checks in the SPIR-V backend (`shader_recompiler/backend/spirv`) for
  buffer loads/stores/atomics on macOS — targeted first (vertex fetch, storage buffers) where
  glitches trace back to OOB reads, everywhere if a native Metal backend is built.
- **Buffer-cache arena window.** *Now:* one lazily committed arena at 0x7000000000 sized to
  direct memory plus 1 GiB (6 GiB retail, 10 GiB with `BB_DMEM_MB=9152`; the game's highest
  mapping ends at 4.80 GiB in every log so far); a GPU buffer outside it stops with "outside the
  macOS guest arena". 16 GB Macs (issue #19) still keep the whole 6 GiB in Metal's working set:
  if they stay slow, a smaller arena needs these. *Refinement:* per-page arenas with copy-based migration (rewriting the
  BDA page table), or zero-copy import (Performance) which removes arenas altogether.
- **Host pointers above 1 TiB reach the guest.** `__error` returns the host thread's errno
  address (0x7fc6…), above the game's 40-bit pointer range. Harmless if used immediately; if
  the game ever stores it packed, route errno through a per-thread low (<1 TiB) slot.
- **Watchdog timeout on macOS ends in a guest fault.** When `BB_TIMEOUT` expires, the SIGUSR2
  stack dump knocks a game worker thread into a SIGBUS (guest 0x53b3260, job-pool loop at
  0x263b246) instead of a clean exit. Only test runs with a timeout are affected; normal play
  has none. *Refinement:* find why the dump signal derails the interrupted guest thread
  (signal delivery under Rosetta with the guest stack/TLS) and exit cleanly.
- **Visual glitches on the Metal renderer at 3456x1944 with MetalFX (user, 2026-10-16 morning).**
  The user called it "very unstable" (visual glitches, kind not recorded). Its logs are clean (no
  errors, no rejected pipelines, closed by the user); 75-s scripted walks in Central Yharnam on
  Metal 3456, Metal 1440p and MoltenVK 3456 all looked correct. *Catch during testing:* keep
  `BB_FRAME_STATS=1` on Metal test sessions and ask for a screenshot when it happens.
- **Dog cage drawn as a see-through ghost (user, 2026-10-16, Central Yharnam).** Not the
  geometry-shader skip first suspected: at the nearest lamp the cages render fully on MoltenVK
  (camera sweep, `BB_FRAME_STATS`, 0 "Geometry shader stage unsupported" lines), so they are
  ordinary draws. In the user's screenshot (camera squeezed against the character) the cage is
  drawn, but dissolved: the game's screen-door fade, which MetalFX's temporal upscaling smooths
  into translucency. Open: whether the fade ever gets stuck (needs a repro with the camera
  pulled back), and which draws the 3457 geometry-shader skips that session were (MoltenVK has
  no geometry stage; the Metal renderer, 0.5.0, emulates it).
- **Character half-rendered after reloading a save (user report, 2026-10-11).** Repro: quit to the
  main menu, reload the save point; the character loads with the top half drawn and the bottom
  half missing. Suspects: pipelines skipped on Metal (`Graphics pipeline … rejected`, geometry
  stages skipped by MoltenVK) or stale buffer-cache/arena contents for re-used guest memory
  after the reload (macOS arena has no sparse unbind). Also: the ghosting halo in character
  creation (Mac GUI/visual list above).
  *2026-10-14, more reports:* legs missing after a spawn (user, M4 Max); legs missing after
  dying and respawning (another user, M2 Pro, reproducible for them); a flight of stairs
  missing in the user's session, present after a relaunch. Predates the `balanced` shader
  mode, so not draws skipped while compiling. *Next:* scripted die/respawn or reload cycles
  with screenshots; at a hit, compare the host's copy of the missing draw's vertex/index data
  with the game's memory (stale mirror or buffer cache vs. a rejected pipeline).
  *2026-10-14, later:* the user saw legs missing again (screenshot: torso, arms and straps drawn,
  legs and lower body absent). Candidate cause found and fixed: a game file read into memory
  the GPU tracks can race the protect relay (bb:GpuProtect re-protects a page between
  `touch_for_write` and `read()`); macOS then fails the whole read with EFAULT (verified: −1,
  file offset still advanced), which the game would treat as a failed asset load. Such reads
  are now redone through a bounce buffer and logged ("redone via bounce buffer"). Not yet
  confirmed as the cause: 6 warm-cache reloads at the Hunter's Dream lamp showed legs every
  time and no bounced read. *Next:* if legs go missing again, check the log for that line
  (present = this race; absent = look at the draw side as above).
  *2026-10-14, night (user session, mac-v0.3.0):* **floor missing** in a Yharnam (m24/m27)
  scene. The log has no bounced read, but the main-thread/GXRenderThread load race (Stability
  entry above) hit at **both** loads of the session: at the first load a corrupted packet made the
  host skip the rest of a 796-byte command buffer, and at the load into this scene the main
  thread's setup step (…ebbca9/…216aa95 chains) shared the immediate context with the render
  thread for 2.9 ms. Skipped or interleaved setup dispatches would leave whatever they produce
  wrong until the next load, which fits "missing until a relaunch". *Next:* Option B of the
  load race (serialize the two threads), then see whether missing geometry still occurs.
  *2026-10-18, #18 (Didjus, M3 Pro 14", 18 GB, 0.5.1; FSR 3.1 Native AA at 720p, 60 cap) and
  the user (M4 Max, 0.5.1, 1440p MetalFX):* parts **disappear progressively after a few minutes
  of play** (a hat, then a ladder, then a floor; NPC clothes, walls) and come back after
  reloading the area (not always); **every version** so far, MoltenVK included;
  `shader_compile=wait` makes no difference. The user: the well floor and a chasm near the
  Hemwick Charnel Lane lamp missing after a 15–20 minute walk from the Witch's Abode, both drawn
  on an earlier pass close to the lamp. Neither log has an error, a skipped draw or a bounced
  read, so the draws are issued with stale/empty data or not issued at all. Both logs have
  `Gnm: context … used by thread A, then B` lines where a job worker (…263b246 chain) and the
  main thread's setup (…ebbc8b) share the immediate context outside the context lock: still
  the Option B suspect above. *Next:* the overlay's Capture Frame at a broken spot, then again
  after reloading the area (asked on #18): diff the two pass lists (draw missing vs present with
  bad data). Then a long-walk repro from a save (Hemwick lamp), and Option B.
  *2026-10-19:* the `…263b246` chain in both logs is the render thread's draw-list task
  `0x219d220`, now under the context lock (2cb8681): re-check whether geometry still disappears.
- **Black loading screen that never finishes (#18, 2026-10-18).** About 1 launch in 10 on an M3
  Pro stops at a black screen with the loading icon; a relaunch works. No log of a stuck run
  yet (asked for one). *Candidate fixed 2026-10-19:* `sceGnmDingDong` waited for the previous
  submission to retire before queueing its compute work, so a graphics WAIT_REG_MEM on a label
  that work writes could never finish (the order DiAvisoo/bbport-mac-optimized changed). The game
  submits compute only at start and while loading (~55 calls a session, measured), none of them
  over a pending SubmitDone in our runs: a timing race, likelier on slower Macs. Now queued
  first, then waited. Confirm with Didjus's reports.
  **Cause found 2026-10-19 (PR #20, cherry-picked as 2cb8681):** Didjus's stuck-run log shows the
  render thread's draw-list task (`0x219d220`, run by GXRenderThread's loop after the frame job)
  writing the immediate context while the main thread was in `0xebb960` under the lock, then
  "bad packet ... at word 777". That task is now hooked under the same lock (the "third direct
  user" of 2026-10-17). Author's tests: 2 of 2 hangs before, 0 of 5 after. Confirm on #18.
- **Metal: a tessellation pipeline refused, its draws skipped (2026-10-18, user, 0.5.1).** Every
  session: `GetTessellationPipeline: … the evaluation shader reads 160 bytes of patch constants,
  the control shader writes 224` (vs 0x2fb9d0c43ee, fs 0xdbb1593690c; pipeline fs dbb222eb hs
  2807dd6c vs f0c066a1 ls 2fb118ad), then `not built … its draws are skipped`
  (`mtl_pso_cache.mm` ~947). Likely the TES reads a prefix of the TCS's patch struct (unused
  members dropped by SPIRV-Cross): compare the two MSL structs; if the TES members sit at the
  same offsets, accept a prefix and index with the TCS stride (`patch_stride` is already the
  TCS's). MoltenVK drew it (0.4.x). Which geometry it is: not known yet (a candidate for some
  missing floors, but not the progressive disappearance above).
  **Fixed 2026-10-19:** not a prefix but a suffix. The TCS writes patch locations 8-21 (14
  float4s); the TES reads 12-21, so SPIRV-Cross dropped 8-11 and its struct started with
  location 12 at offset 0. The TES variant now carries the TCS's patch range
  (`CompileVariant::patch_first/patch_end`, cache tag `_te<N>p<first>_<end>`) and declares every
  location with `add_msl_shader_input` (per patch, vec4): SPIRV-Cross adds placeholders
  (`raw_buffer_tese_input`), so both structs are 224 bytes with the same offsets. `--msl-check`
  on a copy of the user's cache: 649 of 649 pipelines, all 30 tessellation ones. Still to see in
  game: what it draws (the error line should be gone).
- **Flaky unit test: `untracked command buffers count too` (test_mtl_encoder.mm, 2026-10-20).**
  It compares the GPU busy time of two 64 MiB fills; failed once in a full `--test` run while the
  machine was otherwise idle, then passed 5 of 5 alone and in the next full run. A timing bound,
  not a renderer bug; widen the margin or repeat the measurement before failing.
- **Full-screen pacing in the unfocused test harness (2026-10-20).** 30-40 spikes per run (25 ms
  frames) full screen vs 0 windowed, master and the cleanup branch alike (FINDINGS). Check with
  the app focused before treating it as a real problem.
- **GPU host crash in `Scheduler::KickRecording` (2026-10-14, user session).** SIGSEGV at
  address 0x20008 (a null `record_chunk`: its size field follows the 128 KiB buffer) on the draw
  recording thread, `RunWriteData` → `WaitHostCopies` → `KickRecording`, right after an area
  load (cloth threads created). Another thread must have used the scheduler's recording chunk
  while the draw pipe ran (a missing `DrainDrawPipe`). The scheduler now reports overlapping
  chunk users ("two threads use the recording chunk at once", with both callers and a
  backtrace). Not reproduced in 6 reloads. *Next:* read that report from the next crash, add
  the missing drain there.
  *2026-10-17 (co-op on The Hunter's Dream, two phantoms in the world, ~40 min in):* the report
  fired once and the game froze (no frame presented after it; the user closed the window):
  `bb:DrawRec` from `BufferCache::ObtainBuffer+0xcbc` (SmallGuestCopy → Record) while another
  thread had entered from `Vulkan::Runtime::CopyBuffer` (its Record). Paths checked and found
  draining first: the DMA copies (`Rasterizer::CopyBuffer`), queued commands
  (`Liverpool::ProcessCommands`), the fault relay (`bbgpu_host_fault` → `ReadMemory(assume_locks
  = false)` → `SendCommand<true>`), `OnWriteFault` / `InvalidateMemory`; stage B advances
  `consumed` only after a packet's handler returns, so `Drain` does not return early. The
  remaining `Runtime::CopyBuffer` users are `BufferCache::ReadMemory`'s download (runs on A after
  a drain, or inline on B/A with `assume_locks`) and `ObtainBuffer`'s upload (line ~489). Still
  open which thread it was; the log has no thread name for the other side (worth adding to
  `ReportChunkRace`: the other thread's name, captured with its caller). Workaround for long
  co-op sessions: `BB_DRAW_PIPE=0` (no stage B, so no chunk race; ~18% slower draw recording).
  *2026-10-17, later (code review of every thread that can record):* **found and fixed.** The
  other thread was the GPU command thread itself, through `Liverpool::ProcessCommands`: it
  drained stage B only `if (num_commands)` *before* the `while (num_commands)` loop, so a
  command queued between those two reads ran without a drain. The queue is fed by the fault
  relay thread (`bbgpu_host_fault` → `ReadMemory(assume_locks=false)` → `SendCommand<true>`)
  and by non-EOP flips; `ProcessCommands` runs before every PM4 packet, the game faults ~1000
  times a second, so the two-instruction window is hit in a long session. The relayed read
  fault's download (`ReadMemory` → `DownloadMemory` → `Runtime::CopyBuffer` → `Record`,
  the reported caller) then raced stage B's draw. The same hole explains the 2026-10-14
  crash: that download's `scheduler.Finish()` kicks the recorder (`SyncRecording` →
  `KickRecording`, `record_chunk` momentarily null) while stage B kicked from `RunWriteData`.
  Fix: `ProcessCommands` drains after a command was taken, before it runs (the fast path, no
  command, is unchanged: one atomic load); `Rasterizer::WaitDeferredSignals` drains too (its
  kick touches the chunk; every caller drained already). Every other recording thread was
  checked: stage B's own faults run inline (it owns the chunk), the fault relay, the host ring
  thread (`UnmapMemory`, `InvalidateMemory`), the copy threads, the recorder, the priority-ops
  thread, the present/swap threads, the draw-preparation workers and the pipeline builders
  never record or go through `SendCommand`; the EOP flip's `PrepareFrame` runs on the
  signalling thread (B via `RunInOrder`, else A drained); the opt-in texture bind helper binds
  without refreshing images. `ReportChunkRace` now names both threads and, on macOS, prints
  the other thread's stack at report time (the thread is suspended, its frame chain walked),
  so a future report needs no guessing (a same-thread re-entry reports the caller it is inside,
  not its own). Tests pass; an offline run with `BB_GPU_LOG=debug` showed no report. The drain
  fix is in `metal-native` for 0.5.0 (32e7f2f); the report and the render-pass workaround above
  follow in 0.5.1 (PR #11). The co-op reproduction (long session, phantoms) is the user's to
  confirm; `BB_DRAW_PIPE=0` stays a fallback until then.
  *Same day, test runs:* the load-time corrupted command buffer (cloth-thread entry above) also
  killed the host twice in ~14 test loads: `SignalFence: Unreachable code!` (pm4_cmds.h:492)
  after "Gnm check: bad packet", and a SIGSEGV in `Shader::StageSpecialization` building a
  draw from corrupted shader state. The fence (and the other corrupted-stream paths) are now
  skipped; root cause in the cloth-thread entry above.
- **Timed mutex/rwlock waits** (`runtime_mutex.c`, `runtime_rwlock.c`) are trylock + backoff:
  up to ~1 ms late, not fair; error-checking mutexes return `ETIMEDOUT` instead of `EDEADLK`
  on self-relock. *Refinement:* condvar-based timed locks. *Measured 2026-10-14 night:* a full
  walk (load + Hunter's Dream) had **0 contended timed waits** (counters on the trylock loop),
  so the game does not pay for this today; low priority.
- **Rosetta stale translations on aliased memory:** code written through one guest alias and
  executed from another runs the old translation until a protection change on the executing
  alias. *Refinement:* toggle protection (RW→RX) on executable aliases after host writes, if
  the game ever runs code from direct/flexible memory.

- **GPU process split (default since 2026-10-12, the only mode since 2026-10-19).** *Now:*
  (a) every GPU page protection is a synchronous round trip to bb-probe and every fault on a
  protected page is relayed to the host (~950 faults/s in the city, no FPS loss measured) —
  *proper:* batch protections asynchronously with ordering guarantees; (b) input: the host
  window's keyboard reaches the game through the shared block, gamepads are polled by SDL in
  bb-probe (`SDL_UpdateGamepads`, no window there) — *proper:* read pads in the host and publish
  a pad-state block (design 3.5); (c) losing the host after the renderer started ends the game —
  *proper:* restart both; (d) the settings menu's Restart travels host → game as an event (not
  yet exercised in game); (e) watchdog/hang dumps in bb-probe report the game side only.
- **Command buffers parsed after the game rewrote them (single-process mode).** The game reuses
  DCB/CCB memory soon after `sceGnmSubmitCommandBuffers` returns; the GPU thread parses later
  ("Unimplemented PM4 type 0" on save loads). Split mode copies them at submit (arena in the
  shared block). *Proper for single-process:* copy at submit too (upstream's copy-GPU-buffers
  option, or the same arena); nested indirect buffers are still read from guest memory in both.
- **SRT walker faulting loads stay zeroed for good** (upstream behaviour, kept in the portable
  interpreter). *Proper:* zero only for the walk that faulted, if repeated faults are cheap enough.

- **Metal-native (M5): `BB_GPU_CAPTURE` crashes with the default 16 GiB arena.** Apple's
  GPUToolsCapture segfaults in `stopCapture` (`GTTraceDump_writeUnsortedCapture`) while writing
  the `.gputrace` (bbgpu-host dies). Workaround: `BB_ARENA_GIB=4` (884 MB capture in the city,
  FINDINGS 2026-10-17 Task 10). Proper fix: capture with a smaller arena automatically, or a
  `MTLCaptureScope` that leaves the arena out if Apple's tool allows it.

- ~~**The game process grows ~80 MB/min during play (both renderers, 2026-10-18).**~~ Fixed
  2026-10-18: `runtime_low_free` filed freed blocks under the wrong size class (it read the class
  after the free-list link overwrote it), so the small classes carved a new slab every few
  allocations (~4.5 GB/h) and the 8 KiB class could hand out 64-byte blocks. Flat at 2.5 GB now.
  The same fix for 0.4.x is on branch `fix/low-alloc-free-list` (not merged). FINDINGS 2026-10-18.

- **Indirect draw arguments above 4 GiB in the arena** (Metal uses 32-bit `indirectBufferOffset`;
  index buffers are copied to staging instead, #36). Arena windows fixed both but stalled the GPU
  queue through the driver's wired-memory accounting (#42; `BB_ARENA_WINDOWS=1`). Options: split
  the arena into non-overlapping buffers of at most 4 GiB, or copy indirect records like indices.
- **16:10 (and ultrawide) glitches while a level first loads (#75, 2026-10-25).** Seen by the
  owner at 16:10 on 0.5.6 with their own save; settles once the level is in. Suspect: screen-size
  targets created at the 16:9 size for the first frames before the native-size patch's sizes are
  seen (`vk_rasterizer.cpp` "observed scene depth"). Capture a frame (`BB_FRAME_CAPTURE`) while it
  happens.
- **Small ABI and safety items from the Paleblood assessment (2026-10-25, low).** Verified against
  master; none affects the shipped game: semaphore handles are written as 4 bytes
  (`src/runtime_sema.c`, libkernel stores 8); sysmodule IDs are `uint16_t`
  (`src/runtime_content.c`); `BBPORT_GS_SLOT` overrides in `scripts/link_libc.py` are not checked
  against the runtime's fixed slot 384 (record the slot in link.json, refuse a mismatch);
  `address + size` checks in `src/runtime_memory.c` can wrap on garbage sizes (check before
  adding); `tools/bbrecomp/proto/check4.c` (static-recompilation branch) exits 0 on mismatches.
  Fix independently; do not copy Paleblood's GPL-3 sources.
- **Cloth (dresses, coats) corrupts at 120 FPS (2026-10-25, owner, every release back to 0.5.0).**
  The skirt of a dress turns into flat, posterised dark patches; taking the dress off and on fixes
  it until the character walks. Seen at the 120 cap (Uncap FPS++, variable timing); looks like
  cloth simulation that assumes 30/60 FPS steps. PR #77 (adaptive timing) scales cloth constraint
  stiffness by frame time (about min(1, 60*dt)^2), which may be the missing piece; a smaller fix
  could cap the cloth step or run cloth at fixed 60. Test at 60 vs 120 on the same save first.
- **Red/purple face at 16:10 (2026-10-25, owner).** Seen with the 1920x1200 native patch, in a test
  build without 0.5.5's GCN legacy/clamped float translations (that build looked worse overall);
  the face is normal with the 16:10 patch off. Not yet checked on 0.5.7 itself. Suspects: a
  screen-size target or skin (subsurface) pass still sized for 16:9 with the native-size patch, or
  NaN/Inf values (the red-flash class) on the skin shader.

## 2. Performance

- **16–18 GB Macs on macOS 27 run at 5–17 FPS (2026-10-25, top priority, the 0.5.x hotfix).**
  Not GPU-bound: the GPU idles ~60% of each frame (FINDINGS "the slow Macs are 16–18 GB Macs on
  macOS 27"). Needs a slow Mac to test: the kernel log for `required > max`, raising
  `iogpu.wired_limit_mb`, then BB_GC_BUDGET_MB / BB_ARENA_GIB / BB_ARENA_LAZY / BB_FRAMES_AHEAD with
  the benchmark. Likely fix if wiring: size the arena and its residency by RAM, or a growing arena.

- **Tessellated draws split their render pass (2026-10-21).** City at 1440p MetalFX: 67 render
  pass splits per frame, all from `Runs::DrawTessellated` (`BB_ENCODER_STATS=1`): each run of
  tessellated draws encodes its vertex and control kernels in a compute encoder, so the pass
  ends and reopens with Load (a store and reload of every attachment). Tessellated draws are
  15-25% of the city's GPU frame (skipped: 6.6-7.5 -> 5.6 ms/frame, 83 fewer encoders;
  FINDINGS 2026-10-21); the splits are part of that. *Fix:* encode a pass's tessellation
  kernels before its render encoder opens. Needs the pass's commands ahead (stage B would hold a
  render pass until its end, or stage A records tessellated draws' kernel work in a pre-pass
  list) and a conflict check: a copy-shader copy or storage write earlier in the same pass to a
  range a kernel reads (vertex streams, constants) keeps that run in place, as deferred copies
  do for their ranges. Check with screenshots of the cobblestones and other tessellated
  geometry, `BB_ENCODER_STATS` and GPU time; fewer encoders also means fewer ordering waits
  (~1700 a frame).

- **x86-64 GPU library: trim to the remote shim (M7 leftover, 2026-10-19).** The game process
  loads an x86-64 libbbgpu that only runs in remote mode, yet it compiles the whole Metal
  renderer, FSR 3.1 (FidelityFX host code), MetalFX, ImGui and SPIRV-Cross (so bootstrap still
  builds SPIRV-Cross for x86-64). *Done in 0.5.3:* an exported-symbols list
  (`gpu/shim/bbgpu_game.exports`) plus dead stripping: 15.5 → 13.4 MB. The rest stays reachable
  through the single-process branches of gnmdriver.cpp, driver.cpp and video_out.cpp
  (`if (BbSplit::Remote()) ... else <renderer>`). *Measured:* with `Remote()` a compile-time
  `true` in that build the library is 5.0 MB (scratch experiment, not committed). *Why not
  yet:* that changes behaviour before `bbgpu_init_remote` (the mode is still Single then):
  `bbgpu_handle_fault` would relay faults to a host that has not started its renderer, and
  `bbgpu_split_keyboard`/`InvalidateMemory` would dereference a null shared block before
  `bbgpu_split_attach`. *Refinement:* make the game side's mode explicit from the start (or
  guard those entry points), delete the single-process branches, then leave the renderer,
  ffx_metal, MetalFX, the overlay and SPIRV-Cross out of the x86-64 build (and bootstrap).
  Needs game runs: boot, movies (AvPlayer), save load, restart, text entry, fault relays.
- **SDL is still built with its Vulkan support (2026-10-19).** The window is a Metal window now,
  so SDL never loads a Vulkan library, but libSDL3 still contains the loader code and names
  `libvulkan`/`libMoltenVK` as candidates. `-DSDL_VULKAN=OFF` in `bootstrap_deps.sh` would drop
  it; untested (needs a fresh bootstrap of both architectures and a game run).

- **Ideas from DiAvisoo/bbport-mac-optimized (bmy's engine on KosmicKrisp, 2026-10-19).**
  (a) Skipping attachmentless draws without shader writes or queries (`BB_SKIP_EMPTY_DRAWS`, off
  there): Metal starts a maximum-size render pass for each. Our Cathedral Ward capture has no
  such pass, so check one more scene before doing anything. (b) A per-thread cache of mapping
  extents for the draw-preparation workers' region lookups (theirs was a mutex; ours,
  `host/host_runtime.c`, is a read/write lock: measure contention first). (c) Parking spin-waits
  on `std::atomic::wait` after the pause loop instead of yielding (`BB_PIPE_PARK_WAITS`): energy
  on battery. (d) Their sparse arena (KosmicKrisp has sparse binding) only commits bound
  blocks; the Metal equivalent is placement heaps, an alternative to zero-copy (M7) for 16 GB
  Macs.

- **Frame drops while traversing (user's 0.4.5 test, 2026-10-17).** Otherwise very smooth (the user:
  better pacing than the original and the PC version on a 4060). Shader/pipeline compiles or asset
  streaming (texture/buffer uploads, page-protection faults); the Frame stats line and Spike
  lines separate them (compiles, image create/refresh, write faults).
- ~~Frame rate stuck at 60 with the 90 cap in the user's session.~~ Not the game and not the
  Metal HUD (both locked at 60 with and without `MTL_HUD_ENABLED`): the built-in display had
  switched to a fixed 60 Hz (`NSScreen.maximumFramesPerSecond` 60, refresh 60.0; earlier the same
  day 90/120 caps reached 89/117 FPS). Fix: System Settings > Displays > Refresh Rate > ProMotion.
- **Metal-native: one GPU frame of 25-46 ms every 5-40 s, uncapped (Task 11, 2026-10-18).** At
  100% GPU (~100 FPS, Immediate) one command buffer takes 25-39 ms instead of ~10; the extra time
  falls in a different encoder each time (upload, copy-shader dispatch, big compute, G-buffer
  pass). Not the lazy arena, the fence ordering or the fence checker; 0.4.0 shows none. Check
  whether capped play (60/90/120 with display sync) shows it; then an Instruments Metal System
  Trace of a long frame (GPU preemption by another client, a residency/paging event, or a GPU
  power-state change). Diagnostics: `BB_SPIKE_MS=20`, `BB_LOG_LONG_CB_MS=20`,
  `BB_GPU_PROFILE=1 BB_GPU_PROFILE_SLOW_MS=18`. FINDINGS 2026-10-18.
- **Metal-native: a fixed per-frame GPU cost (Task 11 CPU-bound check, 2026-10-18).** At a
  960x540 render the GPU is still busy 8.75 ms/frame (114 FPS against 0.4.0's 120): the cost does
  not scale with pixels. ~350 encoders and ~2000-2400 fence waits per frame, ~120 inline
  copy-shader copies ending render passes. Fewer encoders (hoist or batch the copy-shader copies,
  merge compute dispatches into one encoder) is the lever; at 1440p we already win by 12-18%.
  Deferred by the user (2026-10-18): an efficiency gain, not on the critical path to full Metal
  rendering. Worth it for energy: the user plays at a 60 cap to save power, and this cost is paid
  every frame there too, so measure power at the 60 cap (both builds) before and after. The
  proper fix may need buffer renaming (copy into a fresh buffer instead of reusing one), so the
  copy-shader copies stop forcing encoder splits and fence waits.
- **A 1080p R32F image re-detiled every frame (9.4 MB; shared texture-cache code).** The GPU
  writes its memory through a buffer, which marks the image GPU-dirty, and each use detiles it
  from the arena again (`BB_LOG_UPLOAD_MB=5`). Keep the image in sync with the buffer write
  instead (a buffer-to-image copy at the write, or tile the writer's output). FINDINGS 2026-10-18.
  *2026-10-19 (b0d4557):* the writer is compute shader `0x3d5ebf4e` (a 12 MB storage buffer at
  the image's base; `BB_LOG_UPLOAD_MB` names writers); the image is never rendered to. The
  refresh's 9 MB CPU hash is skipped for such images (command thread 0.6–0.8 → 0.2 ms on slow
  frames on an M3 Pro); the GPU side (~0.12 ms per frame, detile + copy) remains.
- ~~**Metal-native: GPU ~12% slower than 0.4.0 in the city (11.2 vs 9.9 ms/frame; 2026-10-17).**~~
  Level in the Task 11 matrix (97.7 vs 97.5 FPS at 1080p; ahead in all 16 scene/config pairs,
  +12-18% at 1440p native/TAA; FINDINGS 2026-10-18).
  Caps 120 FPS at 89-94 against 0.4.0's ~100. Task 10c's per-command ordering is in; an unsafe
  probe without ordering waits reached 108 FPS, so the waits (or encoder splits they force) are
  the suspect. Measure in Task 11 (fence waits per frame, encoder count vs 0.4.0's).
- **Metal-native: intermittent guest-flip wobble at the 60 FPS cap** (2 of 5 launches: spread
  0.6 ms, flips up to 18.3 ms for the whole run; 0.4.0 never). Per launch, so suspect a phase
  between the 480 Hz vblank ticks, the frame limiter and the display's vsync. FINDINGS 2026-10-17.

- **Metal-native (M5): the city at 1080p (upscaler off) runs 95.4 FPS, 0.4.0 98.8.** Task 10b
  made device-local buffers untracked with explicit ordering (88.3 -> 95.4 FPS; FINDINGS
  2026-10-17 Task 10b). What remains is the barriers' reach: ~80 barriers per frame (copy-shader
  copies rewriting tables between draws), and each makes every later encoder of the scope wait,
  while only the conflicting command needs to (measured: 108 FPS when only the next encoder
  waits, unsafe). Next: per-command ordering, each command waiting for exactly the encoders it
  conflicts with: the renderer records which commands' accesses conflict (command ids per
  256-byte block), the Encoder maps command ids to slot fences (now 256 slots per class) and
  needs transitive pruning of waits (an encoder that waited stands for what it waited for,
  across encoder kinds). A first try retiring per waiting kind ran out of slots (87.7 FPS, at
  64 slots).
- **Metal-native (M5): with object motion on, the frame takes ~19.6 ms whatever the ordering**
  (city, TAA, 1440p, background: untracked 51.0 FPS, tracked 51.3; off: 75.0 against 64.0), so
  object motion's cost grew to +6.3 ms only because the frame without it got faster. Not ordering
  slots (0 busy reuses in the motion class, FINDINGS Task 10b fix round 1): profile the motion
  passes (`BB_GPU_PROFILE=1`; the RGBA32F motion attachment, the motion shader variants).

- **Metal-native (M4): the second frame waits ~344 ms for the preloaded pipelines.** `WarmUp`
  hands the stored pipelines to the build pool and returns at once, so the first frames' draws
  wait for them (first 5 s window: 55 FPS, worst ~410 ms; 0.4.0 preloads before the first frame:
  worst 40 ms). It happens behind the boot logos; later, start the warm-up earlier or let the
  first frames skip draws whose pipeline is not ready.

- **Metal-native (M4): archive write cost grows with the session.** Each write re-adds and
  serializes every recorded pipeline (~2.4 ms each; 258 pipelines ~0.6–1.0 s on M4 Max), every
  10 s while compiles continue: at 2–3k pipelines late in the game that is 5–7 s of utility-thread
  work per write, possibly contending with live compiles, and longer than the 5 s exit-flush
  bound (`bbgpu_flush_caches` then gives up, logged; the old file stays). The "wrote N pipelines"
  line now logs the duration: check it on a 16 GB / 8-core Mac in M5; grow the interval with N.
  **Spaced 2026-10-21:** writes wait at least 20 times the last one's duration (~5% of one
  core; `kWriteSpacing`). *Still:* an incremental archive (a second file for this session's
  new pipelines, both given to `binaryArchives`) would make each write proportional to the new
  pipelines only.
  Seen 2026-10-18 on the M4 Max: quitting after character creation logged "the pipeline archive
  write did not finish within 5000 ms; exiting without it" (pipelines from that session recompile
  next launch). Later the same evening (M4 Max, 0.5.1, on battery): **12–14 s per write at
  ~720–760 pipelines**, repeated about every 10 s while new pipelines compiled, i.e. near-constant
  utility-thread work; an M3 Pro (#18) took ~2.5 s at ~780. Why the M4 Max is 5x slower is
  unknown (battery? the archive file size?). 0.5.2: write only the new pipelines (a second
  archive, or merge on exit) and grow the interval.
- **Metal-native (M4): archive writer throttle allows double writes.** The writer's due time is
  10 s after the last write, computed before it waits: a `Flush` (Sync, exit flush) during that
  wait does not move it, so when more pipelines compile after the flush the writer does a second
  full write sooner than 10 s after it (re-read `last_write` after waking).
  Also archive hits after the session's last write are not counted as unwritten (only compiles
  are), so they are missing from that file and recompile next session (self-healing, small).

- **MoltenVK argument buffers off: done 2026-10-15** (default in `bbgpu_init`; +13–16% FPS
  uncapped at 1080p/1440p, native/FSR/MetalFX; FINDINGS).
- **Native Metal back end (plan stage 3; user priority, 2026-10-15).** *Now:* the arm64 GPU host
  renders through MoltenVK. *Refinement:* a Metal back end in the host behind the renderer
  (pipelines, resource binding, render passes, textures, buffers, sync), keeping the shadPS4
  front end and the shader recompiler (SPIR-V → MSL via SPIRV-Cross). Expected gains: the
  recording thread's MoltenVK share (~43–47% measured single-process), merged render passes with
  load/store actions we control, tessellation without MoltenVK's emulation stalls. Fragment
  shading cost stays about the same. Gate: the step-1 measurements (FINDINGS).
  *Metal path gaps (2026-10-16):* indirect tessellated draws are skipped (MoltenVK skips them
  too; emulate them as the indirect geometry draws are, with a prep kernel sizing the kernels and
  the patch draw); instanced geometry and tessellated draws draw instance 0 only.

- **Resolution targets (user, 2026-10-13): locked 60 FPS; M4 Max 1440p native and 4K
  upscaled, base M5 1080p native, smaller Macs 1080p upscaled.** M4 Max today: 1080p native
  61–63, 4K Performance ~52, 1440p 49, 4K Quality 43 (FINDINGS). Two levers: (a) the draw
  recording thread (MoltenVK encoding in `vkQueueSubmit` ~43%, render pass begins ~12%) caps
  1080p-class scenes at ~62–67 FPS; (b) GPU time at 1440p (~17.5 ms; 60 needs ≤ ~15 ms).
- **Render-pass count is a GPU cost after all (measured 2026-10-13, FINDINGS "Frame timeline").
  Planned for 0.5.3 (user, 2026-10-18).**
  At 1080p native the GPU timeline, not the CPU, sets the frame rate; ~408 Metal encoders per
  frame (190 render passes) cost ~13–16 µs of GPU time per render pass and leave the GPU idle
  ~3.8 ms/frame. *Refinement, in order:* (1) merge passes split by uploads/copies (~38/frame) and
  hoist the remaining copies; (2) put consecutive dispatches in one compute encoder; (3) the 47
  tessellated draws per frame each split a pass (MoltenVK's emulation): batch them or emulate
  tessellation ourselves; (4) attachment-less vertex-only passes (~20/frame) folded together.
  *0.5.3 plan (2026-10-18):* the counts above are from MoltenVK; first re-measure encoders and
  render passes per frame on the native Metal renderer (Metal System Trace, 1080p and 1440p
  native) and list which splits are avoidable (a pass reading what the previous one wrote must
  stay split). Expected: ~1.5–3 ms GPU per frame, ~10–20% FPS where GPU-bound (1440p native
  Hunter's Dream 82 → ~93–103; Upper Cathedral Ward 65 → ~72–80), plus fewer tile store/load
  round trips (grows with resolution); at a 60 cap the same FPS for less GPU energy. Plain Metal,
  any macOS we support (15+); not Metal 4 (that needs macOS 26: a separate decision after this).
  *2026-10-19 (M3 Pro, native Metal, `BB_PASS_STATS=1`, 1080p, Iosefka's Clinic, ~1600 draws):*
  ~170 passes per frame: ~92 attachment changes, ~31 splits by copy-shader copies (same
  attachments reopened; deferred to the pass end since 27a21db, "Versioned copy-shader table"
  below), ~25 ended by dispatches (rarely reopening the same attachments).
- **Shader compilation (done 2026-10-13: background pipeline creation, `shader_compile=wait|skip`;
  `balanced`, the default since 2026-10-13 evening, waits up to `BB_COMPILE_BUDGET_MS` (6) per
  frame and then skips).** A true ahead-of-time precompile is not possible: a pipeline's
  translation depends on draw-time state (fetch shader, buffer/image descriptors, render-target
  formats), so the game's `shader/*.shaderbnd` archives alone cannot produce it.
  Pipeline creation (~70% of a new shader's cost) runs on 4 utility-QoS threads; the preload no
  longer blocks start. *Remaining:* our own GCN→SPIR-V translation (~30%) still runs on the GPU
  command thread; the draw-preparation workers could start compiles for pipelines they find
  missing ahead of it; compute dispatches always wait (skipping one could corrupt later frames).
- **Fixed 2026-10-13 (biggest win so far):** `NoContraction` on every float op became
  `[[clang::optnone]]` helper calls in MSL; dropped in fragment/compute shaders: city respawn
  spot 39 → 60 FPS (vsync cap), GPU 19.6 → 12.1 ms/frame. *Remaining:* vertex stages keep it
  (needed for matching positions across passes); building MoltenVK with a SPIRV-Cross that emits
  precise ops without `optnone` (or `invariant` positions) would remove it there too — measured
  no further gain today, so low priority. The game is now limited by the 60 Hz present: try 120 Hz
  displays / `BB_FPS_LIMIT` and re-profile the CPU side.

- **Render-pass breaks on a tile-based GPU (biggest lever, 2026-10-11).** Metal System Trace
  (`xcrun xctrace record --template "Metal System Trace" --attach <pid> --time-limit 3s`) in a
  ~23 FPS scene: GPU busy 84%, fragment 69%; **~287 Metal render passes per frame, ~32 ms
  fragment time, ~0.11 ms each** (load/store of 1080p targets per pass) for ~1,600 draws.
  Desktop GPUs barely pay for pass breaks; Apple TBDR does. *Refinement:* log why rendering
  ends between draws (barriers, layout changes, copies/clears, compute), merge passes with the
  same attachments, use DONT_CARE load/store where contents are dead.
- **Fixed 2026-10-12:** attachment-less passes with no pixel shader rendered over a 16384×16384
  area (~6 ms/frame of tiling); now 16×16 (27.5 → 33.9 FPS in the city). Remaining pass work:
  G-buffer splits (6 MRT + depth reloaded ~15×/frame) — merge or mark contents.
- **Measured 2026-10-11, correcting the pass-count theory:** of ~287 fragment passes per frame,
  62% take <0.02 ms (3.5% of GPU time); 15% take 0.25–2 ms and hold ~72% — the cost is real
  shading in heavy passes (game defaults; port extras are off), not TBDR load/store. Only ~18% of
  passes are needless splits (same attachments reopened, 85% after buffer copies).
  Per-pass timing tools that do NOT work here: `BB_GPU_PROFILE=1` (MoltenVK timestamps read 0),
  `BB_VK_MARKERS=1` (per-draw labels only), Xcode attach (process list hangs), Metal frame
  capture `BB_GPU_CAPTURE=<s>` + `METAL_CAPTURE_ENABLED=1` (Apple's capture crashes in
  `DownloadHeap`, also with `BB_ARENA_GIB=8`).
- Other measured costs: MoltenVK encodes Metal commands inside `vkQueueSubmit` (~47% of the
  recording thread; `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0` moves it off, small gain while
  GPU-bound); draw-prep pool lock contention + background QoS on E-cores
  (`BB_PREP_WORKERS=0`: 21 → 23 FPS); `cthread_yield` spin in `Scheduler::WaitHostCopies`.

- **Zero-copy guest memory for the buffer cache (top item; plan stage 2).**
  *Now:* MoltenVK has no sparse binding, so on macOS each buffer-cache arena is an ordinary,
  fully backed 2 GiB buffer (`buffer_cache.cpp`) instead of a sparse 4 GiB one: guest data is
  still copied into GPU buffers, and every touched 2 GiB of GPU address space costs 2 GiB.
  *Refinement:* import guest memory directly with `VK_EXT_external_memory_host` (4 KiB
  alignment on M4 Max, unified 48 GiB heap) so the GPU reads/writes guest memory in place —
  removes uploads, read-backs and most write tracking. Mind CPU/GPU coherence and the Rosetta
  aliased-code caveat (Stability).
  *Measured 2026-10-13 (FINDINGS "Copy-shader pass splits"):* the pass splits come from the
  game's copy shader, not uploads, so zero-copy mostly saves CPU work (write faults, staging
  copies), not GPU time. It also needs EOP fences signalled at GPU completion (they are
  signalled at record time today), or the GPU reads buffers the game is already reusing.
- **1440p idle stalls located (2026-10-15, FINDINGS "1440p idle gaps, located").** ~2.9 ms of
  the ~3.6 ms idle per frame are four stalls at fixed encoder positions: ~1 ms inside the
  tessellated-draw run (MoltenVK tessellation emulation; `BB_DROP_TESS=1` removes it, +4.6 FPS
  at 1440p), ~0.7 ms after a `vkCmdCopyImage`, ~0.6 ms at the frame→presenter command-buffer
  boundary, ~0.75 ms between two compute encoders. Not memory allocation, not heaps. *Next:*
  find the MoltenVK barrier/fence at those positions; for tessellation, consider emulating it
  ourselves (vertex-stage expansion in our shader recompiler) instead of MoltenVK's path.
- **Versioned copy-shader table.** ~24 copies per frame rewrite one table between draws inside a
  pass, ending it. *Refinement:* give each draw group its own version of the table (copies built
  ahead of the submission) so the pass stays open; bound ~1 ms GPU at 1440p.
  **Done differently (27a21db, PR #20):** nothing reads the rewritten entries between such a copy
  and the end of its pass (frame event trace, 030bd20), so in-pass copies are queued and run in
  order right after the pass (Scheduler pass-end hook; a command whose bindings or
  vertex/index/indirect/direct-memory reads touch their ranges runs them first).
  `BB_DEFER_COPIES=0` or toggle bit 63: off. M3 Pro 1080p +12.8% (clinic), +13% (Hunter's
  Dream); M4 Max FINDINGS 2026-10-20.

- **MetalFX with asynchronous submits.** *Now:* MetalFX encodes on MoltenVK's queue right after
  a synchronous `vkQueueSubmit` (commit order). *Refinement:* order it with an MTLSharedEvent
  (exported timeline semaphore) so `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0` can be used.
- **PGO on macOS:** profile-guided optimization is off (the profiles are GCC `.gcda`).
  *Refinement:* clang `-fprofile-generate`/`-fprofile-use` with `.profdata`.

### Measured on an M3 Pro, not done (2026-10-20)

- **Tessellated light volumes split the deferred lighting pass.** 73 of ~87 tessellated draws per
  frame at the clinic are light volumes in the lighting pass (2× R11G11B10 + D32S8, 477 draws), in
  43 runs. Each run's vertex/control kernels end the render encoder and reopen it: 82 reopens per
  frame. An empty split costs ~0.05 ms there (48 extra splits: +2.3 ms GPU), so the splits are
  ~4 ms of a 22 ms frame. Skipping the tessellated draws (wrong image) gave +22.5% at the clinic,
  +7% in Hunter's Dream. Fix idea: encode a pass's tessellation kernels before its render encoder
  (buffer the pass's draws as `Runs` does for patch draws).
- **Area loads stall the renderer.** Entering new parts of Central Yharnam creates 400–700
  textures in one frame (130–230 MB) and waits on the host copy threads: 100–300 ms frames.
  Spread the creation and uploads over frames, or use worker threads.
- **Write faults cost the game ~19% of its time in the city.** That is 1,300–2,500 faults/s at
  ~150 µs each: four thread wake-ups across the processes per fault, while the renderer's own
  handling takes ~40 µs. Read-backs of GPU data wait ~10 ms each for the GPU thread. A game-side fast
  path (unprotect locally, apply the invalidation later) was tried and dropped: its 256 KB window
  also covered GPU-written images, whose texture invalidation re-uploaded stale guest memory over
  them. A correct one must leave texture-watched pages to the renderer.

## 3. Mac GUI

- **Launcher: the Controls section pushes Locations off the bottom of the window** (0.5.7, the
  window is sized for a 13" Air). Move the bindings list to Advanced or make Controls one row.

- **16:10 menus keep 16:9 artwork (#75, 2026-10-25).** Kyo's 16:10 patches fix HUD, lock-on and HP
  bar positions, but full-screen menus stay 16:9 with grey bars at the sides. The "16x10 and 4x3
  UI Fixes" mod (Nexus 207) replaces them; consider pointing to it from the launcher when 16:10
  is chosen (the mod manager can load it).

- **Preferred display (#60, 2026-10-24).** The window always opens centred on the main display
  (`gpu/shim/window.cpp`, `SDL_WINDOWPOS_CENTERED`) and the default resolution comes from it
  (`gpu/shim/bbgpu.cpp`). Add a `display=` setting chosen in the launcher, applied with
  `SDL_WINDOWPOS_CENTERED_DISPLAY(id)`, and keep resolution/ultrawide per display so docking does
  not reset them.
- **Leftover game processes (#71, 2026-10-24).** After a force-quit of a hung game, `bb-probe`
  and a GPU host stuck in exit stay alive and the launcher refuses to start ("only one copy").
  `stop_host` no longer waits forever after SIGKILL (fixes-0.5.6); the launcher should also offer
  "End the previous copy" (SIGKILL the pgrep'd PIDs, check again, else say a restart is needed).
- **Show the release version in the launcher (user, 2026-10-23).** Nothing in the window says
  which build is installed, so players (and bug reports) can't tell 0.5.4.2 from 0.5.4.4 without
  opening About. The bundle already carries it: `package_app.sh` writes `CFBundleShortVersionString`
  (e.g. 0.5.4.4) and `CFBundleVersion` (the full tag version), and `LauncherModel.swift` reads the
  short one for crash reports. Show "Version 0.5.4.4" in the bottom bar, next to "Show launcher
  every time", with the full `CFBundleVersion` as its tooltip, and "development build" when the
  key is missing (`make_app.sh` copies). Later: compare it with the latest GitHub release (PR #23,
  auto-update).

- **Session log times of GPU library lines (2026-10-22, session logs).** The launcher stamps each
  line when it arrives (`launcher/App/SessionLogWriter.swift`). bb-probe's stdout is unbuffered
  and bbgpu-host's own lines are single `write`s, but the GPU library inside bbgpu-host prints
  through stdio, which is block-buffered on a pipe: its `GPU [...]` lines can arrive in batches,
  later than written, with one time. *Proper fix:* `setvbuf(stdout, NULL, _IOLBF, 0)` at the
  start of `host/main.c` (one syscall per line; a few thousand lines a session), or the game's
  processes stamp lines themselves. Also possible later: the severity as a field the game writes
  instead of the launcher's heuristics (`SessionLog.classify`).

- **The Network Settings password shows its letters (user's 0.5.3-dev test, 2026-10-20).** The
  text box opens in full screen, takes typing and Escape cancels, but the letters are not
  masked. Masking depends on the game setting `OrbisImeOption` PASSWORD (bit 4,
  `src/runtime_services.c` `ime_init`); either Bloodborne does not set it for this field or it
  arrives elsewhere. Log `param->option` once to tell which; mask the password field by its
  prompt if the game never asks.

- **Ultrawide and quicksave (seen in DiAvisoo/bbport-mac-optimized, 2026-10-19).** 21:9 and 32:9
  output resolutions with an optional Nexus Mods UI pack (the launcher backs up and restores the
  game's menu files), and quicksave/quickload (F2/F3: snapshot the save folder; quickload
  restores it and restarts the game). Ultrawide fits the launcher's Advanced tab (#15). GPL-2.0:
  code can be borrowed with credit.

- **Hide the mouse cursor over the game (request via the user, 2026-10-18).** Nothing hides it
  today. `gpu/shim/window.cpp` (host owns the window): `SDL_HideCursor` while the game window has
  focus and the overlay is closed; show it for the `⌘,` overlay, on focus loss and at quit. First
  step of mouse look below (relative mode captures it anyway).
  *2026-10-19 (PR #20):* mouse look hides it while captured (after a click in the window); the
  uncaptured cursor still shows.
- **Mouse and keyboard support (user, 2026-10-18: "a couple of requests").** Today there is only
  a keyboard fallback in `src/runtime_pad.c`, used when no gamepad is connected: fixed keys
  mapped onto the DualShock 4 (WASD left stick, arrow keys the right stick *and* the d-pad, Space
  Cross, LShift Circle, ...), digital stick values only, no mouse at all. Proper support:
  - **Mouse look:** relative mouse motion (`SDL_SetWindowRelativeMouseMode`, captured while
    playing, released by the overlay/⌘-Tab/menus) to the right stick, with sensitivity,
    invert Y and smoothing; the stick is a rate, so motion per frame → deflection needs a curve
    (and a dead-zone offset) to feel like a direct camera. Upstream bbport 0.5/0.6 has "mouse
    look" (Strategy item 5) — check it before designing our own.
    *Done in PR #20* (`gpu/shim/bbport_mouse.cpp`, `runtime_pad.c` `add_mouse`): a click
    captures, Cmd / focus loss / the ⌘, menu release; speed over the last 33 ms → right stick
    with a dead-zone offset and a radial curve; `mouse_look` (off by default: a click would
    capture the cursor for gamepad players too), `mouse_sensitivity`, `mouse_invert_y` in the
    Display tab. Buttons as wired: left R1, right L2, middle R3, X1 R2,
    X2 L1 (the wheel is not mapped). Cmd+W, easy to hit while running with W, no longer quits
    (`window.cpp`).
  - **Mouse buttons:** left R1 (attack), right L1 (or R2), middle/wheel click R3 (lock on),
    wheel up/down for the item/weapon d-pad slots.
  - **Keyboard:** fix the arrow-key overlap (arrows are both camera and d-pad), analog-ish walk
    (a walk modifier for half deflection), rebindable keys stored in `bbport.ini`, and keyboard
    + mouse active alongside a connected gamepad (last device used wins).
  - **Launcher:** a Controls page (bindings, sensitivity, invert); show the current layout.
  - **On-screen prompts** stay PlayStation glyphs (the game has no keyboard glyphs); document
    the default layout in the README.
  - Split mode: mouse events live in the GPU host's window; extend the shared block's keyboard
    bridge (`bbgpu_split_keyboard`) with mouse deltas and buttons.

- **Updater trust (2026-10-20, launcher About + updates).** The launcher's updater
  (`launcher/App/Updater.swift`) checks the download against GitHub's SHA-256 asset digest, the
  bundle identifier, the version and `codesign --verify`, but the app is ad-hoc signed, so the
  signature proves integrity, not origin: anyone who can publish a release on the repository can
  ship an update. *Proper fix:* Developer ID signing + notarisation, then require the same Team
  ID on the new app (`codesign -d -r-`), or an EdDSA signature over the zip (Sparkle's scheme)
  with the public key in the app. Also: no delta updates (each one is ~50 MB).

- **Launcher: warn when the frame cap is above the display's refresh** (`NSScreen
  .maximumFramesPerSecond`): a display set to 60 Hz silently holds 90/120 caps at 60 (2026-10-17).

- **Native launcher app (user, 2026-10-15). Done 2026-10-16 (mac-v0.4.0, `launcher/`).** A SwiftUI launcher (game folder with the check,
  settings, log, play/stop), designed so other games can be added later. bmy/bbport-mac has one
  (`launcher-macos/`, GPL-2.0) for reference.

- **Native Mac settings.** *Now:* the in-game ImGui overlay (`gpu/shim/bbport_overlay.cpp`), in
  English, opens with ⌘, or L3+R3 and shows FPS, render → output resolution and upscaler.
  *Refinement:* a native Mac settings window in the launcher; if SDL's default app menu
  swallows ⌘, (its "Preferences…" item), wire that menu item to the overlay instead.
- **Text entry (ImeDialog) invisible in fullscreen; network password could not be set. Fixed
  2026-10-17 (online-shadnet).** The typed text was shown only in the window title bar, which
  fullscreen has none of, and the dialog prefilled "Hunter" into any empty field and wrote
  "Hunter" when the entry was empty or cancelled (inherited from the Windows port), so the
  8-character *Network Settings* password became "Hunter" plus two typed letters. Now: upstream's
  on-screen box (`BbOverlay::SetTextPrompt`, drawn by the presenter, also in split mode where the
  host owns the window), the field gets exactly what was typed, cancel leaves it alone, and
  `OrbisImeOption` PASSWORD (bit 4) masks the text as dots (`bbgpu_text_input_begin` gained the
  flag; `BbGpuTextBegin.flags`). Game input is held neutral while the box is open, so Backspace
  (the touchpad) does not reach the game.
- **Menu bugs on macOS.** "Subpixel jitter" ticks then unticks itself; upscaler/preset choices
  made in the menu were not what bbport.ini held afterwards (found upscaler=off, preset=0).
  *2026-10-15:* settings were only written when the menu closed, so closing the game window (or
  a crash) with the menu open lost them; they are now saved 0.3 s after the last change. Still to
  check with the user (`BB_MENU_TRACE=1`): the jitter toggle flipping back.
  *2026-10-17 (Metal renderer, M5):* the menu's upscaler choice is now the saved value
  (`requested_upscaler`); before, a choice equal to the value Load() ran with (e.g. Off after an
  ini naming the removed FSR 4) saved the old ini value back. Scripted menu input
  (`BB_MENU_INPUT`, in game, `BB_MENU_TRACE=1`): jitter off/on, preset, typed sharpness and Off
  were all saved and stayed; the jitter toggle never flipped back. Still open: the user's
  original jitter report (not reproduced). Also open: once (run g2, 2026-10-17) a value typed into the
  Sharpness field showed "0.5", Enter was delivered and a save followed, yet the ini kept 0.30;
  not reproduced in later runs. `BB_MENU_TRACE=1` now also logs sharpness and the requested
  upscaler, so a repeat shows whether the widget ever stored it.
- **Upscaler fallback and later preset changes (Metal renderer).** The fallback for an
  unavailable provider (`BbSettings::FallbackUpscaler`) is resolved at start and on a menu
  choice with the preset of that moment: on a Mac without the MetalFX temporal scaler, TAA chosen
  as fallback at Native AA stays TAA after a change to a scaled preset (and off stays off), and
  with the startup patch (`live_resolution=0`) patches.py still renders fsr3 at the scaled size
  while the fallback is off. Unreachable on Apple Silicon (all have MetalFX); re-resolve on preset
  changes and feed the fallback to patches.py if Intel/other GPUs are ever supported.
- **Overlay font atlas updates while a present may sample it.** imgui_impl_metal (1.92) writes new
  glyphs into the atlas texture with `replaceRegion` during `RenderDrawData`, while an earlier
  present still in flight may read it (upstream behaviour, Huax's port too): at most a transient
  glyph flicker the first time new characters appear. Refinement: double-buffer the atlas or wait
  for the previous present before an update.
- **Launcher and `.app` bundle.** *Done 2026-10-14 night:* `scripts/macos/make_app.sh` builds a
  local `Bloodborne.app` (launcher for this checkout, game icon from `sce_sys/icon0.png`, log in
  `~/Library/Logs/Bloodborne`, error dialog; the window process runs as the bundle's executable
  through a link so the Dock shows "Bloodborne"). *Done 2026-10-15:* a self-contained app for
  downloads (`scripts/macos/package_app.sh`, built by `.github/workflows/macos-release.yml` on
  `mac-v*` tags): universal libraries in Frameworks/, rpaths into the bundle, bundled standalone
  Python, game check and Rosetta check with dialogs, ad-hoc signed. *Remaining:* Developer ID
  signing and notarisation (needs a paid Apple developer account; the game process needs the
  executable-memory entitlement under the hardened runtime); an app icon we may ship (the game's
  own icon cannot be bundled).
