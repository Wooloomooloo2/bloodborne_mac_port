# macOS Port Backlog (refinements)

Work deliberately deferred while getting the game running. When a shortcut is taken, the
proper solution goes here under one of three categories, in priority order:

1. **Stability** — correctness and robustness gaps left by shortcuts.
2. **Performance** — speed and efficiency work.
3. **Mac GUI** — making it look and feel like a polished Mac app.

Each item: what was done for now, what the refinement is, and where it lives.

## 1. Stability

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
- **Buffer-cache arena window.** *Now:* one fully backed 16 GiB arena at 0x7000000000
  (`BufferCache::MacArenaBase/MacArenaPages`); a GPU buffer outside it stops with "outside the
  macOS guest arena". *Refinement:* per-page arenas with copy-based migration (rewriting the
  BDA page table), or zero-copy import (Performance) which removes arenas altogether.
- **Host pointers above 1 TiB reach the guest.** `__error` returns the host thread's errno
  address (0x7fc6…), above the game's 40-bit pointer range. Harmless if used immediately; if
  the game ever stores it packed, route errno through a per-thread low (<1 TiB) slot.
- **Watchdog timeout on macOS ends in a guest fault.** When `BB_TIMEOUT` expires, the SIGUSR2
  stack dump knocks a game worker thread into a SIGBUS (guest 0x53b3260, job-pool loop at
  0x263b246) instead of a clean exit. Only test runs with a timeout are affected; normal play
  has none. *Refinement:* find why the dump signal derails the interrupted guest thread
  (signal delivery under Rosetta with the guest stack/TLS) and exit cleanly.
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
- **GPU host crash in `Scheduler::KickRecording` (2026-10-14, user session).** SIGSEGV at
  address 0x20008 (a null `record_chunk`: its size field follows the 128 KiB buffer) on the draw
  recording thread, `RunWriteData` → `WaitHostCopies` → `KickRecording`, right after an area
  load (cloth threads created). Another thread must have used the scheduler's recording chunk
  while the draw pipe ran (a missing `DrainDrawPipe`). The scheduler now reports overlapping
  chunk users ("two threads use the recording chunk at once", with both callers and a
  backtrace). Not reproduced in 6 reloads. *Next:* read that report from the next crash, add
  the missing drain there.
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

- **GPU process split (default since 2026-10-12; `BB_GPU_PROCESS=0` = single process).** *Now:*
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

## 2. Performance

- **Resolution targets (user, 2026-10-13): locked 60 FPS; M4 Max 1440p native and 4K
  upscaled, base M5 1080p native, smaller Macs 1080p upscaled.** M4 Max today: 1080p native
  61–63, 4K Performance ~52, 1440p 49, 4K Quality 43 (FINDINGS). Two levers: (a) the draw
  recording thread (MoltenVK encoding in `vkQueueSubmit` ~43%, render pass begins ~12%) caps
  1080p-class scenes at ~62–67 FPS; (b) GPU time at 1440p (~17.5 ms; 60 needs ≤ ~15 ms).
- **Render-pass count is a GPU cost after all (measured 2026-10-13, FINDINGS "Frame timeline").**
  At 1080p native the GPU timeline, not the CPU, sets the frame rate; ~408 Metal encoders per
  frame (190 render passes) cost ~13–16 µs of GPU time per render pass and leave the GPU idle
  ~3.8 ms/frame. *Refinement, in order:* (1) merge passes split by uploads/copies (~38/frame) and
  hoist the remaining copies; (2) put consecutive dispatches in one compute encoder; (3) the 47
  tessellated draws per frame each split a pass (MoltenVK's emulation): batch them or emulate
  tessellation ourselves; (4) attachment-less vertex-only passes (~20/frame) folded together.
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

- **MetalFX with asynchronous submits.** *Now:* MetalFX encodes on MoltenVK's queue right after
  a synchronous `vkQueueSubmit` (commit order). *Refinement:* order it with an MTLSharedEvent
  (exported timeline semaphore) so `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0` can be used.
- **PGO on macOS:** profile-guided optimization is off (the profiles are GCC `.gcda`).
  *Refinement:* clang `-fprofile-generate`/`-fprofile-use` with `.profdata`.

## 3. Mac GUI

- **Native Mac settings.** *Now:* the in-game ImGui overlay (`gpu/shim/bbport_overlay.cpp`), in
  English, opens with ⌘, or L3+R3 and shows FPS, render → output resolution and upscaler.
  *Refinement:* a native Mac settings window in the launcher; if SDL's default app menu
  swallows ⌘, (its "Preferences…" item), wire that menu item to the overlay instead.
- **Menu bugs on macOS.** "Subpixel jitter" ticks then unticks itself; upscaler/preset choices
  made in the menu were not what bbport.ini held afterwards (found upscaler=off, preset=0).
- **Launcher and `.app` bundle.** *Done 2026-10-14 night:* `scripts/macos/make_app.sh` builds a
  local `Bloodborne.app` (launcher for this checkout, game icon from `sce_sys/icon0.png`, log in
  `~/Library/Logs/Bloodborne`, error dialog; the window process runs as the bundle's executable
  through a link so the Dock shows "Bloodborne"). *Remaining:* a self-contained, signed app
  that carries the built binaries and does not need the repository or a Terminal build.
