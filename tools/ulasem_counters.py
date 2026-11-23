#!/usr/bin/env python3
"""ulasem_counters -- the live 8-AI Ulasem benchmark's Legion counters, per unit-tick.

W4 gates B1/B2/B3 on counters of the live benchmark (PLAN 3.0, 4 W4): relaxations
(field_work) per unit-tick, refresh_idle, the field-quota peg run, churn per
1500-tick bin. This reads the LPROBE lines a run prints (TAK_LPROBE=100 in the
environment of takserver and of the headless takclient, debug build) and
reduces them; the counters are deterministic, so the referee's and the
client's lines are equal and any platform gives the same numbers.

  ulasem_counters.py LOG [LOG...]              print each log's reduction
  ulasem_counters.py --save OUT.json LOG...    ... and write {seed-keyed} JSON
                                               (the file's name stem, e.g. srv-s1-l1.log -> "s1")
  ulasem_counters.py --base BASE.json LOG...   ... and print the ratio to BASE per seed

Units: unit-ticks is the integral of the probe's `units` (World unit slots, dead
ones until compacted) over the run, trapezoid between probes 100 ticks apart.
Gauges (still_per_residue_max, quota_peg_run_max) are the run's final maxima;
bins are field_work and the legion work classes' deltas per 1500 ticks.

How the run is made (tools/desync-hunt.sh pattern; seeds 0/1/2):
  TAK_LPROBE=100 takserver --port P --data D --no-auth --local --seed S &
  TAK_HEADLESS=1 SDL_VIDEODRIVER=dummy TAK_MP_AIS=7 TAK_BENCH=4 TAK_LEGION=1 TAK_LPROBE=100 \\
      takclient game "Ulasem Arena" --data D --server 127.0.0.1 --serverport P --mphost --time 240
"""
import json
import os
import re
import sys

GAUGES = ("still_per_residue_max", "quota_peg_run_max")
# Counters reduced to per-unit-tick values.
RATE = ("field_work", "first_slot", "first_solo", "refresh_moving", "refresh_idle", "refresh_deferred",
        "lift_members_walked", "waiting_member_ticks", "still_units_processed", "anchor_walk_iters",
        "share_scan_iters", "sched_group_visits", "list_sizes", "group_ticks")
BIN = 1500


def parse(path):
    rows = []
    for line in open(path, encoding="utf-8", errors="replace"):
        if not line.startswith("LPROBE "):
            continue
        d = {}
        for tok in line.split()[1:]:
            k, _, v = tok.partition("=")
            try:
                d[k] = float(v) if "." in v else int(v)
            except ValueError:
                pass
        rows.append(d)
    return rows


def reduce_log(rows):
    if not rows:
        raise SystemExit("no LPROBE lines")
    unit_ticks = 0.0
    prev_t, prev_u = 0, 0
    for r in rows:
        unit_ticks += (r["tick"] - prev_t) * (r["units"] + prev_u) / 2.0
        prev_t, prev_u = r["tick"], r["units"]
    last = rows[-1]
    out = {"ticks": last["tick"], "units_max": max(r["units"] for r in rows), "unit_ticks": int(unit_ticks)}
    for k in GAUGES:
        out[k] = last.get(k, 0)
    for k in RATE:
        out[k] = last.get(k, 0)
        out[k + "_per_unit_tick"] = last.get(k, 0) / unit_ticks if unit_ticks else 0.0
    bins, prev = [], 0
    by_tick = {r["tick"]: r for r in rows}
    for t in range(BIN, int(last["tick"]) + 1, BIN):
        r = by_tick.get(t)
        if r is None:
            break
        bins.append(r["field_work"] - prev)
        prev = r["field_work"]
    out["field_work_bins"] = bins
    return out


def stem(path):
    m = re.search(r"-(s\d+)-", os.path.basename(path))
    return m.group(1) if m else os.path.basename(path)


def show(name, d, base=None):
    print("== %s: %d ticks, %d units max, %d unit-ticks" % (name, d["ticks"], d["units_max"], d["unit_ticks"]))
    for k in RATE:
        v = d[k + "_per_unit_tick"]
        line = "  %-24s %16d  %12.4f / unit-tick" % (k, d[k], v)
        if base and base.get(k + "_per_unit_tick"):
            line += "   x%.3f of base" % (v / base[k + "_per_unit_tick"])
        print(line)
    for k in GAUGES:
        print("  %-24s %16d" % (k, d[k]))
    b = d["field_work_bins"]
    if b:
        print("  field_work per %d-tick bin: %s  (max/min of bins 2..: %.2f)" % (
            BIN, " ".join(str(x) for x in b), max(b[1:]) / max(1, min(b[1:])) if len(b) > 2 else 1.0))


def main(argv):
    save = base = None
    logs = []
    it = iter(argv)
    for a in it:
        if a == "--save":
            save = next(it)
        elif a == "--base":
            base = json.load(open(next(it), encoding="utf-8"))
        else:
            logs.append(a)
    if not logs:
        print(__doc__)
        return 2
    res = {}
    for p in logs:
        d = reduce_log(parse(p))
        res[stem(p)] = d
        show(stem(p), d, (base or {}).get(stem(p)))
    if save:
        with open(save, "w", encoding="utf-8") as f:
            json.dump(res, f, indent=1, sort_keys=True)
            f.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
