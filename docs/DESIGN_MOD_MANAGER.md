# Mod manager for 0.6 (proposal, 2026-10-24)

Status: built (2026-10-25), merged into master for 0.6, owner-approved with cheats, in its own launcher
window ("Mods & Cheats…"). Requested in #16. Online play (branch `online-0.6`): mods (scripts/mods.py), cheats, gameplay changes, debug patches and
patch files are off (scripts/patches.py `ONLINE_OFF`; greyed out in the window); camera and interface tweaks stay. Not yet: drag and drop onto the window, showing a mod's readme. Enemy Control is not offered (it
rewrites code the frame rate patches change). The benchmark runs without mods, patch files and
cheats.

## What exists today

- **Loose-file mods** (`scripts/mods.py`, run at every launch by `run.sh`): each folder in
  `<data>/mods/` is a mod; `<data>/mods.json` holds `order` and `disabled`. Accepted layouts:
  `dvdroot_ps4/`, `app0/dvdroot_ps4/`, `CUSA03173/dvdroot_ps4/`, one wrapper folder, or the game's
  top-level folders directly (`chr/`, `parts/`, ...). The launch gets a private merged view built
  from links (the game and the mod folders are never written); later mods win; file/folder
  conflicts stop the launch with a message; `eboot.bin`, `sce_module` and `sce_sys` replacements
  are refused. `BB_MODS_ENABLED=0` turns all of it off.
- **Patches** (`patches/Bloodborne.xml`, shadPS4/GoldHEN XML, compiled by `scripts/patches.py`
  at launch): 62 entries. The launcher and in-game menu expose only the graphics ones (effects,
  FPS, LOD, ultrawide). The rest are not reachable without editing files: gameplay tweaks
  ("No Rally Decay", "DS1-like physics", "Increased camera distance", "Disable Camera Auto
  Rotation"), cheats ("Player No Dead", "Player Stealth", "Player Silent", "Enemy Control") and
  debug ("Restore Debug Menu").
- **No UI** for either in the launcher.

## Proposal

A **Mods** section in the launcher (its own window or tab; the main window is sized for a 13" Air):

1. **Mods list** (loose files): name, enabled checkbox, drag to reorder, "Add Mod…" (a folder or
   a .zip, unpacked into `<data>/mods/<name>`; the layout check runs at once so a bad archive is
   rejected with the reason), Remove (to the Trash), Show in Finder, the mod's readme if it has
   one. Writes `mods.json` in the existing format, so `run.sh` needs no change.
2. **Conflicts view**: for the enabled set, which files more than one mod replaces and which mod
   wins (mods.py already computes the merge; add a `--report` JSON mode the launcher calls).
3. **Gameplay patches and cheats**: the non-graphics entries of `Bloodborne.xml` as toggles with
   their notes, grouped (Gameplay, Camera, Cheats, Debug). Stored in `bbport.ini` (a `patches=`
   list) and applied by `patches.py` as the graphics switches already are. Cheats carry a plain
   warning line.
4. **Save safety**: the first launch with a changed mod set or any cheat on backs up the save
   folder (the launcher already knows it, `SaveFolder.swift`), keeping the last few. Many mods
   change params that end up in the save.
5. **Online play (0.6)**: online sessions start with mods and cheats off unless the player
   explicitly allows mods that only change files on their side (textures). This rule needs THD's
   view: mismatched params between co-op players desync, and cheats online are unfair.
6. **Benchmark**: always runs without mods and cheats (`BB_MODS_ENABLED=0`, no extra patches), so
   results stay comparable.

## Not in this proposal

- Downloading mods or patch databases from the internet (Nexus etc.): licensing, malware and
  update risk. Players add files they already have. Revisit after 0.6.
- GoldHEN `.shn`/JSON cheat files with runtime pointer chains: the patch compiler only does
  static writes at boot; a cheat engine is a separate project.
- Replacing `eboot.bin` or modules: refused today, stays refused.

## Questions for the owner

1. Separate window, or a tab/section in the existing launcher window?
2. Cheats in the first version, or gameplay tweaks only?
3. Online rule: mods off by default online, or trust players?
4. Any specific mods players have asked for that should work on day one (to test with)?
