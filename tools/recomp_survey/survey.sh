#!/usr/bin/env bash
# Static-recompilation survey of the prepared game image (docs/STATIC_RECOMPILATION_PLAN.md, M0).
# usage: tools/recomp_survey/survey.sh [boot-linked.bin] [out dir]
# The image and the results are derived from the game: keep them out of the repository.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
boot=${1:-out/macdata/out/boot-linked.bin}
out=${2:-out/recomp_survey}
mkdir -p "$out"
python3 -I -c "
import struct,sys
d=open(sys.argv[1],'rb').read(); size=struct.unpack_from('<Q',d,8)[0]
open(sys.argv[2],'wb').write(d[-size:])" "$boot" "$out/image.bin"
python3 -I tools/recomp_survey/ehframe.py "$out/image.bin" "$out/funcs.txt"
inc=deps/x86_64/include libs=(deps/x86_64/lib/libZydis.a deps/x86_64/lib/libZycore.a)
for tool in isastat rspw; do
    clang -arch x86_64 -O2 -I"$inc" "tools/recomp_survey/$tool.c" "${libs[@]}" -o "$out/$tool"
    "$out/$tool" "$out/image.bin" "$out/funcs.txt" | tee "$out/$tool.txt"
done
