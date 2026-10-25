# Keyboard and mouse: bindings, layout, mouse speed

Owner decision 2026-10-27: full keyboard and mouse support with rebinding, on a separate launcher
page; fix the mouse camera feeling too slow (player feedback). Two-step plan for the mouse:
retune the stick emulation first, then a time-boxed spike on direct camera control.

## How input reaches the game

The game reads a DualShock 4 (`scePadReadState`, `src/runtime_pad.c`). Gamepads are polled by
SDL in bb-probe; the window, its keyboard and the mouse are in bbgpu-host, which publishes them
in the shared block (`host/bbgpu_shared.h`: `keys[512]`, `mouse_vx/vy`, `mouse_buttons`,
`mouse_captured`, `mouse_wheel_up/down`). Keyboard and mouse add to the gamepad's state.

## Bindings (done, branch `input-bindings`)

- **Actions** = pad inputs, labelled for Bloodborne: movement (4 directions + walk), camera
  (4 directions), Cross, Circle, Square, Triangle, R1, R2, L1, L2, R3, L3, d-pad (4), Options,
  touchpad left/right. `src/runtime_bindings.c` and `launcher/Model/Bindings.swift` list the same
  actions, ini keys and defaults; `tests/test_pad.c` checks they match.
- **Storage:** `bbport.ini` `bind_<action>=Name,Name` (up to 4). Names are SDL scancode names
  (key positions: other layouts keep the shape; the launcher shows the printed label via
  `UCKeyTranslate`) or `Mouse Left/Right/Middle/Back/Forward`, `Wheel Up/Down`. Absent key =
  default, empty = unbound. The launcher writes only changed actions. Cmd is reserved.
  The in-game menu's Save keeps these keys (not owned).
- **Defaults = the 0.5.9 layout** plus Left Alt = walk (movement keys at half deflection).
- `camera_keys_dpad` (default 1): while the mouse has the camera, keys bound to the camera press
  the d-pad (the 0.5.7 arrow-key behaviour, now for whatever keys are bound there).
- **Wheel:** the host counts whole steps (physical direction, trackpad fractions accumulated);
  the game turns each step into a 50 ms press + 50 ms release, at most 8 queued.
- Mouse buttons and the wheel reach the game only while the window holds the cursor (Mouse
  camera on, after a click), as before.
- **Launcher:** third page "Controls" (the page switcher is not a TabView, #47): Mouse
  (camera, sensitivity 0.1–10, vertical speed 0.25–4×, invert), Keyboard (camera keys as d-pad),
  then the bindings by group with two slots (a third/fourth appears when used), a capture sheet
  (any key incl. Esc/Return, mouse buttons and wheel in its box; Clear, Cancel), a warning when
  one input drives several actions, Reset to Defaults. Bindings apply at the next start.

## Mouse speed

Why it feels slow: the mouse emulates the right stick. Speed over the last 33 ms → deflection,
full at 650 points/s (`MOUSE_FULL_SPEED`), never below 24 (dead zone). Above that, faster mouse
movement changes nothing: the camera's top speed is the game's full-stick turn rate. Sensitivity
only reaches the ceiling sooner. The game's own Camera speed option raises the ceiling.

1. **Retune (next; needs one scripted run on the Mac):** measure camera turn rate per stick value
   with the pad file (`BB_PAD_FILE`, `rx=`), build the inverse table so mouse speed maps linearly to
   turn rate up to the ceiling, shorten the speed window, then a test build for players.
2. **Direct camera (spike, ≤ 1 day):** find the camera yaw/pitch in game memory (scan while
   turning with the pad file), hook the camera update (`src/runtime_hooks.c` style) and add the
   mouse counts while the camera is free; stick emulation otherwise (lock-on, cutscenes, menus).
   If it fails, step 1 stands.

## Not now

Rebinding inside the in-game menu; controller button remapping (same model); keyboard glyphs in
the game's prompts (it only has PlayStation ones).
