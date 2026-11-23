#!/usr/bin/env python3
"""legion_arrival_test -- a formation member gaining on its own slot is not still (Legion 10,000 stage 8).

    legion_arrival_test.py LEGION_SCENARIO SCALE_DIR [--data DIR]

one-1000 (tools/scenarios/scale/): one 1,000-body selection clicked across open ground. A formation slot can lie
beyond the click (the front of the army takes the far side of the area). Until stage 8 the crowd window measured a
body's headway only on the click: a body walking past the click toward its slot gained nothing on it, so the first
window found it "still", queued back to its own crowd behind it and pressed, and it settled 20-60 cells short of
its slot; the area filled from the click backwards and the last ~10% of the army stood outside it (the stage-7
head: 844-893 of 1,000 arrive, 78-110 stuck at the end over offsets 0, 1, -1). A member whose distance to its own
slot fell by half a body or more below its best so far is not still (Member::windowSlotDist). Checked, workers exec:

  * the run exits 0;
  * at least 960 of the 1,000 arrive (scale.arrive.n), at most 30 stand stuck at the end (scale.stuck_end), and
    the 90th percentile arrives (scale.arrive.t90 > 0).

Exit 0 pass, 1 fail, 2 tooling, 77 skipped (no data).
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
    cmd = [exe, os.path.join(scale, "one-1000.scn"), "--mode", "legion", "--offsets", "0", "--workers", "--json", "--scale"]
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
    arrived, stuck, t90 = k.get("scale.arrive.n", -1), k.get("scale.stuck_end", 1 << 30), k.get("scale.arrive.t90", -1)
    print(f"one-1000 arrive.n={arrived} stuck_end={stuck} arrive.t90={t90}")
    bad = []
    if arrived < 960:
        bad.append(f"only {arrived} of 1000 arrived (at least 960)")
    if stuck > 30:
        bad.append(f"{stuck} stuck at the end (at most 30)")
    if t90 <= 0:
        bad.append("the 90th percentile never arrived")
    for b in bad:
        print("FAIL:", b)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
