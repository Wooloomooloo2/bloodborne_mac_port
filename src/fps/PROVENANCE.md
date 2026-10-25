# Adaptive FPS timing

Adapted from [evill33t/bloodborne_mac](https://github.com/evill33t/bloodborne_mac/tree/bbacb95c3338efcd627c0edaec05ff7534e555da),
specifically `gpu/metal/vendor/bbhost/src/engine/frame_rate.cpp`, its three site
tables, `frame_rate_step.h`, `sprj_flipper.h`, and
`gpu/metal/bbhost/wind_vector_patch.h`.

The engine timing originates in [droogie/bbhost](https://github.com/droogie/bbhost/tree/566184a74f071166299bb656c4af479e5bbe2771).
The source credits **Kyo's FPS++ patches**, **Lance McDonald (manfightdragon)**,
and **yogesh1239's high-FPS and timing fixes**. Those credits and the site-level
reasoning are retained. These sources are **GPL-3.0-or-later**; see [LICENSE](LICENSE).
The existing port's GPL-2.0-or-later grant permits the combination. The combined
game runtime is distributed under GPL-3.0-or-later; the independent renderer and
launcher retain their existing notices. Packaging includes the additional license.

Local adaptation:

- Self-contained x86-64 runtime module, linked into `bb-probe`; no renderer,
  window, networking or plugin dependencies and no callbacks across processes.
- Measured-time task/physics/AI steps, per-frame amounts and decays, sprint and
  momentum loads, cloth constraint scaling, half-rate wind phase, stable authored
  wind-vector amplitude, and carried fractional play-time milliseconds.
- Stock `SprjFlipper::Update` field semantics and adaptive sleep/spin pacing.
  Game steps clamp to `[1/cap, 1/30]`; uncapped uses a 240 FPS reference floor.
- Original camera constructor and parameter stores retained. The table-driven
  camera constants are included; bbhost's decompiled camera is **not imported**.
- Strict whole-install admission before any mutation, using the original 1.09
  instruction bytes. No fallback to a partially converted rate. A mismatch aborts
  startup. The launcher also retains the port's existing game fingerprint check.
- Private data tail, normal SysV replacement entry, writable timing pages restored
  after segment protection, and whole-patch-set conflict refusal before writes.
- At 30 FPS the image is unmodified. Fixed/variable legacy modes remain available
  and the existing default remains fixed. Adaptive is an explicit experimental option.

Not imported: plugins, networking, arbitrary fixed-tick hooks, decompiled game
systems, benchmark/swing modes, or the original port's renderer/presentation code.
Debug `BBHOST_LIMITER_MARGIN_US` and `BBHOST_WIND_VECTOR_FIXED` overrides remain
available; they are not launcher settings.

CPU tests in `tests/test_fps_timing.cpp` and `tests/test_fps_install.cpp` adapt the
original port's fixtures. The independent reference in `tests/fps_reference/`
comes from [Yharnam-Hunters/Paleblood](https://github.com/Yharnam-Hunters/Paleblood/tree/af97b90bb1e88853adfc04b8da2b3d7506516565),
via the original port's adaptation of `game/frame_timing/frame_limiter.cpp` and
`game/engine/scalar.h`. It retains **GPL-2.0-or-later** and its separate LICENSE,
and is linked only into the verifier. It is never installed into the game.

No game executable or assets are distributed. Optional real-image tests read the
user's ELF and modify only a private anonymous mapping. CPU fixtures establish
limited timing/installation behavior, not gameplay, moving-platform correctness,
rendering performance, or smooth presentation.
