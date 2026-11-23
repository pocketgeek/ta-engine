#!/usr/bin/env python3
"""Interleaved A/B rounds of ai_revive_bench (Controller::reviveIdle).

  tools/ai_revive_bench.py --base <build-o2> --cand <build-o2> [--rounds 3] [--calls 1000]

Runs every workload below once per build per round, alternating which build
goes first, and prints per workload and build the median over rounds of each
run's mean/median/p95/p99 call time and the largest max. The command count and
stream hash must agree between the builds (the script fails otherwise). Run it
on the timing cores, e.g. under /home/pocket_geek/tak-tmp/tools/timing.sh.
"""
import argparse
import re
import statistics
import subprocess
import sys

WORKLOADS = [
    ("zero: 64 casters, no body", ["--bodies", "0"]),
    ("few: 64 casters, 16 bodies", ["--bodies", "16"]),
    ("near: 64 casters, 300 bodies by them", ["--bodies", "300", "--layout", "near"]),
    ("many near: 64 casters, 2000 bodies", ["--bodies", "2000", "--layout", "near"]),
    ("many: 64 casters, 2000 bodies anywhere", ["--bodies", "2000"]),
    ("small: 4 casters, 200 bodies by them", ["--casters", "4", "--bodies", "200", "--layout", "near"]),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--cand", required=True)
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--calls", type=int, default=1000)
    ap.add_argument("--warm", type=int, default=100)
    a = ap.parse_args()
    builds = {"base": a.base, "cand": a.cand}
    rows = {}
    ok = True
    for r in range(a.rounds):
        for name, args in WORKLOADS:
            order = ["base", "cand"] if r % 2 == 0 else ["cand", "base"]
            for role in order:
                out = subprocess.run([builds[role] + "/ai_revive_bench", *args, "--calls", str(a.calls),
                                      "--warm", str(a.warm)], check=True, capture_output=True, text=True).stdout
                kv = dict(re.findall(r"(\w+)=(\S+)", out))
                rows.setdefault((name, role), []).append(kv)
    for name, _ in WORKLOADS:
        streams = {(kv["commands"], kv["stream"]) for role in builds for kv in rows[(name, role)]}
        same = len(streams) == 1
        ok = ok and same
        print(f"{name}  records={rows[(name, 'base')][0]['records']} commands={next(iter(streams))[0]} "
              f"stream {'SAME' if same else 'DIFF ' + str(streams)}")
        med = {}
        for role in builds:
            rs = rows[(name, role)]
            m = {k: statistics.median(float(kv[k + "_us"]) for kv in rs) for k in ("mean", "median", "p95", "p99")}
            med[role] = m
            print(f"  {role}: n={len(rs)}x{a.calls} mean={m['mean']:.2f} median={m['median']:.2f} "
                  f"p95={m['p95']:.2f} p99={m['p99']:.2f} max={max(float(kv['max_us']) for kv in rs):.2f} us")
        print(f"  cand/base mean {med['cand']['mean'] / med['base']['mean']:.4f}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
