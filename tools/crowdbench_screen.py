#!/usr/bin/env python3
"""Nightly crowdbench screen: deterministic keys only, diffed against a baseline.

Cases (LEGION-PLAN 3.8, T8-synthesis (9)): every crowdbench scenario except
churn (18) x both modes x {200x1, 500x4}, plus 2000x1 for six scenarios and
churn at 2000x1 -- 86 runs, all on ONE binary, so the Retail rows are a re-run
of Retail on the same build, never a frozen file.

Each run keeps only the keys that are a pure function of the simulation (the
state hash, outcomes, counters); wall-clock, memory, allocation and build keys
are dropped, so two runs of the same binary write byte-identical JSONL whatever
-j, the CPU list or the machine load. The rows are written in case order.

  crowdbench_screen.py --binary build-rel/crowdbench --output screen.jsonl \\
      [--baseline base.jsonl] [--anchor anchor.jsonl] [-j 4] [--cpus 16-19] \\
      [--turn-rate N] [--ticks 6000] [--seeds 0]

Exit status: 0 when every case ran and (with --baseline) every baseline case
and key is unchanged; 1 on any difference from the baseline or any failed run.
Keys new in this output are listed but are not a difference (a new key cannot
regress anything) unless --strict-keys. The anchor (the frozen audit-era
screen) is a report of how far the head has moved; it changes the exit status
only with --anchor-fail.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import time

MATRIX_PATH = Path(__file__).with_name("crowdbench_matrix.py")
_SPEC = importlib.util.spec_from_file_location("crowdbench_matrix", MATRIX_PATH)
matrix = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(matrix)

MODES = ("retail", "legion")
# The 18 matrix scenarios (13 + 5 acceptance). maze stays in (C26: three
# themes gate on it).
SCREEN_SCENARIOS = matrix.SCENARIOS + matrix.ACCEPTANCE_SCENARIOS
POPULATIONS = ((200, 1), (500, 4))
LARGE_SCENARIOS = ("doors", "maze", "opposingcolumns", "sharedgoal", "jagged", "singleunit")
LARGE_POPULATION = (2000, 1)
CHURN_POPULATION = (2000, 1)
DEFAULT_TICKS = 6000

# Keys that are not a function of the simulation alone.
NONDETERMINISTIC_PREFIXES = ("tick_ms_", "build_", "tick_cpp_allocation_")
NONDETERMINISTIC_SUFFIXES = ("_ms", "_ns", "_bytes", "_kib")
NONDETERMINISTIC_KEYS = {"allocation_counting", "path_profiling"}
CASE_FIELDS = ("scenario", "units_per_player", "players", "mode", "seed", "ticks", "turn_rate")


def deterministic(row):
    """The row without wall-clock, memory, build or profiling keys."""
    return {key: value for key, value in row.items()
            if key not in NONDETERMINISTIC_KEYS and not key.startswith(NONDETERMINISTIC_PREFIXES)
            and not key.endswith(NONDETERMINISTIC_SUFFIXES) and "wall_ms" not in key}


def case_id(scenario, units, players, mode, seed, ticks, turn_rate):
    rate = "" if turn_rate is None else f"/tr{turn_rate}"
    return f"{scenario}/{units}x{players}/{mode}/s{seed}/t{ticks}{rate}"


def cases(args):
    """The screen's case list, in output order."""
    result = []
    for seed in args.seeds:
        for scenario in args.scenarios:
            pops = list(POPULATIONS)
            if scenario in LARGE_SCENARIOS:
                pops.append(LARGE_POPULATION)
            if scenario == "churn":
                pops = [CHURN_POPULATION]
            for units, players in pops:
                for mode in args.modes:
                    result.append(dict(scenario=scenario, units=units, players=players, mode=mode,
                                       seed=seed, ticks=args.ticks, turn_rate=args.turn_rate))
    return result


def command(binary, case):
    cmd = [str(binary), "--mode", case["mode"], "--scenario", case["scenario"],
           "--units", str(case["units"]), "--players", str(case["players"]),
           "--moving-percent", "100", "--ticks", str(case["ticks"]), "--seed", str(case["seed"])]
    if case["turn_rate"] is not None:
        cmd += ["--turn-rate", str(case["turn_rate"])]
    return cmd


def parse_cpus(text):
    cpus = []
    for part in text.split(","):
        if "-" in part:
            low, high = map(int, part.split("-"))
            cpus.extend(range(low, high + 1))
        elif part:
            cpus.append(int(part))
    return cpus


def run_case(binary, case, cpu, timeout):
    cmd = command(binary, case)
    if cpu is not None:
        cmd = ["taskset", "-c", str(cpu)] + cmd
    cid = case_id(case["scenario"], case["units"], case["players"], case["mode"], case["seed"],
                  case["ticks"], case["turn_rate"])
    began = time.monotonic()
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {"case": cid, "error": f"timeout after {timeout}s"}, time.monotonic() - began
    seconds = time.monotonic() - began
    if result.returncode:
        tail = (result.stderr.strip().splitlines() or [""])[-1]
        return {"case": cid, "error": f"exit {result.returncode}: {tail}"}, seconds
    try:
        line = next(text for text in reversed(result.stdout.splitlines()) if text.startswith("{"))
        row = json.loads(line)
    except (StopIteration, ValueError) as error:
        return {"case": cid, "error": f"no JSON result: {error}"}, seconds
    return {"case": cid, **deterministic(row)}, seconds


def run_screen(binary, case_list, cpus, jobs, timeout, progress=None):
    """Run every case; returns rows in case order and per-case wall seconds."""
    order = sorted(range(len(case_list)), key=lambda i: -case_list[i]["units"] * case_list[i]["players"])
    work = queue.Queue()
    for index in order:
        work.put(index)
    rows = [None] * len(case_list)
    seconds = [0.0] * len(case_list)
    slots = cpus if cpus else [None] * jobs

    def worker(cpu):
        while True:
            try:
                index = work.get_nowait()
            except queue.Empty:
                return
            rows[index], seconds[index] = run_case(binary, case_list[index], cpu, timeout)
            if progress:
                progress(rows[index], seconds[index])

    threads = [threading.Thread(target=worker, args=(slots[k % len(slots)],)) for k in range(jobs)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    return rows, seconds


def load(path):
    rows = {}
    with open(path) as handle:
        for line in handle:
            if line.strip():
                row = json.loads(line)
                rows[row["case"]] = row
    return rows


def delta(old, new):
    if isinstance(old, bool) or isinstance(new, bool) or not isinstance(old, (int, float)) \
            or not isinstance(new, (int, float)):
        return f"{old!r} -> {new!r}"
    change = new - old
    rel = f" ({change / old * 100:+.1f}%)" if old else ""
    fmt = (lambda v: f"{v:g}") if isinstance(change, float) else str
    return f"{fmt(old)} -> {fmt(new)} ({'+' if change >= 0 else ''}{fmt(change)}){rel}"


def diff(reference, rows, label, out=sys.stdout, strict_keys=False):
    """Print per-key deltas of `rows` against `reference`; returns the number of differences."""
    current = {row["case"]: row for row in rows}
    differences = 0
    print(f"== diff against {label}", file=out)
    for cid, row in current.items():
        old = reference.get(cid)
        if old is None:
            print(f"  {cid}: new case (not in {label})", file=out)
            continue
        changed = [key for key in old if key in row and old[key] != row[key]]
        removed = [key for key in old if key not in row]
        added = [key for key in row if key not in old]
        if not changed and not removed and not added:
            continue
        print(f"  {cid}:", file=out)
        for key in changed:
            print(f"    {key}: {delta(old[key], row[key])}", file=out)
        for key in removed:
            print(f"    {key}: removed (was {old[key]!r})", file=out)
        if added:
            print(f"    new keys: {', '.join(added)}", file=out)
        differences += len(changed) + len(removed) + (len(added) if strict_keys else 0)
    for cid in reference:
        if cid not in current:
            print(f"  {cid}: missing (in {label}, not run)", file=out)
            differences += 1
    print(f"== {differences} difference(s) against {label}", file=out)
    return differences


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="JSONL, one row per case, in case order")
    parser.add_argument("--baseline", type=Path, help="committed screen JSONL; any difference fails")
    parser.add_argument("--anchor", type=Path, help="frozen audit-era screen JSONL; report only")
    parser.add_argument("--anchor-fail", action="store_true", help="also fail on anchor differences")
    parser.add_argument("--strict-keys", action="store_true", help="count keys new since the baseline as differences")
    parser.add_argument("-j", "--jobs", type=int, help="parallel runs (default: the CPU list's size, else 1)")
    parser.add_argument("--cpus", help="taskset CPU list, e.g. 16-19 or 0,2,4; one run per CPU at a time")
    parser.add_argument("--turn-rate", type=int, help="passed through as crowdbench --turn-rate")
    parser.add_argument("--ticks", type=int, default=DEFAULT_TICKS)
    parser.add_argument("--seeds", type=int, nargs="+", default=[0])
    parser.add_argument("--modes", nargs="+", choices=MODES, default=list(MODES))
    parser.add_argument("--scenarios", nargs="+", choices=SCREEN_SCENARIOS + ("churn",),
                        default=list(SCREEN_SCENARIOS) + ["churn"])
    parser.add_argument("--timeout", type=float, default=3600)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)
    binary = args.binary.resolve(strict=True)
    cpus = parse_cpus(args.cpus) if args.cpus else []
    jobs = args.jobs or len(cpus) or 1
    if jobs < 1:
        parser.error("-j must be positive")
    case_list = cases(args)
    began = time.monotonic()
    done = [0]

    def progress(row, seconds):
        done[0] += 1
        if not args.quiet:
            state = row.get("error") or row.get("hash")
            print(f"[{done[0]}/{len(case_list)}] {row['case']} {seconds:.1f}s {state}", file=sys.stderr, flush=True)

    rows, seconds = run_screen(binary, case_list, cpus, jobs, args.timeout, progress)
    wall = time.monotonic() - began
    with open(args.output, "w") as handle:
        for row in rows:
            handle.write(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n")
    failed = [row for row in rows if "error" in row]
    print(f"screen: {len(rows)} cases, {len(failed)} failed, wall {wall:.1f}s on {jobs} job(s)"
          f"{' cpus ' + args.cpus if args.cpus else ''}, run time sum {sum(seconds):.1f}s")
    for row in failed:
        print(f"  FAILED {row['case']}: {row['error']}")
    status = 1 if failed else 0
    if args.baseline:
        if diff(load(args.baseline), rows, f"baseline {args.baseline}", strict_keys=args.strict_keys):
            status = 1
    if args.anchor:
        if diff(load(args.anchor), rows, f"anchor {args.anchor}", strict_keys=args.strict_keys) and args.anchor_fail:
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
