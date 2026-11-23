#!/usr/bin/env python3
"""legion_choke_test -- chokepoints as queues: route choice and admission (Legion 10,000 stage 8).

    legion_choke_test.py LEGION_SCENARIO SCALE_DIR [--data DIR]

Two scale files (tools/scenarios/scale/), workers exec, offset 0:

  * bridges-1000: two allied armies of 500 cross a river in opposite directions over bridges 4, 12 and 32 cells
    wide. Until stage 8 both took the nearest (12-cell) bridge and met head on on it: 5 of 1,000 arrived, 547
    stuck at the end. Openings (narrow crossings of sector boundaries) now carry their queues: routes are charged
    travel time plus queue delay (an opposing stream twice on a narrow passage), groups re-plan one at a time when
    the queues would move their choice, and a narrow passage admits one direction at a time. Checked: at least 450
    arrive, at most 80 stand stuck at the end, and both the 12-cell and the 32-cell bridge carry at least 30
    crossings each (scale.arrive.n, scale.stuck_end, gate.bridge1/2.crossings; 40 until protocol 280).
  * maze-1000: 850 bodies to a walled compound's central room through 6-cell doors. Until stage 8 every body queued
    at the west door (632 of 1,000 arrive in 20,000 ticks, 92 stuck); the queue's delay now sends groups round to
    the other doors, and a big group gives squares of its members to a subgroup for another door. Checked: at
    least 850 arrive and at most 10 stand stuck at the end; an opening was charged (scale.choke.charged_fields > 0)
    and a subgroup split off (scale.choke.splits > 0).

Exit 0 pass, 1 fail, 2 tooling, 77 skipped (no data).
"""
import json
import os
import subprocess
import sys


def run(exe, scn, data):
    cmd = [exe, scn, "--mode", "legion", "--offsets", "0", "--workers", "--json", "--scale"]
    if data:
        cmd += ["--data", data]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode == 77:
        return 77, None
    if p.returncode != 0:
        print(p.stdout[-2000:], p.stderr[-4000:])
        return 1, None
    rows = [json.loads(ln) for ln in p.stdout.splitlines() if ln.startswith("{")]
    return (0, rows[0]["keys"]) if rows else (2, None)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    exe, scale = sys.argv[1], sys.argv[2]
    data = sys.argv[sys.argv.index("--data") + 1] if "--data" in sys.argv else None
    bad = []
    rc, k = run(exe, os.path.join(scale, "bridges-1000.scn"), data)
    if rc:
        print("legion_scenario failed on bridges-1000" if rc != 77 else "skipped (no data)")
        return rc
    arrived, stuck = k.get("scale.arrive.n", -1), k.get("scale.stuck_end", 1 << 30)
    b1, b2 = k.get("gate.bridge1.crossings", 0), k.get("gate.bridge2.crossings", 0)
    print(f"bridges-1000 arrive.n={arrived} stuck_end={stuck} bridge1={b1} bridge2={b2}")
    if arrived < 450:
        bad.append(f"bridges-1000: only {arrived} of 1000 arrived (at least 450)")
    if stuck > 80:
        bad.append(f"bridges-1000: {stuck} stuck at the end (at most 80)")
    # Protocol 280 (2026-10-19, flow-around shuffles at travel speed): the 32-cell bridge drains faster, so the
    # queue sends fewer bodies to the 12-cell one (offset 0: 39 crossings and 220, 843 arrive, none stuck); the
    # bound was 40 each.
    if b1 < 30 or b2 < 30:
        bad.append(f"bridges-1000: the 12- and 32-cell bridges carried {b1} and {b2} crossings (at least 30 each)")
    rc, k = run(exe, os.path.join(scale, "maze-1000.scn"), data)
    if rc:
        print("legion_scenario failed on maze-1000")
        return rc
    arrived, stuck, charged = k.get("scale.arrive.n", -1), k.get("scale.stuck_end", 1 << 30), k.get("scale.choke.charged_fields", 0)
    splits = k.get("scale.choke.splits", 0)
    print(f"maze-1000 arrive.n={arrived} stuck_end={stuck} charged_fields={charged} splits={splits}")
    if arrived < 850:
        bad.append(f"maze-1000: only {arrived} of 1000 arrived (at least 850)")
    if stuck > 10:
        bad.append(f"maze-1000: {stuck} stuck at the end (at most 10)")
    if charged <= 0:
        bad.append("maze-1000: no route was charged for a queue")
    if splits <= 0:
        bad.append("maze-1000: no big group gave part of itself to another door (subgroup allocation)")
    for b in bad:
        print("FAIL:", b)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
