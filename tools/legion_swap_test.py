#!/usr/bin/env python3
"""legion_swap_test -- allies swap places (Legion round 2, item 3).

    legion_swap_test.py LEGION_SCENARIO SCALE_DIR [--data DIR]

bridges-1000 (tools/scenarios/scale/): two allied armies of 500 cross a river in opposite directions over bridges 4,
12 and 32 cells wide. At offset 0 the queues keep the streams apart, but at offsets -2, -1, 1 and 2 two bodies of the
opposing streams met head on in a one-body lane, neither had a way round, and the bridge gridlocked: 641 / 643 / 741
/ 558 of 1,000 arrived with 208 / 255 / 182 / 279 stuck at the end (main 793510f3). Two allied members blocking each
other head on now trade places in one step (swapPartner / swapBodies): 999 / 979 / 975 / 909 arrive, none stuck.
Checked at each of those offsets: at least 850 arrive, at most 10 stand stuck at the end, and the swap happened
(scale.arrive.n, scale.stuck_end, work.swaps.total).

Exit 0 pass, 1 fail, 2 tooling, 77 skipped (no data).
"""
import json
import os
import subprocess
import sys

OFFSETS = (-2, -1, 1, 2)


def run(exe, scn, offset, data):
    cmd = [exe, scn, "--mode", "legion", "--offsets", str(offset), "--workers", "--json", "--scale"]
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
    for off in OFFSETS:
        rc, k = run(exe, os.path.join(scale, "bridges-1000.scn"), off, data)
        if rc:
            print(f"legion_scenario failed on bridges-1000 at offset {off}" if rc != 77 else "skipped (no data)")
            return rc
        arrived, stuck = k.get("scale.arrive.n", -1), k.get("scale.stuck_end", 1 << 30)
        swaps = k.get("work.swaps.total", 0)
        print(f"bridges-1000@{off} arrive.n={arrived} stuck_end={stuck} swaps={swaps}")
        if arrived < 850:
            bad.append(f"bridges-1000@{off}: only {arrived} of 1000 arrived (at least 850)")
        if stuck > 10:
            bad.append(f"bridges-1000@{off}: {stuck} stuck at the end (at most 10)")
        if swaps <= 0:
            bad.append(f"bridges-1000@{off}: no allies traded places")
    for b in bad:
        print("FAIL:", b)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
