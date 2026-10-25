# Native Mac Settings

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
`BB_MAC_SETTINGS=0` disables generation and hooks.

`src/runtime_mac_settings.c` validates the System-finalizer call, trampoline padding,
section-table slots/bound and intercepted dialog vtable targets before changing anything.
The existing supported 1.09 executable check also applies. Each Mac Settings dialog gets
its own cloned vtable. Its update handles edits/paging; destructors clear active state.
Native row destruction and layout rebuild providers and selection, and selected widget
types are explicitly revealed when reusing the movie. Normal game dialogs retain their
original vtables.

Settings reads in the Rosetta process use the shared snapshot. Edits are asynchronous
records on the existing renderer submit ring. The keyboard publisher refreshes settings;
there is no synchronous per-frame settings request. Shared protocol version is 5, so the
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
