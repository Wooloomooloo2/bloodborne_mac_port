# Issue #99: absent graphics bind record

The 0.5.10 report faults in the **x86-64 game process**, at guest offset
`0x2169edc`, with address zero and RAX/RBX/RDI zero. This differs from the native
GPU descriptor fault covered by #74/#97 and 0.5.10.

## Evidence and behavior

The prepared Bloodborne 1.09 image places the fault inside the graphics resource
dispatcher at `0x2169ec0`. Its recorded caller at `0xe2f130` passes a resource
record as the third argument. The dispatcher calls the identity accessor at
`0x26a94c0`, then dereferences the returned record. A null record therefore reaches
the exact reported instruction and address. The record's family and type select
Gnm writers, including the shader-register writer at `0x1077850`.

The dispatcher already sends unsupported families/types through the context
finalizer at `0x26af700`, without updating a binding. The runtime guard routes a
null record through that same finalizer, passing `context + 0x10` and retaining
its return value. Every non-null record executes the original instructions.
The hook checks the entry, accessor, existing finalization tail and finalizer
bytes before installing, and refuses a different image. The moved entry
instructions contain no relative addressing. Set `BB_NULL_BIND_GUARD=0` to disable
the guard for comparison. The first eight null records are logged.

This is containment of the reported null dereference. The source of the missing
record remains unknown. A skipped bind may leave earlier graphics state in use.
The preceding `Undefined vertex attribute 0` pipeline failures are not proven to
cause this guest fault and are not repaired here. The logs show transient arena
growth and spare allocations, but do not establish exhausted memory pools.

## Validation

`tests/test_render_guard.c` reads instruction windows from a private prepared
1.09 ELF. It distributes no game image. It replaces the dispatcher's external
writers and finalizer with recorders, then checks:

- The unmodified null-record case faults specifically at `0x2169edc`, address
  zero, in an isolated child.
- All 40 non-null family/type/counter cases preserve full context bytes, ordered
  writer arguments and return values after installation.
- Null records match the original unsupported-family path for both tested
  counter states; a later valid record continues to work.
- Disabled, undersized and mismatching images remain unpatched.

Run on an x86-64 host or under Rosetta:

```sh
clang -arch x86_64 -std=c11 -O2 -g -Wall -Wextra -Werror -I. -Isrc \
  tests/test_render_guard.c src/runtime_render_guard.c -o out/render-guard-test
out/render-guard-test /private/path/to/prepared/eboot.elf
```

`build_macos.sh --test` also runs this fixture when `BB_RENDER_GUARD_ELF` is set;
otherwise it explicitly reports a skip. The private image used for local
verification had SHA-256
`cec1b276e7f9e4db978e57f524f41fbaac594530a3437b002e23f3fab14b4f86`.
Hopper via REA corroborated the dispatcher control flow; the differential test
executes the original dispatcher instructions rather than the decompiler output.

The complete `BB_LTO=OFF bash build_macos.sh --test` run passed locally, with
`BB_RENDER_GUARD_ELF` set, including both GPU architectures, runtime/launcher,
shared-ring/GPU-split and Metal fixtures. Xcode's macro compiler and Metal tests
required execution outside the restricted filesystem sandbox.

The fixture does not execute the real finalizer or Gnm writers. It proves the
routing and observed dispatch equivalence, not long-session gameplay stability,
visual correctness after an absent bind, or a fix for the missing resource's
origin. The reporter's 101-minute Hunter's Dream session has not been reproduced.
