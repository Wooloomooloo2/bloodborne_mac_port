# Native Mac Settings and Exit Game

Bloodborne's System menu has one **Mac Settings** command, before Exit Game when that
command is present. It opens the game's native option widgets. Left/Right on Page or
L1/R1 (keyboard 1/3) selects nine groups: Upscaling, Quality, Output, Renderer, Effects,
Tools, Resolution, Launch and Restart. Inline dropdowns capture input and block paging.

The controls use `BbSettings` and the same `bbport.ini` as the launcher and ImGui overlay.
There is no second renderer configuration. Unsupported saved providers are preserved;
only supported alternatives are offered. Native ultrawide remains configured in the
launcher, and disables conflicting native controls. Launcher visibility is available only
in launcher-started sessions. Explicit environment overrides remain authoritative.

Changes save automatically. A star on the Page value indicates a restart is required.
The last group requires Confirm restart before Restart now. Startup settings are reread
after restart, including configuration-derived frame caps, fullscreen, HUD, compression
and vblank values; explicit caller overrides survive.

The title menu also has **Exit Game** as its last command. It closes the runtime with
status 0; the existing GPU-host parent watcher flushes the pipeline cache and closes
the renderer, and a launcher-started session returns to the launcher. This command is
only on the title screen, after the game has left the world. The in-world System menu's
existing save-and-return-to-title command is unchanged. `BB_EXIT_GAME=0` disables the
new title command independently of `BB_MAC_SETTINGS=0`.

The native slider displays integers: sharpness 30 means 0.30, reactive scale 10 means
1.0, threshold/maximum 20 means 0.20. Model detail 0..4 corresponds to the port's -2..2.
Line help explains these units. Visiting a page never rounds or rewrites a saved value.
Frame generation is outside this branch; unknown future configuration keys are retained.

## Assets and hooks

`scripts/mac_menu_assets.py` clones the player's ControllSetting movie into one hidden
six-widget MacSettings section, with message IDs in 126xxx. It updates text/help groups
200/201 in every existing language archive. New captions are English. No game asset is
committed or distributed. Generated assets live in a private, content-addressed cache
outside the dump and are mounted after user mods through the existing link overlay.
Changed user assets invalidate the cache. Invalid assets leave the native entry disabled.
`BB_MAC_SETTINGS=0` disables the Mac Settings assets and hooks. With Exit Game enabled,
the generator also grows all existing title movies (base, DLC and EU DLC) to six animated
rows and replaces the unused Opinion Box's text/help (401307) in each language bundle.
Both feature selections, title movies and captions participate in the cache identity.

`src/runtime_exit_menu.c` checks the 1.09 title condition call and relocated Opinion Box
callback before making either change. It shows that unused row and replaces its callback
with a clean runtime exit. Hooks require successfully generated assets and refuse modified
or unsupported code. No guest browser request, save hook or shared-protocol addition is
needed for this title-only command.

`src/runtime_mac_settings.c` validates the System-finalizer call, trampoline padding,
section-table slots/bound and intercepted dialog vtable targets before changing anything.
The existing supported 1.09 executable check also applies. Each Mac Settings dialog gets
its own cloned vtable. Its update handles edits/paging; destructors clear active state.
Native row destruction and layout rebuild providers and selection, and selected widget
types are explicitly revealed when reusing the movie. Normal game dialogs retain their
original vtables.

Settings reads in the Rosetta process use the shared snapshot. Edits are asynchronous
records on the existing renderer submit ring. The keyboard publisher refreshes settings;
there is no synchronous per-frame settings request. Shared protocol version is 8, so the
game-side library and native GPU host must be rebuilt together.

## Provenance

Selected native widget/functor/System builders were adapted from
[`evill33t/bloodborne_mac`, bbacb95c3338efcd627c0edaec05ff7534e555da](https://github.com/evill33t/bloodborne_mac/tree/bbacb95c3338efcd627c0edaec05ff7534e555da),
`gpu/metal/bbhost/game_menu.cpp`. That implementation credits
[`droogie/bbhost`, 566184a74f071166299bb656c4af479e5bbe2771](https://github.com/droogie/bbhost/tree/566184a74f071166299bb656c4af479e5bbe2771),
`src/engine/option_menu.cpp`. Asset cloning follows the same Mac source revision's
`gpu/metal/vendor/bbhost/src/engine/menu_assets.cpp` and `scripts/quit_menu.py`.
The selected adapters retain GPL-3.0-or-later; the combined runtime containing them is
GPL-3.0-or-later. The license text is retained in `LICENSES/GPL-3.0-or-later.txt` and the
app package. Existing GPL-2.0-or-later sources retain their original grants.
The title-entry offsets and animated sixth-row technique follow the same Mac source
revision's `scripts/quit_menu.py` (GPL-2.0-or-later).

Dialog lifecycle, string storage, slider bounds, widget visibility and popup-capture
behavior were checked against the player's plaintext 1.09 binary with local LLVM
disassembly. No derived game executable or disassembly is distributed.

## Validation, 24–25 October 2026

- Full arm64 and x86-64 GPU library builds; runtime and both native host variants compile
  with `-Wall -Wextra -Werror`.
- Native settings fixture: defaults, provider fallback/preservation, invalid writes,
  all ten effects, restart/revert state, launcher availability and concurrent writers.
  Existing settings-preservation and upscaler-support fixtures also pass.
- Native hook fixture: all guarded mismatches leave the image untouched, size/disable
  checks, button edges and native popup capture.
- Asset fixtures: 18 pass. Actual generation covered all 22 language bundles; original
  strings and unrelated archive members were preserved, and all 23 source hashes stayed
  unchanged. Warm cache generation was approximately 6 ms.
- Existing pad and mod fixtures pass (mod fixture uses canonical `/private/tmp`).
  Launch-settings restart fixtures pass. Cross-architecture rings pass: two million
  records, 500,000 events and 20,000 requests, with zero corruptions.
- Existing Metal overlay fixture passes normally and with Metal validation enabled.
- Live native menu smoke uses the supplied base-game/DLC paths and an isolated copied
  save/cache. The DLC fixture reports three installed add-ons, including SPEXPANSIONDLC03.
  Native entry, all nine groups, integer slider editing/persistence, page arrows, bumpers
  and close/reopen were exercised. Inline popup capture, restart-needed/reverted state,
  rejection of an unconfirmed restart and a confirmed restart through the normal GPU-host
  request and `run.sh` path were checked. The save loaded into Central Yharnam gameplay.

The DLC provider used for this smoke is an existing ignored test helper; DLC support is
not bundled in this menu PR. This is functional verification, not a performance benchmark
or a packaged-app test. One existing launch-resolution fixture expects live 1080p sizing,
but upstream master now selects a 640x360 startup size for that preset on macOS; the same
failure was reproduced with upstream master's unmodified `run.sh`. An earlier gameplay
smoke ended in the existing GPU watchdog after the window became occluded; menu checks
before it completed, and no renderer synchronization change is included here.

## Exit Game validation, 25 October 2026

- 22 menu-asset fixtures pass, including title-row spacing, scale/rotation, animated
  updates/removal, all title variants, cache identity and independent feature disabling.
- The exit-hook fixture passes natively and under Rosetta: every code/pointer mismatch
  leaves the image untouched; missing assets, undersized images and disable flags refuse
  installation; invoking the installed callback in a child exits with status 0.
- Generation against the player's dump covers all three title movies and 22 language
  bundles. All 26 source-file hashes remain unchanged; warm-cache generation is about 7 ms.
- Both GPU libraries rebuild after the upstream merge. The complete x86-64 runtime and
  both native host variants compile/link with `-Wall -Wextra -Werror` using protocol 6.
  The existing native Mac Settings hook and launch-settings fixtures still pass.
- The existing Rosetta-to-arm64 shared-rings fixture passes with protocol 6: two million
  records, 500,000 events and 20,000 requests, with no corruption or incorrect events.
- Live regular-runtime check with a fresh isolated profile: selected the visible Exit Game
  command using the game's controller input. The runtime and native GPU host both exited
  with status 0, the normal host parent-death path completed its cache flush, and no test
  processes remained. No crash or fatal-stop lines appeared in the log.
- Live saved-game check with the existing ignored DLC-aware content helper and a copied
  save: all six title commands were visible; Continue loaded Central Yharnam. The normal
  System-menu Exit Game confirmation saved and returned to the title screen. Selecting
  the new title-menu Exit Game then closed both processes with status 0 and a completed
  cache flush. Original menu assets and the source save copy retained their hashes.
- Process statuses were observed with macOS kqueue NOTE_EXIT/NOTE_EXITSTATUS as well as
  the launch command's return code; both successful menu exits required no external
  termination signal. The initial DLC-incompatible run was closed through its normal
  window close button and is excluded from these two menu-exit checks.
- These live checks use checkout binaries, not the packaged launcher app; launcher
  return/relaunch behavior and packaged-app shutdown remain untested here.

The branch also merges current upstream master. The shared block retains upstream's
benchmark and mouse-input fields alongside the native settings snapshot. Protocol version 8
distinguishes this layout from upstream's version 6 and the independent achievements PR #70's
version 7. Game and GPU processes must use matching rebuilt libraries.

## Upstream refresh, 25 October 2026

Merged upstream `master` (`af0fae2`) while retaining its mouse controls, keyboard fixes,
launcher Stop behavior, benchmark and session logs. Native and overlay settings writers
keep mouse enablement, sensitivity and inversion; the concurrent-writer fixture now
checks their persistence together with the native launch settings. Protocol version 8
requires rebuilding both game and GPU-host binaries.

Fresh arm64 and x86-64 GPU builds, the complete runtime, both native hosts and the launcher
compile successfully with LTO disabled. Native settings/persistence, provider support,
menu and Exit Game hook guards on both architectures, all 22 synthetic menu-asset tests,
13 mod tests, 19 patch tests, launcher model and both Stop lifecycle tests, runtime,
semaphore, pad and launch/restart settings checks pass. The cross-architecture rings
verify two million records, 500,000 events and 20,000 requests without corruption.
The offscreen Metal overlay passes normally and with `MTL_DEBUG_LAYER=1`.

These are build and synthetic-fixture results. The full Ninja-based `build_macos.sh --test`
was not run; GPU builds used CMake/Unix Makefiles. No gameplay, real-asset generation or
packaged-app session was repeated for this upstream refresh; the live results above
describe the earlier protocol-6 builds.
