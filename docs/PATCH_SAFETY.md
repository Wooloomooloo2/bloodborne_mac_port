# Runtime hook protection and selected patch conflicts

Upstream commit `551c84b` already filters unsafe writes over the original
timed-condition-wait call at PS4 address `0x02483ec1` and rejects the known
Enemy Control/FPS combination. This change retains those fixes and adds general
cross-patch disagreement checks plus native protection of active hook code and
the retained call, including binary patch files that bypass the XML compiler.
The community `RET` edit bypassed stack cleanup if the timed branch executed;
the REA evidence below confirms that instruction hazard. No new gameplay crash
or moving-lift correction is claimed here.

Selected patches are compared using their effective bytes after replaying each
patch's ordered writes. Equal cross-patch writes are accepted. Disagreements
stop preparation before `patches.bin` is written and identify both sources and
the PS4 address. Intentional replacements can use the later XML metadata's
`Overrides="Patch name;file.xml/Other patch"` declaration. This also applies
across built-in, generated resolution and external groups. Existing XML without
disagreeing writes needs no change; existing conflicting external overrides need
the explicit declaration. Load order within one patch is preserved.

The native loader separately scans the complete patch file before applying its
first write. It refuses changes to active context-lock/frame-pool hook prologues
using the hook installer's own descriptors, and to the retained timed-wait call.
Identical original bytes are allowed. Disabling a hook through its existing
`BB_CONTEXT_LOCK=0` or `BB_FRAME_POOL=0` option removes only that hook's protection.
The timed-wait call remains required. XML override declarations cannot bypass
this native protection. Other relocation/mapping admission checks remain in the
loader; a successful scan is not proof that every game patch is compatible.

The original-code interpretation and ordered-overlap method were informed by
[Paleblood `codex/complete-decompilation` at `56a68cf9`](https://github.com/evill33t/Paleblood/tree/56a68cf9f5aa577eeb5d3834d8a752c8b4426b83),
specifically `game/kernel/condition_wait.cpp`, `QUIRKS.md`, and
`tools/patch_overlap.py`. The local REA record is
`ev_c2d0b28ac6cabe337b359fa7906fe1f3ebd7640377c3d3877434e044be34ab50`;
the normalized loaded-image SHA-256 is
`071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a`.
No binary or game asset is included. The checker is adapted to this port's
actual hooks, rather than Paleblood's different replacement list.

Focused checks:

```sh
python3 -m unittest discover -s tests -p test_patches.py -v
python3 -m unittest discover -s tests -p test_patch_guard.py -v
python3 -m unittest discover -s tests -p test_run_settings.py -v
```

The runtime verifier compiles the actual hook metadata and guard for x86-64 and
tests changed/unchanged bytes, disabled hooks, a late conflict, truncated/trailing
entries, overflow and restored file positions. It installs no hook and starts no
game. Gameplay, visual output and performance have not been verified by these
checks.
