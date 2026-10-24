# SPDX-FileCopyrightText: Copyright 2026 Mark Hall
"""Summarise out/m0_vulkan_bench/results.txt (RESULT / CPURESULT lines from run.sh)."""
import re, statistics, sys
from collections import defaultdict

runs = defaultdict(list)   # (arch, prefill) -> list of {metric: (median, p95)}
cpu = defaultdict(list)
checksums = set()
for line in open(sys.argv[1]):
    if line.startswith("RESULT"):
        kv = dict(re.findall(r"(\w+)=(\S+)", line))
        checksums.add(kv["checksum"])
        m = {k: tuple(map(float, v.split("/"))) for k, v in kv.items() if "/" in v}
        runs[(kv["arch"], kv["prefill"])].append(m)
    elif line.startswith("CPURESULT"):
        kv = dict(re.findall(r"(\w+)=(\S+)", line))
        cpu[kv["arch"]].append(float(kv["best_ns_per_iter"]))

print(f"\nchecksums: {sorted(checksums)} ({'identical' if len(checksums) == 1 else 'MISMATCH'})")
metrics = ["record_cpu", "submit_cpu", "frame_cpu", "frame_wall", "process_cpu", "fence_wait"]
for prefill in ("default", "2"):
    print(f"\n== MoltenVK prefill={prefill}  (ms/frame; median of per-run medians; [per-run medians]; p95)")
    print(f"{'metric':<12} {'x86_64 (Rosetta)':<36} {'arm64 (native)':<36} ratio")
    for met in metrics:
        cells, meds = [], {}
        for arch in ("x86_64", "arm64"):
            rs = runs.get((arch, prefill), [])
            if not rs:
                cells.append("-"); continue
            per = [r[met][0] for r in rs]
            p95 = statistics.median(r[met][1] for r in rs)
            meds[arch] = statistics.median(per)
            cells.append(f"{meds[arch]:.3f} [{', '.join(f'{x:.2f}' for x in per)}] p95 {p95:.2f}")
        ratio = f"{meds['x86_64'] / meds['arm64']:.2f}x" if len(meds) == 2 and meds["arm64"] > 0 else "-"
        print(f"{met:<12} {cells[0]:<36} {cells[1]:<36} {ratio}")
if cpu:
    x, a = statistics.median(cpu["x86_64"]), statistics.median(cpu["arm64"])
    print(f"\n== CPU micro-benchmark (ns/iter, best of reps; median over runs)")
    print(f"x86_64 {x:.1f} {cpu['x86_64']}  arm64 {a:.1f} {cpu['arm64']}  ratio {x / a:.2f}x")
