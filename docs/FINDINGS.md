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
# CPU profile while the game runs
sample $(pgrep -x bb-probe) 10 1 -file profile.txt
# GPU timeline while the game runs
xcrun xctrace record --template "Metal System Trace" --attach $(pgrep -x bb-probe) --time-limit 3s --output gpu.trace
```
