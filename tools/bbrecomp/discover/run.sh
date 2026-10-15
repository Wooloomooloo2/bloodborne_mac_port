#!/usr/bin/env bash
# Code discovery report (docs/STATIC_RECOMPILATION_PLAN.md, M1). Needs tools/recomp_survey/survey.sh
# output. usage: tools/bbrecomp/discover/run.sh [boot-linked.bin]
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
out=out/recomp_survey
[[ -f $out/image.bin && -f $out/funcs.txt ]] || bash tools/recomp_survey/survey.sh > /dev/null
python3 -I tools/bbrecomp/discover/bootinfo.py "${1:-out/macdata/out/boot-linked.bin}" > "$out/roots.txt"
clang -arch x86_64 -O2 -Wall -Wextra -Ideps/x86_64/include tools/bbrecomp/discover/discover.c \
    deps/x86_64/lib/libZydis.a deps/x86_64/lib/libZycore.a -o "$out/discover"
"$out/discover" "$out/image.bin" "$out/funcs.txt" "$out/roots.txt" "$out/disc"
