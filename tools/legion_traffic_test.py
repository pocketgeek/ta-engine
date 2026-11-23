#!/usr/bin/env python3
"""legion_traffic_test -- crossing traffic as traffic (Legion 10,000 stage 7).

    legion_traffic_test.py LEGION_SCENARIO SCALE_DIR [--data DIR]

scatter-400 (tools/scenarios/scale/): 400 single walkers, each its own order, clicked over 600 ticks to unrelated far
destinations on a 192 x 192 map. Until stage 7 nothing that plans saw a standing jam -- the soft scan skipped every
Legion member on its way and awareness skips a standing crowd -- so crossing walkers kept walking into the knots they
formed and stood there (the stage-6 head: 304-326 of 400 arrive, 72-95 stuck at the end over offsets 0, 1, -1). A
member held kRestAfter ticks is now soft to other orders (jamStill) and a member held against another order's jam
may plan its short detour (blockedByJam). Checked, serial and workers (--both-exec: they must agree):

  * the run exits 0 and jammed members were stamped (scale.traffic.jam_stamps > 0);
  * at least 380 of the 400 arrive (scale.arrive.n) and at most 10 stand stuck at the end (scale.stuck_end).

Exit 0 pass, 1 fail, 2 tooling.
"""
import json
import os
import subprocess
import sys


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    exe, scale = sys.argv[1], sys.argv[2]
    data = sys.argv[sys.argv.index("--data") + 1] if "--data" in sys.argv else None
    cmd = [exe, os.path.join(scale, "scatter-400.scn"), "--mode", "legion", "--offsets", "0", "--both-exec", "--json",
           "--scale"]
    if data:
        cmd += ["--data", data]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode == 77:
        print("skipped (no data)")
        return 77
    if p.returncode != 0:
        print(p.stdout[-2000:], p.stderr[-4000:])
        print("FAIL: legion_scenario exited", p.returncode)
        return 1
    rows = [json.loads(ln) for ln in p.stdout.splitlines() if ln.startswith("{")]
    if not rows:
        print("FAIL: no JSON line")
        return 2
    k = rows[0]["keys"]
    arrived, stuck, jams = k.get("scale.arrive.n", -1), k.get("scale.stuck_end", 1 << 30), k.get("scale.traffic.jam_stamps", 0)
    print(f"scatter-400 arrive.n={arrived} stuck_end={stuck} jam_stamps={jams}")
    bad = []
    if jams <= 0:
        bad.append("no jammed member was stamped as traffic")
    if arrived < 380:
        bad.append(f"only {arrived} of 400 arrived (at least 380)")
    if stuck > 10:
        bad.append(f"{stuck} stuck at the end (at most 10)")
    for b in bad:
        print("FAIL:", b)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
