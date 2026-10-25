# Achievements

All 40 Bloodborne trophies are tracked locally: 34 for the base game (including platinum)
and six for The Old Hunters. Progress is earned through the game's trophy-service calls.

## Using it

- Launcher: **Achievements**, or **Bloodborne → Achievements…** (`⌘⇧T`). The page refreshes
  every two seconds while visible, including during gameplay.
- In game: **Settings → Achievements**, `F2`, or `⌘⇧T`. Settings still opens with `⌘,`,
  Insert, or L3+R3; L1/R1 switches tabs. D-pad/keyboard navigation reaches every trophy row.
- Search, earned/locked filters, base-game/DLC filters, progress, grades and unlock dates
  are available on both surfaces. Locked descriptions can be revealed explicitly.
- New unlocks display a stationary, input-transparent banner for 6.5 seconds each.
  **Show in-game unlock banners** (`achievement_banners=1`, default) controls them on both
  settings surfaces. Stationary banners accommodate Reduce Motion without animation.
- Trophy artwork is read from the user's `sce_sys/trophy/trophy00.trp`, when available.
  Missing, encrypted or malformed entries use a grade-coloured trophy cup. PNG entries,
  archive sizes and decoded dimensions are bounded. No artwork is installed or distributed.

## Persistence and runtime boundaries

The runtime appends `trophy-id unix-seconds` to `<runtime user directory>/trophies.log`;
the default is `<data directory>/user/trophies.log`, overridden by `BB_USER_DIR` / `--user`.
`BB_SAVE_DIR` moves the game's save files independently and does not relocate this log.
Existing logs retain their earliest valid timestamp; duplicate/invalid records are ignored.
The launcher reads the log and never writes or resets progress. It reports read errors.

The 33 ordinary base-game trophies award platinum automatically. DLC is excluded.
Loading a complete older history quietly adds a missing platinum, using the latest
base-game unlock date. Loading history never replays notifications. A failed log write
returns success to the game and displays an error in the in-game catalog without adding
the failed trophy to history or queuing a banner. Retrying can persist the unlock; file
errors are cleared after a successful write. Invalid trophy IDs, direct platinum requests
and already-unlocked trophies retain their trophy-service error codes.

`src/runtime_trophy.c` owns persistence, deduplication, queries and unlock events under a
mutex. Guest trophy calls do not invoke AppKit or ImGui. The GPU library receives a snapshot
and a session-only notification mask. The Rosetta game process publishes them into
`BbGpuShared` with atomic payload access and a sequence counter. The renderer makes at most three attempts
to read a complete snapshot, keeping its last one when the writer is busy. macOS uses this
split-process architecture exclusively. Shared-control version 5 rejects mismatched
game/host binaries; rebuild/package both. No frame-generation or renderer changes are required.

## Original sources and adaptation

All imported code remains **GPL-2.0-or-later**. The existing project and dependency notices
continue to apply.

| Source | Contribution |
| --- | --- |
| [pyL1nx/bloodborne_pc, Achievement-System-Patch, snapshot `64919f7`](https://github.com/pyL1nx/bloodborne_pc/tree/64919f7da13a24bac53578acec5f74f959fe5c4e) | Original achievement-system integration, local tracking and the dedicated launcher page. |
| [Original `launcher/bbport_trophies.py`](https://github.com/pyL1nx/bloodborne_pc/blob/64919f7da13a24bac53578acec5f74f959fe5c4e/launcher/bbport_trophies.py) | Forty-entry catalog, log/tracker format, earliest-date preservation and the base-game platinum rule. Preserved as `scripts/trophies.py`; C and Swift catalogs are generated from it. |
| [Original `src/runtime_services.c`](https://github.com/pyL1nx/bloodborne_pc/blob/64919f7da13a24bac53578acec5f74f959fe5c4e/src/runtime_services.c) | Guest trophy-service/logging foundation. |
| [Our `src/runtime_trophy.c`](https://github.com/evill33t/bloodborne_mac/blob/5469befbb0c39759ffbf91d20a2e1a514e9b7581/src/runtime_trophy.c) and [header](https://github.com/evill33t/bloodborne_mac/blob/5469befbb0c39759ffbf91d20a2e1a514e9b7581/src/runtime_trophy.h) | Ported mutex-protected state, append-before-unlock behavior, event queue, timestamps and trophy ABI structures. |
| [Our `src/runtime_services.c`](https://github.com/evill33t/bloodborne_mac/blob/5469befbb0c39759ffbf91d20a2e1a514e9b7581/src/runtime_services.c) | Ported game/trophy details, unlock flags and PS4 RTC timestamps. |
| [Our `gpu/metal/game_achievements.mm`](https://github.com/evill33t/bloodborne_mac/blob/5469befbb0c39759ffbf91d20a2e1a514e9b7581/gpu/metal/game_achievements.mm) | Mac browser, queued 6.5-second non-interactive banners, trophy-grade presentation and bounded TRP artwork reader adapted to SwiftUI/ImGui and native Metal textures. |
| [Our runtime fixture](https://github.com/evill33t/bloodborne_mac/blob/5469befbb0c39759ffbf91d20a2e1a514e9b7581/tests/test_macos_trophies.c) and [tracker tests](https://github.com/evill33t/bloodborne_mac/blob/5469befbb0c39759ffbf91d20a2e1a514e9b7581/tests/test_trophies.py) | Ported persistence, concurrency, platinum, ABI and catalog coverage. |

The split-process transport, SwiftUI launcher page, ImGui settings page and native Metal
artwork upload are adaptations for this repository's architecture. This does not import
the original fork's renderer or replace this port's native Metal renderer.

## Validation

`bash build_macos.sh --test` includes the runtime trophy fixture, both architectures of the
shared-memory fixture, tracker tests, launcher model tests and the existing offscreen Metal
overlay fixture (plain and `MTL_DEBUG_LAYER=1`). To check generated catalogs:

```sh
python3 scripts/macos_trophy_catalog.py --check
python3 -m unittest discover -s tests -p test_trophies.py
```

The overlay fixture covers restored-history silence, direct shortcuts, live banners with
the FPS counter off, input transparency, preserving the surrounding image, queued unlocks
after 6.5 seconds, and disabling banners. Set `BB_OVERLAY_TEST_OUTPUT` to an existing directory
to export offscreen fixture screenshots. Fixtures contain no game assets.

Local validation for this port: arm64 and x86-64 GPU-library builds, game/host executable
builds, trophy runtime fixture, both shared-memory fixture slices, eight Python tests,
launcher build/model tests, settings persistence and offscreen Metal checks.
These are synthetic/build checks. Real gameplay unlocks, fullscreen/controller hardware,
VoiceOver and a packaged-app end-to-end session still require live verification.
