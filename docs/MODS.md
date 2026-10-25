# File mods

File mods replace files under the game's `dvdroot_ps4`. Changes take effect on the next
launch. A mod lower in the load order has higher priority; when files collide, the log says
which mod replaced which.

The default mods folder is `mods/` in the data directory (the repository root unless
`BB_DATA_DIR` is set). On Linux the GTK launcher has a **Mods** group (folder selection, a
global switch, a per-mod switch and load-order arrows), and the Linux AppImage uses
`~/.local/share/bbport/mods/`. Unpack a downloaded mod like this:

```text
mods/
  Mod name/
    dvdroot_ps4/
      chr/...
      param/...
      menu/...
```

Also accepted:

- `Mod name/app0/dvdroot_ps4/...` and `Mod name/CUSA03173/dvdroot_ps4/...`;
- an extra wrapper folder, as produced by extracting an archive into a folder named after it:
  `Mod name/Mod name v1.2/dvdroot_ps4/...` (readmes/images may sit next to it);
- a mod without `dvdroot_ps4` that directly contains game folders: `Mod name/chr/...`,
  `Mod name/parts/...` (`chr`, `parts`, `map`, `menu`, `msg`, `param`, `sfx`,
  `sound`, `font` and other `dvdroot_ps4` folders).

Name case inside a mod does not matter: `DVDROOT_PS4/Chr/C0000.chrbnd.dcx` from a mod packaged
on Windows replaces the game's `dvdroot_ps4/chr/c0000.chrbnd.dcx`. Previously such a file was
added alongside under its own name and the game never saw it. The launch log shows how many
game files a mod replaced and how many it added: `Mods: 12 game files replaced, 0 added`.
If a mod should replace files but reports `0 game files replaced`, check the paths inside it.
ZIP/7z archives must be extracted first. Ready-made replacements of `dvdroot_ps4` files are
supported: textures, models, params, UI, sound and fonts. The manager does not merge the
contents of two identical `.dcx`/`.bnd` files: the last replacement wins entirely.

For a folder already prepared for shadPS4, a sibling directory
`CUSA03173-mods/dvdroot_ps4/...` next to `CUSA03173/` works too. It is attached automatically
before the mods from the list. The global switch disables it as well. You can also select a
folder that directly contains `dvdroot_ps4` as the mods folder; such a folder is attached as a
whole, without a list of individual mods.
The `-mods` convention was checked against
[the shadPS4 sources](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/file_sys/fs.cpp).

The game's originals are never overwritten. For the duration of a launch a separate merged
tree of links is created in the data directory at `out/mod-game-*`; reads, stat and directory
listing see the selected replacements plus the remaining original files.
The game's `/app0` and `/hostapp` are read-only. Saves stay in the usual folder and remain
writable. On a normal exit or a stop from the launcher the temporary tree is removed. After
SIGKILL or a crash it may remain; such directories are not reused on the next launch.

Mods that replace `eboot.bin`, `sce_module` or `sce_sys`, DLL injectors and script plugins are
not supported by this loader. Executable code patches stay in the existing `patches/` /
`BB_PATCHES` system. Symbolic links inside a mod are rejected. Whether a given mod is
compatible with CUSA03173 1.09 is up to its author.

From the terminal:

```sh
BB_GAME_DIR=/path/to/CUSA03173 BB_MODS_DIR=/path/to/mods bash run.sh
BB_GAME_DIR=/path/to/CUSA03173 BB_MODS_ENABLED=0 bash run.sh
```

Order and disabled mods are stored in `mods.json` in the data directory:

```json
{"order": ["First", "Second"], "disabled": ["First"]}
```

New folders are enabled automatically and appended at the end in alphabetical order.
`BB_MODS_CONFIG` changes the profile path. `BB_MOD_TRACE=1` together with `BB_MODS_DIR` prints
the first reads of files actually served from mods, for diagnostics.

# Third-party patches (shadPS4 XML)

The `patches/` folder in the data directory (Linux AppImage: `~/.local/share/bbport/patches/`;
in the Linux launcher, the **Third-party patches** group, where another folder can also be
selected). Patch XML files in shadPS4/GoldHEN format are accepted: `Metadata` entries for
`AppVer="01.09"` and `eboot.bin` are used, files with the `TitleID` of other games are skipped.
Default enablement comes from the file's `isEnabled`; the launcher's selection is saved to
`patches.json` in the data directory:

```json
{"enabled": ["file.xml/Patch name"], "disabled": ["file.xml/Another patch"]}
```

Supported line types: `bytes`, `bytes16/32/64`, `float32/64`, `utf8`, `utf16`.
A patch with `mask` lines (pattern search) or with an address outside the eboot is skipped
entirely, with a message in the log. Third-party patches are applied after the built-in ones
(`patches/Bloodborne.xml`). Identical overlaps are accepted. Different bytes from two selected
patches stop preparation with their names and the first conflicting address. For an intentional
override, the later patch's `Metadata` can declare `Overrides="Built-in patch name"`, or
`Overrides="file.xml/Patch name"` for another external patch. Separate multiple names with `;`.
Writes within one patch retain their original order. Required runtime hook prologues and the
original timed-condition-wait call remain protected even with an override declaration.
See [patch safety](PATCH_SAFETY.md). From the terminal:
`BB_PATCHES_DIR=/path BB_PATCHES_CONFIG=/path/patches.json bash run.sh`.

Patches that replace pointers in tables (such as `60 FPS++` and `90 FPS++`) are relocated by
the loader to the actual image address. Previously such patches aborted the launch with
`patch overlaps a relocation`.

# TAA sharpening

The `sharpen` and `sharpness` settings also apply to TAA, in the launcher and the in-game
menu. The range is **0…1**; at 0 or with the switch off there is no extra pass. RCAS is
applied to the finished scene before the HUD. The TAA history stores the color before
sharpening, so sharpening does not compound through accumulation. Flat areas keep their
original color, including HDR; no extra buffer is needed.
