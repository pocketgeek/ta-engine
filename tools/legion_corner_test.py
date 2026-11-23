#!/usr/bin/env python3
"""legion_corner_test -- corner lanes (2026-11-06, protocol 313).

    legion_corner_test.py LEGION_SCENARIO SCENARIO_DIR

corridor-corner-40 (asset-free): 40 2x2 bodies walk down an 8-cell corridor and turn into a second one. Before
corner lanes every body cut the inner corner and walked its straight line from there, so the second corridor
carried a file or two along its inner wall (main d3d6b55a, offsets 0, +-1, +-2, +-3: gate.leg2.files_x100 222..262,
t90 1,409..1,536, median 1,507). A member in a corridor whose way turns into another now commits to its lane counted
from the inner wall and keeps it down the new corridor (cornerCommit / cornerAdvance): 343..387 files, t90
1,266..1,372, median 1,311. Checked: at least 3.0 files past the corner at every offset, and a median t90 of at most
1,400 ticks.

Exit 0 pass, 1 fail, 2 tooling.
"""
import json
import os
import subprocess
import sys

OFFSETS = "0,1,-1,2,-2,3,-3"


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    exe, scenarios = sys.argv[1], sys.argv[2]
    scn = os.path.join(scenarios, "corridor-corner-40.scn")
    p = subprocess.run([exe, scn, "--mode", "legion", "--offsets", OFFSETS, "--serial", "--json"],
                       capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout[-2000:], p.stderr[-4000:])
        return 2
    rows = [json.loads(ln) for ln in p.stdout.splitlines() if ln.startswith("{")]
    if not rows:
        return 2
    keys, varying = rows[0]["keys"], rows[0].get("varying", {})
    offsets = rows[0]["offsets"]
    files = varying.get("gate.leg2.files_x100", [keys["gate.leg2.files_x100"]] * len(offsets))
    t90 = keys["g.a.t90"]
    print(f"corridor-corner-40 leg2 files_x100 {files} t90 median {t90}")
    bad = []
    for off, f in zip(offsets, files):
        if f < 300:
            bad.append(f"offset {off}: {f / 100:.2f} files past the corner (at least 3.0)")
    if t90 < 0 or t90 > 1400:
        bad.append(f"median t90 {t90} (at most 1,400)")
    for b in bad:
        print("FAIL:", b)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
