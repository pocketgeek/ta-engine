#!/usr/bin/env python3
"""legion_scale_check -- one scale file, offset 0, workers exec, against numeric bounds on its JSON keys.

    legion_scale_check.py LEGION_SCENARIO SCALE_DIR FILE [--data DIR] [--offsets LIST] [--min KEY=V]... [--max KEY=V]...

FILE is a name in SCALE_DIR (without .scn). Each --min / --max bounds one key of the run's JSON line (scale.*,
gate.*, or any observer key); a missing key fails. With --offsets (legion_scenario's list, default 0) every
offset's value is bounded, not the median. The CMake tests that use it say why each bound is there (Legion
10,000 stage 8: shared destination areas, awareness by streams). Exit 0 pass, 1 fail, 2 tooling, 77 skipped.
"""
import json
import os
import subprocess
import sys


def main():
    a = sys.argv[1:]
    if len(a) < 3:
        print(__doc__)
        return 2
    exe, scale, name = a[0], a[1], a[2]
    data, offsets, lo, hi = None, "0", [], []
    i = 3
    while i < len(a):
        if a[i] == "--data":
            data = a[i + 1]
        elif a[i] == "--offsets":
            offsets = a[i + 1]
        elif a[i] in ("--min", "--max"):
            k, v = a[i + 1].split("=")
            (lo if a[i] == "--min" else hi).append((k, float(v)))
        else:
            print("unknown argument", a[i])
            return 2
        i += 2
    cmd = [exe, os.path.join(scale, name + ".scn"), "--mode", "legion", "--offsets", offsets, "--workers", "--json", "--scale"]
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
    k, var = rows[0]["keys"], rows[0].get("varying", {})
    bad = []
    for key, v in lo:
        got = min(var[key]) if key in var else k.get(key)
        print(f"{name} {key}={got} (at least {v:g})")
        if got is None or got < v:
            bad.append(f"{key}={got}, at least {v:g}")
    for key, v in hi:
        got = max(var[key]) if key in var else k.get(key)
        print(f"{name} {key}={got} (at most {v:g})")
        if got is None or got > v:
            bad.append(f"{key}={got}, at most {v:g}")
    for b in bad:
        print("FAIL:", name, b)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
