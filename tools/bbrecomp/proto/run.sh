#!/usr/bin/env bash
# Prototype of the recompilation path on pure integer functions: lift to C, compile for arm64,
# compare with the original x86 code run under Rosetta 2 (docs/STATIC_RECOMPILATION_PLAN.md).
# Output (derived from the game, never commit) in out/recomp_survey/proto/.
# usage: tools/bbrecomp/proto/run.sh   (from the main checkout with deps/ and out/recomp_survey)
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
out=out/recomp_survey
[[ -f $out/image.bin && -f $out/funcs.txt ]] || bash tools/recomp_survey/survey.sh > /dev/null
mkdir -p "$out/proto"
clang -arch x86_64 -O2 -Wall -Wextra -Ideps/x86_64/include tools/bbrecomp/proto/lift.c \
    deps/x86_64/lib/libZydis.a deps/x86_64/lib/libZycore.a -o "$out/lift"
arch -x86_64 "$out/lift" "$out/image.bin" "$out/funcs.txt" "$out/proto"
clang -arch arm64 -O2 -w "$out/proto/lifted.c" tools/bbrecomp/proto/check.c -o "$out/proto/check"
arch -arm64 "$out/proto/check" "$out/proto/reference.bin"
# Step 2: leaf functions with memory operands (game image and a data buffer at the same fixed
# addresses on both sides; 6 register + 4 stack arguments; rax and the buffer compared).
clang -arch x86_64 -O2 -Wall -Wextra -Ideps/x86_64/include tools/bbrecomp/proto/lift2.c \
    deps/x86_64/lib/libZydis.a deps/x86_64/lib/libZycore.a -o "$out/lift2"
arch -x86_64 "$out/lift2" "$out/image.bin" "$out/funcs.txt" "$out/proto"
clang -arch arm64 -O1 -w "$out/proto/lifted2.c" tools/bbrecomp/proto/check2.c -o "$out/proto/check2"
arch -arm64 "$out/proto/check2" "$out/image.bin" "$out/proto/reference2.bin"
# Step 3: calls between lifted functions (registers in a shared context, guest return addresses
# on the guest stack, tail jumps); a function qualifies when everything it calls does.
clang -arch x86_64 -O2 -Wall -Wextra -Ideps/x86_64/include tools/bbrecomp/proto/lift3.c \
    deps/x86_64/lib/libZydis.a deps/x86_64/lib/libZycore.a -o "$out/lift3"
arch -x86_64 "$out/lift3" "$out/image.bin" "$out/funcs.txt" "$out/proto"
clang -arch arm64 -O1 -w "$out/proto/lifted3.c" tools/bbrecomp/proto/check3.c -o "$out/proto/check3"
arch -arm64 "$out/proto/check3" "$out/image.bin" "$out/proto/reference3.bin"
# Step 4: AVX (simd.inc): ~70 VEX instructions lane by lane with x86 rules; xmm0-7 arguments,
# xmm0 compared too. Compile without FP contraction (an FMA would round differently).
clang -arch x86_64 -O2 -Wall -Wextra -Ideps/x86_64/include tools/bbrecomp/proto/lift4.c \
    deps/x86_64/lib/libZydis.a deps/x86_64/lib/libZycore.a -o "$out/lift4"
arch -x86_64 "$out/lift4" "$out/image.bin" "$out/funcs.txt" "$out/proto"
clang -arch arm64 -O1 -ffp-contract=off -w "$out/proto/lifted4.c" tools/bbrecomp/proto/check4.c -o "$out/proto/check4"
arch -arm64 "$out/proto/check4" "$out/image.bin" "$out/proto/reference4.bin"
