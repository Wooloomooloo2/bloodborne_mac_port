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

## Status (2026-10-18)

**Current version: [mac-v0.5.1](https://github.com/Wooloomooloo2/bloodborne_mac_port/releases/tag/mac-v0.5.1)** (download the app, or build it as below).

**Work in progress, playable.** On an Apple M4 Max the game boots, character creation works,
saves load, and the game runs at a **locked 60 FPS** at 1080p and 1440p native, or upscaled with
MetalFX, FSR 3.1 or TAA, through its own **native Metal renderer**. Expect occasional rendering
glitches (see [Known issues](#known-issues)).

The target is a locked 60 FPS (the game's physics run at 30 Hz), with even frame pacing, at the
highest resolution each Mac allows; 90 and 120 FPS caps are there for ProMotion displays.

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

Also fixed along the way: a MoltenVK crash from two threads sharing one command pool, and
geometry corruption at 1440p (scenes above 1080p now use live resolution changes).

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
launcher, with the game's settings on one screen and online play on a second tab (no
scrolling, even on a 13" MacBook Air):

- **Display:** output resolution, windowed or full screen, frame rate cap (30, 40, 60, 90, 120).
- **Upscaler:** off, MetalFX or FSR 3.1, its quality level, and the resolution the game
  actually renders at (for example 1280 × 720 → 1920 × 1080); FSR sharpening.
- **Effects:** depth of field, motion blur, chromatic aberration, ambient occlusion, shadows
  from dynamic lights, screen-space reflections, the game's own anti-aliasing, the FPS counter,
  and **skip intro videos** (straight to the title screen, no Sony and FromSoftware logos).
- **Locations:** the game folder (checked before Play is enabled) and the save folder (it
  offers to copy your saves to a new folder; the originals stay where they were).
- **Online** (second tab): play online on or off; the co-op pool, either **The Hunter's Dream**
  with **Sign in with Discord** (the window shows the page and the code, then who you are
  signed in as, with **Sign Out**) or a **shadNet server** with its address, account name and
  password; UPnP port mapping and the co-op UDP port. Passwords and tokens go to your login
  keychain, never into a file or the log. Setup guide: [docs/ONLINE.md](docs/ONLINE.md).
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

- **Missing geometry after a respawn or reload** (character's legs, a flight of stairs, a
  floor), reported on an M4 Max and an M2 Pro; a relaunch brings it back. It coincided with the
  load race below, now fixed, so it may be gone: reports welcome.
- **Occasional judder at a steady 60 FPS** (some game frames take 20–45 ms): much rarer since
  2026-10-15 (about 6x fewer late frames).
- **Uncapped, an occasional single long frame** (20–40 ms) on the Metal renderer; capped play
  (60/90/120) is not affected.
- **Above 1080p, the selected item's icon in the HUD is missing** (an empty frame; #14). It is
  there at 1080p.

### What's next

- **0.5.2: cleanup.** MoltenVK, the Vulkan loader and the unused FSR 4 code leave the app
  (smaller download); keyboard and mouse controls.
- **Fewer Metal render passes.** Profiling found ~408 Metal encoders per frame (190 render
  passes). Each extra render pass costs ~13–16 µs of GPU time on Apple GPUs regardless of
  resolution — about 3 ms per frame — and leaves the GPU idle in periodic gaps. Merging passes
  is the main lever for 1440p and 4K at 60.
- Confirm the missing-geometry reports are gone; the frame-time judder.
- Signing and notarisation of the downloadable app (today it must be released from quarantine).

Details:
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
  `libbbgpu.dylib`, rendering through native Metal (`renderer_metal/`). By default it runs in
  `bbgpu-host`, a **native arm64 process** that maps the game's memory at the same addresses
  (`host/`, `docs/DESIGN_NATIVE_GPU_PROCESS.md`); `BB_GPU_PROCESS=0` runs everything in one
  process.
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
the log to `~/Library/Logs/Bloodborne/game.log`. To change the game folder, use the launcher's
**Locations** (hold ⌥ while opening the app if the launcher is set not to show).

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

# 1. Build pinned dependencies (SDL3, Vulkan loader, FFmpeg, ...) and MoltenVK into deps/:
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
window, and writes the game's log to `~/Library/Logs/Bloodborne/game.log` (the session before
in `game.previous.log`). If the game stops with an error, the launcher offers the log. Run the
script again if you move the repository folder.

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

### Typing text

Text the game asks for (the character's name, the network password under *Network Settings*)
is typed on the keyboard: a box over the frame shows the prompt and the text (as dots for the
password), Backspace deletes, Enter confirms, Esc cancels and leaves the field as it was. The
field gets exactly what was typed; the game's own length limit applies (the log line
`ImeDialog opened (..., up to N chars)` shows it).

### Online play (shadNet)

Step-by-step setup, both pools and a troubleshooting table: [docs/ONLINE.md](docs/ONLINE.md).
The Hunter's Dream is the default co-op pool (bbhost and Windows-port players, works through
any router); shadNet is the alternative described below.

Messages, bloodstains and ghosts come from the community game server (The Hunter's Dream);
bells and summons go through a shadNet server (`srv.shadps4.net:31313` by default) with a
shadNet account. The network and PSN libraries are a separate module, `out/gpu/libbbnet.dylib`
(`gpu/bbnet`: shadPS4 v0.20.0's network, NP and shadNet code with its P2P transport, the same
code mainline shadPS4 and the Windows port run, so bells reach their players), built by
`build_macos.sh` once `scripts/macos/bootstrap_deps.sh` has added its libraries (protobuf,
OpenSSL, miniupnpc, nlohmann_json) to `deps/x86_64`. Offline the module is never loaded, and a
build without it plays offline exactly as before.

Turn it on in the launcher's **Online** tab (**Play online**, the pool, the account; it writes
the keys below), or by hand in `bbport.ini` (`~/Library/Application Support/Bloodborne/bbport.ini`):

```ini
online=1
shadnet_npid=YourOnlineId
shadnet_password=YourPassword
```

or with the same settings as environment variables in `launch.conf` (`BB_ONLINE=1`,
`BB_SHADNET_NPID`, `BB_SHADNET_PASSWORD`). Optional: `shadnet_server=host:port`,
`shadnet_webapi=http://host:31315`, `community_server=https://...`, `upnp=0` (no UPnP port
mapping), `p2p_port=3658` (the UDP port co-op traffic uses; the PS4's 3658 by default, `0` for
any free port), `online_pool=shadnet` or `huntersdream` (The Hunter's Dream by default; see below). An account is needed to sign in: without one the module loads but no shadNet user
connects, and the game stays offline. `run.sh` writes `user/host_overrides.json` for each launch (the game's server
`ss4.scej-network.jp` → the community server; summon signs → the shadNet WebAPI when it has the
Bloodborne summon service), and the log starts with `Online: module ...` when the module is in
use. `BB_GPU_LOG=info` shows the network libraries' own log.

shadNet servers check the client version reported after signing in and sign out builds they
do not accept (the log then says `the shadNet server rejected this client: ...`, and the
account's logins are refused for a few minutes). The module reports the shadPS4 release its
network libraries come from (`shadPS4 v0.20.0`); `BB_SHADNET_CLIENT_VERSION` overrides it. A
refused first sign-in is retried in the background; the title screen's *Play Online* works once
the log shows `Logged in npid=...`.

Co-op (bells, summons) needs UDP to reach the other player. On the shadNet pool the module
asks the router to forward its port through UPnP (`upnp=0` turns that off; forward `p2p_port`
by hand then) and also the port the signaling server observed, but a router that gives every
flow a random external port and answers only the host it was sent to (a symmetric NAT) still
leaves the players unable to reach each other, on either side. `BB_GPU_LOG=debug` logs the
signaling exchange (`NpSignaling`, `Lib_Net`) if a bell is not answered.

**The Hunter's Dream pool** gets through that: its session service relays and punches through
NATs, with bbhost and Windows-port players on the same pool. Sign in once with Discord: in the
launcher's **Online** tab, **Sign in with Discord** (the token goes to your keychain), or from
Terminal, as the Windows port's launcher does:

```bash
python3 scripts/online.py signin --ini ~/Library/Application\ Support/Bloodborne/bbport.ini
```

which writes `online_pool=huntersdream`, `thd_online_id=` and `thd_token=` (keep the token to
yourself) and leaves the rest of the file alone. The Hunter's Dream is the default pool: a file
that names no pool uses it, unless it holds only a shadNet account; `online_pool=shadnet` picks
the shadNet pool. The log then starts with `Co-op: The Hunter's Dream as ...`.
That backend is Supermedo's port of droogie's bbhost (`gpu/bbnet/thd`, GPL-3.0-or-later).
The server tells clients apart by their name and version and asks each port for its own:
`run.sh` presents this build as `bbport-mac <version>` (the repository's latest `mac-v` tag)
unless `BB_CLIENT_VERSION` says otherwise. The backend polls the server's event queue the way
its operators ask (long polling: one request held open up to 25 s on one kept-alive
connection, backing off on errors), and the game's play log goes out at the game's own pace.

### What online play exposes, and what protects it

- **Credentials.** The shadNet password and The Hunter's Dream token can stay out of `bbport.ini`:
  the launcher's **Online** tab always keeps them in the login keychain (it never reads them
  back, only stores, checks for and removes them), and from Terminal
  `python3 scripts/online.py keychain shadnet --npid NAME --ini <bbport.ini>` puts the password
  in the login keychain (item `bbport-shadnet`) and writes `shadnet_password=keychain`;
  `scripts/online.py signin --keychain` does the same for the token (`bbport-huntersdream`,
  `thd_token=keychain`). `run.sh` reads them with `security find-generic-password` at launch (a
  keychain prompt may appear once). A secret written in clear makes the file owner-only
  (`chmod 600`); `run.sh` warns when one is readable by other users. Neither is ever logged
  (the game's `X-BB-Token` header is filtered from the HTTP log).
- **In transit.** The game's own traffic and The Hunter's Dream backend go over HTTPS and the
  server certificate is checked against the system roots, also where the game loaded Sony's
  CAs (upstream shadPS4 turned verification off there; `BB_DISABLE_HTTPS=1` is the explicit
  opt-out). **The shadNet protocol itself is plain TCP** (its server offers no TLS listener: the
  password goes to `srv.shadps4.net:31313` unencrypted) and its WebAPI is plain HTTP, so use a
  password that is only for shadNet. The Hunter's Dream pool has no such exposure.
- **The network.** Co-op opens one UDP port (`p2p_port`, 3658) and asks the router to forward it
  through UPnP while the game runs (removed on exit); peers reach it directly or through the
  server's relay. What arrives is parsed by the game's own network code, as on a PS4. `upnp=0`
  keeps the router untouched.
- **Identity.** The module reports itself to shadNet as `shadPS4 v0.20.0` (the server signs out
  other names) and to The Hunter's Dream as `bbport-mac <version>` (`BB_CLIENT_VERSION`); the Discord
  device sign-in names the device "bbport on macOS", not the Mac's hostname. Your online id,
  position and play log go to the community server as the game sends them.
- **Dependencies** are fetched by tag and checked against a pinned commit (or SHA-256 for the
  MoltenVK tarball) by `bootstrap_deps.sh`; nothing downloaded is executed.

### From Terminal

```sh
cd bloodborne_mac_port
BB_GAME_DIR=/path/to/CUSA03173 bash run.sh
```

`run.sh` prepares the game image, rebuilds anything whose sources changed, and points the
Vulkan loader at the bundled MoltenVK. It uses the same saves and settings as the Finder
launcher.

### In the game

- **Settings overlay:** `⌘,` (or L3 + R3 on a controller): FPS, resolution, upscaler (MetalFX,
  FSR 3.1 or off), shader compile mode. Some changes need a restart. The launcher shows the
  same settings before the game starts.
- **Full screen:** ⌃⌘F or the green window button.
- **Input:** game controllers (through SDL) and the keyboard.
- **First launch and new areas** compile shaders, so the first minutes stutter a little; later
  sessions load them from the cache at start. The first launch after a rebuild takes ~20 s to
  open the window.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | Loader (`probe.c`) and PS4 HLE runtime |
| `gpu/` | shadPS4-derived Vulkan video core, draw pipeline, upscaler integration |
| `host/` | Native arm64 GPU host process (`bbgpu-host`) and the shared rings |
| `scripts/` | Eboot preparation, linking, patch compiler (`patches.py`); `scripts/macos/` dependency bootstrap |
| `patches/` | Community XML patches for Bloodborne 1.09 |
| `Bloodborne.command` | Finder launcher (runs `run.sh` in Terminal) |
| `scripts/macos/make_app.sh` | Builds `Bloodborne.app`, a launcher without a Terminal window (local, not committed) |
| `scripts/macos/package_app.sh` | Builds the self-contained `dist/Bloodborne.app` and its zip for releases (CI: `.github/workflows/macos-release.yml`) |
| `Saves` | Shortcut to the save folder (created on first launch, not committed) |
| `launcher/` | The macOS launcher window (Swift: `Model/` with tests, `App/`; built by `scripts/macos/build_launcher.sh`; the Online tab writes the keys `scripts/launch_settings.sh` reads), plus the Linux port's GTK launcher (reference) |
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
- **[shadPS4](https://github.com/shadps4-emu/shadPS4)** project — the Vulkan video core and
  shader recompiler (`gpu/shadps4/`, GPL-2.0-or-later).
- **FireBurn** — [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan), the native Vulkan
  FSR 3.1 / FSR 4 runtime (MIT; includes AMD FidelityFX SDK sources under MIT).
- **Omar Cornut** — [Dear ImGui](https://github.com/ocornut/imgui), used for the in-game overlay (MIT).
- **Alex Barney (Thealexbarney)** — [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9),
  AT9 audio decoding (MIT).
- **sirit** — SPIR-V emitter used by the shader recompiler (BSD-3-Clause), with Khronos
  **SPIRV-Headers** (MIT-style Khronos license).
- **Christian Rau** — [half](https://half.sourceforge.net/), IEEE 754 half-precision
  library (MIT).
- **AMD** — GCN PM4/register headers in `gpu/third_party/gcn/` (MIT-style AMD license).
- **DejaVu fonts** (Bitstream Vera license) — overlay font.
- **[MoltenVK](https://github.com/KhronosGroup/MoltenVK)** (Apache-2.0), and the dependencies
  built by `scripts/macos/bootstrap_deps.sh`: SDL3, Vulkan-Headers / Vulkan-Loader, fmt,
  xxHash, miniz, Zydis, FFmpeg, magic_enum, robin-map, Vulkan Memory Allocator, xbyak, Boost.
- **Community patch authors** — Kyo, Lance McDonald, illusion, emoose, auser1337 and others
  for the 60 FPS, camera and engine patches in `patches/Bloodborne.xml`.
- **zackcage6** — testing and bug reports for the Windows port.

Each third-party component keeps its own license file in its directory.

## License

GNU General Public License v2.0 or later (GPL-2.0-or-later), because of the shadPS4
components. See [LICENSE](LICENSE).
