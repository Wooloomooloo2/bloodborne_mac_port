# Design: Native ARM64 GPU process (option A)

Status: **M0–M4 done, split mode is the only mode** (default 2026-10-12; the single-process mode and `BB_GPU_PROCESS` were removed 2026-10-19). Gameplay renders correctly through the native host. In the GPU-bound city scene FPS is unchanged (27.5 both modes; the GPU-feeding thread waits on the GPU 59% → 72%), worst frames are steadier (37–58 ms vs 49–75 ms). Next lever: render-pass merging (GPU side). Originally a proposal for decision. Reverses founding decision 1 in `CLAUDE.md`
("everything runs as one x86-64 process under Rosetta"), so it needs the owner's go-ahead.
Evidence behind it: [`FINDINGS.md`](FINDINGS.md), Part 2.

## 1. Problem

At a 720p render resolution (MetalFX/FSR Quality), the game runs at **27–30 FPS**, capped by one
CPU thread, `bb:DrawRec`, which turns ~1,600 guest draws per frame into GPU work:

| `bb:DrawRec` at 720p | Share |
|---|---|
| MoltenVK encoding Vulkan → Metal (`vkQueueSubmit`) | ~51% |
| Spin-waiting on host copies | ~11% |
| Renderer per-draw work | ~9% |
| Idle / waits | ~6% |

Moving work to other threads does not help (async submit, MoltenVK prefill on the recorder
thread, the prep pool: all unchanged) because the frame is serialised through sync points. Every
instruction on that path — the renderer and MoltenVK — is **x86-64 code translated by Rosetta**,
because the game's code is x86-64 and a macOS process cannot mix architectures.

## 2. Proposal

Split the program into two processes that share guest memory:

```
 bb-probe  (x86-64, Rosetta)                    bbgpu-host  (arm64, native)
 ┌──────────────────────────────┐               ┌──────────────────────────────────┐
 │ game eboot + libc.prx         │               │ Liverpool command processor       │
 │ PS4 HLE runtime (src/)        │  shared guest │ shadPS4 renderer (Vulkan)         │
 │ audio (CoreAudio)             │◄═══ memory ══►│ MoltenVK → Metal, MetalFX         │
 │ GnmDriver command builders    │  (same VAs)   │ texture/buffer caches, shaders    │
 │ GPU proxy: submit/flip/events │──── rings ───►│ SDL window, overlay, input        │
 │ page-protection + fault relay │◄── messages ──│ presenter                         │
 └──────────────────────────────┘               └──────────────────────────────────┘
```

- **`bb-probe`** stays x86-64 under Rosetta: loader, game, PS4 runtime, audio. It links a thin
  **GPU proxy** instead of `libbbgpu`.
- **`bbgpu-host`** is a new **native arm64** executable containing today's GPU library
  (renderer, MoltenVK, MetalFX, window, overlay). It runs the hot path natively.
- **Guest memory is mapped at the same virtual addresses in both processes**, so the renderer's
  pointers into command buffers, vertex data and textures stay valid unchanged. Apple Silicon has
  unified memory, so both processes (and the GPU) see the same physical pages — no copying.

### Why this is the right cut

The interface is already narrow (measured from the code):

| Direction | Today | After the split |
|---|---|---|
| runtime → GPU library | ~15 `bbgpu_*` calls (init, symbol resolution, fault handling, overlay/text input, stats) | Messages to the host; init at launch |
| GPU library → runtime | ~18 `runtime_*` calls (memory queries, page protection, write-through-backing, clocks, thread bookkeeping) | Mostly replaced by shared-memory state; protection via messages |
| game → GPU library | 327 HLE functions (GnmDriver, VideoOut, event queues, AvPlayer) | Split by kind — see 3.3 |

## 3. Design

### 3.1 Shared guest memory

- Today the guest pool is one anonymous region; guest views are `mach_vm_remap` aliases
  (`src/runtime_memory.c`). It becomes a **Mach named memory entry**
  (`mach_make_memory_entry_64`), whose send right is passed to the host at launch.
- The host reserves the same guest range (`[0x7000000000, 0xfc00000000)`; the 63–448 GiB
  kernel hole is the same in arm64 processes) and maps views at identical addresses with
  `mach_vm_map` of the memory entry. Every guest map/unmap/protect in the runtime is mirrored to
  the host through the existing GPU hooks (`runtime_memory_set_gpu_hooks`), now as messages.
- The guest image and stacks are not GPU-visible and stay private to `bb-probe`.
- This also realises the **zero-copy** backlog item: the host can import guest pages straight
  into Metal (`VK_EXT_external_memory_host`) instead of copying into a 16 GiB arena.
- **Why this fits the hardware (owner's observation):** the PS4 has one unified memory pool
  shared by CPU and GPU. The PC/Linux renderer copies guest memory into separate GPU buffers only
  because discrete GPUs have their own VRAM — that is where the arena, uploads, write-tracking
  faults and copy spin-waits come from. Apple Silicon is unified like the PS4, so guest pages can
  be shared by the game process, the host process and the GPU at once. Caveat: **textures** still
  need conversion (PS4 tiled layouts → linear/Metal), so the texture cache keeps detiling; buffers
  (vertex, index, constants — most per-frame traffic) can be used in place.

### 3.2 Communication

- **Command rings** in a small shared control block: submit records (DCB/CCB addresses, sizes,
  queue), flip requests, and **event/interrupt records** back from the host (EOP, flips,
  vblank) that the runtime turns into `sceKernel` event-queue wakeups.
- **Doorbells**: Mach semaphores or `os_sync_wait_on_address` on a shared word, so neither side
  spins (today's 11% spin-wait goes away rather than moving).
- **Control messages** (Mach messages over ports exchanged at launch): init, symbol table, page
  protection requests, fault relays, overlay/text input, stats, shutdown.
- **Memory ordering**: Rosetta runs translated threads with Apple Silicon's hardware **TSO** mode;
  the native host does not. All cross-process handoffs use explicit release/acquire atomics; the
  renderer's polling of guest labels (WAIT_REG_MEM etc.) must use acquire loads. This is the main
  correctness risk and gets its own tests.

### 3.3 The 327 HLE functions

| Kind | Examples | Where |
|---|---|---|
| Command builders (write PM4 into guest memory, no GPU state) | `sceGnmDrawIndex`, `sceGnmSetVsShader`, `sceGnmDispatchDirect` … | Compiled into `bb-probe` from the same `gnmdriver` source — no IPC per call |
| Submission / flip | `sceGnmSubmitCommandBuffers`, `sceGnmSubmitAndFlipCommandBuffers`, `sceVideoOutSubmitFlip` | Proxy → ring + doorbell |
| GPU state queries and waits | `sceGnmGetGpuCoreClockFrequency`, `sceVideoOutGetFlipStatus`, `sceGnmSubmitDone` … | Shared-memory status block (cheap reads), message for the rest |
| Event queues | `sceKernelCreateEqueue`, `sceKernelWaitEqueue` with GPU events | Runtime side; host posts events through the ring |
| AvPlayer (FFmpeg video) | `sceAvPlayer*` | Decide in M6: keep in `bb-probe` (x86 FFmpeg) or move to the host |

Classifying all 327 is a mechanical early task (M2). **Done — [`HLE_CLASSIFICATION.md`](HLE_CLASSIFICATION.md):**
Bloodborne imports only **77** of the 327; only **12** need cross-process traffic (5 submit/flip:
`sceGnmSubmitCommandBuffers` (~65–80/frame — ring push + doorbell, no reply),
`SubmitAndFlipCommandBuffers`, `SubmitDone`, `DingDong`, `sceVideoOutSubmitFlip`; 7 load-time
registrations with request/reply), plus 2 shared status values (`GetFlipStatus`,
`AreSubmitsAllowed`). Draws never pass through HLE (the game's static Gnm library writes packets).
Event queues move wholesale into `bb-probe` (the host posts interrupts/flips/vblanks to an event
ring); **AvPlayer stays in `bb-probe`** (guest callbacks, x86 FFmpeg). Timebase: Rosetta's
`rdtsc` equals `cntvct_el0` at 1 GHz on M4 Max (measured in M2-A); pass the process start time.

### 3.4 GPU page tracking across processes

Today the renderer write-protects guest pages in-process and catches the SIGBUS when the game
writes them (~150–800 faults/s in gameplay). After the split:

1. The host asks `bb-probe` to protect/unprotect ranges (batched messages; `runtime_memory_gpu_protect`
   already batches).
2. A fault in `bb-probe` relays the address to the host, which invalidates its cache entries and
   replies; `bb-probe` unprotects and resumes. Round-trip cost is a Mach message pair (to be
   measured in M0; tens of microseconds would be fine at these rates).
3. Read faults (the CPU reading data the GPU wrote) need the host to write data back first — the
   same relay, waiting for completion.

The host's own views stay writable: protection only matters where the game runs.

### 3.5 Window, input, overlay

- `bbgpu-host` owns SDL, the Cocoa window (main thread, as today), the overlay and text input.
- Keyboard and gamepad state are read in the host and published to a shared **pad-state block**
  that `runtime_pad.c` reads (it already polls state rather than consuming events).
- Audio stays in `bb-probe` (SDL audio/CoreAudio work without a window).

### 3.6 Lifecycle

- `bb-probe` spawns `bbgpu-host` (`posix_spawn`, arm64 slice), passes the memory-entry and
  control ports, and waits for "ready" before entering the game.
- If either process dies the other exits; `runtime_restart` restarts both.
- A **single-process fallback** (`BB_GPU_PROCESS=0`, today's design) stays until the split is
  proven, so the game keeps working throughout. *(Removed 2026-10-19: the split is proven, and a
  silent fallback hid failures; a host that cannot start now stops the game with a "Crash:" line.)*

### 3.7 Build

- Second dependency set for arm64 (`deps/arm64`): SDL3, Vulkan loader, fmt, xxHash, miniz,
  Zydis, FFmpeg if AvPlayer moves. MoltenVK is already universal.
- `build_macos.sh` builds `bbgpu-host` (arm64) from today's `gpu/` sources plus a host `main`,
  and the GPU proxy (x86-64) into `bb-probe`.

## 4. Milestones

| # | Milestone | Exit criterion |
|---|---|---|
| **M0** | **Feasibility spike (1–2 days) — go/no-go** | (a) x86-64 and arm64 processes map one memory entry at identical VAs and exchange data correctly under TSO/weak ordering; (b) doorbell and message round-trip latency measured; (c) **the same synthetic Vulkan workload** (~1,600 draws/frame, push descriptors, ~200 render passes) built x86-64 (Rosetta) and arm64, CPU time per frame compared — this number decides whether the project is worth it |
| M1 ✅ | Shared guest memory in the runtime | Game runs single-process on the memory-entry backing (no behaviour change) — done: `pool_entry` in `runtime_memory.c`, zero-in-place `punch()` (resident pages only), `BB_SHARED_GUEST_MEMORY=0` fallback; same FPS and launch time |
| M2 ✅ | arm64 deps, `bbgpu-host` builds, HLE classification | Host starts, opens the window, maps guest memory — done: `deps/arm64`, `out/gpu-arm64/libbbgpu.dylib`, `out/bbgpu-host` (native arm64, ready in ~350 ms), `BB_GPU_PROCESS=1` handshake via `mach_ports_register`, all 20 guest views (4.8 GiB) mirrored at the same VAs with matching checksums; game continues single-process if the host dies; [`HLE_CLASSIFICATION.md`](HLE_CLASSIFICATION.md) |
| M3 | Command path (prerequisite: a portable SRT walker — the shader recompiler JITs **x86** code to read shader resource tables (`FlattenExtendedUserdataPass`/`RegisterWalkerCode`, xbyak/Zydis); on arm64 it hits `UNREACHABLE`, and the pipeline cache serializes those x86 bytes, so the host needs an interpreted walker or an arm64 JIT plus a separate cache) | Title screen renders through the host |
| M4 | Page tracking + events across processes | Gameplay renders correctly; no stale-data glitches |
| M5 | Input, overlay, text input in the host | Fully playable |
| M6 | AvPlayer, restart, crash handling, diagnostics | Feature parity with single-process |
| M7 | Zero-copy textures/buffers from shared memory; remove the 16 GiB arena | Lower memory, fewer copies; benchmark |

## M3 plan (2026-10-12)

**Prerequisite done:** the SRT walker is portable bytecode run by an interpreter on both
architectures (`flatten_extended_userdata_pass.cpp`, `tests/test_srt_walker.cpp`); a load that
faults is zeroed once, through `runtime_fault_recover`. Pipeline cache versions bumped.

**Shared control block** (`host/bbgpu_shared.h`): a second, small Mach memory entry (not guest
memory, mapped anywhere on each side, offsets only). Holds the launch configuration and timebase
(game → host), host facts (label address, HDR support), request reply slots, and two byte rings
of 8-byte-aligned records with release/acquire indices and `os_sync_wait_on_address(SHARED)`
doorbells (with timeouts, so a lost wake or a dead peer cannot hang anyone):

- **Submit ring (game → host)**, one ordered channel for everything: `SUBMIT_GFX`,
  `SUBMIT_GFX_FLIP`, `SUBMIT_DONE`, `DING_DONG`, `UNMAP_COMPUTE_QUEUE`, VideoOut setters and flips,
  and requests that need an answer (`MAP_COMPUTE_QUEUE`, `VO_OPEN`, `VO_REGISTER_BUFFERS`): the
  caller waits on its reply slot, so a request is answered after everything pushed before it.
- **Event ring (host → game)**: `IRQ{id}` (every interrupt the host raises), `FLIP`,
  `EOP_FLIP_QUEUED`, `VBLANK`. A dispatcher thread in `bb-probe` re-raises each IRQ on the game
  side's interrupt controller, so the game's own registrations (`sceGnmAddEqEvent`,
  `ResetSubmissionLock`) run unchanged.

**Game side ("remote" mode of the same GPU library):** GnmDriver/VideoOut register as before but
create no Liverpool, presenter or window. Builders run locally. Submit functions validate, honour
the local `submission_lock` (set by every `SubmitDone`, cleared by the forwarded GpuIdle IRQ —
the host raises GpuIdle after each SubmitDone), then push a record. VideoOut keeps its port
bookkeeping (slots, flip status, event lists) and mirrors every change to the host; flip status
is updated from `FLIP`/`EOP_FLIP_QUEUED` events. `sceVideoOutGetBufferLabelAddress` returns the
host's label array address (P3a): only the host dereferences it.

**Host side:** `bbgpu-host` links the arm64 GPU library and provides the `runtime_*` functions it
imports (memory queries from the mirrored view table, timebase, fault recovery, stubs for
AvPlayer-only ones). A service thread pops the submit ring and calls the real HLE functions; IRQ,
flip and vblank hooks push events. The window, presenter and Vulkan device live here.

**Status (2026-10-12): done — the game's title screen renders through the host (60 FPS, 21 mirror checks match).**
The first in-game run found a real race (fixed for both modes): GpuIdle is raised whenever the GPU
queues run dry, so an idle from a drain *before* the latest `sceGnmSubmitDone` could release the
guest's submission lock; the guest then reused command buffers not yet parsed ("Unimplemented PM4
type 0"). SubmitDone calls are now numbered; the GPU thread publishes the last one it handled and
only a GpuIdle following the latest releases the lock (forwarded GpuIdle events carry the number).
`host/bbgpu_shared.h`, `host/main.c` + `host/host_runtime.c` (the host links the arm64 GPU
library and provides its 18 `runtime_*` imports), `gpu/shim/bbport_split.{h,cpp}`, remote/host
branches in `gnmdriver.cpp`, `video_out.cpp`, `driver.cpp`, `bbgpu.cpp`; `probe.c` calls
`bbgpu_init_remote` when the host is up. The view mirror now uses the submit ring (ordered with
GPU work); AvPlayer invalidations travel as `INVALIDATE` (P8); the host window's keyboard and
the overlay's input capture reach the game through the block (a stopgap until M5). Tests:
`tests/test_bbgpu_rings.c` (x86-64 ↔ arm64: 2 M records in order across 782 wrap-arounds, 500 k
events back, requests 12.8 µs mean) and `tests/test_gpu_split.c` (the real HLE functions in
remote mode against the host: renderer start 0.23–0.38 s warm, SubmitDone → GpuIdle back in
1.5–3 ms, a flip in 13.5–15 ms, vblanks at 62 Hz).

**Not in M3 (M4):** page protection relay. Until then the host never learns about CPU writes
after a resource's first upload, so animated content may be stale; the title screen is the exit
test.

## M0 results (2026-10-11) — `tools/m0_spike/`

**(c) Speed — `vulkan_bench/`.** One game-shaped Vulkan frame (1,690 draws, 270 dynamic-rendering
passes, 1,785 push-descriptor sets, 95 dispatches, 60 submits), same source and the same universal
MoltenVK, built x86-64 (Rosetta) and arm64:

| ms/frame (median, 3×300 frames) | x86-64 (Rosetta) | arm64 | Speed-up |
|---|---|---|---|
| Recording | 0.99 | 0.50 | 2.00× |
| `vkQueueSubmit` (MoltenVK encoding) | 5.55 | 3.23 | 1.72× |
| **Frame CPU** | **6.55** | **3.73** | **1.75×** |
| Generic renderer-style C++ micro-benchmark | | | 1.19× |

Applied to the game's 720p `bb:DrawRec` profile: **~1.35–1.6× faster**, i.e. ~36–48 FPS from 27–30.
Clears the 1.3× threshold (conservative end with little margin; waits don't speed up).

**(a)+(b) Mechanics — `shared_mem/`.** All required mechanisms work between a Rosetta x86-64 and a
native arm64 process:
- Memory entry (`mach_make_memory_entry_64`) mapped at 0x7000000000 in both; aliases coherent;
  parent-side read-only protection with the child's view writable; SIGBUS on parent writes.
- Port hand-off via **`mach_ports_register` + inheritance** (the bootstrap special port is unusable:
  libxpc caches it and the host needs launchd/XPC for Metal and the window server).
- **Ordering:** release/acquire rings: 0 violations in 60M messages; relaxed indices: arm64 reads
  stale payloads ~1 in 20–40k. Every native read of guest-produced data needs acquire semantics.
- **Doorbells:** Mach semaphore / message round trip ~5 µs median (p99 ~9 µs); `os_sync` ~9 µs.
- **Fault relay:** ~23–25 µs per fault vs ~17 µs in-process today (signal delivery under Rosetta
  dominates) — ≤ 2% of a core at the measured 150–800 faults/s.

**Design changes from M0:**
1. Port hand-off via `mach_ports_register` (3.6).
2. Guest-page release must act on the shared entry: today's `punch()` (fresh anonymous pages)
   breaks sharing. Zeroing is only possible from the native side (`MADV_ZERO` fails under Rosetta)
   and rounds to 16 KiB pages (the Rosetta side uses 4 KiB), so the host zeroes aligned 16 KiB
   blocks and the runtime `memset`s unaligned edges (or just `memset`, ~25–45 µs/MiB).
3. Doorbells: Mach semaphores/messages, not `os_sync`.
4. Release/acquire on every cross-process handoff — confirmed mandatory by experiment.

## 5. Expected gain — honestly

- Rosetta typically runs compiled C/C++ at roughly **60–80% of native speed**; heavily branchy,
  pointer-chasing code like MoltenVK's encoder and the renderer can fare worse, plus the cost of
  TSO mode. So the bottleneck thread could speed up by roughly **1.3–1.7×**, i.e. from 27–30 FPS
  to **~35–50 FPS** at 720p, before zero-copy savings. The spike (M0c) replaces this estimate
  with a measurement before any real porting.
- New costs: page-fault relays and doorbells. Expected small at the measured rates; M0b checks.
- Native 1080p would remain GPU-bound (~22–25 FPS) — upscaling stays the way to play.

## 6. Risks

| Risk | Mitigation |
|---|---|
| TSO vs weak memory ordering bugs (intermittent, hard to see) | Explicit atomics at every handoff; M0a tests; audit renderer polling of guest memory |
| Synchronous HLE calls that need host state become slow | Classification (3.3); status block in shared memory for queries |
| Fault relay latency under heavy write tracking | Batch protection; measure in M0b; zero-copy (M7) reduces tracking |
| Two dependency sets, two processes to debug | Shared build script; host logs prefixed; fallback mode |
| Engineering size (estimate: **several weeks** after a successful spike) | Milestones each leave the game working (fallback mode) |
| It may not be enough on its own | M7 zero-copy and fewer submissions (66/frame) remain available on top |

## 7. Alternatives considered

| Option | Why not first |
|---|---|
| B. Native Metal backend (replace Vulkan + MoltenVK) | Removes MoltenVK's ~51% but keeps Rosetta on the renderer; rewrite of a ~50k-line backend; long time to first pixel. Still possible later inside `bbgpu-host`. |
| More threading/tuning in one process | Tried three ways, no gain: the frame is serialised through translated code. |
| Zero-copy memory alone | Worth doing, but removes copies/spin-waits (~11–15%), not the MoltenVK encode cost. Folded in as M7. |
| Recompiling the game to arm64 | Out of scope (decision 1): no source, static recompilation of the eboot is a research project. |

## 8. Decision requested

1. Approve **M0 (spike)** only — 1–2 days, produces the measurement that decides the rest.
2. Or approve the whole plan conditional on M0 meeting a threshold (suggested: the arm64 build
   of the synthetic workload is **≥ 1.3× faster** than the Rosetta build).
