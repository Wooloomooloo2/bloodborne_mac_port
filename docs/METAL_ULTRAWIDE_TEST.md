# Native Metal ultrawide validation

This change adds native 32:9 presets (2560x720, 3840x1080, 5120x1440) and 21:9 presets
(2560x1080, 3440x1440). It leaves the renderer's shader precision, readback and presentation
defaults unchanged. No game assets, UI mods or saves are bundled or replaced.

## Resolution and memory

- Kyo's complete camera/scene/UI patch keeps its dependent MOV/NOP writes and fallback
  globals together. Native ultrawide bypasses conflicting live resizing and upscaling.
- Keep the first retail graphics heap. Above 1920x1080 pixel count, increase only the
  second texture pool to 1536 MiB and default DMEM to 6144 MiB (7 GiB GPU arena).
  Retail capacity reproduced a combat texture allocation failure; larger community
  heaps are deliberately omitted. Explicit developer memory overrides still win.
- Warn when 3440x1440 or 5120x1440 is selected on a Mac with less than 24 GB memory:
  scene targets and textures can approach the Metal budget on 16 GB machines (#19/#22).
- The FPS badge shows observed scene dimensions. Incompatible live controls are disabled;
  turning ultrawide off restores the previous standard resolution on restart.

## Playtest evidence and limits

Mac Studio M2 Max, 64 GB RAM: travel between the first area and Hunter's Dream, combat,
character legs, ground, HUD and menus looked correct at native 3840x1080. The combined
experimental build reached roughly 56–60 FPS after warm-up; the user also observed about
45 FPS at 5120x1440. These are playtests with varied settings, not controlled benchmarks.

That build additionally used Mojahed Abushawish's invariant shader experiment from
[PR #20](https://github.com/Wooloomooloo2/bloodborne_mac_port/pull/20) and BB_READBACKS=2.
Credit to that work for the shader performance gain. Neither experiment is included here;
the maintainer will A/B them independently before considering renderer-default changes.
The ultrawide-only branch does not establish the same FPS or long-session geometry result.

Before merge, verify 3840x1080 and 5120x1440 in several areas, title/inventory menus,
combat and travel, with screenshots and observed scene dimensions. In particular, compare
against the earlier stretched polygons above 1080p and the missing-scenery issue (#18).
Test the memory warning on 16 GB hardware; its presence is not evidence that allocation
will succeed. [Combined-build demo](https://youtu.be/yXYok0QVkys).

## Reproduction and checks

Build normally with build_macos.sh and scripts/macos/build_launcher.sh --test. Select
Ultrawide and a preset in the launcher, or set ultrawide_res=3840x1080 in bbport.ini.
Use a copy of saves for tests. Do not add a separate resolution or graphics-heap patch.

tests/test_ultrawide.py checks complete patch writes, bounded pools and invalid CLI input.
tests/test_launch_settings.sh checks memory/environment cleanup on restart and disable.
Launcher model tests check resolution persistence and the below-24-GB warning boundary;
the renderer scene-resolution/settings tests check that wide targets are not resized twice.
