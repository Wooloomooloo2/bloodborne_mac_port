#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Mark Hall
"""GPU time per render pass (or per draw) from a Metal System Trace of bbgpu-host.

Run the game with BB_PASS_LOG=<file> BB_PASS_LABELS=1 (per pass) or 2 (a pass per draw), record
`xcrun xctrace record --template "Metal System Trace" --attach $(pgrep -x bbgpu-host) --time-limit 2s
--output t.trace`, export two tables and join:

  xcrun xctrace export --input t.trace --xpath '/trace-toc/run[1]/data/table[@schema="metal-gpu-intervals"]' --output gpu.xml
  xcrun xctrace export --input t.trace --xpath '/trace-toc/run[1]/data/table[@schema="metal-application-encoders-list"]' --output enc.xml
  tools/pass_trace.py gpu.xml enc.xml <pass log> <frames per second during the trace>

MoltenVK names render encoders after their Vulkan command buffer ("cb#N", vk_scheduler.cpp); the
k-th render encoder named cb#N is the k-th pass logged for N. Prefer BB_PASS_LABELS=1: with a pass
per draw (2) Instruments drops data.
Command buffers whose encoder count differs from the log (MoltenVK split a pass) are skipped
and counted.
"""
import collections
import re
import sys
import xml.etree.ElementTree as ET


def load(path):
    ids, cols, rows = {}, None, []

    def val(e):
        if 'ref' in e.attrib:
            return ids[e.attrib['ref']]
        v = (e.attrib.get('fmt'), e.text, {c.tag: val(c) for c in e})
        if 'id' in e.attrib:
            ids[e.attrib['id']] = v
        return v

    for _, el in ET.iterparse(path, events=('end',)):
        if el.tag == 'schema':
            cols = [c.findtext('mnemonic') for c in el.findall('col')]
        elif el.tag == 'row':
            rows.append({n: val(c) for n, c in zip(cols, list(el))})
            el.clear()
    return rows


def lab(v):
    return (v[0] or v[1] or '') if v else ''


def num(v):
    try:
        return int(v[1])
    except (TypeError, ValueError):
        return 0


def main():
    gpu_path, enc_path, log_path, fps = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
    passes = collections.defaultdict(list)
    for line in open(log_path, errors='replace'):
        n, _, label = line.rstrip('\n').partition(' ')
        passes[n].append(label)

    # Encoders per Metal command buffer, in order. MoltenVK emulates tessellation with a compute
    # encoder per tessellated draw, which splits the render pass around it: a render encoder right
    # after such a compute encoder that followed the same pass continues that pass.
    per_mtl = collections.defaultdict(list)
    for r in load(enc_path):
        if 'bbgpu-host' not in lab(r.get('process')):
            continue
        per_mtl[lab(r.get('cmdbuffer-id'))].append(
            (num(r.get('start')), lab(r.get('encoder-label')), lab(r.get('encoder-id'))))
    segments = collections.defaultdict(list)  # cb number -> [[encoder ids of one pass]]
    for encs in per_mtl.values():
        encs.sort()
        prev_render, prev_was_tess = None, False
        for _, label, eid in encs:
            m = re.fullmatch(r'cb#(\d+)', label)
            if m:
                n = m.group(1)
                if prev_was_tess and prev_render == n:
                    segments[n][-1].append(eid)
                else:
                    segments[n].append([eid])
                prev_render, prev_was_tess = n, False
            elif 'tess control' in label:
                prev_was_tess = True
            else:
                prev_render, prev_was_tess = None, False

    label_of, matched, skipped = {}, 0, 0
    for n, segs in segments.items():
        logged = passes.get(n, [])
        if len(segs) == len(logged) + 1:
            # The frame's last pass is the host's post-processing pass (Presenter), not logged.
            logged = logged + ['(host post-process pass)']
        if len(segs) != len(logged):
            skipped += 1
            continue
        matched += 1
        for seg, label in zip(segs, logged):
            for eid in seg:
                label_of[eid] = label

    time = collections.defaultdict(lambda: collections.Counter())
    count = collections.Counter()
    total = collections.Counter()
    t0, t1 = None, 0
    for r in load(gpu_path):
        if 'bbgpu-host' not in lab(r.get('process')):
            continue
        s, d = num(r.get('start')), num(r.get('duration'))
        t0 = s if t0 is None else min(t0, s)
        t1 = max(t1, s + d)
        channel = lab(r.get('channel-name'))
        total[channel] += d
        label = label_of.get(lab(r.get('encoder-id')))
        if label:
            time[label][channel] += d
            if channel == 'Fragment':
                count[label] += 1
    frames = fps * (t1 - t0) / 1e9
    print(f'{matched} command buffers joined, {skipped} skipped (encoder count differs); '
          f'~{frames:.0f} frames')
    print('all GPU work, ms/frame: ' + ', '.join(f'{c} {v / 1e6 / frames:.2f}' for c, v in total.items()))
    joined = sum(sum(c.values()) for c in time.values())
    print(f'joined to passes: {joined / 1e6 / frames:.2f} ms/frame')
    print(f'{"frag ms/f":>9} {"vert ms/f":>9} {"n/frame":>8}  pass')
    for label, c in sorted(time.items(), key=lambda x: -x[1]['Fragment'])[:40]:
        print(f'{c["Fragment"] / 1e6 / frames:9.2f} {c["Vertex"] / 1e6 / frames:9.2f} '
              f'{count[label] / frames:8.1f}  {label}')


if __name__ == '__main__':
    main()
