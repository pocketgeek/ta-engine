#!/usr/bin/env python3
"""legion_parallel_test -- stage 9's parallel planning decides nothing (Legion 10,000 stage 9, brief phase 8).

    legion_parallel_test.py LEGION_SCENARIO SCALE_DIR [--data DIR] [--quick]

Legion builds chain tiles as asynchronous jobs beside its serial field pass, and the cell layer of a plane's first
build in row chunks, on the SimPool (src/sim/legion.cpp tilePrefetch, computeRows). Both are exact by construction:
the jobs and their logical budgets are chosen from sim state before any job runs, each job reads immutable inputs of
its own and writes only its own slot, and the serial pass consumes the results by exact key, in its own order,
waiting for a job still running -- so the state hash and every observer key must equal the run without any of it,
whatever the worker count and whatever order the jobs finish in. Per fixture this runs:

  * the reference: --serial (no SimPool workers: nothing is built ahead, every tile is built in the serial pass);
  * --workers with TAK_SIM_WORKERS = 1, 2, 3, 7 and 63: every count the default rule picks (1, 2, 3), and the
    widest pool there is (SimPool::kMaxWorkers, the top of the SIMULATION THREADS setting and of takserver
    --sim-threads on a 64-thread machine), so far more workers than this machine has CPUs. Plain runs are the
    cheap arms (2 to 17 s a fixture; the perturbed and verifying arms below cost several times that), which is
    why the widths added for that setting, 2 and 63, are plain ones;
  * --workers with 3 and 7 workers and --par-perturb SEED (the jobs run in a shuffled order with random delays,
    so they complete in a different order on every run);
  * --serial --par-force --par-perturb SEED under TAK_LEGION_VERIFY (the jobs inline, shuffled; every staged tile
    the serial pass takes is rebuilt there and compared, and every derived index is checked each tick);

and checks: every run exits 0; every run's state hash and observer keys equal the reference's; and the parallel
paths really ran (NAVPAR on stderr): over the set, tiles were staged and taken (chains), and plane rows were built
in chunks. A run whose path ran nothing would prove nothing.

Exit 0 pass, 1 fail, 2 tooling, 77 skipped (no game data).
"""
import json
import os
import re
import subprocess
import sys

# (scale fixture, ticks): chains with many unrelated destinations; several movement classes (first plane builds of
# each, flyers and hovers); static churn (chain regions re-resolved on new geometry, plane refreshes) with re-clicks.
FIXTURES = (("scatter-2500", 2000), ("mixed-1000", 1200), ("churn-1000", 2000))
QUICK = (("scatter-2500", 900),)


def run(exe, path, data, ticks, workers, extra, env_extra):
    cmd = [exe, path, "--mode", "legion", "--offsets", "0", "--json", "--ticks", str(ticks)] + extra
    if data:
        cmd += ["--data", data]
    env = {k: v for k, v in os.environ.items() if not k.startswith("TAK_") or k == "TAK_SETTINGS_FILE"}
    env.pop("TAK_SETTINGS_FILE", None)
    env["TAK_SIM_WORKERS"] = str(workers)
    env.update(env_extra)
    p = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if p.returncode == 77:
        return None
    if p.returncode != 0 or not p.stdout.strip():
        raise RuntimeError(f"{' '.join(cmd)}: exit {p.returncode}\n{p.stderr[-2000:]}")
    d = json.loads(p.stdout.splitlines()[0])
    m = re.search(r"^NAVPAR .*$", p.stderr, re.M)
    par = dict(kv.split("=") for kv in m.group(0).split()[4:]) if m else {}
    return d, {k: float(v) if "." in v else int(v) for k, v in par.items()}


def main(argv):
    args = argv[1:]
    data = None
    if "--data" in args:
        i = args.index("--data")
        data = args[i + 1]
        del args[i:i + 2]
    quick = "--quick" in args
    args = [a for a in args if a != "--quick"]
    if len(args) != 2:
        print(__doc__)
        return 2
    exe, scale = args
    arms = (
        ("w1", 1, ["--workers"], {}),
        ("w2", 2, ["--workers"], {}),
        ("w3", 3, ["--workers"], {}),
        ("w7", 7, ["--workers"], {}),
        ("w63", 63, ["--workers"], {}),
        ("w3-perturb", 3, ["--workers", "--par-perturb", "11"], {}),
        ("w7-perturb", 7, ["--workers", "--par-perturb", "4242"], {}),
        ("serial-forced-perturb-verify", 0, ["--serial", "--par-force", "--par-perturb", "7"], {"TAK_LEGION_VERIFY": "1"}),
    )
    failures, staged, used, rows = [], 0, 0, 0
    for name, ticks in (QUICK if quick else FIXTURES):
        path = os.path.join(scale, name + ".scn")
        try:
            ref = run(exe, path, data, ticks, 0, ["--serial"], {})
            if ref is None:
                print("SKIP: no game data")
                return 77
            for arm, workers, extra, env in arms:
                got = run(exe, path, data, ticks, workers, extra, env)
                d, par = got
                staged += par.get("staged", 0)
                used += par.get("used", 0)
                rows += par.get("plane_rows", 0)
                same_hash = d["hash"] == ref[0]["hash"]
                # Under TAK_LEGION_VERIFY the navigator counts its own soft-hash checks (a verify-only work counter,
                # folded into work.legion_all): the one difference a verify run may show (section 7.2 of the record).
                verify_only = ("work.soft_hash_verify_ticks.", "work.legion_all.") if "TAK_LEGION_VERIFY" in env else ()
                diff = sorted(k for k in set(d["keys"]) | set(ref[0]["keys"])
                              if d["keys"].get(k) != ref[0]["keys"].get(k) and not k.startswith(verify_only))
                status = "ok" if same_hash and not diff else "DIFF"
                print(f"{name:14s} {arm:30s} hash {d['hash']['0']} {status} staged={par.get('staged', 0)} "
                      f"used={par.get('used', 0)} plane_rows={par.get('plane_rows', 0)}"
                      + (f" keys differ: {diff[:8]}" if diff else ""))
                if status != "ok":
                    failures.append(f"{name} {arm}: hash {d['hash']} vs {ref[0]['hash']}, {len(diff)} keys differ")
        except RuntimeError as e:
            print(f"TOOLING: {e}")
            return 2
    if staged == 0 or used == 0:
        failures.append(f"no tile was staged and taken over the set (staged {staged}, used {used}): the prefetch never ran")
    if rows == 0:
        failures.append("no plane rows were built in chunks over the set: the row split never ran")
    for f in failures:
        print("FAIL:", f)
    print("PASS" if not failures else "FAIL")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
