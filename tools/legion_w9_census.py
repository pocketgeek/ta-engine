#!/usr/bin/env python3
"""legion_w9_census -- the W9 predicate census as a gate (W9 plan, step 0).

`tools/scenarios/w9-census.json` holds, per scenario, the debug-build census counters (src/sim/legion.cpp struct Census:
how often each W9 mechanism's trigger WOULD fire) of a Legion run at offset 0, serial, taken on the W8 land head
(8fcc03d6) with `legion_scenario --mode legion --offsets 0 --serial --w9`. A W9 step may move only the scenarios its
own predicates fire on; a scenario it moves that the census excludes means the diagnosis was wrong: STOP AND
RE-DIAGNOSE (the plan's census rule; this is not a tuning round).

  legion_w9_census.py table [--census F]                  which scenarios each step's predicates fire on
  legion_w9_census.py moved STEP --base RUN.jsonl --cand RUN.jsonl [--census F]
        scenarios whose Legion state hash at offset 0 differs between two runs (legion_scenario --json lines,
        any offsets, the hash at offset 0 is read) and the census of STEP does not cover. Exit 1 if any.
  legion_w9_census.py controls [--census F]               the flowing controls (doubleturn, uturn, opentangent,
        wall-4x50) read 0 for the B2 / B3 fold / C1 predicates (exit 1 if not)

STEP is one of A1 A2 A3 A4 B1 B2 B3 B4 B6 C1.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT = os.path.join(HERE, "scenarios", "w9-census.json")
# The counters whose non-zero value says a step's trigger fires in a scenario.
STEPS = {
    "A1": ("a1_inside",),
    "A2": ("a2_seal",),
    "A3": ("a3_creep",),
    "A4": ("a4_release_slotted",),
    "B1": ("b1_engaged_wall", "b1_center_blocked"),
    "B2": ("b2_lane_differs", "b2_margin", "a4_release_crossed"),
    "B3": ("b3_fold_binds", "b3_guard_rejects"),
    "B4": ("b4_clear_alt",),
    "B6": ("b6_blocked",),
    "C1": ("c1_headon_entries", "c1_unseen"),
}
CONTROLS = ("doubleturn", "uturn", "opentangent", "wall-4x50")
CONTROL_KEYS = ("b2_commits", "b2_lane_differs", "b3_fold_binds", "c1_headon_entries", "c1_unseen")


def load_census(path):
    with open(path) as f:
        return json.load(f)


def covered(census, step):
    """Scenarios whose census counters for `step` are non-zero."""
    return {n for n, c in census.items() if isinstance(c, dict) and any(c.get(k) for k in STEPS[step])}


def hashes(paths):
    out = {}
    for p in paths:
        for ln in open(p):
            ln = ln.strip()
            if not ln.startswith("{"):
                continue
            r = json.loads(ln)
            if r.get("mode") == "legion" and "hash" in r:
                out[r["scenario"]] = r["hash"].get("0")
    return out


def moved(census, step, base, cand):
    cov = covered(census, step)
    return sorted(n for n in set(base) & set(cand) if base[n] != cand[n] and n not in cov)


def controls_clean(census):
    bad = []
    for n in CONTROLS:
        c = census.get(n)
        if not isinstance(c, dict):
            bad.append("%s: no census" % n)
            continue
        bad += ["%s: %s = %d" % (n, k, c[k]) for k in CONTROL_KEYS if c.get(k)]
    return bad


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=("table", "moved", "controls"))
    ap.add_argument("step", nargs="?")
    ap.add_argument("--base", action="append", default=[])
    ap.add_argument("--cand", action="append", default=[])
    ap.add_argument("--census", default=DEFAULT)
    a = ap.parse_args(argv)
    census = load_census(a.census)
    if a.cmd == "table":
        for s in STEPS:
            cov = sorted(covered(census, s))
            print("%s (%s): %d scenarios: %s" % (s, ", ".join(STEPS[s]), len(cov), ", ".join(cov)))
        return 0
    if a.cmd == "controls":
        bad = controls_clean(census)
        print("\n".join(bad) if bad else "the flowing controls read 0 for the B2 / B3 fold / C1 predicates")
        return 1 if bad else 0
    if a.step not in STEPS or not a.base or not a.cand:
        ap.error("moved STEP --base RUN.jsonl --cand RUN.jsonl")
    out = moved(census, a.step, hashes(a.base), hashes(a.cand))
    if out:
        print("STOP AND RE-DIAGNOSE: %s moved scenarios its census excludes: %s" % (a.step, ", ".join(out)))
        return 1
    print("%s: every moved scenario is one its census covers" % a.step)
    return 0


if __name__ == "__main__":
    sys.exit(main())
