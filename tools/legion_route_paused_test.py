#!/usr/bin/env python3
"""legion_route_paused_test -- the route consumers on paused routes (Legion 10,000 stage 5).

    legion_route_paused_test.py LEGION_SCENARIO SCENARIO_DIR [--data DIR]

The game pauses only some first builds (solo groups; docs/legion-pathfinding.md "Paused first builds"), so the
consumers that state their region -- the lanes (passage gate, passage lanes, pinwheel), packed slots, reach rings,
the formation entrance, awareness -- rarely meet a paused route in the gate fixtures. This runs fixtures that
exercise each of them with --pause-all (LegionNavigator::setPauseAllForTest: every first build may pause), serial
and workers, under TAK_LEGION_VERIFY when the caller sets it (the ctest does), and checks:

  * every run exits 0 (no verify failure: indexes, the needs list, "a relaxation could lower a settled potential")
    and serial == workers;
  * the consumers really ran on paused routes: over the set, the lanes, awareness and the entrance asked a paused
    build for their region (scale.route.needs.*), and a reach group's first build paused with its ring's spots
    settled (attackring-mob-100: work.fields_paused);
  * no route had to run on to done for an uncertain read (scale.route.run_on.* 0 over the set: every read was
    certain where the consumer acted), and outcomes stay close to the normal run (each fixture's arrivals within
    10% of the run without --pause-all, at least -2 bodies).

Exit 0 pass, 1 fail, 2 tooling.
"""
import json
import os
import subprocess
import sys

FIXTURES = ("gap8", "gap14", "corner-4x50", "wall-4x50", "attackring-mob-100", "aware-cross", "doorplug-112",
            "motion-cross", "crosslong")


def run(exe, path, data, pause):
    cmd = [exe, path, "--mode", "legion", "--offsets", "0", "--both-exec", "--json", "--scale"]
    if data:
        cmd += ["--data", data]
    if pause:
        cmd.append("--pause-all")
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode == 77:
        return None, "skipped"
    if p.returncode != 0:
        return None, "exit %d: %s" % (p.returncode, p.stderr[-2000:])
    try:
        return json.loads(p.stdout.strip().splitlines()[-1]), None
    except Exception as e:
        return None, "no JSON line: %s" % e


def arrivals(keys):
    return sum(v for k, v in keys.items() if k.startswith("g.") and k.endswith(".arrived"))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    exe, scn = sys.argv[1], sys.argv[2]
    data = sys.argv[sys.argv.index("--data") + 1] if "--data" in sys.argv else os.environ.get("TAK_TEST_DATA", "")
    fails, needs, run_on, paused = [], {}, {}, {}
    for name in FIXTURES:
        path = os.path.join(scn, name + ".scn")
        rec, err = run(exe, path, data, True)
        if err == "skipped":
            print("  %-22s skipped (needs --data)" % name)
            continue
        if err:
            fails.append("%s: %s" % (name, err))
            continue
        base, err = run(exe, path, data, False)
        if err:
            fails.append("%s (normal run): %s" % (name, err))
            continue
        k = rec["keys"]
        if rec.get("serial_eq_workers") is not True:
            fails.append("%s: serial != workers" % name)
        for key, v in k.items():
            if key.startswith("scale.route.needs."):
                needs[key[18:]] = needs.get(key[18:], 0) + v
            if key.startswith("scale.route.run_on."):
                run_on[key[19:]] = run_on.get(key[19:], 0) + v
        paused[name] = k.get("work.fields_paused.total", 0)
        a, b = arrivals(k), arrivals(base["keys"])
        ok = a >= min(b - 2, b * 9 // 10)
        print("  %-22s paused %-5s arrivals %d (normal %d) %s" % (name, paused[name], a, b, "ok" if ok else "FAIL"))
        if not ok:
            fails.append("%s: arrivals %d against %d without --pause-all" % (name, a, b))
    print("  needs over the set:", needs, " run-ons:", run_on)
    for consumer in ("lanes", "awareness", "entrance"):
        if not needs.get(consumer):
            fails.append("the %s never asked a paused build for its region" % consumer)
    if "attackring-mob-100" in paused and not paused["attackring-mob-100"]:
        fails.append("attackring-mob-100's reach group never paused")
    if any(run_on.values()):
        fails.append("a route ran on to done for an uncertain read: %s" % run_on)
    for f in fails:
        print("FAIL", f)
    print("legion_route_paused_test:", "FAIL" if fails else "ok")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
