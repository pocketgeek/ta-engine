#!/usr/bin/env python3
"""Cursor-picking study (perf follow-up C): matched A/B runs of the debug render
study with the scripted pointer (TAK_RENDER_CURSOR, GameView::renderStudyCursor).

Every run prints one CURSORSTATS line per frame (desiredCursor wall time, the
unitUnderCursor calls it made, the cursor it chose) and the usual FRAME lines.
This tool runs A B B A groups per scene, reports the cursor evaluation and the
whole frame (mean/median/p95/p99/max after the warm-up), and REFUSES a result
whose world hash or per-frame cursor sequence differs between any two runs of a
scene: picking must choose exactly what a scan over every unit chooses.

  python3 tools/cursor_pick_study.py --a <A takclient> --b <B takclient> \\
      --data ~/tak_data --output <dir> [--scenes subset10k-sel ...] [--repeats 2] \\
      [--wrap /home/pocket_geek/tak-tmp/tools/timing.sh]

--wrap prefixes every client command (e.g. the timing-core lock script).
--a-env/--b-env KEY=VALUE apply to one side (same-binary references such as
TAK_PICK_FULL_SCAN=1). Uses only the Python standard library.
"""

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys

DEFAULT_MAP = "Ulasem Arena"
SUBSET = {"TAK_SHADOW_BENCH": "10000", "TAK_PROFILE_ZOOM": "1.0",
          "TAK_RENDER_CENTER": "7600,7600", "TAK_RENDER_OFFSET": "4200,5400"}
SCENES = {
    # 10,000 units, a small corner of the army in view, 20 units selected: the
    # selected-hover pass classifies every unit (the case the index targets).
    "subset10k-sel": dict(env=dict(SUBSET, TAK_RENDER_CURSOR_SELECT="20")),
    # The same view with nothing selected (own-unit highlight pass).
    "subset10k-nosel": dict(env=dict(SUBSET)),
    # The same view with Attack armed (the armed-attack cursor's hover pass).
    "subset10k-attack": dict(env=dict(SUBSET, TAK_RENDER_CURSOR_SELECT="20", TAK_RENDER_CURSOR_CMD="a")),
    # 10,000 units all in view: the index answers with the whole list (overhead only).
    "visible10k-sel": dict(env={"TAK_SHADOW_BENCH": "10000", "TAK_PROFILE_ZOOM": "0.25",
                                "TAK_RENDER_CURSOR_SELECT": "20"}),
    # An ordinary small battle (200 units, two sides), 20 selected.
    "small-battle-sel": dict(env={"TAK_SHADOW_BENCH": "200", "TAK_SHADOW_BENCH_TYPE": "araarch",
                                  "TAK_PROFILE_ZOOM": "1.0", "TAK_RENDER_BATTLE": "1",
                                  "TAK_RENDER_CURSOR_SELECT": "20"}),
    # 2,000-unit battle with flyers, construction, corpses, panning camera, moving units.
    "effects-pan-sel": dict(env={"TAK_SHADOW_BENCH": "2000", "TAK_PROFILE_ZOOM": "0.75",
                                 "TAK_RENDER_BATTLE": "1", "TAK_RENDER_EXTRAS": "1",
                                 "TAK_RENDER_CAMERA": "1", "TAK_RENDER_MOTION": "1",
                                 "TAK_RENDER_CURSOR_SELECT": "20"}),
}
SETTINGS = {
    "fullscreen": "0", "vsync": "0", "maxFps": "0", "uiScale": "1", "scorecardScale": "1",
    "treeSway": "1", "unitShadows": "1", "healthBars": "1", "statsPanel": "0",
    "hardwareCursor": "1", "smoothMotion": "1", "edgeScroll": "0", "masterVol": "0",
}
CURSOR_RE = re.compile(r"^CURSORSTATS frame=(\d+) ns=(\d+) tests=(\d+) cursor=(-?\d+) tint=(\d) x=([-\d.]+) y=([-\d.]+)", re.M)
FRAME_RE = re.compile(r"^FRAME ms=([\d.]+) update=([\d.]+) anim=([\d.]+) draw=([\d.]+) present=([\d.]+)", re.M)
DONE_RE = re.compile(r"RENDER_STUDY frames=(\d+) hash=([a-f0-9]+)")


def pct(values, q):
    if not values:
        return float("nan")
    s = sorted(values)
    k = (len(s) - 1) * q
    lo, hi = int(k), min(int(k) + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def summary(values):
    return dict(n=len(values), mean=statistics.fmean(values) if values else float("nan"),
                median=pct(values, 0.5), p95=pct(values, 0.95), p99=pct(values, 0.99),
                max=max(values) if values else float("nan"))


def run(args, client, label, scene, leg):
    name = f"{scene}-{leg:02d}-{label}"
    root = args.output / name
    if root.exists():
        shutil.rmtree(root)
    profile = root / "profile"
    pref = profile / "TAKengine" / "TAKingdoms"
    pref.mkdir(parents=True, exist_ok=True)
    (pref / "settings.ini").write_text("".join(f"{k} = {v}\n" for k, v in SETTINGS.items()))
    env = {k: v for k, v in os.environ.items() if not k.startswith("TAK_") and k != "DISPLAY"}
    env.update(SDL_VIDEODRIVER=args.video_driver, SDL_RENDER_DRIVER=args.render_driver, SDL_AUDIODRIVER="dummy",
               XDG_CONFIG_HOME=str(profile), XDG_DATA_HOME=str(profile), HOME=str(profile),
               TAK_PROF="1", TAK_PROF_FRAMES="1", TAK_RENDER_STUDY="1", TAK_RENDER_FRAMES=str(args.frames),
               TAK_RENDER_CURSOR="1", TAK_RENDER_STATS="0")
    env.update(SCENES[scene]["env"])
    for extra in args.env + (args.a_env if label == "A" else args.b_env):
        k, _, v = extra.partition("=")
        env[k] = v
    cmd = [str(client), "game", SCENES[scene].get("map", DEFAULT_MAP), "--data", str(args.data),
           "--testbuild", "--winsize", str(args.width), str(args.height), "--novsync", "--maxfps", "0"]
    if args.wrap:
        cmd = [args.wrap] + cmd
    with (root / "stdout.log").open("w") as out, (root / "stderr.log").open("w") as err:
        rc = subprocess.run(cmd, env=env, stdout=out, stderr=err, timeout=args.timeout).returncode
    text = (root / "stdout.log").read_text()
    if rc:
        raise RuntimeError(f"{name}: exit {rc}; see {root}")
    done = DONE_RE.search(text)
    rows = [(int(f), int(ns), int(t), int(c), int(ti)) for f, ns, t, c, ti, _, _ in CURSOR_RE.findall(text)]
    frames = [[float(v) for v in m] for m in FRAME_RE.findall(text)]
    if not done or len(rows) != args.frames or len(frames) < args.frames:
        raise RuntimeError(f"{name}: incomplete run (needs a client with the cursor study); see {root}")
    frames = frames[-args.frames:]
    seq = hashlib.sha256(" ".join(f"{c}:{ti}" for _, _, _, c, ti in rows).encode()).hexdigest()[:16]
    w = args.warmup
    return dict(name=name, label=label, hash=done[2], cursor_seq=seq,
                cursor_ms=[ns / 1e6 for _, ns, _, _, _ in rows[w:]],
                tests=[t for _, _, t, _, _ in rows[w:]],
                frame_ms=[r[0] for r in frames[w:]], draw_ms=[r[3] for r in frames[w:]],
                present_ms=[r[4] for r in frames[w:]],
                cursors={c: sum(1 for r in rows if r[3] == c) for c in sorted({r[3] for r in rows})})


def fmt(s):
    return (f"n={s['n']} mean={s['mean']:.4f} median={s['median']:.4f} p95={s['p95']:.4f} "
            f"p99={s['p99']:.4f} max={s['max']:.4f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--a", required=True, type=Path)
    ap.add_argument("--b", type=Path)
    ap.add_argument("--data", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--scenes", nargs="+", default=["subset10k-sel"], choices=sorted(SCENES))
    ap.add_argument("--repeats", type=int, default=1, help="ABBA groups per scene")
    ap.add_argument("--frames", type=int, default=600)
    ap.add_argument("--warmup", type=int, default=120)
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=960)
    ap.add_argument("--video-driver", default="offscreen")
    ap.add_argument("--render-driver", default="opengl")
    ap.add_argument("--wrap", default="")
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--a-env", action="append", default=[])
    ap.add_argument("--b-env", action="append", default=[])
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    order = ["A", "B", "B", "A"] if args.b else ["A"]
    failed = False
    for scene in args.scenes:
        runs = []
        for g in range(args.repeats):
            for k, label in enumerate(order):
                r = run(args, args.a if label == "A" else args.b, label, scene, g * len(order) + k)
                runs.append(r)
                print(f"{r['name']}: hash={r['hash']} cursor_seq={r['cursor_seq']} "
                      f"cursor_ms {fmt(summary(r['cursor_ms']))} tests_mean={statistics.fmean(r['tests']):.1f}",
                      flush=True)
        hashes = {r["hash"] for r in runs}
        seqs = {r["cursor_seq"] for r in runs}
        print(f"== {scene}: world hashes {sorted(hashes)} cursor sequences {sorted(seqs)} cursors {runs[0]['cursors']}")
        if len(hashes) != 1 or len(seqs) != 1:
            print(f"!! {scene}: runs DIFFER (world hash or cursor sequence)")
            failed = True
        for label in sorted({r["label"] for r in runs}):
            mine = [r for r in runs if r["label"] == label]
            pool = lambda key: [v for r in mine for v in r[key]]
            print(f"   {label} cursor_ms  {fmt(summary(pool('cursor_ms')))}")
            print(f"   {label} frame_ms   {fmt(summary(pool('frame_ms')))}")
            print(f"   {label} draw_ms    {fmt(summary(pool('draw_ms')))}")
            print(f"   {label} present_ms {fmt(summary(pool('present_ms')))}  (includes the cursor overlay)")
            print(f"   {label} tests/frame mean={statistics.fmean(pool('tests')):.1f} "
                  f"run means cursor_ms={[round(statistics.fmean(r['cursor_ms']), 4) for r in mine]} "
                  f"frame_ms={[round(statistics.fmean(r['frame_ms']), 3) for r in mine]}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
