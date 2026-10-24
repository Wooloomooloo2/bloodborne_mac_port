#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 Mark Hall
# Run the M0(c) benchmark: each (arch, MoltenVK config) RUNS times, alternating architectures,
# then the CPU micro-benchmark. Raw lines go to out/m0_vulkan_bench/results.txt; a summary
# table (medians of per-run medians) is printed at the end.
#   ./run.sh [runs=3] [frames=300] [warmup=60]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "${HERE}/../../.." && pwd)"
OUT="${ROOT}/out/m0_vulkan_bench"
RUNS="${1:-3}"; FRAMES="${2:-300}"; WARMUP="${3:-60}"
RES="${OUT}/results.txt"
[ -x "${OUT}/vkbench-arm64" ] || "${HERE}/build.sh"
: > "${RES}"
export MVK_CONFIG_LOG_LEVEL=1
for r in $(seq 1 "${RUNS}"); do
    for prefill in default 2; do
        for arch in x86_64 arm64; do
            if [ "${prefill}" = default ]; then
                env -u MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS "${OUT}/vkbench-${arch}" "${FRAMES}" "${WARMUP}" 2>/dev/null
            else
                MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS="${prefill}" "${OUT}/vkbench-${arch}" "${FRAMES}" "${WARMUP}" 2>/dev/null
            fi | tee -a "${RES}"
        done
    done
done
for r in $(seq 1 "${RUNS}"); do
    for arch in x86_64 arm64; do "${OUT}/cpubench-${arch}" | tee -a "${RES}"; done
done
python3 -I "${HERE}/summarize.py" "${RES}"
