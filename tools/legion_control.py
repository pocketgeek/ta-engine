#!/usr/bin/env python3
"""legion_control -- the W8 lane control: the same army as eight selections and as one.

  legion_control.py [--runner BIN] [--a corner-8x56] [--b corner-1x448] [--offsets 0,1,-1,2,-2]
                    [--mode legion] [--jobs 4] [--cores 4-23] [--json]

Runs two scenario files that put the SAME bodies on the SAME map and differ only in how the click reaches
the navigator: corner-8x56 issues eight selections of 56 on ticks 1-8 (the legion-r9 61923 shape, one Point
per selection until W3's merge) and corner-1x448 issues one selection of 448 (issueSelection lands it as
seven parts of 64 on ticks 1-7). The plan (T2 3.2 Gates, C14) asks that, under S1, 8x56 is no worse than its
own base and that the 1x448 loss is not reproduced on 8x56: this prints both arms side by side per offset and
as medians, with the 8x56 / 1x448 ratio of every count, so a W8 step can be judged on the control without
a hand-made table.

Per arm and offset the keys are aggregated over the scenario's groups: arrived (sum), t90 and done (the
slowest group, -1 when a group never gets there), and the global counters below. Deterministic keys only;
wall time is not printed.
"""
import argparse
import json
import os
import re
import statistics
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEYS = ["arrived", "t90", "done", "reversals", "spins", "stop_go", "stopped_permille", "wall_touch_permille",
        "contact_own_permille", "gate.top.files_x100", "gate.top.crossings", "side.east.t90",
        "lanes.pivot_work", "work.legion_total.total"]


def run(runner, scn, mode, offset, cores):
    cmd = [runner, scn, "--mode", mode, "--offsets", str(offset), "--serial", "--json"]
    if cores:
        cmd = ["taskset", "-c", cores] + cmd
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit("legion_control: %s offset %s failed (%d): %s" % (scn, offset, p.returncode, p.stderr[-300:]))
    for line in p.stdout.splitlines():
        if line.startswith("{"):
            d = json.loads(line)
            if d.get("mode") == mode:
                return d
    raise SystemExit("legion_control: no %s result for %s offset %s" % (mode, scn, offset))


def aggregate(d):
    k = d["keys"]
    out = {key: k.get(key) for key in KEYS if key not in ("arrived", "t90", "done")}
    arrived, t90, done = 0, [], []
    for key, v in k.items():
        m = re.match(r"^g\.([A-Za-z0-9_]+)\.(arrived|t90|done)$", key)
        if not m:
            continue
        if m.group(2) == "arrived":
            arrived += v
        elif m.group(2) == "t90":
            t90.append(v)
        else:
            done.append(v)
    out["arrived"] = arrived
    out["t90"] = -1 if not t90 or min(t90) < 0 else max(t90)
    out["done"] = -1 if not done or min(done) < 0 else max(done)
    out["hash"] = d["hash"]
    return out


def med(vals):
    vals = [v for v in vals if v is not None]
    if not vals:
        return None
    if any(v < 0 for v in vals if isinstance(v, int)) and all(isinstance(v, int) for v in vals):
        # a never counts as infinitely late: the median is -1 only when most arms never got there
        vals = [10 ** 9 if v < 0 else v for v in vals]
        m = statistics.median_low(vals)
        return -1 if m >= 10 ** 9 else m
    return statistics.median_low(vals)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runner", default=None, help="legion_scenario binary (default: build-o2/legion_scenario)")
    ap.add_argument("--a", default="corner-8x56")
    ap.add_argument("--b", default="corner-1x448")
    ap.add_argument("--offsets", default="0,1,-1,2,-2")
    ap.add_argument("--mode", default="legion", choices=["legion", "retail"])
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--cores", default="", help="taskset cpu list for the runs (default: none)")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    runner = a.runner or os.path.join(ROOT, "build-o2", "legion_scenario")
    offsets = [int(x) for x in a.offsets.split(",")]
    scn = {n: os.path.join(ROOT, "tools", "scenarios", n + ".scn") for n in (a.a, a.b)}
    jobs = [(n, o) for n in (a.a, a.b) for o in offsets]
    with ThreadPoolExecutor(max_workers=a.jobs) as ex:
        res = list(ex.map(lambda j: (j, aggregate(run(runner, scn[j[0]], a.mode, j[1], a.cores))), jobs))
    table = {}
    for (n, o), r in res:
        table.setdefault(n, {})[o] = r
    summary = {n: {key: med([table[n][o][key] for o in offsets]) for key in KEYS} for n in (a.a, a.b)}
    if a.json:
        print(json.dumps({"offsets": offsets, "mode": a.mode, "per_offset": table, "median": summary}, indent=1, sort_keys=True))
        return
    print("legion_control %s: %s vs %s, offsets %s (median_low over offsets; -1 = never)" % (a.mode, a.a, a.b, a.offsets))
    print("%-26s %12s %12s %8s   per offset (%s)" % ("key", a.a, a.b, "a/b", ", ".join(str(o) for o in offsets)))
    for key in KEYS:
        x, y = summary[a.a][key], summary[a.b][key]
        ratio = "%.2f" % (x / y) if isinstance(x, (int, float)) and isinstance(y, (int, float)) and y > 0 and x >= 0 else "-"
        per = " ".join("%s/%s" % (table[a.a][o][key], table[a.b][o][key]) for o in offsets)
        print("%-26s %12s %12s %8s   %s" % (key, x, y, ratio, per))


if __name__ == "__main__":
    sys.exit(main())
