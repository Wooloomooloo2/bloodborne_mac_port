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
