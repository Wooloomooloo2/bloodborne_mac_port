#!/usr/bin/env bash
# Builds testgen and writes the instruction test vectors (docs/STATIC_RECOMPILATION_PLAN.md, M1).
# Needs tools/recomp_survey/survey.sh's image.bin and funcs.txt. Must run on x86-64 or under
# Rosetta 2. usage: tools/bbrecomp/testgen/build.sh [cases per form, default 32]
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
out=out/recomp_survey
[[ -f $out/image.bin && -f $out/funcs.txt ]] || bash tools/recomp_survey/survey.sh > /dev/null
clang -arch x86_64 -O2 -Wall -Wextra -Ideps/x86_64/include tools/bbrecomp/testgen/testgen.c \
    deps/x86_64/lib/libZydis.a deps/x86_64/lib/libZycore.a -o "$out/testgen"
arch -x86_64 "$out/testgen" "$out/image.bin" "$out/funcs.txt" "$out/vectors.vec" "${1:-32}"
