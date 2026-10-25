# Semaphore handle wire contract

`sceKernelCreateSema` publishes a complete eight-byte handle. The runtime keeps
positive int32 registry IDs, sign-extends each ID into an int64 output, and uses
`memcpy` so the caller's slot can be unaligned. IDs run from 1 through INT32_MAX
and are never reused; exhaustion returns the existing `0x8002001c` status without
changing the output. The caller must supply eight writable bytes on success.

Wait, poll, signal, cancel, delete and the diagnostic waiter snapshot retain all
64 input bits through lookup. A different high half cannot acquire, signal,
cancel or delete the object with the same low half. Unknown full-width handles
use the runtime's existing `0x80020003` error. Non-NULL create options return
`0x80020016` before other argument access, instead of terminating the process.

The existing count rules, timeout accounting, FIFO traversal, ability to satisfy
a smaller request behind a larger one, priority approximation, cancellation and
deferred deletion remain the implementation's scheduling and lifetime policy.

## Evidence and provenance

The reference is [Paleblood's `codex/runtime-semaphore` branch at a7e0a4a9cfc24bee9602929c3f469b52a550e44b](https://github.com/evill33t/Paleblood/tree/a7e0a4a9cfc24bee9602929c3f469b52a550e44b), specifically
`runtime/syslib/kernel_semaphore.c` and `docs/semaphore-lifecycle.md`. That
component supports creation/deletion only; this port retains its own counted
wait/signal/cancel implementation. The change adapts the observed wire behavior,
rather than importing the reference's narrower lifecycle policy.

The supplied libkernel artifact has SHA-256
`b3e13917da667d01a353ccf49025d6b1308a32cf86f3f351f96b3a13cd5c7c2f`.
REA/Hopper's retained CreateSema instruction evidence is
`ev_0b6f1bff5b42cc0625954043801402755c7dd3fdb06f74e06dbb88f97930ccf8`.
The isolated function extract has SHA-256
`9ae99bb20d20e5fa4be8606a171db038f95866bf689953f017a8c8afbb6ae3aa`.
Its `test r9,r9` branch returns `0x80020016` before accessing other arguments;
the successful path executes `cdqe` followed by `mov qword [rbx],rax`.
The reference's Delete wrapper forwards the complete 64-bit argument. These
are static wrapper observations. Firmware/build compatibility with the title
and console kernel scheduling semantics are not established by that evidence.
Applying complete-handle lookup to all operations is the host registry's
consistent opaque-handle policy; the retained instruction extract covers create.

The original port reproducer at PR #77 head
`eb6d9d576f2a2515d4104cd76de773a841d50bfe` independently observed an output
prefilled with `0xa5a5a5a5a5a5a5a5` becoming `0xa5a5a5a500000001`, and delete
of `0x100000001` deleting ID 1. Those observations motivated the publication
and alias regressions. No title binary is needed by the committed tests.

## Focused validation

Run the x86-64 fixture under Rosetta on macOS:

```sh
python3 -m unittest discover -s tests -p test_sema.py -v
```

The Python suite compiles the real registry with lightweight clock/thread
diagnostic stubs and runs seven cases: lifecycle, FIFO/mixed token requests,
cancel/delete wakeups, priority approximation, unaligned eight-byte publication
and canaries, invalid high-bit handles through every operation, protected-memory
options rejection, and positive-int32 exhaustion. The exhaustion fixture
includes the source to reach its private counter without a production test hook.
The ordinary `build_macos.sh --test` build still compiles `test_sema.c` against
the full runtime and retains its resolver namespace checks.

For AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
mkdir -p out
/usr/bin/clang -arch x86_64 -mmacosx-version-min=15.0 -std=c11 -O1 -g \
  -Wall -Wextra -Werror -pthread -DBB_SEMA_STANDALONE -Isrc \
  -fsanitize=address,undefined tests/test_sema.c tests/sema_fixture.c \
  -o out/sema-sanitized
for mode in '' --wire --options --id-limit --concurrency --cancel-delete --priority; do
  /usr/bin/arch -x86_64 out/sema-sanitized "$mode" || exit
done
```

On 2026-10-25 all seven ordinary and all seven sanitized cases passed. Four
independent temporary mutation controls were rejected: four-byte publication,
32-bit guest input truncation, fatal options handling and allocating beyond
INT32_MAX. These are host fixture results, not a full GPU build, console
comparison, game launch or gameplay/FPS measurement.
