#!/usr/bin/env python3
"""legion_unreachable_test -- the AR-11 orders-complete probe on crowdbench's `unreachable` row.

  legion_unreachable_test.py CROWDBENCH [--units 200] [--ticks 10000] [--require-w2]

Runs `crowdbench --scenario unreachable` (every unit gets its own goal across a full-height
wall, so each walks to its nearest reachable spot, its approach point) in Legion and Retail
and prints, per unit class, when units reached that point and when their orders completed:

  approach_arrived          units Legion held at their approach point (member state 5)
  approach_arrive_tick_*    the tick they arrived (p50/p95/max over all movers; -1 = not all)
  orders_retired_tick_*     the tick their orders emptied
  approach_wait_max         worst (order completed - arrived) among units that did both

Invariants asserted always: the run is deterministic (a second run prints the same row),
every order completes within the horizon in both modes (an order never hangs for ever), and
Legion never completes an order before the unit arrived (approach_wait_max >= 0).

--require-w2 additionally asserts the W2 step 2 acceptance (PLAN section 0 row 11, 3.4 AR-11):
each order completes as soon as its unit reaches its spot, approach_wait_max <= 8 ticks, every
unit gets there (a unit held behind its own army's stopped bodies has reached the nearest spot
IT can reach: the holder rule), and every order has completed by tick 3000 (3.4: "0 -> 200 by
3000"). On the W2 step-0 head the wait is the whole kTrappedRetire (9000 from the order) and
only about half the units ever reach their point; with the arrival retire alone (step 2a) the
units queued behind still keep their orders to the 9000-tick age test. ctest runs it with
--require-w2 from step 2c on.
"""
import argparse
import json
import subprocess
import sys


def run(binary, mode, units, ticks):
    cmd = [binary, "--mode", mode, "--scenario", "unreachable", "--units", str(units), "--players", "1",
           "--moving-percent", "100", "--ticks", str(ticks), "--seed", "0"]
    out = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    row = json.loads(next(l for l in reversed(out.splitlines()) if l.startswith("{")))
    keep = {k: v for k, v in row.items()
            if k.startswith(("approach_", "orders_", "hash", "alive")) and "wall_ms" not in k}
    return keep


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("crowdbench")
    ap.add_argument("--units", type=int, default=200)
    ap.add_argument("--ticks", type=int, default=10000)
    ap.add_argument("--require-w2", action="store_true")
    args = ap.parse_args()
    bad = []
    rows = {}
    for mode in ("legion", "retail"):
        rows[mode] = run(args.crowdbench, mode, args.units, args.ticks)
        print("unreachable %dx1 %s %s" % (args.units, mode, json.dumps(rows[mode], sort_keys=True)))
        if run(args.crowdbench, mode, args.units, args.ticks) != rows[mode]:
            bad.append("%s: a second run differs" % mode)
        if rows[mode]["orders_complete"] != args.units:
            bad.append("%s: %d of %d orders never completed in %d ticks"
                       % (mode, rows[mode]["orders_complete"], args.units, args.ticks))
    leg = rows["legion"]
    if leg["approach_arrived"] and leg["approach_wait_max"] < 0:
        bad.append("legion: an order completed before its unit reached its approach point")
    if args.require_w2 and not (0 <= leg["approach_wait_max"] <= 8 and leg["approach_arrived"] == args.units
                                and 0 <= leg["orders_retired_tick_max"] <= 3000):
        bad.append("W2 AR-11: approach_wait_max %d (want <= 8), arrived %d of %d, last order completed %d (want <= 3000)"
                   % (leg["approach_wait_max"], leg["approach_arrived"], args.units, leg["orders_retired_tick_max"]))
    for b in bad:
        print("FAIL " + b)
    print("PASS" if not bad else "FAIL")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
