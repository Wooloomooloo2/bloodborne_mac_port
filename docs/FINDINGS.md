# macOS Port — Findings

What it took to get Bloodborne running on Apple Silicon, and what every test since has shown.
Hardware: Apple M4 Max, macOS 26.6. Game: CUSA03173 v1.09. All numbers are from 2026-10-11
unless stated. Plan: [`MACOS_PORT.md`](MACOS_PORT.md); deferred work: [`MACOS_BACKLOG.md`](MACOS_BACKLOG.md).

## Summary

- The game **boots, renders and plays** on macOS: title screen, character creation, saving, and
  the first level, with DualSense input and CoreAudio sound.
- Everything runs as **one x86-64 process under Rosetta 2** (the game's code is PS4 x86-64
  machine code) with the shadPS4-derived Vulkan renderer on **MoltenVK**.
- Performance in a fixed test scene: **22–25 FPS at native 1080p**, **27–31 FPS** with FSR 3.1 or
  **MetalFX** upscaling from 720p. At 720p the limit is one CPU thread recording GPU commands.

## Part 1 — Getting it to run at all

Each item below was a hard stop until fixed, roughly in the order they were hit.

### Toolchain and dependencies

| Problem | Finding / fix |
|---|---|
| Game code cannot be recompiled | It is PS4 machine code with no source. The whole process (game + runtime + GPU library) is built **x86-64** and runs under Rosetta 2; deployment target macOS 15 (Rosetta's AVX2 support). |
| arm64 Homebrew libraries leak into an x86-64 build | All dependencies (SDL3, Vulkan loader, fmt, FFmpeg, Zydis, …) are built x86-64 into `deps/x86_64` by `scripts/macos/bootstrap_deps.sh`, with an isolated `PKG_CONFIG_LIBDIR` and `CMAKE_IGNORE_PREFIX_PATH=/opt/homebrew`. |
| Zydis' installed CMake config needs Zycore, which it doesn't install | Zycore is built and installed separately (same bundled copy). |
| `xcrun --find clang` cannot find the SDK | Use the `/usr/bin/clang` shims, which locate the SDK themselves. |

### Build system and GPU library (`gpu/`)

| Problem | Finding / fix |
|---|---|
| CMake assumed Windows or Linux+X11 | Apple branch: no X11, `-undefined dynamic_lookup` (runtime symbols come from the loader), `VK_USE_PLATFORM_METAL_EXT`, Metal/QuartzCore/MetalFX frameworks. `build_macos.sh` builds everything. |
| Linux-only APIs in the GPU library | `gettid`, CPU affinity, `SCHED_IDLE`, `RUSAGE_THREAD`, `pthread_getcpuclockid`, `ucontext` register names — macOS equivalents (Mach thread ids/times, QoS classes, `uc_mcontext->__ss`). |
| libstdc++-only C++ | `std::bitset::_Find_first/_Find_next`; `atomic_ref<const T>`; `u64` vs `unsigned long` casts. |
| Embedded font in assembly | Mach-O needs `_` symbol prefixes and `__TEXT,__const`. |

### Runtime (`src/`) — the PS4 emulation layer

| Problem | Finding / fix |
|---|---|
| **Guest TLS.** The game reads its thread block through `fs:[0]`; Linux rewrites that to `gs:[0]` and sets the GS base with `arch_prctl`. On macOS, GS points at the pthread TSD array and cannot be moved. | The loader rewrites the 17,127 `fs:[0]` loads to `gs:[0xc00]`; the runtime reserves **pthread key 384** at startup and stores each thread's guest TCB there. First free key under Rosetta is 258, with SDL/MoltenVK loaded ~270. |
| **Address space.** The game packs pointers into 40 bits, so guest memory must sit below 1 TiB. | macOS keeps **`[0xfc0000000, 0x7000000000)` (63–448 GiB) as a permanent no-access hole** in every process. Guest memory now starts at 0x7000000000; the whole range is reserved up front (large host `mmap(NULL)` calls otherwise land in it). |
| No `memfd`, `fallocate`, `MAP_FIXED_NOREPLACE` | One anonymous backing region; guest views are `mach_vm_remap` aliases; freeing = fresh anonymous pages mapped over the range; low maps by hint-and-verify. Page size under Rosetta is 4 KiB. |
| Write faults | A write to a write-protected page arrives as **SIGBUS** on macOS, not SIGSEGV; the GPU's page tracking handles both. |
| Thread dumps, restart, timed locks, clocks | `task_threads` + `pthread_kill`; close-fd loop; trylock + backoff for timed mutex/rwlock; monotonic sleep loop; `pthread_cond_timedwait_relative_np`; `arc4random_buf`. |
| Mach-O assembly | Top-level asm names need the `_` prefix (`SYM()` macro). |
| Windows-fork regressions also broke Linux | A `DWORD` log outside a Windows guard, an undeclared `struct stat`, tests needing `restore_guest_fs`. Fixed; Windows support later purged from our code. |

### First boots

| Stop | Finding / fix |
|---|---|
| `--cpu-only` crashes in graphics setup | Expected on any platform: it skips the GPU library, so `sceGnm*` calls return NULL. The CPU side (TLS, memory, ~30 threads, FMOD audio) works up to graphics init. |
| "No available video device" | **Cocoa only works on the real main thread.** The game now runs on a 64 MiB-stack worker thread and the main thread runs `CFRunLoopRun()`; the GPU library posts its window loop to the main queue. `dispatch_main()` does *not* work (it exits the main thread). |
| `ErrorIncompatibleDriver` | MoltenVK is a portability driver: needs `VK_KHR_portability_enumeration` + the enumerate-portability flag, and `VK_KHR_portability_subset` on the device. shadPS4 also overwrote `VK_DRIVER_FILES`. |
| Device creation: `robustBufferAccess2` | MoltenVK lacks **`robustBufferAccess2`** and **`nullDescriptor`**. Option A chosen: create the device without them, bind a zero-filled stand-in buffer for null bindings. Shader-side bounds checks are in the backlog (Stability). |
| `Failed to get buffer device address` | MoltenVK has **no sparse binding**; the buffer cache mirrored the 40-bit GPU address space with sparse 4 GiB arenas. macOS uses one ordinary 16 GiB arena at the guest base — **Metal commits its memory lazily** (measured: a 16 GiB buffer adds ~0 to the footprint until touched). Zero-copy guest memory is the top Performance backlog item. |
| Corrupted shader cache | A run that crashed mid-write leaves a bad cache; deleting it fixes it. |
| ~520 "Command buffer cannot accept commands before vkBeginCommandBuffer" | **Metal cannot write compressed (BC) textures**, even through uncompressed views; MoltenVK rejected BC images created with storage usage. They are filled by buffer copies anyway, so macOS drops the flag. This cleared the errors and got the game **through character creation into the first level.** |
| Pipelines Metal rejects | Metal is stricter than desktop drivers: a fragment input the vertex stage doesn't write (MoltenVK has **no geometry shaders**, which the renderer skips), clashing buffer indices. Rejected pipelines are now logged and their draws skipped instead of stopping the game. |
| `Unknown data_format=10 num_format=7` | An impossible format (8-bit "float") from a garbage or unused descriptor; macOS now uses a same-size stand-in. |

### False alarms worth knowing about

- **The "job-pool crash"** (SIGBUS at guest 0x53b3260): every instance came right after the
  test-run timeout (`BB_TIMEOUT`) fired. Its stack dump derails a worker thread. Normal play has
  no timeout and never hits it.
- **"Unimplemented PM4 type 0"** (intermittent, often when loading a save): a diagnostic showed the
  command buffer's contents identical through the guest's view and the backing store — *not* a
  macOS memory-aliasing bug. The bad word is the tail of the previous packet: the parser fell one
  word out of step, most likely by reading a packet header while the game was still writing it
  (a CPU/GPU-thread race that Rosetta's timing may expose more often). Under investigation; the
  diagnostic now also re-walks the buffer to confirm.

## Part 2 — Performance investigation

### Starting point

| Test | Result |
|---|---|
| Native 1080p, first level | ~21–23 FPS, frame time 42–57 ms |
| Same game, Intel Core Ultra (358H) with Arc graphics, Windows port | 40–50 FPS at 1080p, no upscaling (owner's reference) |

### What it is *not*

| Hypothesis | Test | Result |
|---|---|---|
| Logging | Line counts per run | ~700 lines/minute, warnings only — negligible |
| Page-protection faults (GPU write tracking) | `BB_FRAME_STATS=1` | ~140–800 faults/s, 0 in the GPU thread — negligible |
| Asynchronous submission | `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0` | 21–22 → 23–25 FPS at native; no measurable change at 720p |
| Draw-preparation worker pool | `BB_PREP_WORKERS=0` | 21 → 23 FPS (lock contention; workers ran at background QoS on E-cores) |
| Render-pass count on a tile-based GPU | Metal System Trace + `BB_PASS_STATS=1` | See below — **theory disproved** |

### CPU side (native 1080p, `sample` of the draw-recording thread `bb:DrawRec`)

| Share of thread time | Where |
|---|---|
| ~47% | `vkQueueSubmit` at frame flip — MoltenVK encoding the whole frame into Metal, synchronously |
| ~21% | Renderer per-draw work (resource and texture binding) |
| ~12% | Spin-waiting for host copies (`cthread_yield`) |
| ~9% | Idle |

The thread is busy ~35–40 ms per frame (~1,600 draws, ~95 dispatches, 66 submissions).

### GPU side (Metal System Trace, 3 s in a ~23 FPS scene)

| Measure | Result |
|---|---|
| GPU busy | **84%** of wall time (fragment 69%, vertex 14%, compute 6%) |
| Metal render passes per frame | **~287**, ~32 ms fragment time per frame |
| Pass duration distribution | 62% of passes take < 0.02 ms (3.5% of time); **15% take 0.25–2 ms and hold ~72% of time**; the top 5% hold 49% |
| Needless pass splits (same targets reopened right after a break) | ~18% of passes (~40/frame), 85% of them after mid-frame buffer copies |

**Correction:** the first reading of ~287 passes suggested per-pass load/store overhead on Apple's
tile-based GPU. The duration distribution showed the opposite — most passes are nearly free, and
the cost is real shading work in a minority of heavy passes. Reducing pass count would gain little.

Per-pass GPU timing tools that do **not** work in this setup: the renderer's `BB_GPU_PROFILE=1`
(MoltenVK can't place timestamps between draws — every pass reads 0 ms), `BB_VK_MARKERS=1`
(per-draw labels only, no named passes), attaching Xcode (its process list hangs on this
process), and a Metal frame capture to file (`BB_GPU_CAPTURE`; Apple's capture tool crashes in
`DownloadHeap`).

### Resolution and effects

| Test (same scene) | Result |
|---|---|
| Render at 1280×720 (`BB_RENDER_RES`) | Fastest of the tests — cost scales with pixels |
| Game effects off (SSAO, DoF, motion blur, dynamic shadows, chromatic aberration) | A little faster — no single pathological effect |
| Upscaler presets via the **live** path at 1080p output | No FPS difference between Quality/Balanced/Performance: the live path keeps the guest at 1080p and copies scene proxies back, cancelling the saving on this GPU |

So the GPU was **pixel-shading bound** at native 1080p. On macOS, presets at a 1080p output now
patch the game to render the whole frame at the preset size (a restart per change);
`live_resolution=1` keeps the live path.

### Upscaling benchmark (same scene: characters walking into the distance; start → end FPS)

| Mode | FPS | Image |
|---|---|---|
| Native 1080p, no upscaler | 22 → 25 | Reference |
| FSR 3.1 Quality (720p → 1080p) | 28 → 31 | Softer, some smearing, less shimmer |
| MetalFX Quality (720p → 1080p) | 27 → 30 | Sharper, more distant shimmer |
| FSR 3.1 Quality + async submits | 27 → 31 | Same as without |
| FSR 3.1 Balanced (live path, earlier, different scene) | 38–45 | Not comparable (different scene and path) |

**Profile at 720p (MetalFX Quality, `sample` of `bb:DrawRec`):** ~51% inside `vkQueueSubmit`
(MoltenVK encoding Vulkan → Metal, incl. 8% creating Metal render encoders and ~15% per-draw
state/descriptors), ~11% spin-waiting on host copies, ~6% idle, and only ~9% the renderer's own
per-draw work. The GPU command thread spends ~73% waiting for `bb:DrawRec`; the Vulkan recorder
thread is ~76% idle.

| Experiment at 720p | Result |
|---|---|
| `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2` (encode on the recorder thread as recorded) | 27 → 30 FPS, unchanged |

The frame is effectively serialised through Rosetta-translated code: the draw thread regularly
syncs with the recorder thread (direct command-buffer access), so moving MoltenVK's encoding to
another thread moves the wait, not the work.

Rendering 44% of the pixels gains only ~25%: at 720p **the GPU is no longer the limit — the
draw-recording CPU thread is** (~33–40 ms of work per frame). MetalFX and FSR are therefore
equally fast.

### Native-process feasibility spike (M0)

See [`DESIGN_NATIVE_GPU_PROCESS.md`](DESIGN_NATIVE_GPU_PROCESS.md#m0-results-2026-10-11--toolsm0_spike):
a game-shaped Vulkan frame costs **1.75× less CPU natively** than under Rosetta (MoltenVK
encoding 1.72×, recording 2.0×; generic C++ only 1.19×); cross-process shared memory, protection,
doorbells (~5 µs) and fault relays (~24 µs) all work.

**Measurement gotcha:** under Rosetta, `CLOCK_THREAD_CPUTIME_ID` is badly wrong (reported 21 ms
for 871 ms of work). Use `thread_info(THREAD_EXTENDED_INFO)` / Mach thread times instead.

### Native GPU host (M2, 2026-10-12)

`BB_GPU_PROCESS=1` starts `out/bbgpu-host` (native arm64): ready in 350–380 ms, maps the guest
memory entry at the same addresses (0x7000000000–0xfc00000000) and mirrors all 20 guest views
(4.8 GiB); 9 periodic checksum comparisons per run all match. Killing the host leaves the game
running single-process. Without the variable nothing changes (same menu run, same warnings).

### GPU process split (M3, 2026-10-12, without the game)

Shared control block between `bb-probe` (x86-64/Rosetta) and `bbgpu-host` (arm64): a ring
moves ~1.2 M variable-size records/s in order; a request/reply round trip averages 12.8 µs.
With the real HLE functions in remote mode (`tests/test_gpu_split.c`, warm caches): renderer
start in the host 0.23–0.38 s, `sceGnmSubmitDone` → host GpuIdle interrupt → submissions
unlocked in 1.5–3 ms, a flip presented in 13.5–15 ms (≈ one 60 Hz vblank), vblanks at 62 Hz.
Cold (no pipeline cache) the first flip took 3 s, and the present thread then caught up on the
vblanks it missed in a burst (existing behaviour, not split-specific).

**In the game (2026-10-12):** with `BB_GPU_PROCESS=1` the title screen renders through the
native host at 60 FPS (same as single-process). The first run crashed with "Unimplemented PM4
type 0": a GpuIdle from an earlier queue drain released the submission lock after
`sceGnmSubmitDone`, and the game rewrote command buffers the host had not parsed yet (the
diagnostic showed memory consistent but the submitted size cutting a packet). The same race
exists, with a much smaller window, in single-process mode — the likely cause of the
intermittent crash on loading a save. Fixed in both modes by numbering SubmitDone calls.

### GPU time per render pass (2026-10-12, city scene, split mode)

`BB_PASS_LOG=<file> BB_PASS_LABELS=1` + a Metal System Trace of `bbgpu-host` + `tools/pass_trace.py`
attribute GPU time to render passes (MoltenVK names render encoders after their command buffer,
numbered here; tessellated draws split a pass into several encoders, which the tool merges).
Per frame: fragment 25.5 ms, vertex 5.5 ms, compute 2.2 ms (MetalFX ~0.5 ms, tessellation
emulation ~0.3 ms). **Biggest single cost: ~20 passes per frame with no attachments and no pixel
shader (vertex shaders that only write memory) kept the device-maximum render area, 16384×16384:
~0.3 ms of tiling each, ~6 ms per frame.** A 16×16 area for such passes: **27.5 → 33.9 FPS**,
image unchanged (toggle `NoOutputPassArea`, bit 58).

**G-buffer splits** (`BB_PASS_STATS=1`): ~38 per frame reopen the same attachments; 76% came
right after `CopyBuffer` (uploads of CPU-written guest data into the buffer arena, which Vulkan
forbids inside a render pass). Upper bound measured by dropping in-pass copies (wrong image):
43 FPS. **Upload hoisting** (toggle `UploadHoisting`, bit 59): an in-pass upload into arena pages
nothing in the submission has used yet is recorded into an upload command buffer submitted ahead
of the submission (barriers before and after), and the pass stays open: upload-caused splits
983 → 560/s, **33.9 → 34.8 FPS**, image unchanged. The remaining splits follow uploads into pages
already used in the submission (4 KiB tracking) and HLE copy shaders (`MultiCopy`, GPU data,
not hoistable).

### Pipeline cache was wiped every launch (2026-10-13)

Frame stats counted ~500 shader/pipeline compiles in every session in the same area (365 at the
level load, the rest mid-game: the user's "shader stutter when attacking"). `PipelineCache::WarmUp`
cleared the whole cache whenever any one cached pipeline failed to preload, and one always did. Now
it clears only for old-format entries and re-saves the cache profile: next session **preloaded 226
of 227 pipelines, 2 compiles during play** (was 517). The native host also used the wrong cache
directory (`BB_GPU_USER_DIR` is read when the library loads; `run.sh` now exports it).

### "Precise" float math made every pixel shader unoptimizable on Metal (2026-10-13)

`MVK_CONFIG_SHADER_DUMP_DIR=<dir>` (with an empty `BB_GPU_USER_DIR`, so every shader converts)
dumps the SPIR-V and MSL; the pass log names each pass's shaders, and matching the SPIR-V bytes
of `<user>/cache/*.spv` to the dump maps guest hashes to MSL. The hottest G-buffer pixel shader
(1.8 ms/frame) did every float add, multiply and FMA through `spvFAdd`/`spvFMul` helpers marked
`[[clang::optnone]]`: SPIRV-Cross's translation of SPIR-V `NoContraction`, which the recompiler
put on every FP add/sub/mul/fma (`emit_spirv_floating_point.cpp`) — in 223 of 308 shaders.
Now only vertex/tessellation/geometry stages keep it (positions must match bit for bit across
the passes that draw the same geometry: depth prepass, G-buffer, shadows):

| Respawn spot, Central Yharnam (1379 draws/frame) | FPS | GPU busy/frame | fragment | vertex | compute |
|---|---|---|---|---|---|
| precise everywhere (before) | 39.4 | 19.6 ms | 13.4 | 5.3 | 2.3 |
| precise in vertex stages only (now) | 60 (vsync cap) | 12.1 ms | 3.7 | 7.2 | 2.4 |
| precise nowhere + invariant position | 60 (vsync cap) | 12.4 ms | 5.4 | 5.1 | 3.5 |

(Per-stage times overlap on a tile-based GPU; the union "GPU busy" is the comparable number.)
Dropping it from vertex shaders too gains nothing more, so they keep it. Screenshots are the same
(mean difference 3.9/255, MetalFX history and idle animation). Single-process mode: 60 FPS too.
The scene is now held by the 60 Hz present, with the GPU ~72% busy. `BB_SPIRV_PRECISE=all`
(with a fresh shader cache) restores the old code. This corrects the 2026-10-11 conclusion that
the remaining fragment time was "real shading": most of it was this.

### Resolution targets on the M4 Max (2026-10-13, Central Yharnam respawn spot)

Target: a locked 60 FPS (the game's physics run at 30 Hz; more is not wanted), as high a
resolution as that allows. Uncapped for measurement: `BB_PRESENT_MODE=Immediate BB_FPS_LIMIT=0`.

| Output | Scene | Path | FPS uncapped | Limit |
|---|---|---|---|---|
| 1080p MetalFX Quality (default) | 1280x720 | startup patch | 65–77 (60 locked) | — |
| 1080p native | 1920x1080 | — | 61–63 | GPU timeline incl. render-pass overhead (see frame timeline) |
| 4K MetalFX Performance | 1916x1078 | startup patch | ~52 | GPU 69% busy (not yet broken down) |
| 1440p native | 2560x1440 | live | 49 | GPU (~17.5 ms of a 19–20 ms frame) |
| 4K MetalFX Quality | 2560x1440 | live | 43 | GPU |

- **The startup resolution patch breaks scenes above 1080p** (2560x1440: polygons stretched across
  the screen, both process modes; 1916x1078 is fine). `run.sh` now takes the live path above
  1080p, which renders correctly. Earlier 1440p/4K-Quality numbers (33/30 FPS) measured the broken
  image.
- Draw recording thread (`bb:DrawRec`, 1080p native): 43% in `vkQueueSubmit` (MoltenVK encodes
  Metal commands there), 12% beginning Metal render passes, ~4% MoltenVK formatting encoder
  labels from command buffer names (now unnamed). `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0`: no
  gain (62 vs 62 FPS). `BB_FRAMES_AHEAD=2`: no gain at 1440p (49.2 vs 49.1).
- Frame times snap to 480 Hz vblank ticks (2.08 ms): 49 FPS is 10 ticks per frame.
- Dropping NoContraction from vertex shaders too ("precise nowhere", with invariant positions)
  broke geometry: some vertex shaders need exact rounding. Vertex stages keep it.

**Frame timeline (2026-10-13, `BB_FRAME_STATS=1` "Frame timeline" line, 1080p native uncapped,
~61 FPS):** per 16.4 ms frame the draw recording thread records for ~5.4 ms, spends 4.8 ms in the
flip's `vkQueueSubmit` (MoltenVK encoding the frame), **waits 4.5 ms for the GPU to finish the
previous frame** (frames-ahead bound) and idles 1.7 ms; the command thread waits ~10 ms for it.
Each flip finishes on the GPU ~20 ms after its submit, one every ~16.4 ms. With
`BB_FRAMES_AHEAD=2` the latency doubles (38 ms) and the rate stays (16.8 ms): **the GPU timeline,
not the CPU, limits 1080p native**, so CPU work (encoding, recording) would not raise it.

**Why the GPU timeline is longer than its busy time:** a Metal System Trace shows the GPU idle
~3.8 ms per frame (18–24% of wall time), in 0.3–2 ms gaps inside one command buffer, almost all
right after a render pass's fragment work, **recurring every ~115 Metal encoders** (every ~85 with
a pass per draw) whatever the passes around them are. The frame has **~408 encoders**: 190 render
passes, 81 buffer-copy blits, 77 compute encoders (one per dispatch), 47 tessellation compute
encoders (MoltenVK's tessellation emulation, each also splitting its render pass). A standalone
Metal test (`tools/metal_encoder_cost/encoder_cost.m`, 1080p, 3 attachments loaded and stored,
same small draw): **a render pass costs ~13–16 µs of GPU time** (8.6 µs each at 50 per command
buffer, 14–16 µs at 100–400) **against ~0.7 µs for the same draw inside one pass**; a compute
encoder costs ~1 µs more than a dispatch in a shared encoder; a blit encoder ~4 µs. So ~190
passes are ~3 ms of fixed GPU cost per frame, independent of resolution — the lever for both
1080p headroom and 1440p (GPU ~17.5 ms; 60 needs ≤ ~15).

**Pipeline preload:** the 217 cached pipelines preload serially in 0.29–0.39 s at start (macOS's
own shader cache warm); pipelines the game has not used before compile on the GPU command thread
when first drawn, ~1–3.5 ms each (more when Metal's cache is cold): the in-game hitches.

**Why render passes end (2026-10-13, `BB_PASS_STATS=1`, 1080p native, ~190 passes/frame):**
~33/frame other attachments, ~45 a target about to be sampled (render-to-texture), ~26 buffer
uploads (`CopyBuffer`), ~15 the game's GPU copies (`MultiCopy`), ~22 dispatches; only ~32/frame
reopen the same attachments right after (mostly after copies). Per frame the game also issues
**~37 tessellated draws and ~32 rect/quad-list draws** (emulated with tessellation), each of which
MoltenVK splits the render pass around (compute for the vertex/control stages). Upper bound at
1440p native (`BB_DROP_TESS`, wrong image): dropping the tessellated draws 49.1 → 51.9 FPS
(~1.1 ms/frame including their own shading). Dropping rect lists is not measurable that way (they
include full-screen post passes; the game stalled at the title). So pass merging is worth roughly
0.5 ms (copy splits) + ≤ 1 ms (tessellation) + ≤ ~1 ms (rect lists) at 1440p, short of the ~3.7 ms
1440p needs for 60 on its own.

**1440p is held by GPU idle time, not GPU work (2026-10-13, Metal System Trace, 1440p native live,
uncapped ~48–49 FPS):** the GPU is busy **15.5 ms per 20.7 ms frame** (vertex 6.0, fragment 9.1,
compute 1.9 — already under the 16.7 ms that 60 FPS needs); it idles ~3.5 ms per frame in gaps
between encoders inside the frame's command buffer and ~1 ms between command buffers. At 1080p
the vertex stage is the largest (5.9 ms vs fragment 4.7; the main G-buffer pass 1.6 ms vertex,
0.4 ms fragment). Dropping precise math from vertex shaders too (`BB_SPIRV_PRECISE=none`) with
Metal fast math off everywhere (`MVK_CONFIG_FAST_MATH_ENABLED=0`, so positions stay consistent
between passes): geometry correct, GPU 15.5 → 14.9 ms — only ~0.4 ms of vertex time; fast math off
alone costs ~0.2 ms of fragment time. Uncapped FPS cannot show gains under ~2 ms: frame times snap
to 480 Hz ticks (49 FPS = 10 ticks). Measure GPU time (`gpubusy`) instead.

**Zero-copy feasibility:** Metal on the M4 Max wraps 8 GiB of existing host memory as one buffer
without copying (`newBufferWithBytesNoCopy`; `maxBufferLength` 28.1 GiB); the guest's GPU-visible
pool is 6.4 GiB. Importing it would remove the per-frame upload copies (81 blit encoders, ~26 pass
splits), the host-copy waits and most write-tracking faults.

**Background pipeline creation (2026-10-13).** With an empty shader cache (macOS's own Metal
cache warm), a new shader cost ~1.2 ms: ~70% creating the pipeline (MoltenVK SPIR-V→MSL and the
Metal compiler), ~30% our translation. `FreePermIndex` (a `stat()` per probe) ran on every program
lookup and cost more than both during loads; now only for new permutations. Pipeline creation now
runs on a 4-thread pool (`BB_PIPELINE_THREADS`): level load from an empty cache, compile time on
the GPU command thread 440 → 130 ms; the render path waited ~260 ms for the pool with
`shader_compile=wait` (worst loading frame 248 → 187 ms) and ~35 ms with `skip` (76 ms; ~2,000
draws skipped during the load, none after). Warm cache: 217 pipelines built in the background,
nothing waited, start-up preload step 290–390 → 110 ms. Gameplay images correct in both modes.

**Pipeline cache permutations (fixed):** Metal rejected 6 pipelines per session once the cache was
kept between sessions ("cannot reserve 'buffer' resource location at index 0"): a new shader
permutation took the index of a stored one the session had not loaded and overwrote it, and
pipelines saved earlier then loaded a shader numbered for other bindings; their draws were
skipped (part of a G-buffer pass). Also, permutations of shaders whose resources were all null
ignored their first binding. Four sessions alternating 720p/1440p: 0 rejected.

Instruments' GPU limiter counters cannot be recorded headless: the Metal System Trace template's
`counterprofile` values 1–3 are "not supported on target device", and the default profile records
only "RT Unit Active".

**Host crash found while profiling (fixed):** hoisted uploads (`Scheduler::RecordHoistedCopy`) were
recorded on the draw thread into a command buffer from the same `VkCommandPool` the recorder thread
was recording into; MoltenVK allocates command objects per pool, so the host died with SIGSEGV in
`MVKCmdCopyBuffer::setContent` (address 0x20). Upload command buffers now come from their own pool.

**Rosetta gotcha:** attaching lldb to a translated process and killing it wedged Rosetta for
hours (new x86 processes hung in state U); it recovered on its own.

### Copy-shader pass splits and the zero-copy premise (2026-10-13)

Checked before starting zero-copy: which per-frame copies end render passes (`BB_PASS_STATS=1`,
`Runtime::CopyBuffer` now names its caller). At 1080p **all ~26 buffer copies per frame that end
a pass come from the game's own copy shader** (`ExecuteCopyShaderHLE`), plus ~15 from its
multi-range form (`MultiCopy`). Buffer-cache uploads already run ahead of the pass (upload
hoisting, ~17 per frame), so zero-copy would not remove these splits.

- Copy-shader copies are now hoisted like uploads when neither range was used earlier in the
  submission (`Runtime::HoistCopy(..., shader_copy)`, toggle bit 60). Only ~3 per frame
  qualify: the ~24 that break passes rewrite entries of one ~37 KB table (`0x70d1ce3b40`)
  between the draws that read it, a true dependency. On a tile-based GPU a write between draws
  cannot stay inside one pass (vertex work of the whole pass runs before its pixels), so
  keeping them in the pass would need a versioned copy of the table per draw group.
- Bound (`BB_HOIST_UNSAFE=1`, wrong image), 1440p native live, Metal trace: GPU busy
  15.3 → 14.3 ms/frame, FPS 48.7 → 49.0. The splits cost ~1 ms of GPU time and are not
  what holds 1440p.
- 1440p pacing: frames finish every ~20.5 ms with 15.3 ms of GPU work. The recording thread
  spends ~8 ms per frame in the frames-ahead wait and ~5.4 ms in `vkQueueSubmit` (MoltenVK
  encoding, 3 submits per frame); `BB_FRAMES_AHEAD=2` does not help (39–47 FPS). Frame times
  snap to 2.08 ms ticks (480 Hz), so 53 FPS needs ≤ 18.75 ms.
- Zero-copy risk found on the way: EOP/fence writes run when the draws are *recorded*, not when
  the GPU has run them. Copies snapshot the data; reading guest memory in place would race with
  the game reusing its buffers unless fences are signalled at GPU completion.

### Where the 1440p GPU idle time is (2026-10-13 evening, on battery)

Metal System Trace, 1440p native live, ~49 FPS; GPU busy 15.3 ms of a ~20.5 ms frame.

- **Not CPU starvation.** Comparing each Metal command buffer's commit time
  (`metal-application-command-buffer-submissions`) with the GPU timeline: the GPU waits for a
  commit 0.1 ms/frame. Command buffers are committed a median 13.6 ms before the GPU reaches
  them. Encoding earlier or in smaller submits would not help.
- **~4.4 ms/frame of idle with work queued**, as ~4–5 stalls of ~1 ms each inside the frame's
  main command buffer (~580 encoders), at **fixed positions every frame** (before encoder ~110,
  ~385, ~506, smaller at ~238). With `BB_PASS_LOG` they sit around the 2560x1440 G-buffer passes
  (vs 4cd21a0f, c6cdba2b) and the 5x5/10x10 B10G11R11 downsample chain, i.e. even between tiny
  passes.
- Ruled out: other processes' GPU work during the stalls (WindowServer etc.: ~0), GPU
  performance-state changes (82 of 601 stalls near one), driver memory wiring (4 of 584), and
  a per-command-buffer encoder limit (synthetic: no step at 128 encoders, also with mixed
  render/compute/blit encoders and 48 targets).
- **Measurement caveat:** the Mac ran on battery from ~14:00 (pmset log). The GPU performance
  state varies from run to run (Minimum/Medium/Maximum, on mains as well), and on battery a
  synthetic render pass cost ~32 µs instead of 13–16 µs. Compare variants within one session
  and check `gpu-performance-state-intervals` in each trace.
- Tools (scratch, not in repo): commit-vs-GPU join, stall position per command buffer, stall
  labels via the `pass_trace.py` join.

### Shader compile hitches during play: `balanced` mode (2026-10-13 evening, on mains)

Ahead-of-time compiling of the game's shaders is not possible: the GCN→SPIR-V translation and the
pipeline key depend on draw-time state (vertex formats from the fetch shader, buffer/image
descriptors, render-target formats and blend), so only the persistent pipeline cache (already
preloaded in the background at start) can hold "precompiled" work. What remains is the frame
that first meets a new pipeline. Measured with an empty pipeline cache (`BB_GPU_USER_DIR` in a
scratch folder) and a scripted walk after loading the save (`BB_PAD_FILE` `ly=0`/`rx=`):

- With macOS's Metal compiler cache warm (shaders this Mac compiled before), a new pipeline costs
  ~4 ms on the creation threads; walking met ~20 of them and frame pacing stayed within
  p99 17–22 ms in both modes. One run hit a cold Metal cache: **~110–150 ms per pipeline**, and
  walking froze frames for 70–270 ms (the player's "shader caching really hurts"). Renaming the
  host executable does not reliably give a cold Metal cache.
- Simulated cold compiles (`BB_COMPILE_DELAY_MS=100`, test knob), same walk:

| `shader_compile` | Frames over 40 ms while walking | Worst | Draws skipped |
|---|---|---|---|
| `wait` | 15 (120–190 ms each) | 190 ms | 0 |
| `balanced` (6 ms budget per frame) | 1 | 41 ms | ~700 over ~20 s, a few frames per new pipeline |

`balanced` (now the default) waits for pipelines still compiling out of a budget refilled at
`BB_COMPILE_BUDGET_MS` (default 6) per 60 Hz frame, never more than one frame's worth, and skips
the draw once it is spent. Images correct after the walk. Compute pipelines always wait.
`BB_MENU_OPEN=1..3` opens the settings menu on that tab at start (menu screenshots).

The compile stats used the arm64 cycle counter as nanoseconds: right on the M4 (1 GHz), 41×
too low on M1–M3 (24 MHz); they use `steady_clock` now.

### Load crashes, 62-load hunt (2026-10-14)

Scripted save loads (`BB_PAD_FILE`, Continue twice) with an empty pipeline cache, as in the two
"Bad PM4 packet" crashes of 2026-10-13 evening: **62 loads, no bad packet**: 42 with a ring of
every `libSceGnmDriver` call (pointer, size, result, thread) and 20 without, each DCB/CCB
walked at submit in the game process, each arena copy hashed when the host receives it. One
other crash: `MapNumberConversion` UNREACHABLE for an SnormNz descriptor with data format 0
during the load (now a one-time warning; reads as zeros on the GPU). The bad packet's two
diagnostics stay on for the next occurrence (MACOS_BACKLOG).

The next walk run (2026-10-14) hit it while loading, and the submit check caught it in the
game process: a 506-word DCB whose last 448 words were never written (zeros), right after a
correct `sceGnmSetCsShader`. The command processor now skips type-0 packets as the hardware
does (logged once per buffer); see MACOS_BACKLOG for what remains.

### Load crash: root cause (2026-10-14 evening)

Test loads of the same save gave very different rates: 9 of ~20 walks in the morning, 0 of
~100 loads in the afternoon, then about 1 in 5 with 16 busy processes beside the game, and 6
of 9 walk runs in the evening (no extra load). The lock trace (`BB_SYNC_TRACE=1`) did not hide it.

What the game process recorded (Gnm call ring with each call's guest return chain, the
caller's `rbx` = the Gnmx `CommandBuffer*`, and that context's owner field):

- Each overlap is the same two threads: the main thread (not created by the game, so no
  "is host thread" line) and `GXRenderThread`, both writing through context `0x70282a2850`,
  whose owner is `GXRenderThread`'s `scePthreadSelf` handle. Calls alternate and the write
  positions only grow (both append through one cmdptr); no chunk was handed out twice. A
  check for GPU fence writes that free a chunk still in use (status 1) never fired.
- The main thread's chain: `0x2168630` (dispatch on a context) ← `0xebb960` ← `0x2276501` ←
  `0x22c4fcc` ← `0x22f8d9d`. `0xebb960` creates three render resources and clears them with
  three compute dispatches on `*0x55406c8` → `+0x10` → `+8`: the engine's global immediate
  context. No lock is held; no ownership check is on this path (the check at `0x26d6b52`
  compares `scePthreadSelf()` with the owner at `+0xb718` on another path).
- Timeline of one overlap (μs): `GXRenderThread` ends a frame and waits (+0); the main thread
  wakes from its frame handoff wait (+3); `GXRenderThread` is silent 16 ms (vblank wait, not
  traced); the main thread starts its load step (+8,600) and its three dispatches (+15,350 …
  +15,440); `GXRenderThread` starts the next frame at +15,400, in the middle of them.

So the game assumes the main thread's setup ends before the next vblank frame. Natively it is
short; here it took ~7 ms, longer on a loaded Mac. Our primitives behaved correctly
(`scePthreadSelf` handles are unique: thread records are never freed; mutex, rwlock,
semaphore and condvar orders in the trace are consistent).

Option A: the corrupted packets no longer end the GPU host (invalid types, unknown opcodes,
bad EOP selects, early flip IRQ, copy shader with bad strides: skipped and logged). None of
the evening runs lost the GPU host; the two full walks played on at 60 FPS. Option B
(MACOS_BACKLOG): make the main thread's step and the render thread's next frame exclusive.

Tools left in place: the Gnm check's report lists every library call into the bad buffer
with thread, caller chain (fiber stacks too) and `CommandBuffer`; "Gnm: context X used by
thread A, then B" (owned contexts only) shows overlaps even when the packets stay valid;
`BB_SYNC_TRACE=1` merges lock calls into the report; guest threads log their host thread id
and `scePthreadSelf` handle.

### Load race fixed: one lock around both context users (2026-10-14 night)

Option B (`src/runtime_hooks.c`): the main thread's setup step (`0xebb960`) and the render
thread's frame job (`0x2199af0`, which holds every render-thread chain seen in the overlaps)
are hooked at their entries. Their first instructions (checked byte for byte, all
position-independent) move to a stub; the entry becomes an absolute jump to a common hook that
saves the argument registers, takes one lock, calls the stub and releases the lock. A wait
gives up after 100 ms and runs the function anyway, with a log line, so a hidden dependency
cannot hang the game. Neither function takes stack arguments, so the hook's extra return
address moves none.

- 10 walk runs with 16 busy processes (9 loaded the level; one stayed at the title, the
  scripted presses came too early): **0 corrupted buffers, 0 host losses, 0 timeouts.** In 5
  of the 9 loads the main thread's step waited 0.3–0.7 ms for the render thread's frame — the
  overlap that corrupted packets in 6 of 9 runs before. Every remaining "Gnm: context X used
  by thread A, then B" line is the main thread starting 0.1–0.4 ms after the render thread's
  frame ended, i.e. serialized.
- Cost: one uncontended mutex per render-thread frame; the main thread's step runs only during
  loads.

### Retina drawable cost (2026-10-14 night)

Full screen and windowed now use the display's pixels (`BB_HIGH_DPI=0` restores point size).
Uncapped walks (`BB_PRESENT_MODE=Immediate BB_FPS_LIMIT=0`, 1080p, upscaler off), alternating
two runs each: point size 1496x839 vs Retina 2992x1678: FPS per 5 s window 81–96 in both, Retina
~0.5 FPS lower (~0.1 ms per frame for the larger present pass). Check slower Macs.

### Judder: the flip submit (2026-10-15, after midnight)

The spike log now carries the game threads' CPU time per frame (main thread, flip thread, all
guest threads, the busiest one; sampled by the game process at each flip submit). In the
30 ms game frames that cause judder, **all game threads together use ~12 ms of CPU, the same
as in a 16.7 ms frame**: the game is waiting, not computing.

What it waits behind: MoltenVK encodes the frame's Metal commands inside `vkQueueSubmit`, and
the recorder's submit at the end of each frame took **~13 ms** ("flip submit" in the frame
timeline). The flip and the fences the game waits on come after it, so a frame that is a little
late misses the next vsync. `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2` makes MoltenVK encode
while our recorder thread records (spread over the frame); the submit drops to 0.04 ms.

Walk route (title → load → Hunter's Dream), 1080p, 28 windows of 5 s after the level appears:

| | late game frames | per 5 s | flip submit |
|---|---|---|---|
| encode at submit (before), upscaler off, 3 runs | 111, 142, 123 | 4.0–5.1 | 13.1 ms |
| prefill, upscaler off, 4 runs | 19, 19, 22, 17 | 0.6–0.8 | 0.04 ms |
| prefill, FSR 3.1 | 25 | 0.9 | 0.04 ms |
| prefill, MetalFX | 936 | 33 | 0.02 ms |
| encode at submit, MetalFX | 134, 109 | 3.9–4.8 | 5.8 ms |

Uncapped FPS is unchanged (80–97 in both). MetalFX gets much worse with prefill: it commits its
own Metal command buffer on MoltenVK's queue, which then (apparently) queues behind the next
frame's prefilled command buffer. So prefill is on by default except when the upscaler at
start is MetalFX (`gpu/shim/bbgpu.cpp`; the environment variable overrides). Switching to MetalFX
in the menu keeps prefill until the next start.

10-minute soak with prefill (upscaler off, walk then standing in the Hunter's Dream): 0.2 late
game frames per 5 s over 120 windows, GPU host memory flat at 2,570 MB for 8 minutes, no
crash.

### 1440p idle gaps, located (2026-10-15 night)

Metal System Trace, 1440p native live, uncapped, Hunter's Dream walk spot (save at the wall),
prefill on: 56.9 FPS, GPU busy 14.0 ms of a 17.6 ms frame, ~431 encoders per frame. Idle per
frame by gap size: <5 µs 397 gaps / 0.17 ms, 5–20 µs 21 / 0.17 ms, 20–200 µs 5 / 0.35 ms,
**0.2–1 ms 3.0 / 1.65 ms, >1 ms 1.0 / 1.24 ms**. So the idle time is a handful of long stalls
at fixed places, not per-pass overhead.

Where (every frame, same encoder positions in the ~450-encoder main command buffer):

- **~1 ms between two tiny render passes inside the run of tessellated draws** (encoders
  ~270→271: render 13 µs, then nothing for ~1 ms, then the next render pass). With
  `BB_DROP_TESS=1` the >1 ms gaps fall from 1.0 to 0.2 per frame (1.24 → 0.29 ms) and FPS
  56.9 → 61.5: **MoltenVK's tessellation emulation** (compute for the vertex/control stages,
  then a render pass) stalls the GPU there. Not fresh memory: the trace's
  `metal-resource-allocations` shows no buffer allocations during frames (only ~7 zero-byte
  texture views per frame). `MVK_CONFIG_USE_MTLHEAP=1`: no change (3.3 ms of big gaps).
- **~0.7 ms after a `vkCmdCopyImage` blit encoder** before the next render pass (encoder ~389).
- **~0.6 ms at the end of the frame's command buffer**, before the presenter's first encoder
  (`vkCmdClearColorImage` on the swapchain image).
- **~0.75 ms between two compute encoders** around encoder ~84.

Next: which MoltenVK barrier/fence or event sits at those positions (a MoltenVK debug build
with encoder-boundary logging, or `MTL_DEBUG_LAYER` + `MTLCaptureManager` on one frame now that
split mode avoids the old capture crash), and whether the tessellated draw's control stage
waits for something the previous encoder only signals at its very end.

### Stability run and a measurement caveat (2026-10-15, 01:43–02:34)

20 consecutive walks (title → load → Hunter's Dream walk) on master with the load lock,
prefilled command buffers, Retina and the settings autosave: **20/20 clean** (no corrupted
command buffers, no host loss, 60.0 FPS, worst frame 16.7 ms in the windows checked, 0.4–0.6
late game frames per 5 s, screenshots correct).

Caveat: in these runs the GPU command thread needed **6–7 µs per draw** and the recorder was busy
~4.7 ms per frame, against **19–21 µs and ~15.8 ms** for the same build, route and settings a few
hours earlier; the context lock never had to wait (5 of 9 loads before). The Mac was on mains
in High Power mode throughout; the power log shows the display going to sleep around 01:57.
Cause not found (display asleep, background work earlier such as indexing, CPU scheduling).
So compare variants only back to back in one session, as the A/B runs above did.

Prefill A/B repeated in this faster state (02:35–02:47, alternating, 2 runs each): late game
frames per 5 s **3.0 and 5.2 without prefill, 0.5 and 0.5 with it**; frames late on the display
per run **80 and 157 without, 5 and 7 with**; GPU command thread 15–17 vs 7 µs per draw (the
encoding no longer competes with it). The judder fix holds in both machine states.

### Frame spike log: where judder comes from (2026-10-14, on mains)

`BB_FRAME_STATS=1` now prints a "Spike #N" line for every frame that arrives more than 1.25
frame periods after the previous one at any stage (`BB_SPIKE_MS` overrides), and a 5 s
"Spikes (…): game, flip, present, display" count. Stages per frame: the game's submit (time
stamped in bb-probe, carried in the submit record), arrival in the host, the command
processor reaching the flip, frame preparation, the flip, acquire / submit / `vkQueuePresent`,
and when the frame was actually shown (VK_GOOGLE_display_timing, enabled only with stats; Metal
reports it on the uptime clock, converted). Each line also has what the GPU process did since
the previous flip (GPU thread CPU time, draws, recording busy/idle, frames-ahead wait, recorder
and host-copy waits, compiles).

Walk in Central Yharnam, the user's settings (1080p, MetalFX Native AA), 24 windows (2 min):

| | late frames |
|---|---|
| game submits (21–46 ms apart) | 30 |
| flips | 24 |
| presents | 19 |
| **shown on screen a vsync late (33.3 ms)** | **9** |

- The display stays at 16.7 ms through most game spikes (the queued frame absorbs them), but
  the game's delta-time step does not: a 30–45 ms game frame moves the world further in one
  displayed frame. That is judder at a steady 60 FPS.
- Real dropped vsyncs follow GPU hiccups: a frames-ahead wait of 34–36 ms (the GPU took ~50 ms
  for one frame), then the game's next submit is late too.
- Runs of 4–5 frames with the recording thread busy 20–25 ms (normal ~15, same ~1025 draws), and
  the present's submission blocked behind it (8–23 ms).
- Display latency, `vkQueuePresent` to the screen: 34–76 ms, mean 51 (2–4 vsyncs).

### Display latency: two drawables, and what sets present-to-screen time (2026-10-14)

Walks in the Hunter's Dream (user settings, 1080p, upscaler off), `BB_FRAME_STATS=1`; the 5 s
summary now has present->display mean/max and the game's GPU page-fault waits per frame.

| swapchain images | present->display, gameplay mean | FPS | frames shown late |
|---|---|---|---|
| 3 (old default) | 48.4–48.7 ms (65 ms in some stretches) | 59.92 | 0–1 per 90 s |
| **2 (new macOS default)** | **31.7 ms** (48.5 in some stretches) | 59.92 | 0–1 per 90 s |

- The latency is bimodal (31.6 or 48.5 ms with two images) and switches every minute or so.
  It is not a queued frame: skipping a flip slot did not lower it. It tracks when the frame's
  GPU work ends relative to the vsync: "flip submit -> GPU done" rose 13.0 -> 14.4 ms when it
  went 31.6 -> 48.5. A frame that finishes after the vsync is shown one vsync later. The
  steady-clock frame limiter drifts through the vsync interval, which moves that point.
- Tried and dropped: pacing flips by the display's vsync times (VK_GOOGLE_display_timing:
  measured period and phase, flips just after a vsync, no catch-up after a late frame). 3+3
  walks, pacing on vs off: latency the same (one run of each jumped to 48 ms), frames shown
  late 17 vs 3: a frame slightly late for its grid slot waited a whole vsync.
- Game-side GPU page faults are not the long game frames: ~1–3 per frame, ~0.1 ms waited.
- Long game frames in this scene are rare in gameplay (0–3 per 5 s); the 33 ms "game" spikes in
  the first minute are the title and loading screens, which run at 30 FPS.

## Part 3 — Open issues

| Issue | Status |
|---|---|
| CPU-bound draw recording at 720p | Next: re-profile at 720p to see what dominates now |
| Intermittent "Unimplemented PM4 type 0" / bad fence packets on loading a save | **Root cause found 2026-10-12:** the game rewrites command buffers after the submit returns, before the (asynchronous) parser reads them. Split mode (default) now copies DCB/CCB at submit into a shared arena: 0 crashes in 5 stressed save loads (was 2 in 3). Single-process mode still parses the game's memory (smaller window): backlog. **Seen again 2026-10-13 in split mode** (1 in ~8 runs, at load): not fully fixed |
| Character half-rendered after quitting to the menu and reloading | Logged (backlog, Stability) |
| Ghosting halo around the character in character creation | Logged; null image stand-ins not done yet |
| MetalFX distant shimmer | Try `BB_METALFX_JITTER_SIGN=-1`; jitter must be on |
| Menu: disabled controls looked broken; settings not persisting as expected | Root cause: controls under an "Off" upscaler are disabled, with near-invisible styling. Menu redesign in progress |

## How to reproduce

```sh
# Run (data, saves and settings in out/macdata)
BB_DATA_DIR=$PWD/out/macdata BB_GAME_DIR=~/Bloodborne/CUSA03173 bash run.sh
# Benchmarks: fixed settings files
BB_CONFIG=$PWD/out/macdata/bench-{native,fsr3-quality,metalfx-quality}.ini …
# Diagnostics (environment variables)
BB_FRAME_STATS=1   # per-frame stall breakdown
BB_PASS_STATS=1    # why render passes end, split passes
BB_MENU_TRACE=1    # menu clicks and setting changes
BB_TRACE=1         # import calls, unbound imports
# Metal-native renderer: write what is on screen as a PNG (works with the screen locked).
# Write the trigger atomically (temp file + mv); its first line is the PNG path.
BB_SNAPSHOT_TRIGGER=/tmp/snap.trigger …   # then: echo /tmp/a.png > /tmp/t.tmp && mv /tmp/t.tmp /tmp/snap.trigger
# CPU profile while the game runs
sample $(pgrep -x bb-probe) 10 1 -file profile.txt
# GPU timeline while the game runs
xcrun xctrace record --template "Metal System Trace" --attach $(pgrep -x bb-probe) --time-limit 3s --output gpu.trace
```

## 2026-10-15: self-contained release app

`scripts/macos/package_app.sh`: 126 MB app, 48 MB zip (libbbgpu 26 MB and libavcodec 27 MB
universal; standalone Python 46 MB after trimming). Unzipped to a scratch folder: codesign
verifies, no binary names the checkout, `DYLD_PRINT_LIBRARIES` shows every non-system library
(MoltenVK included) loading from the bundle, and `open --env BB_DATA_DIR=<test data>` reached
the title screen at 60 FPS. The bundled Python runs `game_check.py`; a one-byte change to
eboot.bin is rejected as `wrong_eboot` with the pre-patched-executable explanation.
Pre-existing, unrelated: `tests/test_run_settings.py` fails one case on master (third launch
gets 640x360 instead of the live path at 1080p).

## 2026-10-15: "buffer.cpp:105" on Macs with less memory

Reported from a player's Mac (and seen on the GitHub CI VM): the start stops at
`Failed allocating buffer with error ErrorOutOfDeviceMemory` (buffer.cpp:105). The macOS buffer
cache maps the guest GPU range as one 16 GiB Metal buffer, and one Metal buffer is limited to
`maxBufferLength`, about 60% of the Mac's memory (28.1 GiB with 48 GB): Macs with less than about
28 GB could not start. The game's GPU memory sits in the first few GiB: the city walk ran
correctly with `BB_ARENA_GIB=4`. The arena now shrinks to the largest multiple of 4 GiB that fits
(log line "GPU arena: N GiB"); `BB_MAX_BUFFER_GIB=9` (a 16 GB Mac) gave 8 GiB and a clean walk.
Below 4 GiB (8 GB Macs) it stops with a message naming the limit.

## 2026-10-15: M2 Max at 2–3 FPS (issue #2)

The log shows `bbgpu-host: cannot reserve the guest range 0x7000000000-0xfc00000000 (got 0x7b8e400000)`
on every launch: something already occupies the start of the guest range in the arm64 host on that
Mac, so the game falls back to single-process mode. That fallback is not the slowdown by itself:
recreating it here (`BBGPU_TEST_BLOCK_RANGE=1`, the host maps 1 MiB at 0x7000000000 first) still
walked the city at 60 FPS (worst frames 18–44 ms vs 16.7 ms with `BB_GPU_PROCESS=0`). They also
tried 640x360 with FSR and 720p without, still slow, so it is not GPU resolution. The host now lists
the regions in the way (address, size, VM tag, file) when the reservation fails. Waiting for a log
with that and `BB_FRAME_STATS=1`.

## 2026-10-15: sizing a native Metal back end (split mode, prefill on, M4 Max)

Central Yharnam respawn spot after loading the test save, standing, uncapped
(`BB_PRESENT_MODE=Immediate BB_FPS_LIMIT=0`), upscaler off; `sample` of `bbgpu-host` for 8 s
and a 3 s Metal System Trace in the same run.

| | 1440p native (live) | 1080p native |
|---|---|---|
| FPS uncapped | 55–58 | 71–77 |
| GPU busy per frame | 14.6 ms (vertex 5.3, fragment 9.3, compute 1.7) | 10.8 ms (vertex 5.3, fragment 4.9, compute 1.7) |
| GPU idle per frame | ~3.1 ms; 2.6 ms in ~4 stalls inside the prefilled command buffer | ~2.9 ms; 2.3 ms in ~3 stalls |
| Metal encoders per frame | ~409 | ~409 |
| `bb:VkRecorder` busy | 47% (MoltenVK 6.1, AGX driver 10.0, Metal/IOGPU/objc 5.7, malloc 3.0, memcpy etc. 6.3, ours 11.7) | 54% (MoltenVK 8.2, AGX 11.8, Metal/IOGPU/objc 7.8, malloc 4.0, memcpy etc. 8.2, ours 9.9) |
| `bb:DrawRec` busy | 26% (ours 20) | 30% (ours 24) |
| `GpuCommandProcessor` busy | 15% | 21% |

Reading:
- **On the M4 Max no CPU thread is saturated**: the GPU timeline sets the frame rate. The
  recording thread is about half MoltenVK + Apple's driver; a Metal back end would roughly halve
  its cost. That matters on slower CPUs (M1/M2, and the rival port, which is CPU-bound) more
  than here.
- **GPU idle (~3 ms/frame) is MoltenVK's**: the same stalls located on 2026-10-15 night
  (tessellation emulation, after an image copy, command-buffer boundary, between compute
  encoders). Our own encoding would avoid them.
- **Vertex time is 5.3 ms at both resolutions**: geometry, binning and pass structure, not pixels.
  ~409 encoders/frame (190 render passes at ~13–16 µs each, ~3 ms) plus re-binning per pass;
  merging passes needs control over pass boundaries and load/store actions that Metal gives us
  directly.
- **Estimate**: removing the idle stalls and merging the needless passes (~1–2 ms) takes 1440p
  from ~17.7 ms to ~13–14 ms per frame (~72–77 FPS uncapped; 60 locked with headroom), and
  1080p from ~13.7 to ~10–11 ms. Fragment shading itself does not change.

## 2026-10-15: head to head with Huax-1839/bloodborne-macos (direct Metal renderer)

Their port (GPL-2.0, single x86-64 process under Rosetta, a ~20k-line Metal back end, default on
macOS 15+) built here at ee3120a (one local change: two macOS 27 SDK lines guarded for the 26.5
SDK, frame interpolation only). Same protocol for both (`cmp.sh`): the same test save (Hunter's
Dream headstone), 1920x1080 native, upscaler off, uncapped (`BB_PRESENT_MODE=Immediate
BB_FPS_LIMIT=0`), title + 30 s, Cross twice, mean of the last six 5-s windows after 75 s in game,
warm shader caches, runs interleaved. Screenshots: same scene, same image.

| | Run 1 | Run 2 | Run 3 |
|---|---|---|---|
| **Ours** (split mode, MoltenVK, prefill) | 80.9 | 81.1 | 82.6 |
| Theirs, Metal | 53.1 | 52.7 | |
| Theirs, Vulkan (MoltenVK) | 44.4 | 44.4 | |
| Ours, `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=0` | 94.0 | | |

- Ours is ~53% faster than their Metal renderer and ~83% faster than their MoltenVK path here.
  Their shaders keep `NoContraction` on every float op and compile with `MTLMathModeSafe` (the
  cost we removed on 2026-10-13), and their renderer runs translated in the game process.
- **Their MoltenVK tip works for us too:** argument buffers off gave 82.6 → 94.0 FPS (+14%),
  image correct. Not yet the default (Backlog: check 1440p, the city, pacing and the
  menus/upscalers first).

### MoltenVK argument buffers off, A/B (2026-10-15 evening)

Same protocol as the head to head (Hunter's Dream, uncapped, alternating), `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS`:

| | on (MoltenVK default) | off |
|---|---|---|
| 1080p native | 80.9, 81.1, 82.6 | 94.0 |
| 1440p native (live) | 58.6, 58.7 | 66.2, 65.9 |
| 1080p FSR 3.1 Quality | 92.6 | 107.1 |
| 1080p MetalFX Quality | 91.8 | 106.2 |

Capped at 60 (walk route, 2 runs each): late game frames 0.7–0.9 per 5 s (on) vs 1.0 (off), late
display frames 6–8 vs 4–6: the same. Screenshots correct (native, FSR, MetalFX). 8 walks with it
as the default: all clean. **Now the default** (`bbgpu_init`; the environment overrides). 1440p
native is above 60 FPS uncapped for the first time.

### Head to head with DiAvisoo/bbport-mac-optimized v0.1.0 (2026-10-15 night)

A fork of bmy/bbport-mac (`macos-native-0.5`: KosmicKrisp, native arm64 GPU process), released as
"M2 optimised" (tested on an M2 Max: 50–60 FPS at 1080p, 30–40 at 32:9). Their release app's engine
run headless with their launcher's profile (`BB_NATIVE_GPU=1 BB_SKIP_EMPTY_DRAWS=1
BB_VK_RECORD_THREADS=1 BB_FRAMES_AHEAD=2 BB_GPU_QOS=1 …`), isolated data folder, same protocol and
save as the Huax-1839 comparison (1080p native, upscaler off, uncapped, alternating):

| | Run 1 | Run 2 |
|---|---|---|
| **Ours** (mac-v0.3.3 code) | 94.2 | 94.4 |
| DiAvisoo v0.1.0 | 73.3 | 74.4 |

Ours ~28% faster on the M4 Max; same scene, both 1920x1080, image correct in both. Their ~74 here is
consistent with their 50–60 on an M2 Max, which suggests ours would do ~65–75 there, and that issue
#2's 2–3 FPS on an M2 Max is a bug in ours, not the hardware.

## 2026-10-16 night: the whole frame through our own Metal encoding (branch metal-backend)

`BB_METAL_FRAME=1` (arm64 host): a layer over vulkan.hpp's dispatcher owns every command buffer and
encodes it in Metal (`renderer_metal/mtl_vk_layer.mm`, `mtl_frame.mm`); MoltenVK still creates the
resources. Pipelines: the game's built from our pipeline keys (Huax-1839's PSO builder), every other
one translated from its Vulkan create info. Hunter's Dream, upscaler off, uncapped, alternating:

| | Metal frame path | MoltenVK |
|---|---|---|
| 1080p native | 97.1, 96.4, 96.3, 95.9 (96.4) | 94.4, 94.5, 94.7 (94.5) |
| 1440p native | 67.7, 67.7 | 67.9, 67.2 |
| 1080p FSR 3.1 Quality (with overlay) | 99.9 | — |
| 1080p MetalFX Quality (encoded in the frame's command buffer) | 106.7, 106.0 | 106.5, 106.9 |
| Walk pacing (late game / display frames) | 0.9 per 5 s / 3–4 | 1.4–2.4 / 8–9 |

The Metal path also draws the geometry-shader draws MoltenVK cannot (emulated with compute), and
6 walks ran clean. What mattered, in order: unretained-reference command buffers (Metal's
completion thread was saturated releasing resources, "frame done" late by ~7 ms); a residency set
for the device-address buffers with only the guest arena declared per encoder, read-only on
render encoders (declaring writes serialized every pass); tessellated draws in runs (78% follow
another: one compute pass for the run's kernels, one reopened render pass for the patch draws);
render encoders opened at the first draw (~45 passes per frame have no draws); tessellation
scratch reused across frames (fresh GPU memory stalled the run ~1 ms per frame).
Did not work: committing the frame in chunks so the GPU starts early (`BB_METAL_CHUNK`): the
renderer writes guest data into GPU memory from the CPU while recording, and an early chunk read
data meant for later draws (broken geometry while walking).

### Green flashes indoors (user report, 2026-10-16 morning)

The user saw a bright green, flickering panel (sometimes a burst of green streaks over the whole
screen) inside a building on the Metal path, never on MoltenVK; worst when entering and moving.
Reproduced from a copy of the save (window captures: 3 and 1 of 75, 4 of 120 with green; MoltenVK
0 of 75; `BB_METAL_GEOMETRY=0` 0 of 150). Not stale scratch memory (prefilling the strip indices
with restarts, or also zeroing the vertex records: still 5 and 3 of 240). Not depth of field.
`BB_GS_DEBUG=1` (the geometry draw in magenta, blending off) showed the draw covering the whole
screen or exactly one quadrant in ~40% of captures. Cause: the one geometry-shader pipeline here
(points expanded to sprites: the dust motes in the air) is drawn with **vkCmdDrawIndirect**, and
`MetalFrame::DrawIndirect` had no geometry (or tessellation) case: it bound the pass-through
pipeline, which reads finished vertex records from buffer 0, and drew directly, so the records
were whatever buffer was bound there. Fix: indirect geometry draws run Huax-1839's indirect
kernels (prep reads the guest's arguments into a control block, then the export and geometry
kernels and the strip draw take their sizes from it); indirect tessellated draws are skipped (as
MoltenVK does) until they are emulated the same way. After the fix: 0 of 240 captures with green,
and in magenta the draw is small motes spread through the room.

## 2026-10-16: Metal-native renderer, first light (M4, branch metal-native)

The game presents through a `CAMetalLayer` with no Vulkan swapchain and no MoltenVK in the frame.
Runs: split mode, windowed 2992x1672 Retina drawable (1496x836 points), native 1080p, upscaler
off, `fps_cap=60`, test data in `$MAIN/out/macdata`, saves restored from `save-backups/` before
and after. The screen was locked from 21:12 until between 21:54 and 22:04 (window captures failed: "could not
create image from window"), so those runs' pictures come from the presenter's new
`BB_SNAPSHOT_TRIGGER` readback of the drawable; the later runs use window captures. Runs from
22:15 on skip the intro videos (`skip_intro=1`): their startup times are not comparable with the
earlier ones.

| | Metal-native (this branch) | 0.4.0 (MoltenVK), same ini and save |
|---|---|---|
| Title screen | 60.0 FPS, worst 16.7 ms (screen locked) / 17–40 ms (display on) | 60.0 FPS, 16.7 ms locked / 17–29 ms display on |
| Central Yharnam lamp (`cages-20261016`), warm cache | 60.0 FPS steady; around the load one window of 58–59 FPS with a 92–144 ms frame | 60.0 FPS steady; one 89–106 ms frame around the load |
| Same, partly cold cache (Task 2's cache, 28 pipelines) | load windows 124 ms and 389 ms | — |
| Same, empty cache | load window 399 ms (391 shader/pipeline compiles) | — |
| Hunter's Dream (`loadcrash-20261014-134937`), warm for the city only | 60.0 FPS steady; load windows 148 ms, **542 ms with 39 compiles** (Dream pipelines not in the archive yet), 47.6 ms | not run |
| Draws per frame (city / Dream) | 884–888 / 815–843 (67 tessellated in the city) | 886–887 / — |
| First window to first `Frame stats` (incl. run.sh's no-op build), intro playing | 22 s cold, 20–24 s warm | 23 s |
| Same with `skip_intro=1` | 12 s (title up 12 s after the first stats line) | 12 s |
| First 5 s window | 55 FPS, worst ~410 ms: the 2nd frame's `PrepareFrame` waits ~344 ms for the preloaded pipelines, which build on the pool in the background | 60.6 FPS, worst 39.8–53.9 ms (preload of 644 pipelines, 321 ms, before the first frame) |

The late 181–185 ms windows in the first runs came from the snapshot hook itself (the PNG was
written on Metal's serial completion thread); after moving it to its own thread the later runs
show none.

- **Pictures:** title (logo, hunter, Play Online/Offline, copyright line) with correct colours and
  gamma, identical by eye to 0.4.0's (`t4-master-fs-a.png` vs `t4-r2-t.png`). The city save at the
  lamp (cages, fence, HUD with Oil Urn and blood echoes) matches 0.4.0 except one thing: **the
  figure in the cage left of the pillar (behind the bars) is drawn on 0.4.0 and missing on
  Metal-native** at +40 and +50 s (`t4-m-cages-a/b.png` vs `t4-w-cages-a/b.png`, and the crops
  `crop-t4-*`). No skipped draw, failed pipeline or error is logged. Hunter's Dream at the headstone
  with its prompt is correct (no 0.4.0 capture). `BB_HIGH_DPI=0` gives a 1496x836 drawable with
  the same picture.
- **Window resize:** the window resized by System Events to 1200x900 points while in the city
  refits the picture at 16:9 with black bars above and below (`t4-resize2-a.png`); no stall, 60.0
  FPS with worst frames 17–33 ms afterwards. The full-screen switch was not tried with the screen
  unlocked.
- **Binary archive:** cold cache (empty GPU dir): writes of 21, 28, 29 and 205 pipelines, 0 from
  the archive; the save load compiled 391 shaders/pipelines (worst frame 399 ms, during the load
  screen). Warm (same dir): **195 pipelines from the archive, 0 compiled**, save-load worst frame
  92 ms. (From 2026-10-16 22:12 the "compiled" count covers every pipeline kind; before, render
  pipelines added to a loaded archive only.)
- **Validation (`MTL_DEBUG_LAYER=1`, nslog, title + city, ~34 FPS under the layer):** two distinct
  messages. `depthStencilState must not be nil` (once per frame: the post-process pass after the
  rasterizer's draws) is fixed: `Encoder::SetDepthStencil(nullptr)` now binds a no-test state.
  Left for M5: "For depth/stencil attachment, the renderPipelineState pixelFormat must be
  MTLPixelFormatInvalid, as no texture is set" (~600/s): the pipeline key takes the depth and
  stencil formats from `DepthValid()`/`StencilValid()`, but the pass binds the depth buffer only
  when depth or stencil testing is enabled. Where both are bound, the formats match the image's
  (no format-mismatch message). No command-buffer errors.
- **GC on 16 GB:** `Device::AllocatedSize()` 17.0 GiB at the title, 18.9 GiB in the city;
  resident buffers (guest arena and guest buffers) 16.5 GiB in both. The collector compares the
  difference: 0.45 GiB at the title, 2.4 GiB in the city, against a trigger of 26.2 GiB here
  (70% of the M4 Max's recommended working set). On a 16 GB Mac the trigger is **estimated** at
  ~7.5 GiB (70% of a ~10.7 GiB recommended working set; not measured), well above 2.4 GiB: the
  arena is not counted. 0 images freed.
- **vblank 480 Hz** in the stats is by design (`BB_VBLANK_HZ=0` for the uncapped preset; the
  frame limit stays 60 here) and is the same on 0.4.0.

### M4 final-review fixes (2026-10-16 late, M4 Max, split mode)

- **Minimise:** the bbgpu-host window minimised (System Events `AXMinimized`) for 20 s in the city
  (`cages-20261016` save) and restored. With the fix (presents skipped while SDL reports the window
  minimised, hidden or occluded) Frame stats kept printing at **60.0 FPS while minimised** and
  60.0 FPS (worst 16.7 ms) after restore; the picture after restore is correct
  (scratchpad `res/fix-min-a.png`). One ~1 s hitch at the moment of minimising (spike log:
  present 1011.9 ms): the present already inside `nextDrawable` when the Dock animation began.
- **Exit flush of the binary archive:** archive deleted, the window closed 5 s after the renderer
  started (before the writer's first 10 s write): `Metal binary archive: wrote 258 pipelines in
  575 ms` then `Exit: pipeline archive flushed in 575 ms`; the file exists after the exit. The next
  run: **234 from the binary archive, 0 compiled**. Before the fix the host's `_exit` skipped
  this write. Archive writes of 258 pipelines take 575–1009 ms here.

## 2026-10-16 night: Metal-native M5 Task 3, pipeline depth formats follow the bound depth buffer

- **Fix:** `BoundDepthAttachment(regs)` (`renderer_vulkan/vk_depth_attachment.h`) is now the one
  rule for the render pass's depth attachment (`Rasterizer::PrepareRenderState` binds the depth
  buffer only while a depth or stencil test uses it; `RenderState` binds each valid aspect) and
  the pipeline key's `z_format`/`stencil_format` (`RefreshGraphicsKey`, which also uses it for
  `motion_possible` and the depth sample count). Every render pipeline, including the
  geometry-shader and tessellation ones, is built from the key in one place
  (`PsoCache`, mtl_pso_cache.mm), so they follow too. `depth-attachment-test` (11 cases) in
  `build_macos.sh --test`.
- **Validation (`MTL_DEBUG_LAYER=1`, nslog, `shot.sh`: title, Cross twice, cages save, 93 s):**
  before **96,721** messages (~1,040/s; "For depth/stencil attachment, the renderPipelineState
  pixelFormat must be MTLPixelFormatInvalid, as no texture is set", depth+stencil 94,963x, depth
  only 1,668x); after **0** (only "Metal API Validation Enabled"). City 36 FPS under the layer in
  both runs.
- **Pipeline key version 7 → 8:** the stored pipeline cache is cleared once at the first start
  (`227 pipelines of an older cache format: clearing the cache`, 846 files; 361 compiles at the
  save load, worst frame 106 ms, then 60 FPS). The Metal binary archive (`<cache>/metal/`) is not
  in the cleared directory and stays: that run took 173 pipelines from it and compiled 17 (the
  pipelines that lost their depth format); the next run preloaded 183 of 184 with no archive
  writes. msl-check is unaffected: the stored corpus (`$SP/archive-cache`) is in 0.4.0's v6
  format, which it still reads (645 keys, 644 built, PASS).
- **Caged figure (Central Yharnam lamp):** still missing after the fix (crop 950,1000 300x300 of
  the +40/+50 s snapshots, scratchpad `res/t3-cmp2.png`, 0.4.0 vs M4 vs this). The depth-format
  mismatch was not the cause; the diagnostics continue (M5 Task 11).

## 2026-10-17: Metal-native M5 Task 5, camera motion; what object motion costs

- **Measuring uncapped:** `fps_cap=0` with `BB_VBLANK_HZ=480` alone still showed exactly 60.0 FPS
  on the Metal-native renderer: CAMetalLayer display sync was on (acquire waited ~13 ms). Add
  **`BB_PRESENT_MODE=Immediate`** (display sync off, `vk_presenter.cpp`; on 0.4.0 the MoltenVK
  swapchain present mode). Both builds below were measured with both variables.
- **Object motion on vs off** (cages save, Central Yharnam spawn, standing, 2560x1440 native,
  `upscaler=taa preset=0`, uncapped; three alternating runs each, mean of the last six
  `Frame stats` lines per run). Metal-native's upscaler is still a stand-in (TAA not running);
  0.4.0 runs TAA.

  | Build | object_motion=0 | object_motion=1 | Cost |
  |---|---|---|---|
  | metal-native (1a23e5f) | 86.4 FPS (86.2-86.5), 11.58 ms | 67.8 FPS (67.3-68.2), 14.75 ms | +3.17 ms (-21.5%) |
  | 0.4.0 (MoltenVK, 62a4c15) | 61.5 FPS (61.2-61.7), 16.27 ms | 51.0 FPS (50.9-51.3), 19.59 ms | +3.32 ms (-17%) |

  The cost in milliseconds is the same: it is inherited from 0.4.0, not a Metal-native
  regression.
- **Not pass splits:** `BB_PASS_STATS=1` gives the same passes per frame with object motion on
  (9,148 begins/s at 68.4 FPS = 134/frame) and off (11,773/s at 87.6 FPS = 134/frame), and the
  same split-pass reasons (shader-HLE copies, MultiCopy, CopyImage). Not the per-draw position
  stores/loads either: with them disabled (toggle bit 29, every draw gets parameter 0) the
  frame rate stays at 64.6 (same session's on/off: 64/80.5). Not the buffer declarations: the
  positions buffer declared read/write on every encoder or only on motion passes measures the
  same. What remains is the motion pipelines themselves (the extra RGBA32F colour attachment 7
  on the G-buffer passes, the motion shader variants); GPU timings per pass (M5 Task 10) can
  split those.

## 2026-10-17: Metal-native M5 Task 10, why the native renderer is ~11% slower than 0.4.0

Central Yharnam spawn (cages save), standing, `upscaler=off preset=0 output_res=1920x1080`,
uncapped (`fps_cap=0 BB_VBLANK_HZ=480 BB_PRESENT_MODE=Immediate`), the same ini on both builds,
mean of the last six `Frame stats` lines per run. GPU busy: the union of every Metal command
buffer's `GPUStartTime..GPUEndTime` in bbgpu-host, measured the same way on both builds by an
interposer (`DYLD_INSERT_LIBRARIES` through a `BB_GPU_HOST` wrapper; SIP strips `DYLD_*` from
`run.sh`'s shells), and on metal-native also by the new `BB_FRAME_STATS` "GPU busy" (they agree).

| Build | FPS (3 runs) | GPU busy | GPU ms/frame |
|---|---|---|---|
| 0.4.0 (MoltenVK) | 98.83 (98.50 / 98.93 / 99.05) | 98.3% | 9.94 |
| metal-native | 88.25 (88.23 / 88.27 / 88.25) | 98.9% | 11.21 |

Both are GPU-bound; the native renderer spends **1.27 ms more GPU time per frame** (+12.8%).
It records fewer encoders (147 render encoders/frame against MoltenVK's 237) with the same
texture usage (render targets `ShaderRead|ShaderWrite|RenderTarget|PixelFormatView` on both) and
the same load/store actions (Load/Store; MoltenVK defers its store action, `Unknown` at encoder
creation). The difference is **Metal's hazard tracking on two buffers every encoder uses**:

| Experiment (metal-native, resources made untracked, rendering unsynchronised) | FPS |
|---|---|
| none (baseline) | 88.25 |
| all textures | 87.4 |
| guest arena only / fault buffer only / BDA page table only / DeviceLocal staging rings only | 89.5 / 88.1 / 89.0 / 89.1 |
| guest arena **and** fault buffer | 109.9, 110.0 (GPU 8.88 ms/frame) |
| guest arena, fault buffer and page table | 110.0 |
| every DeviceLocal buffer | 119.7 (the game's ~120 FPS limit; GPU busy 94-98%) |
| every buffer | 118.0 |

The guest arena (bound to nearly every draw and dispatch as `device` memory and declared
read/write on compute encoders) and the fault buffer (bound writable to every shader that reads
guest memory by address) are tracked: any encoder that may write them waits for every earlier
encoder that uses them, and every later one waits for it, so adjacent encoders stop
overlapping. Either buffer alone keeps the serialisation; with both untracked the frame takes
8.9 ms, 11% *less* GPU time than 0.4.0 (MoltenVK places its memory in untracked placement heaps,
103 `MTLHeapTypePlacement` heaps with `MTLHazardTrackingModeUntracked` in this scene, and
orders work by Vulkan barriers). Untracked buffers need explicit ordering (fences or barriers
for the encoders that really write guest memory, the fault processing kernel's atomics): that
is the M5 Task 11 fix (`docs/MACOS_BACKLOG.md`, Performance). Object motion's +3.2 ms (Task 5
above) declares its positions buffer read/write at the vertex stage on every motion pass: also
worth re-measuring once the buffers are untracked.

- The Metal-native GPU profiler (`BB_GPU_PROFILE=1`, encoder-boundary timestamps) costs about
  3% (85.5 FPS) and shows the serialisation: the encoders' start-to-end times sum to 21 ms per
  11.7 ms frame tracked, 103 ms per 9.2 ms frame untracked (encoders start early and overlap).
  0.4.0's profiler reads the same timestamp everywhere but at submissions (all 10.1 ms/frame in
  "between submissions"), so the per-pass comparison with MoltenVK is not possible there.
- Metal System Trace (`xctrace record --template 'Metal System Trace' --attach <bbgpu-host>`,
  or `--instrument GPU`) slows both builds to 64-81 FPS and serialises encoders (0.4.0 then shows
  12.3 ms/frame of GPU time against 10.45 for metal-native): useful for the shape of a frame, not
  for comparing the two renderers' GPU time.
- `BB_GPU_CAPTURE=<s>` with `METAL_CAPTURE_ENABLED=1` now captures the renderer's own device:
  with the default 16 GiB arena Apple's capture tool crashes in `stopCapture` while writing;
  with `BB_ARENA_GIB=4` it writes the file (884 MB, city).

## 2026-10-17: Metal-native M5 Task 10b, untracked guest buffers with explicit ordering

Device-local buffers (guest arena, page table, fault buffer, device-local staging, camera and
object motion buffers) are now created untracked (`mtl_buffer.mm`) and ordered explicitly;
`BB_METAL_HAZARD_TRACKING=1` restores tracked buffers and no ordering (logged once). Design:
`mtl_buffer_barriers.h` (the renderer's range barriers), `mtl_order.h` (the fences),
`mtl_encoder.h` (how commands declare what they touch).

- **The renderer orders the arena like the Vulkan renderer did** (0.4.0's barrier tracker): it
  records each command's ranges (bound buffers with their written flag, vertex, index and
  indirect-argument ranges, copies, fills, image uploads and downloads, tiling) per 256-byte
  block since the last barrier, with the kind of encoder (render pass, copy, compute dispatch,
  hoisted upload) and, for render passes, the stage of a read. A conflicting command (RAW, WAR,
  WAW, never RAR) gets a barrier that waits for the conflicting kind only: render-pass ends,
  their vertex stages only (WAR against vertex fetch and vertex-stage shaders), copies, compute
  dispatches or the hoisted uploads. Other kinds keep overlapping.
- **The Encoder implements a barrier with MTLFences**: every Metal encoder that touches untracked
  memory updates a fence of its own (64 slots; render encoders one more after their vertex
  stage); after a barrier, new encoders wait for the named encoders' fences. Fences are evaluated
  in commit order, so this holds across command buffers; hoisted uploads (the command buffer
  committed ahead) are chained through one fence and waited for after a barrier on them; uploads
  whose ranges a submission in flight still uses are no longer hoisted. Writes to untracked
  buffers the renderer does not track are isolated (barriers before and after).
- **Fault buffer:** the processing kernel takes the bits with `atomicExchange` (bits set by
  shaders still running stay for the next processing).

City, cages save, standing, `upscaler=off preset=0 output_res=1920x1080`, uncapped
(`fps_cap=0 BB_VBLANK_HZ=480 BB_PRESENT_MODE=Immediate`), mean of the last six `Frame stats`
lines per run (`t10-run.sh`, as Task 10):

| Build | FPS (runs) | GPU ms/frame |
|---|---|---|
| 0.4.0 (MoltenVK, Task 10) | 98.83 (98.50 / 98.93 / 99.05) | 9.94 |
| metal-native before (Task 10) | 88.25 (88.23 / 88.27 / 88.25) | 11.21 |
| **untracked + explicit ordering** | **95.40** (95.42 / 95.87 / 94.92) | 10.48 |
| the same, `BB_METAL_HAZARD_TRACKING=1` | 86.95 (87.32 / 87.12 / 86.42) | 11.50 |

+9.7% over tracked buffers, 3.5% short of 0.4.0. (Two more runs are left out: one where the
game window was being played, 969 draws/frame instead of 883; one at 94.4 FPS with lower GPU
busy, 94-96%, possibly disturbed too.) About 80 barriers per frame, nearly all on copies (the
copy-shader HLE copies rewrite table entries between the draws that read them).

What the ordering costs, by experiment (each one unsafe, measured once, same scene):

| Change | FPS |
|---|---|
| no fences at all | 120 (the game's limit) |
| a barrier's waits only for the next encoder (the conflicting one), not every later one | 108 |
| tracking per 4 KiB page instead of 256-byte blocks | 85-88 (most render-pass barriers came from neighbouring draws' constants sharing a page) |
| hoisted uploads never waited for | +0.9 |
| barriers wait for the latest encoder of a kind only | +1 |
| copy encoders chained in order instead of copy barriers | 94-95 (slower) |
| barriers retired and waited per waiting kind of encoder (render / copy / compute) | 87.7 (slots run out: kinds that rarely wait keep them busy) |

So the remaining time is the barriers' reach: a barrier makes every later encoder wait, while
only the conflicting command needs to. Ordering per command (each command waits for exactly
the encoders it conflicts with) is the next step (`docs/MACOS_BACKLOG.md`).

**Object motion** (Task 5's +3.2 ms; city, `upscaler=taa preset=0 output_res=2560x1440`,
uncapped, alternating runs):

| Mode | object_motion=0 | object_motion=1 | Cost |
|---|---|---|---|
| untracked + ordering (3 runs each) | 71.73 FPS (70.60 / 72.77 / 71.82), 13.94 ms | 50.71 (50.90 / 50.55 / 50.68), 19.71 ms | +5.77 ms |
| `BB_METAL_HAZARD_TRACKING=1` (1 run each) | 62.07, 16.11 ms | 51.00, 19.61 ms | +3.50 ms |

With object motion on, untracked buffers gain nothing (50.7 against 51.0): the motion frame is
as slow as before, so its cost in milliseconds grows. Not explained yet (backlog).

### Task 10b fix round 1 (review): slots, object motion, tag memory

Runs from here on keep the game window in the background (`SDL_MAC_BACKGROUND_APP=1
SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0`, so it does not take focus from whoever uses the Mac). In the
background the GPU is ~12% slower: the city scene at 1080p, tracked, runs 76.9 FPS (86.95 in
front); compare only runs made the same way.

- **Ordering slots ran out.** With 64 slots per class, ~150 encoders per frame found every slot
  busy and reused one, waiting for an unrelated encoder first (new `BB_FRAME_STATS` field
  "busy-slot reuses"): render-pass slots stay needed until a render-pass barrier (~2 per
  frame) or the command buffer completes, and ~450 encoders are in flight. With 256 slots: 0
  reuses; city 1080p in the background 85.05 -> 85.95 FPS (tracked: 76.9).
- **Object motion** (hypothesis from review: its history slots run out): ruled out, 0 busy
  reuses in the motion class. Background, `upscaler=taa preset=0 2560x1440`, uncapped:

  | Mode | object_motion=0 | object_motion=1 | Cost |
  |---|---|---|---|
  | untracked (2 runs each) | 74.99 FPS (75.03 / 74.95), 13.34 ms | 50.99 (51.05 / 50.92), 19.61 ms | +6.3 ms |
  | tracked (1 run each) | 64.03, 15.61 ms | 51.32, 19.48 ms | +3.9 ms |

  The motion frame takes ~19.6 ms in every mode (front or back, tracked or not): untracked
  buffers shorten the frame without object motion only, so the difference grows while the
  motion frame itself is no slower. Where the motion frame's time goes is still open
  (`docs/MACOS_BACKLOG.md`).
- **Tag memory:** the access tags were allocated with `make_unique<T[]>`, which writes every
  byte (+216 MB footprint, review). Now `calloc`: zero pages committed only where accesses
  touch them. In play (city, 110 s): MALLOC_LARGE 1470 MB untracked against 1465 MB tracked
  (`footprint`). A buffer whose tags cannot be allocated is logged and ordered conservatively
  (every access to it waits for everything before).

## 2026-10-17: a GPU hang took the whole Mac down (Task 10c run); GPU watchdog

- During a Task 10c benchmark run, the native renderer's GPU stopped making progress about a
  minute after launch. The scene was nearly empty (title or loading screen: ~9 encoders, 8 draws
  per frame, 46 FPS). About 90 s later WindowServer, whose main thread was blocked in the AGX
  driver submitting its own work, missed its watchdog and was restarted, which ended the session
  (`WindowServer_…userspace_watchdog_timeout.spin`, bb-probe had forked 151 s earlier). `kill -TERM`
  did not end the game; `-KILL` did.
- **`Metal::Device` GPU watchdog** (`BB_GPU_WATCHDOG_MS`, default 3000, 0 = off): if committed
  command buffers make no progress for that long, the process prints `STOP: GPU watchdog …` and
  `_exit(75)`s, so the driver drops our work before WindowServer stalls. Late wake-ups (system
  sleep, a stopped process) do not count.
- **Not reproduced** in three runs with the same code under the watchdog (city 2.5 and 4 min at
  ~88 FPS, title 3 min). A CPU-side fence checker (`BB_METAL_FENCE_CHECK`, with the Task 10c work)
  found no wait on a slot fence that another uncommitted command buffer updates, no commit-order
  inversion and no encoder waiting on its own slot fence. Cross-buffer waits seen are all on
  `AheadFence` with the hoisted-upload buffer committed first.
- Test runs: an outer watchdog in the run script KILLs the game when frame stats stop for 15 s.

## 2026-10-17: Task 10c closed at a measured state; frame pacing vs 0.4.0 at 60/90/120

Task 10c (per-command ordering: each wait narrowed to the encoders that conflict) ends below its
gate: city spawn, 1080p, upscaler off, uncapped, `BB_PRESENT_MODE=Immediate`: **88.8 FPS against
0.4.0's 98.8** (GPU 11.2 vs 9.9 ms/frame). Images checked (cages, lamps, fog, motion blur while
turning). Closed at this state per the plan; the GPU gap carries into Task 11.

Pacing (same scene and ini, windowed, display vsync, stand 40 s then turn 20 s; FPS / worst frame
standing / spread of guest flip intervals):

| Cap | 0.4.0 (MoltenVK) | Metal-native, two drawables | Metal-native, after the fix |
|---|---|---|---|
| 60 | 60.0 / 16.7 ms / 0.01 ms | 60.0 / 16.7 ms / 0.01 ms (3 of 5 runs) | same |
| 90 | 90.0 / 11.1 ms / 0.00 ms | **60.0** / 29.7 ms | 89.2 / 12.5 ms / 0.34 ms |
| 120 | 98-102 / 13.1 ms / 0.24 ms | **60.0** / 24.2 ms | 89-94 / 29.9 ms / 0.94 ms |

- **90 and 120 locked at 60:** with two drawables on the 120 Hz display, `nextDrawable` blocked
  16-17 ms every frame (a frame longer than one refresh waits a whole extra refresh for a
  drawable). Fix: three drawables when the frame limit is above 60 or off, two up to 60 (two keep
  the lower latency measured 2026-10-14). The presenter now prints its drawables and frame limit.
- **Intermittent 60-cap wobble:** 2 of 5 Metal runs at the 60 cap had a spread of 0.55-0.72 ms
  for the whole run (guest flips up to 18.3 ms, p99 18.2), the others 0.01 ms; 0.4.0 0 of 3.
  Present-to-present gaps stayed under 25 ms. Per launch, not per scene: backlog.
- Pacing script notes: the game's stdout is block-buffered into a file, so a log-silence
  watchdog needs a pty (`script -q /dev/null bash run.sh`) or a long limit.

## 2026-10-17: the GPU arena was committed in full (24 GB); lazy arena

The user saw ~25 GB in Activity Monitor. Dense Cathedral Ward scene from the user's save
(`save-backups/dense-20261017`), 2560x1440 native, 90 cap, `top` physical footprint:

| Arena | GPU process | Game process | FPS standing / turning |
|---|---|---|---|
| 16 GiB private (default before) | 24 GB | 3.2 GB | 57.6 / 59.8 |
| 8 GiB private | 15 GB | 3.3 GB | 57.6 |
| 4 GiB private | 11 GB | 3.3 GB | 58.4 |
| **16 GiB lazy (`BB_ARENA_LAZY`, default)** | **7.4 GB** | 3.2 GB | **59.1 / 59.8** |
| 0.4.0 (MoltenVK) | 22 GB | 3.2 GB | 48.8 / 51.7 |

- A private `MTLBuffer` is backed in full when created. The arena is now `vm_allocate`d memory
  wrapped with `newBufferWithBytesNoCopy` (shared storage, untracked): only touched pages count.
  Same FPS, images correct.
- How much of the arena the game reaches varies per run: 4.0 and 4.5 GiB in the same scene, so a
  4 GiB arena is not safe. `GPU arena: the game reached N GiB` is logged at each new 256 MiB step.
- In this scene the native renderer beats 0.4.0 by ~18% with steadier frames (0.4.0: 120 frames
  presented > 25 ms apart in 60 s; ours none).

## 2026-10-18: Metal-native M5 Task 11, parity matrix vs 0.4.0

Four scenes (`cages-20261016` Central Yharnam lamp, `livecopy-20261016` Cathedral Ward,
`dense-20261017` the user's dense Cathedral Ward view, `loadcrash-20261014-134937` Hunter's Dream)
x four configs, both builds with the same ini and save, uncapped (`fps_cap=0`,
`BB_PRESENT_MODE=Immediate`, `BB_VBLANK_HZ=480`), windowed, stand 30 s after the load. The display
was at a fixed 60 Hz (the user's setting), hence Immediate. FPS standing (mean of the 5 s windows):

| Scene | 1080p native | 1440p native | 1440p MetalFX Quality | 1440p TAA |
|---|---|---|---|---|
| cages | 97.7 / 97.5 | **75.1 / 66.2** | 95.7 / 95.1 | **69.9 / 61.9** |
| livecopy | 104.8 / 103.2 | **82.7 / 72.0** | 102.3 / 99.5 | **78.3 / 67.0** |
| dense | 83.2 / 78.5 | **65.2 / 57.2** | 81.0 / 76.7 | **61.7 / 54.4** |
| loadcrash | 104.3 / 98.0 | **82.0 / 69.5** | 100.9 / 92.6 | **76.1 / 64.9** |

(Metal-native / 0.4.0.) Metal-native is ahead or level in all 16; +12-18% at 1440p native and
TAA, +0-9% at 1080p and with MetalFX (more CPU-bound there). The city at 1080p, 88.8 vs 98.8 at
the end of Task 10c (display at 120 Hz then, eager private arena), is now level (97.7 / 97.5).

- **Pacing:** p99 frame times match (10.4 ms at ~100 FPS), but some Metal runs have one frame of
  25-46 ms every 5-40 s standing (0.4.0: none in the same windows). `BB_SPIKE_MS=20` (the default
  limit at 480 Hz is 2.6 ms, so the 20 printed spikes were ordinary frames) shows the recording
  thread waiting for the GPU (frames-ahead wait 21-36 ms, game thread on time, no compiles).
  `BB_LOG_LONG_CB_MS`: one command buffer takes 25-39 ms of GPU time instead of ~10, with no
  idle gap before it. `BB_GPU_PROFILE_SLOW_MS`: the extra time lands in a different encoder each
  time (an image upload, a copy-shader dispatch, a 32640-group compute, the G-buffer pass), so no
  single pass or shader. Not the lazy arena (`BB_ARENA_LAZY=0`: 43.8 ms), not the fence ordering
  (`BB_METAL_HAZARD_TRACKING=1`: 39.6 ms), not the fence checker (`BB_METAL_FENCE_CHECK=0`:
  41.7 ms). Uncapped only so far (GPU at 100%); the 60-cap check is in series 2. Backlog.
- **Skipped pipelines (all scenes):** three pipelines, the full-screen vertex shader 0x89cda9e4
  with fragment shaders 70bb42af, dbeccd9c and 1c3953c6 (G-buffer passes), read locations the
  vertex shader never writes. MoltenVK draws them; Metal refuses ("Fragment input(s) ... not
  written by vertex shader"), so their draws were skipped. The `fs 0x0` in the error's label is
  the wrong slot of the key (cosmetic); these were not geometry-shader pipelines. Fix: those
  inputs read as zero (`mtl_spirv_inputs.h`, `spirv-inputs-test`). At the cages save the
  atmospheric light behind the bars on the left is drawn now; 0.4.0's picture could not be taken
  (screen locked), so the caged figure stays open.
- **A 1080p R32F image re-detiled every frame:** `BB_LOG_UPLOAD_MB=5`: the tiled 1920x1080 R32F
  image at 0x7069fd0000 is refreshed ~11000 times in a run (every frame): its memory is written by
  the GPU through a buffer, which marks it GPU-dirty, and the refresh detiles 9.4 MB from the
  arena. Shared texture-cache code (0.4.0 likely does the same). Backlog (Performance).
- Memory: GPU process 6.3-6.6 GB in the dense scene (lazy arena).

### Task 11 series 2 (2026-10-18 night): attribution, validation, walks, CPU-bound

- **Ordering, city at 1080p uncapped:** default 97.4 FPS; `BB_HOIST_UNSAFE=1` 85.6;
  `BB_METAL_HAZARD_TRACKING=1` 86.6. The default (untracked buffers, per-command fences) is the
  fastest; the unsafe probe that reached 108 FPS in Task 10 is now slower than the default.
- **60 cap with display sync (Cathedral Ward, 60 s standing):** 60.0 FPS, every frame 16.7 ms,
  no spike over 25 ms: the long single GPU frames seen uncapped do not show in capped play.
- **CPU-bound check (960x540 render, uncapped, cages):** Metal-native 114.3 FPS, 0.4.0 119.6.
  Not CPU-bound on Metal: GPU busy 8.75 ms/frame (100%), so ~8.4-8.8 ms of GPU time stays at a
  quarter of the 1080p pixels. A fixed per-frame GPU cost (~350 encoders, ~2000-2400 fence waits, ~120
  inline copy-shader copies splitting passes) that 0.4.0 does not pay as much; at 1440p the
  faster pixel work outweighs it. The next GPU lever is fewer encoders, not shader time.
- **`MTL_DEBUG_LAYER=1` (errors to the log) at cages, Cathedral Ward, Hunter's Dream (60 cap):**
  no messages (32-37 FPS under the layer). With `MTL_DEBUG_LAYER_WARNING_MODE=nslog` the
  performance warnings (unused bindings, redundant state) wrote 16.7 GB in 10 minutes and the host
  was killed (signal 9): leave warnings off. The run scripts now stop at a 1 GB log.
- **`MTL_SHADER_VALIDATION=1`** crashed the host at startup inside Metal while loading the binary
  archive (`MTLGPUDebugDevice newBinaryArchiveWithDescriptor`); no archive under shader validation
  now. Then at the three saves: no messages (26-29 FPS).
- **15-minute walks** (60 cap; cages 1080p, Cathedral Ward 1440p MetalFX Quality): no crash, no
  `<Error>` line, 60.0 FPS mean (min 58.7 / 57.7), 2 of 181 windows with a frame over 25 ms
  each, no compiles. GPU process flat (6.4 / 6.2 GB after the first minute); arena high-water
  4.5 / 4.0 GiB.
- **The game process grows ~80 MB/min while walking, on 0.4.0 too** (cages walk: Metal-native
  2.5 -> 3.8 GB, 0.4.0 2.5 -> 4.0 GB over 15 minutes, linear). Shared game-side code, not the
  renderer.
- **Full screen (`BB_FULLSCREEN=1`):** the window stayed 2992x1686 (windowed) for the whole run;
  the screen was locked, and macOS runs the full-screen Space transition asynchronously (likely
  not at all while locked). To check with the screen unlocked.

### The game process grew ~4.5 GB an hour: low-allocator free lists (2026-10-18)

Cages walk at a 60 cap, `top` physical footprint of the game process (bb-probe): 2.5 -> 6.2 GB
over 45 minutes, linear (~75 MB/min), no plateau; 0.4.0 the same (2.5 -> 4.0 GB in 15 minutes).
The GPU process stayed flat (6.3 GB). `BB_MEM_STATS=1` (new: every 60 s) placed all of it in the
low range (`runtime_low_map`: +130 MB/min): host malloc 12 MB and the guest pool's resident pages
(2.7-2.9 GB) were flat. Per size class, blocks carved / live / on the free list after 3 minutes:
64 B 4,412,416 / 260 / 415 (rwlock holder records, ~24,000 allocations a second), 128 B and
32 B likewise.

Cause: `runtime_low_free` wrote the free-list link, which overlays the header's magic and size
class, and then indexed the free lists with the size class it had just overwritten, i.e. the
upper half of the old list head (8 for low-range pointers, 0 for an empty list). Freed blocks
went to the 8 KiB (or 32 B) list; the small classes never saw them again and carved a new 64 KiB
slab every few allocations, and an 8 KiB allocation could receive a 64-byte block (heap
corruption around the guest's lock objects). Fixed by reading the class first. Also: blocks over
64 KiB are unmapped when freed (they stayed mapped), and `runtime_low_map` starts over from the
bottom once it reaches the top of the low range.

After the fix: 2.48 -> 2.54 GB over 6 minutes (Metal-native), every class balanced (created =
live + listed); 0.4.x with the same fix 2.48 -> 2.73 GB, flat after the third minute. The fix
for 0.4.x is on branch `fix/low-alloc-free-list` (from origin/master, not merged).

## 2026-10-18: Metal-native M6, FSR 3.1 on Metal (gate) and 0.5.0 release checks

FSR 3.1 runs through `gpu/shim/ffx_metal/`, a Metal back end for AMD's `FfxInterface` (SDK 1.1.4
host code and SPIR-V blobs from the fsr-vulkan submodule, compiled to MSL by our compiler). On
synthetic frames (`tests/fsr3_synthetic.h`, golden data from the Vulkan path on MoltenVK in
`tests/data/fsr3/`) the Metal output is **bit-identical** to Vulkan's (HDR + auto exposure,
an odd 171x96 render size, a reset at frame 5). Two back-end bugs found that way: R32_UINT clears
must store the float's bits (FSR clears depth with 1.0f), and constant buffers must be padded to
the shader's size (148 -> 160 bytes, debug layer).

Uncapped (Immediate, 480 Hz vblank), stand 30 s, M4 Max 48 GB, arena 12 GiB on both builds (the
memory-sized default from PR #7's arena commit), FSR 3.1 Quality, 0.4.x (`master`, MoltenVK) vs
Metal-native:

| Scene | 1080p | 1440p |
|---|---|---|
| cages (Central Yharnam lamp) | 117.0 / **119.5** | 95.6 / **98.0** |
| livecopy (Cathedral Ward) | 122.0 / **126.6** | 98.8 / **108.6** |
| dense (Upper Cathedral Ward) | 92.0 / **97.7** | 75.7 / **84.8** |
| loadcrash (Hunter's Dream headstone) | 108.0 / **120.2** | 92.6 / **99.0** |

Gate "≥ 107 FPS at 1080p Quality" (measured at the headstone): 120.2. Worst frame per 5 s
window is higher uncapped on Metal (21-42 ms vs 12-17 ms): the known long single GPU frames
(backlog), not seen capped. `BB_FFX_FP16=0` (fp32 blobs): 121.8 vs 119.5 (one run each, within
noise). Pictures at cages and livecopy, 1080p and 1440p (0.4.x by window capture): same
image, the caged figure drawn. `MTL_DEBUG_LAYER=1` (errors to the log) at cages, livecopy and
the headstone with FSR Quality: no validation messages. 15-minute walk at a 60 cap, 1080p FSR
Quality: 60.0 FPS mean (min 58.8), no errors, the game reached 4.5 GiB of the 12 GiB arena,
game process 2.5 GB at the end (flat), GPU process 5.5 GB.

Single process (`BB_GPU_PROCESS=0`, the macOS 27 fallback until the reservation fix): the Metal
renderer runs in the x86-64 game process at a steady 60.0 FPS (every frame 16.7 ms), game
process 9.0 GB.

Not measured: the in-game preset switch (Quality -> Performance -> Native AA); the wrapper test
shows a size change recompiles no pipeline.

## 16 GB Macs: arena sized to the game, textures to what it leaves (2026-10-19)

Issue #19 (M1 Pro 16 GB, macOS 27, 0.5.1): 4-9 FPS at 1080p, GPU 39-117 ms a frame, and Metal's
allocated size 9.57-11.53 GB against a working-set limit of 11.55 GB (MTL HUD report). The
arena was 8 GiB there (a quarter of memory, at least two 4 GiB pages), and the texture
collector left the resident buffers out of its usage but measured against the whole working
set, so images could reach ~8 GB beside it.

- The game's direct memory is the same every run: 14 allocations, 4.48 GiB, at start; with
  flexible memory the highest mapping ends 4.80 GiB above 0x7000000000 in every log (ours,
  Didjus's, the #19 reporter's 88-map session).
- Arena = direct memory + 1 GiB, rounded up to 512 MiB, on every Mac: 6 GiB (10 GiB with
  `BB_DMEM_MB=9152`). Exact sizes now: the 4 GiB page table points at it and `GetArena` checks
  the end.
- Texture budget = working set minus resident buffers (at least 2 GiB); the trigger stays at 70%.
- M4 Max, Hunter's Dream headstone, 1080p, 60 cap, stand + turn: 60.0 FPS, no spikes, with
  6 GiB and with `BB_ARENA_GIB=12` alike; GPU process 5.4-5.8 GB vs 6.0-6.5 GB with 12 GiB.
  Arena reached 4.50 GiB.
- The same with a 16 GB Mac's limits (`BB_GC_BUDGET_MB=11015 BB_MAX_BUFFER_GIB=9`): 60.0 FPS;
  Metal allocated 7.1 GB at start, 9.6 GB at most (6.7 GB of it resident buffers), image
  trigger 3.0 GiB, 0 images freed. Not measured on a real 16 GB Mac yet.


## 16 GB Macs, part 2: no extra direct memory at or below 1080p (2026-10-19)

Issue #22 (M5 16 GB, macOS 27, 0.5.2, 1280x720): 9 FPS, GPU 68 ms a frame (vertex 44 ms), Metal
11.86 GB allocated. run.sh's startup-patch path (live resolution off, any scaled output: 720p, or
an upscaler at 1080p and below) still raised direct memory to 9152 MiB, so the arena was 10 GiB.
The raise came from upstream (644ec0f) for scenes above 1080p, together with patches.py's heap
patch; on macOS those scenes always take the live path. Now only scenes larger than 1920x1080
get it (the heap patch's condition). Native 1080p never took this path.

- M4 Max, Cages save, 16 GB limits (`BB_GC_BUDGET_MB=11015 BB_MAX_BUFFER_GIB=9`), windowed, 60 cap:
  720p startup patch: direct memory 5056 MiB, arena 6.0 GiB (reached 3.75), 60.0 FPS stand and
  turn, 0 spikes, GPU process 4.6-5.0 GB, scene correct. 1080p native: arena 6.0 GiB, 60.0 FPS,
  GPU process 5.5-5.9 GB.

## The refused tessellation pipeline: TES patch struct missing the unread locations (2026-10-19)

Every session logged `the evaluation shader reads 160 bytes of patch constants, the control
shader writes 224` and skipped that pipeline's draws. From the user's MSL cache and the SPIR-V
(`spirv-dis`): TCS 0x2807dd6c writes Patch locations 8-21 (14 vec4s), TES 0xf0c066a1 reads
12-21. SPIRV-Cross dropped the unread 8-11, so the TES struct put location 12 at offset 0 and
indexed the patch buffer with a 160-byte stride. Fix: the TES variant declares the TCS's whole
patch range as per-patch inputs; with `raw_buffer_tese_input` SPIRV-Cross adds `uint4`
placeholders. Rebuilt structs: 4 placeholders + 10 values, 224 bytes, offsets as the TCS.
`bbgpu-host --msl-check` on a copy of the user's pipeline cache: 649 built, 0 failed (30 of 30
tessellation). The first attempt padded from location 0 and only worked because SPIRV-Cross
skips locations the per-vertex inputs (0-7) occupy; the range now starts at the TCS's first
patch location. Not yet seen in game.

## PR #20's performance changes on the M4 Max, one at a time (2026-10-20)

Master 1cf9f4a vs master plus each change (deferred copies = c3785a5 + 382cec9), windowed,
uncapped (Immediate, 480 Hz vblank), warm caches, mean of 3 alternating runs (stand / turn FPS),
GPU-process CPU from `top`:

| | Dream headstone 1440p native | GPU process | City (Cages) 1080p native | GPU process |
|---|---|---|---|---|
| master | 82.1 / 80.4 | 225% | 109.4 / 115.9 | 295% |
| deferred copies | **86.5 / 85.8 (+5.4% / +6.7%)** | 228% | **125.6 / 118.8 (+14.8% / +2.5%)** | 304% |
| sleep instead of spin | 81.6 / 80.2 | **175% (-22%)** | 108.3 / 113.3 | **265% (-10%)** |
| no texture hash | 81.9 / 80.4 | 222% | 110.4 / 119.7 | 292% |

- Deferred copies, uncapped at 1440p: more game-side spikes (7-12 per run vs 3-4, worst 56 ms
  once); not in the city. At the 60 cap (1440p, 2 runs each) both hold 60.0 FPS with the same
  pacing (stddev 0.01 ms, 0 frames over 1.5x median); 1-3 spikes per run vs 0-1, one present gap
  of 25.7 ms. Headstone screenshots identical to master's.
- All three together: Dream 86.0 / 85.5 with the GPU process at 177%. City: 5 alternating runs
  of the combined build, sleep vs `BB_PIPE_BLOCKING_WAIT=0` (spin): median 125.6 vs 127.1 FPS,
  host 283% vs 305%; each mode had one slow run (sleep 114.5, spin 123.3). An earlier pair of
  combined city runs measured 110.9 against master's 110.2 in the same session: the city scene
  swings between sessions, so only alternating runs compare.
- Taken onto master (authorship kept) for 0.5.3.

## Lossless texture compression and frame pacing on an M3 Pro (2026-10-20)

**Compression.** Metal compresses private textures losslessly only when they are created without
`MTLTextureUsageShaderWrite` and `MTLTextureUsagePixelFormatView`. Every non-BC image had both, so
nothing was compressed. On the M3 Pro (150 GB/s, a third of the M4 Max) that is the largest
cost of the frame.
- **The bound:** creating textures without the two usages gave +22% at the clinic, but the scene
  turned red: some images are written by compute or read in another pixel format.
- **Lazy usages:** images get both usages, once, when a binding needs one (storage, another pixel
  format, a stencil view). The texture is re-created and its contents copied.

Which views need PixelFormatView (Metal API validation, 64×64 test textures):

| View | Needs PixelFormatView? |
|---|---|
| sRGB ↔ linear, swizzles, level and layer ranges | no |
| reinterpreting formats (RGBA8 → R32Uint), RGBA ↔ BGRA, stencil views of depth | yes |

M3 Pro 18 GB, macOS 15.7, `m3pro-fixes-v1` (0.5.1 + PR #20) against the same plus this change,
alternating, warm caches, 60 s each:

| Scene | Before | After | GPU ms/frame | CPU ms/frame | CPU % (uncapped) |
|---|---|---|---|---|---|
| Iosefka's Clinic lamp, 1080p (2 runs) | 37.6 | **44.3 (+17.8%)** | 26.5 → 22.4 | 82.2 → 77.4 | 309 → 343 |
| Hunter's Dream, 1080p uncapped (2) | 49.7 | **62.2 (+25.2%)** | 20.0 → 15.9 | 57.5 → 53.8 | 286 → 335 |
| Hunter's Dream, 1440p live (2) | 28.5 | **40.2 (+41%)** | 35.0 → 24.8 | 68.3 → 56.5 | 195 → 227 |

CPU % rises only because more frames are drawn per second; CPU time per frame falls.

Images are within run-to-run differences:

| Scene | After vs before | Two unchanged runs |
|---|---|---|
| Clinic | 1.3–1.4 | 1.1 |
| Hunter's Dream 1080p | 6.9–7.7 | 5.7 |
| Hunter's Dream 1440p | 6.3–6.5 | 5.4 |

Most upgrades happen at load. In play, area loads add a few dozen, which the frame stats report as
`Texture usage upgrades: N (M MB copied)`. Metal API validation (`MTL_DEBUG_LAYER=1`) passes at the
clinic and at 1440p.

Two bugs were found before the PR:
- **Wrong colours on streamed textures.** The game writes RGBA8 uint by compute and samples it as
  unorm/sRGB. When a dispatch bound the image for writing and a later binding of the same dispatch
  upgraded it, that dispatch wrote the replaced texture, leaving garbage mips (purple or noisy
  enemies). Now every binding of a stage is upgraded before any binding takes a texture, and an
  upgrade gives both usages.
- **A stencil view without PixelFormatView.** The 1440p depth resample created one, caught by
  validation. Its source and proxy are now upgraded first.

**Frame pacing.** On the ProMotion panel a steady 60 FPS cap showed frames for 12.5 / 20.8 ms
alternately, although they were delivered every 16.7 ms, even with the GPU idle (title screen).
Per-frame display times came from `presentedHandler`.
`presentDrawable:afterMinimumDuration:` (97% of the cap's period) evens it out:

| Hunter's Dream 1080p, 60 cap | Off | On |
|---|---|---|
| Consecutive frames shown for different times | 97% | 4% |
| Frames shown 16.7 ms | ~40% | 98% |
| Late frames/min | 84 | 30 |
| Game flip → on screen (median) | 39.7 ms | **63.4 ms** |

It costs about a frame of input latency, so it is a menu toggle (`frame_pacing`), off by default.

## Full screen in the background harness (2026-10-20)

Full screen (`fullscreen=1`, MetalFX 1440p, 60 cap, app not focused): 59.5-59.8 FPS with 30-40
game-side spikes per run and worst frames of 25 ms (three 120 Hz refreshes), on master and on
the cleanup branch alike; windowed the same runs have 0 spikes. The user plays full screen and
reports smooth play, so this is likely the unfocused harness. Not chased.
**Wrong (2026-10-21):** it was the two drawables, not the harness (#25; three drawables: 0-2
spikes; see below).

## 0.5.3 cleanup: no MoltenVK or Vulkan loader in the app (2026-10-19)

Milestone M7 of the Metal renderer plan, branch `cleanup-0.5.3`: MoltenVK, the Vulkan loader,
VMA, Zydis, xbyak, FSR 4's tools and the `renderer` setting left the build and the app.
`bash scripts/macos/package_app.sh` on the M4 Max, before (5e27895) and after:

| | Before | After |
|---|---|---|
| Zip | 51,280,513 bytes | 46,736,600 bytes (−4.5 MB, −8.9%) |
| App (sum of files) | 134,629,957 bytes | 119,700,516 bytes (−14.9 MB) |
| libbbgpu (x86-64 slice) | 15.9 MB | 13.6 MB (exported symbols list + dead stripping) |
| Gone from Frameworks | | libMoltenVK (10.9 MB, universal), libvulkan.1 (1.4 MB) |

- No Mach-O in the bundle links a Vulkan library or imports a `vk*` symbol (`otool -L`,
  `nm -u`); `package_app.sh` now fails if one does. Before: bb-probe imported 26 `vk*` functions
  (the `--vulkan-only` smoke test), bb-gpu-capabilities 7.
- SDL created a Vulkan window (`SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN`), which makes SDL load a
  Vulkan library; the window is a Metal window now. The split test (host window) passes.
- `live_resolution=auto`: the removed Vulkan check answered 0 on every Mac (MoltenVK reports
  Apple GPUs as integrated; it wanted a discrete GPU), so run.sh answers 0 itself on macOS.
- The x86-64 library with `BbSplit::Remote()` a compile-time `true` (scratch experiment): 5.0 MB.
  Not done: it changes behaviour before `bbgpu_init_remote` (MACOS_BACKLOG, Performance).
- `build_macos.sh --test`: the same PASS lines as before minus the two MoltenVK interop runs
  (221 vs 223), the same error lines. A build against a copy of `deps/` without the loader,
  MoltenVK, VMA, Zydis and xbyak (symlink overlay) succeeds.


2026-10-20 — opt-in audio dropout diagnostics (split from PR #26). BB_AUDIO_STATS=1
adds converted-PCM peak/RMS, non-finite samples and SDL output failure counts to the
existing per-port five-second reports. Measure minimum queued bytes/starvation before
silence refill, rather than after submission. BB_AUDIO_TRACE=1 reserves a separate
100-error budget for unexpected AJM results; expected partial-input/output-full results
can exhaust the original trace budget before a later real failure appears. Samples,
gain, clipping and scheduling are unchanged; diagnostics stay disabled by default.

An earlier combined M2 Max 64 GB playtest lost music/effects, with menu effects still
working. The next session with existing trace flags did not reproduce it; the user
reported correct audio. Initial output had about 938 buffers/5s (937 expected), a
6.7 ms maximum gap and a non-empty queue. This is instrumentation, not an audio fix.
If reproduced, retain Audio stats/Audio trace lines and whether it followed travel/death
or ordinary play. No reproducible dropout or causal claim is asserted in this split.

Split validation: build_macos.sh completes for the x86 guest/library and both arm64
host variants. The extraction changes only runtime_audio.c, runtime_ajm.c and this note;
no playback algorithm, renderer default or launcher setting is changed.
2026-10-20 — launcher validation fixes (split from PR #26): the duplicate-launch
guard now recognizes bbgpu-host-macos27 as well as bbgpu-host/bb-probe. Game-folder
validation passes Python -B in both checkout and packaged layouts; importing prepare.py
must not create __pycache__ inside the signed app. The combined build passed strict
signature verification after launcher folder validation. Launcher model tests cover both
checker commands. No renderer defaults, lifecycle or resolution changes in this split.


## Invariant positions, full-screen drawables, ultrawide, and where traversal hitches come from (2026-10-21)

M4 Max, master after 0.5.3, windowed unless noted, warm caches, test data folder.

**Invariant positions (PR #20's `BB_SPIRV_PRECISE=invariant`, now the default).** Uncapped
(`BB_PRESENT_MODE=Immediate`), alternating runs, the invariant variant with its own GPU cache
folder (the mode is part of the shader cache version):

| | master (vertex NoContraction) | invariant |
|---|---|---|
| Hunter's Dream 1440p native, stand (3 runs) | 85.6 / 85.7 / 85.9 | 98.8 / 99.1 / 98.7 (+15%) |
| turn | 85.3 / 85.2 / 85.4 | 96.1 / 96.0 / 96.3 (+13%) |
| City 1080p (not GPU-bound) | ~119.8 | ~119.9 |

- The same frames match in screenshots in both scenes (mean pixel difference ~4-5 of 255,
  foliage and fog): no z-fighting or holes, unlike NoContraction's removal on MoltenVK
  (2026-10-13). City: 13-20 late game frames per run against 6-12 (watch).
- A shader meta version mismatch used to leave every old entry in the store for good (never
  preloadable, "could not be preloaded" each session). It now counts as an older cache format:
  the first session clears the store once and rebuilds it during play (test data: 460 old
  entries cleared, 0 late frames in that run at the 60 cap; next start 214 of 215 preloaded).
  The MSL check excludes older formats from its 90% rule (268 of 268 invariant pipelines built
  from the variant's cache).

**Full screen: three drawables.** 60 cap, city, `BB_SWAPCHAIN_IMAGES` 2 vs 3, two runs each:
late frames standing 15 / 10 vs 1 / 0, turning 11 / 18 vs 2 / 0, worst frame turning 24.9 vs
16.7 ms. A MacBook Air M5 dropped to 30-40 FPS full screen with two (#25). The presenter now
keeps three while the window is full screen and two in a window. The overnight harness could
not enter full screen with nobody at the Mac (no Space transition, `Window:` size unchanged,
also as a foreground app), so the switch itself still needs one look: `Presenter: 3 drawables
(full screen)` in the log.

**Ultrawide (#30).** 3840x1080, city: 60.0 FPS, pacing stddev 0.01 ms, scene depth observed
at 3840x1080, game arena 5.0 of 7.0 GiB, correct geometry and HUD. 5120x1440, Dream: correct,
55-57 FPS, GPU process 10-11 GB (the warning below 24 GB is justified).

**Traversal hitches.** Two 10-minute Hemwick walks (1440p MetalFX Balanced, 60 cap,
`BB_SPIKE_MS=20`), cold then warm pipeline cache: 42 spikes each, 15 archive writes in the cold
one and none in the warm one, so archive writes are not the cause here. Most spikes are the
first minute (title and loading at 30 FPS). In gameplay, 2-3 per minute, and nearly all are
"game late, flip on time": the game's frame took 21-27 ms while flips stayed at 16.7 ms
because frames were queued, so nothing visible. Visible ones (a late flip or present): 1-2 per
10 minutes. In the late game frames the main thread used ~6 ms of CPU; the game waited on
"frames ahead" (the GPU finishing the previous frame, 7.9 ms in an example) with 1.3x the
usual draws (1532 vs ~1215), 50-130 write faults (2-6 ms) against ~31 a frame. So the
headroom that hides them is GPU time: a slower GPU turns these into visible hitches, and GPU
time per frame is the lever for smoothness on base and 16 GB Macs.
- `BB_SAMPLE_MS` (main-thread sampler): the few long main-thread frames (14 over 20 ms in 10
  minutes) spread thinly over many guest addresses, no hotspot; our runtime ~3% (read, waits).
- The first sampler sent SIGPROF to the main thread every 1 ms: the game's texture streaming
  failed every time within minutes (`[Dantelion2 Panic] CSGXTextureCreateFragment.cpp(137)`,
  then the game's deliberate null write). It now suspends the thread and reads its state
  instead (no failures). Signals into guest threads are not harmless: the watchdog's SIGUSR2
  dump is the same kind of interruption (CLAUDE.md: it derails a worker).

**Encoders and render-pass splits.** `BB_ENCODER_STATS=1`, city at 1440p MetalFX, per frame:
~372 Metal encoders: 95 render passes started at pass boundaries, 23 after EndEncoding, **67
render passes split "after compute"**, 67 compute encoders opened inside a pass, ~24 compute and
~24 blit after ordering waits, ~32 blits after compute. All 67 in-pass computes come from
`Runs::DrawTessellated` (the vertex and control stages as compute kernels): tessellated draws
sit between ordinary draws, so each run ends the pass, runs its kernels and reopens the pass
with Load (a store and reload of the attachments on Apple's GPU). Upper bound, tessellated draws
skipped entirely (scratch switch, not kept), uncapped: GPU busy 6.60 / 7.49 -> 5.66 / 5.55
ms/frame, encoders 237 / 228 -> 155 / 154. Tessellation (kernels, patch draws and splits) is
15-25% of the city's GPU frame; hoisting the kernels out of the pass saves part of it
(MACOS_BACKLOG, Performance).

**First session after updating** (a copy of a 0.5.3-era store, 649 pipelines, then a 10-minute
Hemwick walk): the store cleared once ("649 pipelines of an older cache format"), 334 pipelines
compiled during the walk, the archive wrote 9 times (writes now spaced to 20x their duration;
the earlier cold walk wrote 15 times), 60 FPS mean, 14 game-side spikes in the minute entering
the area and 5 visible ones (late flip or present) over the remaining 9 minutes. A one-time cost
per player; the next start preloads.

**Frames ahead near the GPU limit.** Dream at 3456x1944 native, 60 cap (GPU busy 15.2-15.5
ms/frame), `BB_FRAMES_AHEAD` 1 (default) vs 2, two runs each: with 1, pacing stddev 0.01 ms and
worst frame 16.7 ms standing and turning in every run; with 2, turning had worst frames of 31-33
ms and stddev 1.1-1.5 ms. More frames ahead does not absorb heavy frames here, it bunches them;
the default stays 1.

## Metal's 32-bit index and indirect offsets; texture compression on an M4 Max (2026-10-21, evening)

**Vanishing geometry (#36, #35, #18).** DiAvisoo found that indexed draws more than 4 GiB into the
guest arena drew nothing on an M2 Max. Plain Metal probes (no port code) on the M4 Max:

| Command, offset into a 8 GiB no-copy buffer | below 4 GiB | 4 GiB + x |
|---|---|---|
| `drawIndexedPrimitives` (`indexBufferOffset`), direct and indirect | correct | reads at x |
| `drawPrimitives:indirectBuffer:` (`indirectBufferOffset`) | correct | reads at x |
| vertex/fragment/compute `set*Buffer:offset:` read by the shader | correct | correct |
| any of the above through a second no-copy buffer over the same pages, low offset | correct | correct |

(A first probe wrongly blamed shader buffer bindings: its "drawn" check was always true and the
failure was the indirect arguments.) Fix: `Metal::LowOffset` (`mtl_buffer.mm`) draws indexed,
indirect and patch draws and indirect dispatches from 3.5 GiB on through a window, a second
`MTLBuffer` over the arena's pages, 1 GiB long every 512 MiB. Metal counts a window's whole length
in `currentAllocatedSize` (the Metal HUD's "Metal" figure) without any memory: a 4 GiB window
added 4 GiB there with the footprint unchanged (0.50 → 0.50 GiB), and the first version (4 GiB
windows from 2 GiB on) showed ~4.2 GB more in the HUD in play with App memory equal. Normal play
reached the 2–4 GiB range in every test run; beyond 4 GiB needs streaming through areas.

**Texture compression (#33) on the M4 Max**, compression off (`BB_LAZY_TEXTURE_USAGE=0`) vs on,
alternating, uncapped (presenter limit 120): Hunter's Dream 1440p native 101.7 / 102.0 / 102.1 →
119.6 / 119.8 / 119.8 (at the limit); Central Yharnam 1080p 120 both. Screenshots: off-vs-on
differences equal off-vs-off (swaying flowers, fog); overall brightness overlaps across runs.
`MTL_DEBUG_LAYER=1` play runs clean in both scenes. 3–9 upgrades per run, at load, 14–35 MB.

**The user's play session, 0.5.3 → 0.5.4 (invariant default + compression + windows)**, Hemwick,
full screen 4K native, Metal HUD: 51.3 FPS / GPU 20.1 ms → 70.1 / 14.9; another view 80.7 / 12.6
→ 109.1 / 9.3.

**Window lookup cost.** With the window lock and dictionary lookup on every draw at 3.5 GiB and
above, the city (1080p, uncapped, window visible) fell from ~137 to 97–108 FPS over a minute as
streaming moved meshes there. With a per-thread cache of the last windows (no lock on a hit) it
held 125–148 (compression on) and 112–119 (off). Earlier runs read exactly 120 because the
harness window was in the background (macOS limits it); uncapped comparisons need it visible.

2026-10-20 — native ultrawide (split from PR #26): keep Kyo's complete scene/camera/UI
patch, including the dependent MOV/NOP writes and fallback globals. Removing only part
of that sequence caused a squeezed scene, stretched/black menus and a 640x360 scene.
The full sequence fixes those dimensions; bypass live resizing/upscaling for this path.
Keep the first retail graphics heap. Above 1920x1080 pixel count, grow only the second
texture pool to 1536 MiB with 6144 MiB DMEM (7 GiB GPU arena): retail capacity reproduced
an error-9 combat texture allocation failure. Do not import the oversized community heaps.

The launcher warns for 3440x1440/5120x1440 below 24 GB physical memory. Invalid
ultrawide_res values now stop with one actionable line rather than a Python traceback.
Patch tests cover all five scene/camera/UI presets and heap bounds; CLI, launcher and
shell tests cover invalid input, memory-warning boundaries and disable/restart behavior.
The combined M2 Max 64 GB playtest rendered travel, combat and menus correctly, but also
used PR #20's invariant shaders and precise readbacks. Those experiments and performance
claims are not changes to the defaults in this PR. See METAL_ULTRAWIDE_TEST.md; independent
3840x1080/5120x1440 visual validation on the maintainer's machine remains requested.

Split validation: both architecture builds, launcher/model tests, 16 patch/ultrawide tests,
shell restart tests, settings-preservation and native scene-target roundtrips pass. The
broader build_macos.sh --test run stops at the unchanged mtl-encoder-test timestamp check
"the render encoder's fragment end is sampled, after the blit" on this M2; the Metal
encoder sources and that test are identical to master. No full-suite pass is claimed.

## Instanced tessellated draws were skipped: missing trees (2026-10-21, late)

After 0.5.4 the user still saw objects vanish. The log had `Metal draws skipped in the last 5 s:
tessellation 300-2100` and `instanced tessellated draws are not encoded (skipped)`: the Metal
renderer skipped every tessellated draw with more than one instance (since 0.5.0). The test scenes
(Central Yharnam, Hunter's Dream) never issue one, so no test saw it. On a copy of the user's save
(Forbidden Woods) at 1080p, 60 cap: with `BB_TESS_INSTANCES=0` (skipped, as before) ~1900 skipped
draws per 5 s and the large mossy tree trunks around the player missing; with each instance drawn
as its own tessellated draw (own kernels and scratch, instance number in the vertex kernel's
stage-in region and the patch draw's base instance) no skips, the trees drawn, 60 FPS held (worst
frame 17.7 ms standing). City unchanged (no instanced tessellated draws there).

## Arena windows stalled the GPU queue: off by default (2026-10-23)

0.5.4 and 0.5.4.1 drew high index and indirect offsets through windows: no-copy `MTLBuffer`s over
the arena's own pages. They add no memory, but the GPU driver counts each window's whole length
as wired on top of the arena (noelmiller, #42, M5 Max 36 GB, macOS 27.0.1): command buffers asked
for ~30.9 GB against `maxWired` 30.2 GB (`IOGPUCommandDescriptor::prepare: required > max`,
~470,000 refusals an evening, starting exactly when a window was created). The driver paged on
every submission until the queue stalled: GPU watchdog exits (#43, #44, #45, #49, all 0.5.4.1),
WindowServer killed, once `bbgpu-host` stuck exiting and the GPU unusable until a reboot. With
no windows (`BB_ARENA_LAZY=0`): zero refusals, nine co-op sessions without a crash.

Windows are now off unless `BB_ARENA_WINDOWS=1`; high index ranges use #36's staging copy again.
Checked: index test (copy path, windows path, Metal validation), 116 unit tests, the Forbidden
Woods save (trees drawn, 60 FPS, no skips), the city (60), the Hunter's Dream 1440p uncapped
(127 FPS standing). Indirect arguments above 4 GiB are unhandled again, as before 0.5.4.
Lesson: "no extra memory" in the footprint is not "no extra wired budget" for the GPU driver.

## Upscaler haze in motion: vertical motion vectors reversed (M3 Pro, 2026-10-22)

A player's report: MetalFX at the Quality preset turns hazy while moving, pixels stay in place
while new ones appear, sharp again after stopping; FSR 3.1 worse. MacBook Pro M3 Pro, macOS
15.7.5, 1280x720 -> 1920x1080 (the tonemapped path, `RunScaled`).

**Ruled out, one play session each** (the player's verdict):
- motion blur and depth of field off: unchanged;
- the game's own AA off: unchanged (a ping-pong temporal pass after tonemapping, which the
  upscaler takes as its input; still worth skipping when upscaling);
- FSR 3.1 instead of MetalFX: also hazy, worse. So the shared inputs, not MetalFX's setup.

**Logged** (a logging build, per upscaled frame): the camera was found every frame; the motion
vectors were 0.01 px with the camera still, grew with movement, and matched the rotation's
prediction within 0.7-1.6x: present and the right size.

**Found:** the logging build also saved the centre of the upscaler's input (256x144 grey) every
frame. Phase correlation of consecutive frames gives how far the picture moved; the mean motion
vector of the same region (previous minus current) should be its negative.

| 491 / 1,275 frames moving > 3 px, lag 0 | horizontal error | vertical error | vertical, reversed |
|---|---|---|---|
| y-down reconstruction (before) | 1.76 px | 3.99 px | **0.77 px** |
| y-up reconstruction (after) | 1.63 px | **0.90 px** | 6.06 px |

Lags -2..+2 match worse than 0: the timing was right. Example frames before: picture moved -5.25 /
-13.79 / +3.98 px vertically, the vectors said +4.86 / +13.20 / -4.18. The y-down convention was
checked on MoltenVK on 2026-10-06; the native Metal renderer changed the picture's orientation under
it. `taa.comp` already used y up. The object motion vectors (recompiler, through the viewport scale)
have the picture's vertical sign (checked with object motion on); only the camera vectors were
reversed. After the fix the player saw no haze with MetalFX or FSR.

**Also found:** `TemporalUpscaler::Run` (Native AA, the live path above 1080p) returned silently when
the scene colour had no ShaderWrite usage, which lazy texture usages never gave it: those modes ran
without the upscaler. It now upgrades the scene colour once (16.7 MB at 1080p); MetalFX Native AA
runs again ("metalfx 1920x1080 -> 1920x1080 (HDR scene color)").

Not checked: whether the jitter's direction is right on the Metal renderer (the player plays with
jitter off; `BB_METALFX_JITTER_SIGN` exists for that test).

## The memmove crash: the render thread's per-frame command pool (#34, #37, #38, #54, #57, 2026-10-23)

Five reports, one site: guest 0x21d5b45, a NULL destination. Seen on macOS 26.5.1 and 27.0.1,
M3 Max (36 and 128 GB) and M5 Pro, ports 0.5.3 to 0.5.4.1, always while characters spawn, with
new pipelines compiling just before. It is not the Mac's memory.

What the disassembly shows:
- **The crash:** a draw command (0x21d5a80, run by a render job worker: 0x263b246 → 0x21b9841 →
  0x21b7c02 → 0x294a400 → 0xec7470/0xec7c90) copies a 0x398-byte record into a 0x4d0-byte node
  of the frame's command list, so it can run later on the render thread's immediate context.
- **The allocation:** it comes from a chunked bump arena (0xe45560, minimum chunk 0x800) whose
  parent is a 256 KiB linear heap per list. The renderer carves that heap once
  (0x2195c09/0x2195c46). The heap's allocate (0xe453b0) returns NULL when full, and nothing
  checks it.
- **The flip** (0x2198fd0, once per frame) swaps the two lists and resets the other one's heap.
  That's plain CPU code; no GPU or runtime signal is involved, and the pool doesn't leak.

Measured with the new probe on the Forbidden Woods save (standing and turning, 60 FPS): a
**peak of 8 KiB of 256 (3%)**, 73 of 216 records. A crashing frame is a rare spike of more
than 30 times the normal load. Its cause is unknown.

The fix (src/runtime_hooks.c, observer hooks: no lock):
- The flip and the heap's allocate are hooked. The log gets the pool's high-water mark, and
  each allocation that fails on a frame heap.
- Such an allocation gets memory from a spare pool instead (16 MiB per frame heap, from the
  low allocator). The spare pool is reused once the game resets that heap.
- Checked with `BB_FRAME_POOL_TEST=2`, which treats everything past 2 KiB as failed. Every
  frame went through the spare pool for about 8,700 frames: 60 FPS, the picture intact, and the
  spare pool never full. `BB_FRAME_POOL=0` turns the hooks off.

## Sinking through a lift: the uncapped frame rate patch (user report, 2026-10-23)

The user died every time (4–5 tries) in one lift: the character sank through its floor and fell.
The port always applied **Uncap FPS++** (`BB_FPS=uncap`, run.sh's default), which advances the
game by each frame's measured time; its own note warns the timestep fixes vary with a
fluctuating frame rate. At that spot (3840x2160 native, cap 60, M4 Max) frames alternated
10 / 35 ms (`BB_FRAME_STATS=1`: 50–120 GPU page faults a frame, up to 30 ms waited). Same save
with **60 FPS++** (`BB_FPS=60`, fixed 1/60 s steps): the lift worked. Frame pool peak in those
sessions 15 of 256 KiB (memmove fix never needed).

Change: bbport.ini `game_timing=fixed|variable` (launcher: General → Game timing; default
fixed). `scripts/launch_settings.sh` picks BB_FPS from it and `fps_cap`: fixed gives the
unpatched game at 30, 60 FPS++ at 60, 90 FPS++ (+ sprint fix) at 90; 40 and 120 have no
fixed-step patch and stay on Uncap FPS++. Fixed steps run the game slightly slower when a scene
can't hold the cap. An explicit BB_FPS still wins; a value picked by the launcher is re-picked
after an in-game restart (BB_FPS_AUTO).

## Red flash on the hunter's breath: GCN legacy and clamped float ops (Cainhurst, 2026-10-24)

On the Cainhurst rooftops the breath vapour (about every 4 s) flashed as a bright red ball, with
the whole frame's exposure shifting on the same frames: one non-finite or huge red value spread by
bloom and seen by auto exposure. Screen recordings (60 FPS, red pixels counted per frame):

| Build | Frames with the flash |
|---|---|
| 0.5.4.5 | 70 of 4,225 (1 in 60), scattered frames through each breath |
| `BB_MSL_SAFE_MATH=1` | 9 of 2,179 (1 in 240), one 3-frame flash at the start of each breath |
| legacy/clamped ops translated | 6 of 3,229 (1 in 540), only at some camera angles |

Safe math turning the scatter into one flash at the particle's birth pointed at a value that is
infinite on IEEE hardware but defined on GCN. The translator mapped `v_mac_legacy_f32` and
`v_mad_legacy_f32` to plain multiply-add (GCN: 0 * anything = 0, so 0 * Inf gave NaN here) and
`v_rcp_clamp_f32`, `v_rsq_clamp_f32` and `v_log_clamp_f32` to the unclamped ops (GCN: Inf becomes
+-FLT_MAX). The game uses `v_mul_legacy`, `v_mac_legacy`, `v_mad_legacy` and `v_rsq_clamp` (a
"Shader: game uses …" line is logged once for each). `v_rcp_legacy_f32` took the sign from a
value conversion instead of the bits. Second part: Metal's fast math (the default) folds
`isnan`/`isinf` to false, which turned every NaN-safe translation (legacy min/max, `v_cmp_class`,
these ops) back into plain ones; NaN and Inf are now tested on the bit pattern, and the clamp
output modifier turns NaN into 0 as GCN does. Shader cache meta version 11 (rebuilt once).
City at 1080p still holds 60 FPS; all 214 pipelines of the test cache build in Metal.

## PRs #39, #58, #59 merged onto 0.5.5, M4 Max (2026-10-24)

Each PR merged onto master f5b7073 in a worktree; test saves only, upscaler off, uncapped
(`BB_FPS=uncap BB_FPS_LIMIT=0 BB_VBLANK_HZ=480`). At 1440p every build, master included, sits at
the presenter's 120 FPS ceiling in both scenes, so GPU time and 4K runs separate them.

| Build | 4K Dream FPS | 4K city FPS | GPU ms/frame 1440p Dream / city | Recording µs/draw (city) | 300 s walking soak: frames > 1.5× median / worst |
|---|---|---|---|---|---|
| master | 77.7 | 73.8 | 7.97 / 8.33 | 4.5 | 119 / 52 ms |
| #39 held draws | 80.5 (+4%) | 82.8 (+12%) | 7.64 / 7.50 | 4.5 | 150 / 156 ms (one run) |
| #58 pass merge + no-op skip | 78.3 (+1%) | 75.6 (+2%) | 7.87 / 8.08 | 4.6 | — |
| #59 smoothness | — | — | 8.14 / 8.27 | 3.6 | 78 / 44 ms |

- #39: the biggest GPU win, survived the soak without a hang, but its one walking soak had worse
  pacing than master's (p99 27 vs 14.6 ms). Repeat the soak before merging.
- #58: small gains; the pass-variant map is keyed by a bare 64-bit hash (no key compare) — ask
  for a full key compare before merging the pass merge; the profiler fix and no-op skip are fine.
- #59: recording CPU −20% and fewer walking hitches, as claimed, but 2 of 4 standing city runs
  were slower (99 and 113 FPS; master never below 117). Suspect the default user-interactive QoS on
  spin loops; try with the QoS commit reverted. `BB_DIRECT_READBACK=verify`: 15 read-backs
  checked, 0 mismatches — far too few to prove it (standing and walking cause few read-backs).
- Scripts: scratchpad `prbench.sh`/`prbench2.sh` (driver over the usual run.sh harness).

## macOS 27.0.1 on the M4 Max: first check (2026-10-24, evening)

master eb10466 (0.5.5 + launcher merges), `bbgpu-host-macos27`, same conditions as the PR table
above (test saves, upscaler off, uncapped, `BB_PRESENT_MODE=Immediate`), windowed 2992x1680,
mean of the last six `Frame stats` windows, standing after the load, no compiles:

| Scene | 4K FPS macOS 26.6 (master f5b7073) | 4K FPS macOS 27.0.1 | GPU ms/frame |
|---|---|---|---|
| Hunter's Dream (`loadcrash-20261014-134937`) | 77.7 | 74.4 (−4%) | 13.43 |
| Central Yharnam lamp (`cages-20261016`) | 73.8 | 71.0 (−4%) | 14.08 |

1080p sits at the 120 FPS presenter ceiling on both. Both GPU-bound (GPU ~100% busy). The user
was using the Mac (Safari, Discord) during the 27 runs, the 26.6 runs had an idle Mac, so −4% is
an upper bound; no sign of the 4x drop reported on an M3 Pro 18 GB (#69). Harness: scratchpad
`bench.sh` (restores a snapshot, Continue twice through `BB_PAD_FILE`, `BB_FRAME_STATS=1`,
kills the game if Frame stats stop for 20 s).

## Benchmark: first runs, route repeatability, PR #39 (2026-10-24 night)

Built-in benchmark (branch `benchmark`, `docs/DESIGN_BENCHMARK.md`), M4 Max, macOS 27.0.1, test
data, `--benchmark` runs alternating builds, no compiles in any run.

- **Repeatable route:** the same streaming hitches (100-150 MB of uploads in one frame) land at
  6.9 s, 44.3 s and 63.5 s of the route in nearly every run, both builds. "Other" hitches (frame
  44-50 ms with game ~16 ms and GPU ~8 ms: the present side) recur too; worth a "present" cause.
- **Every frame** of the city walk uploads ~14 MB and takes ~46 GPU-tracked faults (median), so
  the hitch classifier judges streaming against the run's medians.
- **Standing uncapped at 1080p:** ~124 FPS, game-CPU-limited (game frame ~7.9 ms, GPU ~6.8 ms);
  a 27-39 ms GPU frame every 5 s (the known long command buffer, 2026-10-18).
- **PR #39** (held draws in tessellation runs) on master, 3 runs vs master 2:

| | master | #39 |
|---|---|---|
| Standing FPS | 123.4 / 124.8 | 125.2 / 124.8 / 125.3 |
| Standing GPU ms/frame | 6.90 / 6.76 | 6.23 / 6.16 / 6.11 (−9%) |
| Route GPU ms/frame | 9.12 / 9.18 | 8.93 / 9.11 / 8.51 |
| Route frames > 33 ms | 3 / 4 | 7 / 5 / 5 |
| Route ms lost to hitches per minute | 34 / 39 | 74 / 53 / 46 |
| Route 0.1% low FPS | 28.4 / 28.5 | 25.3 / 26.7 / 28.7 |

  GPU time −9%, but more hitches on the route in every run (extra single frames of 35-60 ms GPU),
  as in the 300 s soak. Repeat before merging (posted on #39).

## 2026-10-25: 16:10 native resolutions (#75)

Kyo's 16:10 patches (1280x800, 1920x1200, 2560x1600) go through the same path as the ultrawide
presets (`ultrawide_writes`: scene/UI size, camera aspect, the texture pool above 1080p).
M4 Max, benchmark save in Central Yharnam, upscaler off: both 1920x1200 and 2560x1600 render the
whole scene at that size ("observed scene depth"), the HUD and item boxes sit correctly, 60 FPS
at the cap. 2560x1600 gets the under-24 GB memory warning like the 1440p ultrawides.

## 2026-10-25: benchmark route repeatability (replay clock)

The traversal replay stepped by the GPU host's presented frames, which lag the game by a varying
frame or two. Draw counts per 10 s block (% from a reference run): the first ~50 s always matched,
later blocks differed by 25-57% in most runs (two or three repeating paths). The game reads the pad
once per frame (7815 reads = 7815 frames shown), so the replay now counts pad reads. Three runs
with the new clock (M4 Max, test data): 0-5% in every block. The route file's wall-clock times
still make the character bump walls/items near spots where the recording hitched (the same way
every run); re-recording with frame numbers is planned (DESIGN_BENCHMARK "Next").

## PR #20 on an M3 Pro, against 0.5.1 (2026-10-19)

MacBook Pro M3 Pro (11 CPU / 14 GPU cores), 18 GB, macOS 15.7.5. A copy of a player's save at
the lamp in Iosefka's Clinic: camera still, 1594-1595 draws per frame in every window of every
configuration, no compiles while measuring; native 1080p, upscaler off, all effects, 60 cap (not
reached). Each configuration with its own warmed shader cache, 60 s measured after a 20 s
settle, process CPU from OS CPU-time deltas. Stock = the 0.5.1 release's GPU library; the game
process carries the load lock (2cb8681) in every configuration. "All" = the PR on 0.5.1 with
`BB_SPIRV_PRECISE=invariant` and the PR's arena sizing (dropped since; 0.5.2 sizes the arena).
Stock and all ran twice (31.4/31.3 and 38.1/37.9 FPS), the others once (±0.6%).

| | 0.5.1 | all | change |
|---|---|---|---|
| FPS | 31.3 | **38.0** | **+21.2%** |
| GPU busy, ms per frame | 31.9 | 26.3 | -17.5% |
| CPU per frame, both processes | 102.7 ms | 80.8 ms | -21.3% |
| GPU command thread per draw | 17.7 us | 5.2 us | -71% |

Each one switched off alone (all others on):

| Change | FPS without | gain | also |
|---|---|---|---|
| Copy-shader copies deferred to the pass end (27a21db) | 33.7 | **+12.8%** | GPU 29.7 → 26.3 ms |
| Invariant positions (`BB_SPIRV_PRECISE=invariant`; opt-in then, the default since 0.5.4) | 33.9 | **+12.0%** | GPU 29.5 → 26.3 ms |
| Draw-pipe sleeping wait (44300ab) | 38.6 | -1.5% | CPU 95.7 → 80.8 ms per frame, GPU process 58.8 → 44.5 |
| Arena sized from guest memory (dropped) | 37.7 | +0.6% (noise) | GPU memory -2.4 GiB |

Invariant positions on this Mac: no flicker, z-fighting or missing geometry seen in the clinic
or Hunter's Dream screenshots; the player, who has played with it on since 2026-10-17, reported
no artifacts (one check, 2026-10-17). Deferred copies: 0 conflicts in both scenes, ~170 copies deferred per frame in the
clinic; screenshots 0.7 s apart switching it on and off differ less (1.88 mean per channel)
than same-setting screenshots 1.4 s apart (3.10).

## 2026-10-25: FPS++ patches wrote RET into the game's condition wait

30/60/90/Uncap FPS++ write `C3 90 90 90 90` at 0x02483EC1, the timed-wait `call` inside the
game's condition wait (0x02483E80: push rbp/r15/r14/rbx/rax; untimed waits, edx = -1, take another
call). A RET there returns to the saved rax: any timed wait would crash (one caller would jump to
0). Never seen, so timed waits are evidently not reached in normal play. patches.py now drops that
write from every patch file (UNSAFE_WRITES), leaving the game's own wait. M4 Max, test data: 60 FPS
benchmark walk 59.9 FPS, 8 frames > 33 ms (as before), same route; 120 cap with Uncap FPS++ 60 s,
119.4 FPS, no crash. Enemy Control is now refused alongside an FPS patch (overlapping bytes at
0x1972a7d). Source: an assessment comparing the port with Paleblood, verified by disassembly.

2026-10-20 — launcher Stop (split from PR #26): retain this launcher's Process;
Play becomes Stop/Stopping and settings are disabled while playing. Intentional Stop/Quit
does not produce a crash sheet. For sessions started through the launcher window, keep
it available behind the game with both launcher and game Dock icons. Direct launch
("Show launcher every time" off, without the Option override) keeps the window hidden
and the launcher an accessory app, so only the game has a Dock icon. Reopen explicitly
reveals the launcher and leaves direct mode. Quit waits for its child to finish. This
touches the same gameEnded/showWindow lifecycle as update PR #23; rebase after #23 merges.

PR #31 review follow-up: onStarted now preserves the direct path's orderOut/accessory
behaviour instead of unconditionally showing a launcher window. Startup selection,
Option override and the existing direct-path exit/error handling are unchanged.

An additional stop test exposed SDL's SIGTERM handler in the windowless game: it only
queues an SDL quit event, which the guest does not consume. run.sh now exports
SDL_NO_SIGNAL_HANDLERS=1. The native host keeps its own termination handling. Without a
mod view, run.sh execs bb-probe; with one, its existing trap forwards SIGTERM, waits for
the child, then removes only its private merged view. tests/test_launcher_stop.py drives
the production GameRunner.start and Process.terminate against run.sh with a real x86 SDL
probe, both directly and with an actual mods.py merged view, using synthetic assets only.
Preparation is stubbed; this is process-lifetime coverage, not a full gameplay test.
The previous combined build also passed game Stop/restart/Quit with copied saves, and the
SDL signal fix passed a restored-game stop with both guest and renderer confirmed gone.

Split validation: both production-Process termination tests pass on macOS with x86 SDL,
including removal of the merged view and unchanged synthetic source/mod files. Launcher
build/model tests pass. No private save data is used by these regression tests.

## 2026-10-25: the slow Macs are 16–18 GB Macs on macOS 27 (report survey)

Every performance issue and attached log was surveyed (two read-only investigations). Persistently
slow (5–17 FPS) reports after 0.5.0 all come from `bbgpu-host-macos27` on a 16–18 GB Mac:
#19 M1 Pro 16 GB, #22 M5 16 GB, #53/#69 M3 Pro 18 GB, a Reddit M2 Pro. Not slow: the same M3 Pro
18 GB model on macOS 15.7 (50–62 FPS at 1080p), 16 GB Airs on macOS 26 (#47, #25 after its fix),
big Macs on 27 (M4 Max 48 GB −4%, M2 Max 64 GB, M5 Max 36 GB after the 0.5.0 arena fix).
#22 is the cleanest A/B: the same M5 16 GB ran 0.4 (one MoltenVK process, the 27 host could not
start) at 45–60 FPS; every split-mode 0.5.x build about 4x slower.

The slow Macs are not GPU-bound. #69 (M3 Pro, 0.5.5, BB_FRAME_STATS): GPU busy 27–31 ms but
only 37–44% of wall time, frames-ahead waits 57–67 ms, GPU ticks block the GPU thread 86–89%; on
the title menu 6–14 draws, GPU busy 5–6 ms, still only 23–26 FPS. So a fixed delay per submission
before the GPU starts, the signature of #6 (idle GPU, 1.2 s submit-to-start, constant wiring) and
#42 (`IOGPUCommandDescriptor::prepare: required > max`). Wired 9.97 of 18 GB, swap 0.

Suspects (unconfirmed): (1) the GPU driver's wired-memory limit on small Macs under macOS 27, so
residency (the 6 GiB arena is in a queue residency set and declared on every encoder, the same
size on every Mac) is re-wired per submission; (2) something specific to split mode on 27 (the
mirrored guest memory, the exec-time reservation host). The arena is direct memory + 1 GiB
(`buffer_cache.cpp`), only the texture budget scales with RAM (`texture_cache.cpp`).

DiAvisoo's bbport-mac-optimized (the Reddit comparison) is KosmicKrisp (Vulkan on Metal) with a
sparse arena, tested only on an M2 Max 64 GB, and has been marked obsolete in favour of this port
since 2026-10-20; no M2 Pro result for it was found.

To confirm on a slow Mac: `log show --last 2m --predicate 'eventMessage CONTAINS "required > max"'`
while slow; `sysctl iogpu.wired_limit_mb`; then (diagnostic, resets at reboot)
`sudo sysctl iogpu.wired_limit_mb=14000` on an 18 GB Mac; and one at a time BB_GC_BUDGET_MB=6000,
BB_ARENA_GIB=5, BB_ARENA_LAZY=0, BB_FRAMES_AHEAD=2 with the benchmark. If (1) holds, the fix is an
arena that grows with the game's mappings (backlog idea (d), placement heaps / sparse-like).

## 2026-10-25: full screen vs window on an external 4K display (Reddit: "full screen is slower")

Several Reddit players report full screen slower than a window, especially on external displays.
Not reproduced on the M4 Max (48 GB, macOS 27) with an external 4K panel (GS27U, "looks like
1920x1080", 120 Hz, main display); throughput pass (standing, 45 s), test data:

| | window (3840x1902 px) | full screen (3840x2160) | full screen, drawable = image size |
|---|---|---|---|
| 1080p, 120 cap | 119.2 FPS, 1% low 70.2 | 119.8, 99.7 | 119.6, 91.2 |
| 1080p, uncapped, no vsync | 122.9, 75.8 | 120.1, 85.6 | 119.6, 77.8 |
| 1440p render, uncapped, no vsync (GPU 95% busy) | 110.8, 75.5 | 111.6, 88.0 | 110.8, 75.2 |

- The frame is prepared at the window's pixel size (`PrepareFrame` fits the image into the window),
  so in full screen the post-process pass, the present copy and the three drawables are at the
  display's full backing size (5120x2880 for "looks like 2560x1440" on a 4K panel). On this Mac
  that costs nothing measurable, even GPU-bound.
- Game Mode turns on for full-screen runs (gamepolicyd logs `gameDidLaunch`, other apps' states
  change) but bb-probe's and bbgpu-host's thread priorities are the same as in a window.
- Experiment on branch `fullscreen-drawable`: `BB_FULLSCREEN_DRAWABLE=output` sizes the frame and
  drawable to the image (in the display's shape) and lets Core Animation scale; `BB_DISPLAY=n`
  opens on the n-th display (#60). Not merged: no gain here; the drawable size follows each
  image, so it changes during loads (1080p loading screen vs 1440p scene); the overlay is drawn
  at the smaller size. *Next:* logs and benchmark reports from affected players (Mac, display,
  scaling, refresh rate, window vs full screen), and `BB_HIGH_DPI=0` as their quick test.
- **Direct vs composited (Metal HUD), later the same evening.** Full screen with nothing over the
  window is presented *Direct*; a window, or another app over it, is *Composited*. Without frame
  generation Direct costs nothing here: 4K native, 120 cap, 74.9-75.8 FPS full screen (Direct,
  sawtooth of one and two refreshes) vs 74.6 windowed. **With #64's frame generation it does:**
  the owner measured 60 real / 120 displayed composited but 40 / 80 Direct (interpolator delta
  23.67 vs 16.71 ms): a real + generated pair often takes three refreshes. Reported on #64.
- **Game Mode:** the packaged master app's scripted benchmark (own save, 1080p, test data), window
  vs `BB_FULLSCREEN=1` via launch.conf (gamepolicyd active for both, far more app-state changes in
  full screen): route 60.0 / 60.0 FPS (1% low 37.8 vs 42.5), uncapped 108.4 vs 107.1, bb-probe
  CPU 208% / 259% in both. No Game Mode penalty on this Mac.
- Still untested: a scaled mode on the external panel ("looks like 2560x1440" = 5120x2880
  surface), a 60 Hz external display, and slower Macs.

## 2026-10-26: 16-18 GB Macs slow down to 10-20 FPS: pinned GPU memory (#69)

Reported on 16-18 GB Macs on macOS 27 (#69: M3 Pro, 14 GPU cores, 18 GB, 27.0.1): 54 FPS at load,
then 10-20 FPS within a minute, at 1080p native, upscaler off. The reporter found that
`BB_ARENA_GIB=5 BB_GC_BUDGET_MB=6000` restores 59-60 FPS. Their slow logs show ~9 GB of GPU
allocations (6.5 GiB of it resident buffers: the 6 GiB arena and the 520 MiB page table and
fault buffer), under Metal's working set, no texture evictions, and `required > max` kernel
messages (`requiredBytes` ~17-20 GB against `maxWired` 14.3 GB).

**Reproduction on the M4 Max (48 GB).** `sudo sysctl iogpu.wired_limit_mb=14000` (Metal's
working set 37.4 → 13.7 GiB, the reporter's is 13.3) alone does not reproduce it: 60 FPS. With
a helper holding 30 GiB of locked, incompressible memory as well (~18 GB left, swap in use), it
does: 53 FPS then 15-17, as reported. The benchmark route drifts at low frame rates, so the
comparisons below stand at the route's start (benchmark throughput pass capped at 60, 90 s,
1080p native, upscaler off, windowed, no patches or mods).

| Build / setting | Resident GPU buffers | FPS after the first 15 s |
|---|---|---|
| master (0.5.8) | 6,664 MiB | 15-17 |
| `BB_GC_BUDGET_MB=6000` (texture budget ~1.4 GB, 405 images freed) | 6,664 MiB | 15-20 |
| PR #85 (page table and fault buffer only with direct memory access) | 6,144 MiB | 21-30 |
| `BB_ARENA_GIB=5` | 5,640 MiB | 45-60 |
| `BB_ARENA_GIB=5 BB_GC_BUDGET_MB=6000` (the reporter's) | 5,640 MiB | 52-60 |
| #85 + `BB_ARENA_GIB=5` | 5,120 MiB | 58-60 |
| **#85 + #86 `BB_ARENA_LAYOUT=runs`** | **3,530 MiB** | **58-60** |

The texture budget does not matter; the pinned (wired) buffers do. A fixed 5 GiB arena is not a
safe default: with 6 GiB of direct memory (16:10, ultrawide) the game's mappings reach past
5.0 GiB, and a request past the arena stops the game. #86's `runs` layout pins only the three
runs the game maps for the GPU (3,325 + 13 + 0.125 MiB, 64 MiB guard each) and covers any
mapping. The full benchmark route with #85 + #86 runs under the same limits: 58-60 FPS,
`result=ok`, 25.2 million arena requests all inside a run, no late run changes, no fallback.

**For 0.5.9 (a test build first):** #85 for every Mac; `runs` by default on Macs with 24 GB or less, or with
Metal's working set at 18 GiB or less; `legacy` on the others (`BB_ARENA_LAYOUT` chooses).

## 2026-10-26: the opening-movie freeze and crash on new games (#46, #50, #51, #24, #84)

Reported only on 16-18 GB Macs (macOS 27), never reproduced on the M4 Max until now: the opening
movie of a new game (and, per Reddit, the movie after signing the contract) freezes (`no new frame
for 30 seconds`) or crashes (`signal 11 at guest offset 0x26c01c3`, address 0xd2).

**Reproduction without a slow Mac.** A new game driven through `BB_PAD_FILE` (title → Play
Offline → New Game → brightness → controls → movie; scratch scripts `ng/start.sh`,
`ng/to-movie.sh`, `ng/stress-run.sh`) on an empty test save, with 16 busy-loop processes started
when the movie begins. Unstressed it reaches the Contract screen at ~150 s at 60 FPS; stressed
(movie at ~32 FPS) it failed 5 of 5 runs: 4 freezes at the movie's end (3101-3155 frames) and one
`0x26c01c3` crash.

**Two causes:**

1. **Freeze: AvPlayer stop request lost.** New hang report (`runtime_thread_dump_all`, printed by
   the hang watchdog: every guest thread's runtime wait and call chain, read with the thread
   suspended, host frames named with `dladdr`) showed main in `sceAvPlayerClose →
   ~AvPlayerSource → Stop → Thread::Join`, GXRenderThread waiting for main. At EOF the demuxer
   joins the decoders, and `Libraries::Kernel::Thread::Join` *moves the jthread out*; the owner's
   `Stop()` then called `RequestStop()` on the empty object, so the video decoder, waiting for a
   frame buffer the closing game never returns, was never told to stop. Fix: `Thread` keeps the
   jthread's `stop_source`, and `RequestStop`/`Stop` go through it.
2. **Crash: an unlocked path on GXRenderThread's immediate context.** During the movie the main
   thread records into GXRenderThread's context from `0x28fe7c0` (directly at `0x28feb09` /
   `0x28feb40`, and via `0x21696c0 → 0x26bc2e0 → 0x26bc320`, the draw list that faults on a null
   item), outside `0xebb960`. gnmdriver now also logs a write to an owned context *while another
   thread holds the context lock* ("outside the context lock"), with the call chain, and the
   handover line prints the previous user's chain too. Locking only `0x21696c0` still crashed
   (the direct writes); `0x28fe7c0` is now hooked (20-byte prologue incl. `and rsp,-32`, no stack
   arguments). `BB_MOVIE_LOCK=0` leaves it unhooked (A/B, fallback).

| stressed runs (16 busy) | froze | crashed | overlaps/run | reached Contract |
|---|---|---|---|---|
| before | 4 of 5 | 1 of 5 | 22 | 0 of 5 |
| player fix only (`BB_MOVIE_LOCK=0`) | 0 of 3 | 1 of 3 (GPU process, `SignalEop`) | 12-32 | 2 of 3 |
| both fixes | 0 of 4 | 0 of 4 | 0 | 4 of 4 |

Gameplay cost: benchmark route 60.0 FPS, 11 frames over 33 ms (master 2-10); the new hook never
waited during the route (no wait lines), so that is run-to-run noise.

