# Built-in benchmark (design, 2026-10-24)

## Why

Performance reports so far are "about 12 FPS somewhere" read off the FPS counter (#53, #69).
Players feel traversal hitches (smooth for minutes, then halved frame rate or 1-2 s freezes on
entering a new area), which a static scene never captures. The benchmark gives every Mac the same
route, the same settings and the same report, so results compare like for like. It is for players
with a genuine problem (and for us); a public results site (evill33t offered hosting) may come
later, so the summary is also written as JSON.

## What it does

The launcher's **Benchmark** button runs two passes, each a separate game launch, without touching
the player's save or settings:

1. **Traversal**: a recorded route through Central Yharnam (enemies cleared, ~130 s), replayed at
   the fixed 60 FPS game timing, so every Mac walks the same path. Measures drops and stutter
   (shader compiles, streaming, GPU, game CPU). Max FPS is meaningless here (capped at 60).
2. **Throughput**: standing at the route's start, uncapped (`BB_FPS=uncap`, no frame limit,
   480 Hz vblank, immediate present), 30 s. Measures max/average FPS and lows.

Settings are fixed (the "standard" benchmark): 1920x1080 output, native (upscaler off), game
effects at their defaults, windowed, texture compression on. The player's shader cache is used
(warm, as they play); the first pass of a fresh install shows compile hitches, which is real.

Assets (not in git: the save is a player-made test character, shipping it is the owner's call):
`benchmark/savedata/...` (the save folder tree, `1/CUSA00207/SPRJ0005...`) and `benchmark/route.pad`.
Packaged app: `Contents/Resources/benchmark/`; checkout: `<repo>/benchmark/` (in git since 0.5.6); `BB_BENCH_ASSETS`
overrides. Without them the button explains why it is unavailable.

## Interface: launcher → game (environment, per pass)

| Variable | Traversal | Throughput |
|---|---|---|
| `BB_BENCHMARK` | `traversal` | `throughput` |
| `BB_BENCH_OUT` | `<run dir>/traversal.csv` | `<run dir>/throughput.csv` |
| `BB_BENCH_SECONDS` | — | `30` |
| `BB_PAD_REPLAY` | `<assets>/route.pad` | — |
| `BB_SAVE_DIR` | `<tmp>/savedata` (fresh copy each pass) | same |
| `BB_CONFIG` | `<tmp>/benchmark.ini` | same |
| `BB_FPS` | `60` | `uncap` |
| `BB_FPS_LIMIT`, `BB_VBLANK_HZ`, `BB_PRESENT_MODE` | — | `0`, `480`, `Immediate` |
| `BB_FRAME_STATS` | `1` | `1` |

## Game behaviour in benchmark mode

- bb-probe presses Cross every 2.5 s from the first pad read until the game opens a map file
  (`/map/m..`): title screen, then Continue. No more presses after that.
- Gameplay is detected in the GPU host: frames of more than 1000 draws for 2 s, with no second
  without one (the loading screen draws ~130; uncapped, lighter frames come in between). The host publishes each frame's draws in the shared control block; bb-probe waits
  3 s more, then sets the benchmark phase to "measuring" and (traversal) starts the replay.
- The replay advances by **game frames** in benchmark mode (frame n plays the sample at
  n × 1000/60 ms), so a Mac below 60 FPS still walks the recorded path (the fixed timestep
  slows the game instead of skipping).
- End: traversal when the replay finished, throughput after `BB_BENCH_SECONDS`. bb-probe sets the
  phase to "done"; the host writes the CSV footer and closes the window (the game exits 0).
- Timeouts: no gameplay 150 s after start, or a pass over 10 minutes: the CSV ends with
  `# result=error: <reason>` and the game quits.

## Interface: game → launcher (`BB_BENCH_OUT` CSV, written by the GPU host)

```
# bloodborne-benchmark 1
# pass=traversal
# started=1729800000.123          (Unix time, seconds, when measuring began)
# output=1920x1080
t_ms,frame_ms,game_ms,gpu_ms,draws,compiles,compile_wait_ms,upload_mb,game_cpu_ms,gpu_thread_ms,ahead_ms,game_faults
16.7,16.7,16.6,9.1,4210,0,0.0,0.4,21.5,5.9,0.0,0
...
# ended=1729800130.456
# result=ok
```

Per frame (one row per presented frame while measuring): `t_ms` since measuring began;
`frame_ms` presented-to-presented (display time when the display reports it); `game_ms` between
the game's flip submits; `gpu_ms` GPU busy time since the previous frame; `draws`; `compiles` and
`compile_wait_ms` (pipeline compiles, time the GPU thread waited for them); `upload_mb` texture and
buffer uploads; `game_cpu_ms` all guest threads' CPU over the last game frame; `gpu_thread_ms`
the GPU host's command thread on CPU; `ahead_ms` the recording thread's wait for the GPU
(frames ahead: GPU-bound); `game_faults` GPU-tracked page faults in the game.

## Launcher: sampling, analysis, report

- **System sampler** (1 Hz during each pass): CPU % and physical footprint of `bb-probe` and the
  GPU host (`proc_pid_rusage`; times are Mach ticks), swap used (`vm.swapusage`), swap-ins
  (`host_statistics64`), memory pressure (`kern.memorystatus_vm_pressure_level`), thermal state.
- **System info**: Mac model, chip, CPU cores (performance/efficiency), GPU cores, RAM, macOS
  version and build, app version and commit, display, power source, Low Power Mode.
- **Metrics** (from the CSV):
  - Average FPS = frames / total time. Max FPS = best 1 s window. 1% / 0.1% low = 1000 / mean of
    the slowest 1% / 0.1% frame times (at least one frame).
  - Stutter: frames over 33.3 / 50 / 100 ms, longest frame, hitches (frame > max(2 × median,
    25 ms)), time lost to hitches (Σ frame − median) per minute, and the worst 10 hitches with
    time into the route and likely cause: compile (compile wait ≥ 30% of the frame), streaming
    (uploads ≥ max(32 MB, 3× the run's median) or faults ≥ max(20, 3× the median): every frame
    uploads ~14 MB and takes ~46 faults on the city walk), GPU (gpu_ms ≥ 60% or ahead_ms ≥ 50%
    of the frame), game CPU (game_ms ≥ 80% of the frame), else other.
  - GPU: busy % (Σ gpu_ms / Σ frame_ms) and ms/frame. CPU: mean/peak % per process. Memory: peak
    footprint per process, swap start/end/peak, swap-ins, worst pressure.
- **Output**: `~/Library/Logs/Bloodborne/Benchmarks/<date-time>/` with both CSVs, `system.csv`,
  the game logs, `summary.json` and `report.md`. A results window shows the report with **Copy
  Report** and **Show in Finder**.

## Scripted runs

`Bloodborne.app --args --benchmark` opens the launcher and runs the benchmark at once (no
question), for A/B comparisons of builds. Results land in `~/Library/Logs/Bloodborne/Benchmarks/`.

## First results (M4 Max, macOS 27.0.1, 2026-10-24)

Traversal: 60.0 FPS, 1% / 0.1% lows 41.9 / 31.7, 3 frames over 33 ms (worst 38.7 ms: two streaming
frames with 100-120 MB of uploads, one GPU spike), GPU 55% busy. Throughput (1080p, uncapped):
121.6 FPS average, best second 126, limited by the game's CPU frame (~7.9 ms) more than the GPU
(6.8 ms, 83% busy); a GPU spike of 27-39 ms every 5 s standing (the known "one long command
buffer" item, MACOS_BACKLOG Performance, FINDINGS 2026-10-18). A pass takes ~2.5 min (traversal)
and ~1.5 min (throughput) from launch.

## Route clock (2026-10-25)

The traversal replay first stepped by the GPU host's presented frames. The host runs a varying
frame or two behind the game, so an input sometimes landed a game frame early or late: runs
matched for the first ~50 s, then split into two or three repeating paths (draw counts per 10 s
block, 2026-10-24/25 runs). The game reads the pad exactly once per frame (7815 reads for 7815
frames shown on the route), so the replay now counts the game's own pad reads.

The route file still stores wall-clock milliseconds from the recording session. During recording
the game ran fixed steps, so a slow frame advanced the game one 1/60 s step while wall time moved
on further; replaying by game frames shifts inputs near those spots, and the character bumps
walls and items (the same way every run, so results stay comparable).

## Next: a benchmark suite (owner, 2026-10-25; after 0.5.6)

- Recording stores game frame numbers (pad reads since the recording started) instead of wall
  time, so a replay is exactly what was played. Re-record the Central Yharnam route with it.
- Several tests instead of one route; the owner records them:
  - traversal routes (capped at 60, the fixed timestep), including the current one;
  - uncapped tests;
  - "lantern" tests: load at another lantern and turn the camera a full 360° (GPU and streaming
    load of a whole area without movement).
- The launcher runs the set and reports each test separately.
