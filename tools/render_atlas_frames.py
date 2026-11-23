#!/usr/bin/env python3
"""Workstream B: per-run atlas maintenance split by frame kind (repaint-pass
frames vs. others), measured frames only (frame >= warmup). Usage:
render_atlas_frames.py <run dir> [warmup]

Reads each run's stdout.log under a tools/render_benchmark.py output directory
(RENDERSTATS lines) and prints a markdown table per scene and label: mean
t_atlas over all frames, over frames that repainted atlas regions, and over the
rest, plus the per-repaint-frame region/copy/flush/target-switch counts.
Spread = (max - min) / mean over the runs."""
import glob, os, re, statistics, sys
from collections import defaultdict

KV = re.compile(r"(\w+)=(-?[\d.]+)")
root = sys.argv[1]
warm = int(sys.argv[2]) if len(sys.argv) > 2 else 300
agg = defaultdict(lambda: defaultdict(list))
for d in sorted(glob.glob(os.path.join(root, "*-*-[0-9][0-9]-[AB]"))):
    name = os.path.basename(d)
    scene, label = name.rsplit("-", 3)[0], name[-1]
    rows = []
    for line in open(os.path.join(d, "stdout.log"), errors="replace"):
        if line.startswith("RENDERSTATS "):
            r = {k: float(v) for k, v in KV.findall(line)}
            if r.get("frame", -1) >= warm:
                rows.append(r)
    if not rows:
        continue
    passf = [r for r in rows if r.get("atlas_regions", 0) > 0]
    other = [r for r in rows if r.get("atlas_regions", 0) == 0]
    key = (scene, label)
    agg[key]["t_atlas_all"].append(statistics.fmean(r.get("t_atlas", 0) for r in rows))
    if passf:
        agg[key]["t_atlas_pass"].append(statistics.fmean(r.get("t_atlas", 0) for r in passf))
        agg[key]["pass_frames"].append(len(passf))
        agg[key]["regions_pass"].append(statistics.fmean(r["atlas_regions"] for r in passf))
        agg[key]["flushes_pass"].append(statistics.fmean(r.get("atlas_flushes", 0) for r in passf))
        agg[key]["copies_pass"].append(statistics.fmean(r.get("atlas_copies", 0) for r in passf))
        agg[key]["switch_pass"].append(statistics.fmean(r.get("target_switches", 0) for r in passf))
    if other:
        agg[key]["t_atlas_other"].append(statistics.fmean(r.get("t_atlas", 0) for r in other))
    agg[key]["created"].append(sum(r.get("atlas_created", 0) for r in rows))
    agg[key]["flushes_all"].append(sum(r.get("atlas_flushes", 0) for r in rows))

def fmt(v):
    if not v:
        return "-"
    m = statistics.fmean(v)
    spread = (max(v) - min(v)) / m * 100 if m else 0
    return f"{m:.4f} ({spread:.0f}%)" if m < 100 else f"{m:.0f} ({spread:.0f}%)"

cols = ["t_atlas_all", "t_atlas_pass", "t_atlas_other", "pass_frames", "regions_pass",
        "copies_pass", "flushes_pass", "switch_pass", "created", "flushes_all"]
print("| scene | label | " + " | ".join(cols) + " |")
print("|" + "---|" * (len(cols) + 2))
for (scene, label), v in sorted(agg.items()):
    print(f"| {scene} | {label} | " + " | ".join(fmt(v[c]) for c in cols) + " |")
