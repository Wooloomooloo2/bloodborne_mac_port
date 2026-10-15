# Static recompilation plan: running Bloodborne without Rosetta 2

Branch: `static-recompilation` (started 2026-10-14). Status: **plan + M0 survey**. Master keeps
the Rosetta path until this branch reaches parity.

## Why

Everything x86-64 in this port (the game's `eboot.bin`, the PS4 `libc.prx`/`libSceFios2.prx`
we link into it, and our own HLE runtime in `src/`) runs under Rosetta 2. Apple's stated
plan (June 2025, repeated since; macOS 26.4 shows a "Rosetta is going away" notice): Rosetta
stays a general-purpose tool **through macOS 27**; from **macOS 28 (expected autumn 2027)** only
"a limited subset" remains, for older unmaintained games that rely on Intel frameworks. Apple
has not said what the subset includes. We cannot assume a loader that maps a PS4 executable
and jumps into it will qualify, so the port needs to run natively on arm64.

This reverses decision 1 in `CLAUDE.md` ("no recompiling the game") for this branch, at the
user's request (2026-10-14).

### Options considered

| Option | Verdict |
|---|---|
| Keep Rosetta | Works until macOS 27; unknown after. Keep as the fallback while this lands. |
| Our own dynamic translator (Box64/FEX style JIT) | Months of work and permanently slower than Rosetta; JIT on macOS needs `MAP_JIT` + W^X toggling per thread; Box64/FEX are Linux-only. |
| **Static recompilation** (this plan) | The game never generates code at run time (PS4 games cannot JIT), the code is fixed (v1.09), and the eboot carries unwind tables for nearly all functions. Ahead-of-time translation with a small interpreter fallback for anything missed. |
| Rewrite / decompile the game | Not realistic. |

**Legal constraint (unchanged):** the recompiled output is derived from the player's own game.
It is produced on the player's Mac at install time (like `boot-linked.bin` today), never
committed and never distributed. Only the recompiler is part of the repository.

## What has to be translated (M0 survey, 2026-10-14)

`tools/recomp_survey/survey.sh` (results in `out/recomp_survey/`, not committed):

| Measure | Value |
|---|---|
| Executable segment | 0x0 – 0x50d96dc (84.8 MB incl. read-only data); code 0xa0 – 0x2bbe4a5 (~45.9 MB) |
| Functions with an FDE (`.eh_frame_hdr`, PT_GNU_EH_FRAME at 0x4f41510) | **162,959**, covering 41.7 MB |
| Instructions (linear decode of FDE ranges, Zydis) | **10.1 million**, 400 distinct mnemonics |
| Functions containing undecodable bytes | 2,242 (73.5 KB): data in code, padding, or alignment; to classify |
| Direct calls / indirect calls / indirect jumps | 426k / **173k** (virtual calls) / **26.6k** (12.2k via memory: switch tables, import thunks) |
| SIMD | **1.0M VEX (AVX) instructions**, 8.8k using 256-bit `ymm`; **no AVX2, no FMA** (the PS4's Jaguar has neither); a few SSE2 |
| Other ISA extensions | BMI1 4.8k, LZCNT 506, MOVBE 987, POPCNT 118, CMOV 29.6k |
| x87 | 2.4k instructions |
| Atomics | 12.2k `lock`-prefixed |
| Time/CPU | `rdtsc` 3.2k, `cpuid` 4, `syscall` 0 (all system calls go through imports) |
| TLS | 17.1k `fs:` accesses (rewritten to `gs:[0xc00]` by our loader) |
| Stack switches (`mov rsp, X`, X ≠ rbp) | 92: the engine's own job fibers (no `sceFiber` imports) and stack realignment |
| Relocations | 231k `R_X86_64_RELATIVE` (pointers in data: vtables, function tables, switch tables) |
| Imports | 686, all named in `src/import_names.inc`; include **C++ exceptions** (`__cxa_throw`, `_Unwind_Resume`, `__cxa_begin_catch`, …) and `setjmp`/`longjmp` |

Not yet counted: the linked `libc.prx` and `libSceFios2.prx` (same treatment, ~1.3 MB; libc is
linked into the image at 0x56e0000), and the patch sites of `patches/Bloodborne.xml` (60 FPS,
resolution, camera).

**M0 findings (2026-10-14 night):**

- **Undecodable bytes are switch tables.** All 2,242 cases are arrays of 32-bit relative
  offsets that the compiler placed at the end of the function, inside its FDE range. Discovery
  must follow control flow from entry points (recursive descent), never decode linearly, and the
  tables give the switch targets directly.
- **Code without FDEs (3,717 ranges, 2.85 MB of real instructions) is static initialization**
  (`_GLOBAL__sub_I_*`-style constructors that fill globals), reached through the initializer
  array: relocation targets find them.
- **No stack switching in game code.** Besides `alloca` (`mov rsp, reg` after `sub`), nothing in
  the eboot loads `rsp`. So no hand-written fiber switch needs special handling in the game code
  itself (still to check: libc).
- **Run-time counts** (`BB_CALL_COUNTS=1`, count-only hooks on the linked libc, ~2.5 min: title,
  load, walk): `__cxa_throw` **0**, `_Unwind_Resume` **0**; `setjmp` **> 262,000** (~1,700/s,
  from a handful of call sites, e.g. image+0x296dc3b, +0x23057cf), `longjmp` **> 512** (from
  image+0xa3217a, +0xa315a8, …). The protected-call pattern of a scripting VM (FromSoftware's engine
  uses Havok Script, a Lua derivative: `pcall` = `setjmp`, errors/yields = `longjmp`). So C++
  exceptions can start as "abort with a report", but `setjmp`/`longjmp` must be fast and
  correct from day one.

**M1 progress (2026-10-15): instruction test vectors.** `tools/bbrecomp/testgen/build.sh` finds
**866 distinct instruction forms** (mnemonic, encoding, prefixes, operand kinds and sizes) among
the 7.1M testable instructions of the game's functions (2.9M more are control flow, stack or
system instructions, tested differently), re-encodes one of each with its memory operand moved to
a scratch buffer and a random immediate, and runs 32 random cases per form under Rosetta 2:
16 GPRs, RFLAGS, the FXSAVE image (x87, MXCSR, xmm), upper ymm halves and 256 bytes of memory,
in and out. 27,712 cases in 3.4 s; spot checks against independent arithmetic match
(`add`, `vaddps`). Only divides trap (#DE, by design in a quarter of their cases). The vectors
(69 MB) stay in `out/` with the other derived files; `vecdump.py` reads them.
Learned on the way: **Rosetta 2 keeps translations by address** — rewriting a code page and
toggling its protection still ran the previous instruction (SIGILL on valid code); each case now
gets a fresh address. Next for the generator: vary the registers per case (today each form uses
its first occurrence's registers) and decode by recursive descent (linear decoding still meets
data: privileged instructions are filtered out).

**M1 progress (2026-10-15): code discovery.** `tools/bbrecomp/discover/run.sh`: recursive descent
from 162,959 FDE starts, the entry point, 2 module inits, 218 libc exports and 167,625 relocation
values that land in code (function pointers, vtables, initializer arrays). Result: **1.90M basic
blocks, 11.2M instructions, 97.5% of the FDE function bytes reached** plus 3.4 MB of code
outside FDEs (static initializers); 2,982 switch tables recovered (lea + movsxd + add + jmp;
361,728 entries: some generated functions hold 1,873-case switches). Indirect control flow:
170k indirect calls; indirect jumps that are not switches are tail calls through a pointer
(5,805 after an epilogue, 2,969 through a vtable `jmp [reg+disp]`, 119 `jmp [rip+slot]`, and
~4,700 in frameless functions, same shape in every sample). So no computed jumps inside
functions remain beyond switches; every other indirect target goes through the address table.
Next: mark which of the 2.5% unreached FDE bytes are padding/tables vs. code only reachable
through computed pointers, and record execution traces under Rosetta to check coverage.

**First end-to-end proof (2026-10-15): game functions running natively on arm64.**
`tools/bbrecomp/proto/run.sh` takes every game function made only of a small integer subset
(moves, add/sub/logic/shift/imul, compares, cmov/setcc, branches, register push/pop, frame
pointer moves; no memory beyond its own stack): **279 functions, 2,239 instructions** (largest
58; e.g. a UTF-8 length routine). It writes each as a C function (registers as locals, flags
computed explicitly), compiles them with clang **for arm64**, runs the original x86 bytes under
Rosetta 2 on 64 random argument sets each, and compares: **279 of 279 functions, 17,856 of
17,856 cases identical.** Small, but it exercises the whole chain: decoding, control flow,
sub-register writes, flags, C emission, native compilation, differential testing.
Next: memory operands (guest addresses = host addresses), then calls between lifted functions.

## Architecture

```
install time (player's Mac)                         run time (one arm64 process)
───────────────────────────                         ────────────────────────────
eboot.bin ─► prepare/link (today's scripts)          recompiled game (.dylib or static)
          ─► patches applied (per patch set)            │ guest calls ─► HLE runtime (src/, arm64)
          ─► bbrecomp: discover functions,              │ indirect calls ─► address table
             lift x86-64 → C, per shard                 │   miss ─► x86 interpreter (logs the address)
          ─► clang -O2 (arm64), cached by hash          └─ GPU core (gpu/, arm64, already native)
```

### 1. Recompiler (`tools/bbrecomp/`, arm64 host tool)

- **Decoder:** Zydis (already a dependency).
- **Function discovery:** FDE starts (163k) + direct call targets + relocation targets that land
  in code (vtables, function pointers) + exports + the module init/entry points. Every address
  that indirect control flow may reach must be an entry in the address table.
- **Switch tables:** pattern-match the compiler's forms (`cmp/ja` bound, `lea base` +
  `movsxd [base+idx*4]` + `add` + `jmp reg`, or `jmp [rip+table+idx*8]`) and emit a C `switch`.
  Unresolved intra-function indirect jumps fall back to a per-function label dispatch.
- **Output:** C (one file per shard of a few thousand functions), one C function per guest
  function, taking a `GuestContext*`. Each guest basic block is a label. C (not raw LLVM IR) so
  the output is readable and debuggable; clang does register allocation, flag dead-code
  elimination and vectorization. If clang's compile time becomes the bottleneck (10M
  instructions), switch the back end to emitting LLVM bitcode directly (same front end).
- **Semantics library (`bbrecomp/semantics.h`):** one C implementation per x86 instruction form,
  shared by the emitter and the interpreter fallback, so both always agree. SIMD via
  [SIMDe](https://github.com/simd-everywhere/simde) (x86 intrinsics on NEON, MIT): AVX-128/256
  map to `simde_mm*`/`simde_mm256*` calls; 256-bit ops are two NEON halves.

### 2. Guest state and ABI

- **Registers:** `GuestContext` holds the 16 GPRs, RIP-free (code addresses are static), flags,
  16 YMM registers, MXCSR, the x87 stack and the TLS base. Inside a function the emitter keeps
  registers in C locals and writes them back only at calls, indirect branches and returns; clang
  promotes them to arm64 registers.
- **Flags:** computed lazily as C expressions; the emitter tracks flag liveness per block and
  only materializes flags that are read (most `cmp/test` results feed the next `jcc` directly).
- **Memory:** guest addresses stay identical to host addresses: the guest image and all guest
  memory are mapped at the same virtual addresses in the arm64 process. This already works
  today: `bbgpu-host` (arm64) maps all guest memory at its guest addresses (M1/M3 of the native
  GPU process). Loads and stores are plain pointer accesses; no base register, no bounds checks.
- **Guest stack:** the guest `rsp` stack stays real guest memory (code takes addresses of
  locals, the engine walks its own stack, our diagnostics read guest frames). `call` pushes the
  guest return address and then makes a host C call; `ret` pops it and returns. A return
  address that does not match (a tail-jump trick, a longjmp) is detected and routed through the
  dispatcher.
- **Imports (HLE):** each import becomes a thunk that reads SysV arguments from the context
  (`rdi, rsi, rdx, rcx, r8, r9`, `xmm0–7`, stack) and calls our runtime function natively; the
  return value goes back into `rax`/`xmm0`. The 686 imports are already implemented in `src/`
  and `gpu/`; their prototypes drive the thunk generator.
- **TLS:** `fs:`/`gs:[0xc00]` accesses read the TLS base from the context; no segment registers.
- **`rdtsc`/`sceKernelReadTsc`:** the host counter scaled to the frequency the game is told
  (`CyclesToNs` already handles M1–M3 24 MHz vs M4 1 GHz).

### 3. The hard parts

1. **Memory ordering.** x86 is TSO; arm64 is weakly ordered. Rosetta uses the hardware TSO mode
   of Apple Silicon, which macOS does not expose to ordinary processes. Plan: shared-memory
   loads as `LDAPR` and stores as `STLR` (ARMv8.3 RCpc; that is how FEX emulates TSO) via
   `__atomic_*` with acquire/release, plain accesses for provably thread-local data
   (`rsp`/`rbp`-relative stack slots that do not escape), `lock`-prefixed ops as LSE atomics
   with sequential consistency. Measure the cost early (M2); try relaxing per region with
   tests. The load race we fixed on 2026-10-14 is the kind of bug wrong ordering would create.
2. **Engine fibers** (M0: no stack switch found in the game code; the 92 `mov rsp, …` are
   `alloca`. Confirm in libc and at run time before building this.) If the job system switches
   stacks itself: Recompiled code
   runs on the host stack while the guest stack lives in guest memory, so a guest stack switch
   must also switch host stacks. Identify the switch routines (M0), and replace each with an
   intrinsic: every guest fiber stack gets a host stack, and the switch is a host context
   switch plus the guest register swap.
3. **C++ exceptions** (0 throws in a 2.5 min run; can start as "abort with a report").
   The game imports `__cxa_throw`/`_Unwind_Resume`. Throwing must unwind
   guest frames (eh_frame is present for all of them) and resume at a landing pad, which is a
   label inside a recompiled function. Plan: functions with landing pads get a host-level catch
   point (`setjmp`-style) that the guest unwinder `longjmp`s to with the pad's label; the guest
   unwinder works on guest stack and context. First measure whether throws happen at all during
   play (a counter on `__cxa_throw` under Rosetta, now).
4. **`setjmp`/`longjmp`** (libc, ~1,700 `setjmp`/s in play): calls to libc `setjmp` are known
   statically, so the emitter writes them as a real host `setjmp` inside the generated C function
   (clang handles "returns twice" there) that also saves the guest registers into the guest
   `jmp_buf`; `longjmp` restores the guest registers and `longjmp`s the host buffer. No guest
   stack scanning is needed.
5. **Coverage of indirect targets.** Any address reached indirectly but missing from the table
   goes to the **interpreter fallback** (an x86-64 interpreter in arm64 built on the same
   semantics library: slow but correct), which logs the address; the next recompile includes
   it. Ship a seed list from our own play-throughs.
6. **x87** (2.4k instructions): 80-bit precision via a software float (or `double` if tests
   show it is enough; check what the 2.4k sites compute).
7. **Floating-point details:** MXCSR rounding modes and DAZ/FTZ map to FPCR; NaN payloads and
   min/max operand order differ from NEON and must follow x86 semantics in the library.
8. **Code patches.** The 60 FPS/camera/resolution patches rewrite code bytes. Recompile with the
   patch set applied and cache by `(eboot hash, patch set hash, recompiler version)`. Resolution
   changes from the menu restart the game today; they must not trigger a full recompile, so
   patched functions are compiled in every variant the menu can select and chosen at start.
9. **Our fault-driven GPU tracking** (write-protect + fault relays) keeps working: faults now
   arrive with an arm64 `ucontext`; the handlers only need the faulting address.
10. **Build time on the player's Mac.** 10M instructions is about 5× a large Xbox 360 game in
    XenonRecomp. Shard the output, compile in parallel, cache objects, and consider the direct
    LLVM back end. Target: under 15 minutes once on an M-series Mac.

### 4. Validation (start now, while Rosetta exists)

- **Instruction test vectors:** for every instruction form found in the eboot, generate random
  inputs, execute the real instruction under Rosetta, and store inputs/outputs (registers,
  flags, memory). These are generated from encodings, not game code, so they can be committed.
  The semantics library must reproduce them bit for bit. After macOS 28 these cannot be
  regenerated on Apple Silicon, so collect them early.
- **Import-call traces:** record the sequence and arguments of HLE calls during a scripted run
  (title → load → walk; our `walk.sh` setup) under Rosetta; replay the same input with the
  recompiled build and diff deterministic parts.
- **Function-level differential runs:** while both paths exist, run single functions (pure
  math, string, decompression routines) in both and compare.
- Existing checks: `build_macos.sh --test`, screenshots of the walk routes, the Gnm command
  buffer checks.

### 5. Process layout afterwards

The split into `bb-probe` (x86-64 under Rosetta) and `bbgpu-host` (arm64) exists only because
of Rosetta. Once the game is native, the runtime and the GPU core run in **one arm64 process**
again, removing the shared-memory rings and protect/fault relays (and their latency). Keep the
split path working until the merged one is proven.

## Milestones

| | Milestone | Exit criteria |
|---|---|---|
| M0 | **Survey** (mostly done) | Done: numbers above, undecodable ranges (switch tables), non-FDE code (static init), stack switches (none), exception/`setjmp`/`longjmp` counts. Left: survey `libc.prx`/`libSceFios2.prx`; list patch sites; confirm how job "fiber stacks" are entered. |
| M1 | **Discovery + test vectors** (vectors started) | `bbrecomp` lists functions, blocks, switch tables, indirect-target table; coverage report vs. an execution trace of a play session. Instruction test-vector generator committed; vectors for every form in the eboot. |
| M2 | **Integer core** | Semantics for all integer/control-flow forms; emitter produces compilable C for the whole eboot; vectors pass. Memory-ordering cost measured on a synthetic benchmark. |
| M3 | **SIMD, x87, FP** | AVX/SSE via SIMDe, x87, MXCSR; vectors pass. |
| M4 | **Runtime on arm64** | `src/` builds for arm64; context, import thunks, TLS, `setjmp`/`longjmp`, exceptions, fibers, fault handlers; interpreter fallback. Unit tests pass on arm64. |
| M5 | **First native boot** | Recompiled eboot reaches the title screen with the arm64 GPU core in-process (split mode kept as an option). |
| M6 | **Playable** | Load a save, walk routes from `walk.sh`, no interpreter hits on the routes, no Gnm check failures; screenshots match the Rosetta build. |
| M7 | **Performance** | ≥ Rosetta's FPS on the M4 Max city and 1440p scenes; ordering relaxations validated. |
| M8 | **Install pipeline** | `run.sh`/launcher recompile on first launch (progress shown), cache, patch variants; Rosetta path becomes optional. |

Rough size: M0–M2 are the bulk of the new code (recompiler and semantics); M4 is mostly porting
work we understand; M6–M7 are open-ended debugging. Expect months, not weeks, with a working
title screen (M5) as the first real proof. The Rosetta build stays the shipping path until M6.

## Risks

- **Memory-ordering cost** could eat the gain from dropping Rosetta (Rosetta has hardware TSO).
  Mitigation: escape analysis for stack accesses, per-region relaxation backed by tests.
- **Missed indirect targets** in rarely visited content (bosses, NG+): interpreter fallback and
  logging keep the game running; collect from long play sessions.
- **Exceptions and fibers** are the most invasive runtime features; prototype both in M4 before
  scaling up.
- **Compile time and output size** on smaller Macs: sharding, caching, a direct LLVM back end.
- **Apple's subset might cover us after all:** then this work buys performance and a single
  native process, not survival; still worth it, but the urgency would change.

## Next steps

1. Finish M0: libc/Fios2 survey, patch sites, how jobs reach their own stacks.
2. Start the instruction test-vector generator (M1) while Rosetta is available.
3. ~~Prototype the whole pipeline on leaf functions~~ (done for pure integer functions: 279 of
   279 match); extend it to memory operands and calls.

Sources for the Rosetta timeline: Apple's developer documentation as reported by
[MacTrast](https://www.mactrast.com/2025/06/apple-phasing-out-rosetta-2-with-release-of-macos-28-as-intel-support-sunsets/amp/),
[TechRepublic](https://www.techrepublic.com/article/news-apple-macos-26-4-rosetta-2-support-ending/)
and [Michael Tsai](https://mjtsai.com/blog/2025/06/10/ending-macos-intel-support/).
