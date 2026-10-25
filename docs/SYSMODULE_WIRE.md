# System-module argument validation

The system-module exports receive a complete `uint32_t` ID. Previously their
`uint16_t` parameter silently converted `0x100b4` into AppContent `0xb4`, so a
wider load, query or unload could affect that unrelated module.

The port retains its existing 16-bit host module namespace, reference counts,
configured AppContent provider and initialization lifecycle. Wider arguments
are validated before any array lookup, provider check or reference mutation.
Unsupported wider requests stop with the API name and complete ID. This stop
is an explicit host-policy limit, not a recovered native error result.

The independently recovered native early errors are narrower:

| API | Early invalid-ID condition | Numeric result |
| --- | --- | --- |
| `sceSysmoduleLoadModule` | `(id & 0x7fffffff) == 0` | `0x805a1000` |
| `sceSysmoduleIsLoaded` | `id == 0` | `0x805a1000` |
| `sceSysmoduleUnloadModule` | `id == 0` | `0x805a1000` |

In particular, loading `0x80000000` returns the known error. Querying or
unloading that ID reaches the unsupported host boundary; no native result is
invented. Other native validation, loading, duplicate-load and release behavior
remain unresolved. The port's existing narrow reference-count policy is
preserved rather than replaced by Paleblood's declared-stub policy.

## Provenance

The argument-width and early-error evidence comes from
[Paleblood](https://github.com/Yharnam-Hunters/Paleblood), fork
[`evill33t/Paleblood`, branch `codex/runtime-sysmodule-wire`](https://github.com/evill33t/Paleblood/tree/codex/runtime-sysmodule-wire),
commit [`f9c5e5556db987d9fb4fa60b837d8a54817a0718`](https://github.com/evill33t/Paleblood/commit/f9c5e5556db987d9fb4fa60b837d8a54817a0718):
`docs/own-runtime-sysmodule-wire.md` and `runtime/syslib/kernel_sysmodule.c`.
Its supplied libSceSysmodule SHA-256 is
`43c456e00b59b2bd1303023cbcb463f50c0647854b4837468f8fc6c290b55c23`.
That evidence covers load at ELF `0x410`, its validator at `0x480`, query at
`0x580`, and unload at `0x520`.

The mac-port REA/Hopper assessment independently observes the complete DWORD
being saved as `r14d` and forwarded as `edi` in the load wrapper. Its extracted
ELF SHA-256 is `fc038865866e634c58f5e66ab8460d23434121945930ddf20381c6ff809db308`,
evidence ID
`ev_f9e1e4f0a5cd01429320fad5e5e62df872ebf8bd6fbe1e864ce9e0ce53094040`.
No firmware execution or universal firmware/title compatibility is claimed.

## Focused verification

```sh
mkdir -p out
/usr/bin/clang -arch x86_64 -mmacosx-version-min=15.0 -std=c11 -O2 -g \
  -Wall -Wextra -Werror -pthread -I. -Isrc \
  tests/test_content.c src/runtime_content.c -o out/content-test
python3 -m unittest discover -s tests -p 'test_content.py' -v
```

The focused suite checks the existing AppContent profile, output boundaries,
initialization and reference lifecycle; known early errors before and after
loads; and 34 subprocess cases for wider load/query/unload IDs. The stopped
cases check exact narrow reference counts and initialization state through
`atexit`, including loaded and unloaded AppContent, independent `0xb5`, and
`0xffff` boundary records. Low-16-bit-zero IDs and the load/query/unload
distinction for `0x80000000` are covered. These are isolated runtime checks,
not gameplay or performance validation.
