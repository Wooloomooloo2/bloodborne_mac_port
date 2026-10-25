#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# CPU fixtures only: no app, window, GPU device or game startup.
# BB_TIMING_ELF=/path/to/eboot.elf also tests private 1.09 image installation.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
mkdir -p out/fps
cxx=${CXX:-/usr/bin/clang++}
flags=(-arch x86_64 -mmacosx-version-min=15.0 -std=c++20 -O2 -g -Wall -Wextra -Werror
       -ffp-contract=off -fno-fast-math -I.)
"$cxx" "${flags[@]}" tests/test_fps_timing.cpp tests/fps_reference/limiter.cpp src/fps/timing.cpp -o out/fps/timing-test
"$cxx" "${flags[@]}" tests/test_fps_install.cpp src/fps/timing.cpp -o out/fps/install-test
export ROSETTA_ADVERTISE_AVX=1
out/fps/timing-test --self-test > out/fps/baseline.json
set +e
out/fps/timing-test --self-test --mutate > out/fps/negative-control.json
status=$?
set -e
[[ $status == 1 ]] || { echo 'FAIL: timing negative control must fail'; exit 1; }
"${PYTHON:-python3}" - <<'PY'
import json
from pathlib import Path
baseline=json.loads(Path('out/fps/baseline.json').read_text())
negative=json.loads(Path('out/fps/negative-control.json').read_text())
assert baseline['failed']==0 and baseline['passed']==20 and baseline['step_checks']>=93
assert negative['failed']==20 and all(not c['passed'] for c in negative['cases'])
print(f"PASS: {baseline['passed']} limiter cases, {baseline['step_checks']} timing checks, "
      f"{baseline['wind_instruction_cases']} wind instruction cases; all 20 mutations detected")
PY
out/fps/install-test
if [[ -n ${BB_TIMING_ELF:-} ]]; then
    for cap in 30 40 60 90 120 144 165 240 0; do
        out/fps/install-test --elf "$BB_TIMING_ELF" --cap "$cap"
    done
    # Limiter, constants, task-step cave, physics, wind, play-time and cloth admission.
    for offset in 0x2034770 0x63fc3f 0x2018e3d 0x17f9b69 0x2314a60 0x19419d6 0x493bd00; do
        out/fps/install-test --elf "$BB_TIMING_ELF" --cap 120 --mutate "$offset"
    done
else
    echo 'SKIP: real 1.09 image admission (set BB_TIMING_ELF)'
fi
