# Prepared guest TLS slot contract

The macOS runtime reserves pthread key **384** and stores each guest thread's
TCB there. Preparing an image for another key used to succeed, although the
runtime still populated key 384. The first rewritten TLS load could then read
an unrelated host value. This change keeps the existing allocation, worker
attachment, low-address TCB storage and per-module TLS model.

Both linkers now validate `--gs-slot` and `BBPORT_GS_SLOT` before reading game
inputs or opening generated output. An explicit argument takes precedence over
the environment. On macOS only 384 is accepted; the Linux reference path keeps
its zero-displacement contract. The default remains unchanged.

The existing BBPROBE3/4/5 layouts use capability bit **0x2** to announce a
uint64 little-endian GS byte displacement immediately after the capabilities
word (byte offset 56). Existing linked metadata follows at offset 64 instead
of 56. Capability bit 0x1 retains its existing meaning. The two linkers emit
this extension in BBPROBE4 and BBPROBE5; the loader accepts it in all three
linked layouts. No new format number or unrelated cache version is needed.

Before loading guest code or running a native module initializer, the loader
checks the displacement's bounds/alignment and compares it with the runtime's
actual compiled value, exported by `runtime_thread_gs_displacement()`. Unknown
capability bits, truncated metadata and metadata on unlinked formats are
rejected. `--strict-imports` does not bypass the check. Older runtime binaries
already reject capability 0x2 as unknown, so they cannot misparse new output.

Old linked images do not record a selected slot. On macOS the loader refuses
them with `boot image lacks TLS slot metadata; prepare the game again`.
Preserving them safely would require inspecting guest executable contents or
guessing their preparation settings. Both valid and mismatched old linked
images must therefore be regenerated. `run.sh` already reruns preparation and
both linkers for every launch, so ordinary launches regenerate them without a
cache migration. BBPROBE1/2 remain compatible, as does the Linux loader's legacy
linked-image admission. No game dump is modified.

## Provenance

Methodology references from the `evill33t/Paleblood` fork's
`codex/darwin-boot-startup` and `codex/loader-tls-displacement` branches,
with no runtime source transplanted:

- [Paleblood c412572dce337663197070fa68dfd259a8bbe3be](https://github.com/evill33t/Paleblood/commit/c412572dce337663197070fa68dfd259a8bbe3be):
  qualify the owned Darwin TSD slot before rewriting/entry.
- [Paleblood 6492260ee75ff5e0dbdf898e6bacc80d6760c796](https://github.com/evill33t/Paleblood/commit/6492260ee75ff5e0dbdf898e6bacc80d6760c796):
  checked GS displacement and bounded synthetic TLS fixtures.

## Validation

Owned synthetic fixtures cover defaults, matching hexadecimal environment
values, explicit precedence, explicit/environment mismatches, key bounds,
unchanged existing output on refusal, both generated headers and rewriting
across main plus two modules. Native loader fixtures cover all three linked
layouts, stale/mismatched slots, strict mode, missing metadata, every truncated
slot-word length, invalid alignment/bounds and unknown flags. A native fixture
executes the emitted GS load and checks the attached TCB's self pointer. Runtime
tests exercise worker TLS isolation. These are CPU-only fixtures; they do not
establish gameplay or FPS.

The focused macOS loader binary is built from the production loader/runtime
against existing x86-64 dependencies, with GPU transport replaced by four
no-op fixture functions. The regular loader unconditionally starts the GPU
process before parsing `--cpu-only`; using the fixture transport prevents that
unrelated startup from opening a renderer during header/CPU tests. A separate
production transport build is also compiled. GPU transport, gameplay and a full
renderer rebuild are outside this validation.

Validation on 2026-10-25 (base `f2c041c`):

- `python3 -m unittest discover -s tests -p 'test_tls_slot.py' -v`: **12/12**,
  no skips.
- `python3 -m unittest discover -s tests -p 'test_link*.py' -v`: **8/8**.
- `out/runtime-test`: **PASS**, including worker/libc TLS isolation.
- Production loader/runtime and the CPU transport fixture compiled with
  Apple Clang `-arch x86_64 -mmacosx-version-min=15.0 -Wall -Wextra -Werror`.
  Existing x86-64 SDL, GPU and ATRAC9 build artifacts were reused.
- Existing `test_probe.py`: **7 passed, 7 failed**, unchanged from the base
  loader plus the base test file. Those seven tests expect unsupported imports
  to exit 20; the current runtime returns zero instead. New positive TLS tests
  use an owned `UD2` at the native export, and therefore do not depend on that
  unrelated import policy. All admission/bounds cases pass.

Local build argument arrays, the four-function fixture transport, test logs and
the before/after loader-suite comparison are saved under `out/` for review.
