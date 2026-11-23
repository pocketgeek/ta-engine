#!/usr/bin/env python3
"""Final navigation matrix: frozen baseline versus candidate crowdbench.

Two phases, deliberately separate:

  outcome  Deterministic counters only (arrivals, crossings, completion, stalls,
           path length, search/deferred work, route request->delivery latency in
           simulation TICKS, tick-time C++ allocation counts). Runs every case
           once per binary with --latency --allocations, serial simulation, one
           process per pinned CPU and several processes in parallel. Wall-clock
           fields from this phase are recorded but never reported as timing.
  timing   Plain runs (no diagnostics), strictly serial, every process pinned
           to ONE cpu, >=3 repeats. Within a repeat each case runs baseline and
           candidate back to back, alternating which goes first; odd repeats
           also reverse the case order. Only this phase's tick times are timing.

One `units` value is PER PLAYER. A population is units:players:moving-percent.
Outputs in --output: manifest.json (commands, binaries' sha256, hardware,
provenance), results.jsonl (raw), results.csv, summary.md. `report DIR`
regenerates the CSV/markdown from results.jsonl. `estimate` prints the case
count and a runtime estimate without running anything.

This runner never builds or modifies the engine. Run timing on an idle host.
"""
import argparse
import csv
import itertools
import json
import math
import os
from pathlib import Path
import platform
import queue
import shutil
import statistics
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, str(Path(__file__).resolve().parent))
import crowdbench_matrix as cm  # noqa: E402  (provenance/hardware/sha256 helpers)

SCENARIOS = cm.SCENARIOS
ALL_SCENARIOS = cm.ALL_SCENARIOS
MODES = cm.MODES
# Default: the mode the frozen baseline can run. Add Legion with --modes.
DEFAULT_MODES = cm.LEGACY_MODES
FULL_POPULATIONS = ("200:1:100", "500:1:100", "1000:1:100", "2000:1:100", "500:4:100", "250:8:50")
LONG_SCENARIOS = ("doors", "maze", "sharedgoal", "opposingcolumns")
TIMING_SCENARIOS = ("open", "doors", "maze", "opposingcolumns", "sharedgoal", "mixedfootprints",
                    "exploration", "dynamicobstacle", "rapidreplacement", "recovery")
TIMING_POPULATIONS = ("200:1:100", "2000:1:100", "500:4:100")
# Seconds per million unit-ticks. Calibrated 2026-10-05 from the 672-run
# checkpoint outcome matrix (23,376 process-seconds) on an i9-275HX running 20
# pinned processes on cpus 4-23 while other agents loaded the host (load ~40):
# conservative. An idle P-core should need roughly half. Estimates only.
COST_PER_MUNIT_TICK = {"retail": 3.2, "legion": 5.1}  # Legion: uncalibrated
DIAGNOSTIC_OVERHEAD = 1.15

# Deterministic outcome fields: identical across repeats of one binary. Also the
# fields compared between binaries ("before/after").
OUTCOME_FIELDS = (
    "hash", "moving_units", "arrived_settled", "crossed_middle", "all_arrived_tick",
    "whole_group_latest_order_ticks", "arrival_tick_p50", "arrival_tick_p95",
    "physical_in_goal", "orders_complete", "max_goal_distance_px", "stalled_unit_ticks",
    "path_length_px", "path_to_initial_straight_ratio", "illegal_footprint_samples",
    "illegal_final_movers", "first_cross_tick", "last_cross_tick", "peak_pending",
    "retail_work_sum", "retail_requests", "retail_completions", "retail_failures",
    "route_requests_observed", "route_deliveries_observed", "route_delivery_failures",
    "route_requests_still_pending", "route_request_cancelled", "route_request_replaced",
    "route_request_stale", "route_request_cleared",
    "route_request_to_delivery_ticks_received_only_p50",
    "route_request_to_delivery_ticks_received_only_p95",
    "route_request_to_delivery_ticks_received_only_p99",
    "route_pending_age_ticks_max",
    "tick_cpp_allocation_calls", "tick_cpp_allocation_requested_bytes",
)
TIMING_FIELDS = ("tick_ms_mean", "tick_ms_p50", "tick_ms_p95", "tick_ms_p99", "process_peak_rss_kib")
CSV_LEAD = ("phase", "role", "mode", "scenario", "units_per_player", "players", "moving_percent",
            "ticks", "seed", "repeat", "cpu", "wall_s", "error")


def parse_population(text):
    units, players, moving = (int(part) for part in text.split(":"))
    if not (1 <= units <= 2000 and 1 <= players <= 8 and 0 <= moving <= 100):
        raise argparse.ArgumentTypeError(f"bad population {text!r}: units 1..2000, players 1..8, moving 0..100")
    return units, players, moving


def parse_cpus(text):
    cpus = []
    for part in text.split(","):
        if "-" in part:
            low, high = map(int, part.split("-"))
            cpus += range(low, high + 1)
        elif part:
            cpus.append(int(part))
    return cpus


def cases(args):
    """Every (scenario, population, ticks, seed, mode) of the requested phase."""
    result = []
    populations = [parse_population(p) for p in args.populations]
    for scenario, population, seed, mode in itertools.product(args.scenarios, populations, args.seeds, args.modes):
        result.append((scenario, *population, args.ticks, seed, mode))
    if args.long_ticks:
        long_population = parse_population(args.long_population)
        for scenario, seed, mode in itertools.product(args.long_scenarios, args.seeds, args.modes):
            if scenario in args.scenarios:
                result.append((scenario, *long_population, args.long_ticks, seed, mode))
    return result


def estimate_seconds(case, diagnostics):
    scenario, units, players, moving, ticks, seed, mode = case
    # Static bodies are cheap but not free; count them at a third.
    bodies = units * players * (moving / 100 + (1 - moving / 100) / 3)
    seconds = COST_PER_MUNIT_TICK[mode] * bodies * ticks / 1e6 + 0.3
    return seconds * (DIAGNOSTIC_OVERHEAD if diagnostics else 1)


def command_for(binary, case, diagnostics, trace=None):
    scenario, units, players, moving, ticks, seed, mode = case
    command = [str(binary), "--mode", mode, "--scenario", scenario, "--units", str(units),
               "--players", str(players), "--moving-percent", str(moving), "--ticks", str(ticks),
               "--seed", str(seed)]
    if diagnostics:
        command += ["--latency", "--allocations"]
    if trace:
        command += ["--trace", str(trace)]
    return command


def run_one(command, cpu, timeout):
    pinned = ["taskset", "-c", str(cpu)] + command if cpu is not None and shutil.which("taskset") else command
    start = time.monotonic()
    try:
        result = subprocess.run(pinned, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, time.monotonic() - start, "timeout"
    wall = time.monotonic() - start
    if result.returncode:
        return None, wall, f"exit {result.returncode}: {result.stderr.strip()[-400:]}"
    lines = [line for line in result.stdout.splitlines() if line.startswith("{")]
    if not lines:
        return None, wall, "no JSON output"
    return json.loads(lines[-1]), wall, None


def derived(data):
    """Fields computed from raw counters, identical for every binary."""
    return data


def check_identity(data, case):
    scenario, units, players, moving, ticks, seed, mode = case
    expected = {"mode": mode, "scenario": scenario, "units_per_player": units, "players": players,
                "moving_percent": moving, "ticks": ticks, "seed": seed, "workers": False}
    for key, value in expected.items():
        if data.get(key) != value:
            return f"benchmark identity mismatch: {key}={data.get(key)!r}, expected {value!r}"
    if data.get("route_telemetry_unmatched"):
        return "unmatched route telemetry lifecycle events"
    if data.get("route_lifecycle_balanced") is False:
        return "route request lifecycle does not balance"
    # Older harnesses omit build identity; a present false is always fatal.
    if data.get("build_ndebug") is False or data.get("build_optimized") is False:
        return "benchmark binary is not an optimized NDEBUG build"
    return None


def execute(args):
    binaries = {}
    for spec in args.binary:
        role, _, path = spec.partition("=")
        if not path:
            raise SystemExit("--binary needs ROLE=PATH")
        binaries[role] = Path(path) if args.command == "estimate" else Path(path).resolve(strict=True)
    if args.phase == "timing" and len(binaries) != 2:
        print("note: timing with one binary has no before/after pairing", file=sys.stderr)
    all_cases = cases(args)
    diagnostics = args.phase == "outcome"
    total = sum(estimate_seconds(c, diagnostics) for c in all_cases) * len(binaries) * args.repeats
    cpus = parse_cpus(args.cpus)
    # Timing may use several pinned cpus: each case's whole repeat/binary
    # sequence stays on ONE cpu, so every before/after pair shares a core.
    parallel = len(cpus)
    print(f"{len(all_cases)} cases x {len(binaries)} binaries x {args.repeats} repeats = "
          f"{len(all_cases) * len(binaries) * args.repeats} runs; estimated {total / 3600:.2f} CPU-hours, "
          f"~{total / parallel / 3600:.2f} h wall on {parallel} cpu(s)", flush=True)
    if args.command == "estimate":
        return
    out = args.output
    out.mkdir(parents=True, exist_ok=True)
    if (out / "manifest.json").exists():
        raise SystemExit(f"{out} already holds a manifest; choose a new directory")
    provenance = {}
    for role, binary in binaries.items():
        source = getattr(args, f"{role}_source", None) if role in ("baseline", "candidate") else None
        build = getattr(args, f"{role}_build", None) if role in ("baseline", "candidate") else None
        provenance[role] = cm.provenance(binary, source, build)
        (out / f"{role}-provenance.json").write_text(json.dumps(provenance[role], indent=2) + "\n")
    manifest = {
        "schema": 1, "phase": args.phase, "label": args.label, "started_unix": time.time(),
        "binaries": {role: {"path": str(path), "sha256": provenance[role]["binary_sha256"],
                            "revision": provenance[role].get("revision"),
                            "dirty": provenance[role].get("dirty"),
                            "source_tree_sha256": provenance[role].get("source_tree_sha256")}
                     for role, path in binaries.items()},
        "hardware": cm.hardware(), "cpus": cpus, "parallel_processes": parallel,
        "arguments": {k: (str(v) if isinstance(v, Path) else v) for k, v in vars(args).items()},
        "arrival": "live, empty orders, zero speed, legal full footprint, authored goal area, 30 unchanged ticks",
        "measurement": ("outcome: deterministic counters and tick-latency only; wall fields are not timing"
                        if diagnostics else
                        "timing: World.tick wall time only; each case's repeats and both binaries run "
                        "serially on one pinned cpu, leader alternating per repeat"
                        + (f"; {parallel} cases in parallel on separate cpus" if parallel > 1 else "")),
        "estimated_cpu_seconds": total,
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    roles = list(binaries)
    jobs = []
    stable = {case: index for index, case in enumerate(all_cases)}
    for repeat in range(args.repeats):
        ordered = all_cases if repeat % 2 == 0 else list(reversed(all_cases))
        for case in ordered:
            # Alternate which binary runs first using the case's STABLE index,
            # so reversing the case order cannot cancel the alternation.
            pair = roles if (stable[case] + repeat) % 2 == 0 else list(reversed(roles))
            for role in pair:
                jobs.append((repeat, role, case))
    if diagnostics and parallel > 1:
        # Outcomes do not depend on order: start the longest runs first so the
        # pool does not end on a tail of 12000-tick cases. Stable, so each
        # case's baseline/candidate pair stays adjacent.
        jobs.sort(key=lambda job: -estimate_seconds(job[2], diagnostics))
    free = queue.Queue()
    for cpu in cpus[:parallel]:
        free.put(cpu)
    lock = threading.Lock()
    results = (out / "results.jsonl").open("w")
    errors = []
    done = [0]
    started = time.monotonic()

    def work(job, pinned=None):
        repeat, role, case = job
        cpu = free.get() if pinned is None else pinned
        try:
            data, wall, error = run_one(command_for(binaries[role], case, diagnostics), cpu, args.timeout)
        finally:
            if pinned is None:
                free.put(cpu)
        if data is not None and error is None:
            error = check_identity(data, case)
        scenario, units, players, moving, ticks, seed, mode = case
        row = {"phase": args.phase, "role": role, "repeat": repeat, "cpu": cpu, "wall_s": round(wall, 3),
               "error": error, "command": command_for(binaries[role], case, diagnostics)}
        row.update(derived(data) if data else {"mode": mode, "scenario": scenario, "units_per_player": units,
                                               "players": players, "moving_percent": moving, "ticks": ticks,
                                               "seed": seed})
        with lock:
            results.write(json.dumps(row) + "\n")
            results.flush()
            done[0] += 1
            if error:
                errors.append((job, error))
            elapsed = time.monotonic() - started
            print(f"[{done[0]}/{len(jobs)} {elapsed:.0f}s] {role} {mode} {scenario} {units}x{players}@{moving} "
                  f"t{ticks} s{seed} r{repeat}: " + (f"FAILED {error}" if error else
                  f"arrived {data['arrived_settled']}/{data['moving_units']} crossed {data['crossed_middle']} "
                  f"{wall:.1f}s"), flush=True)

    if parallel == 1:
        for job in jobs:
            work(job)
    elif not diagnostics:
        groups = {}
        for job in jobs:
            groups.setdefault(job[2], []).append(job)
        ordered_groups = sorted(groups.values(), key=lambda g: -sum(estimate_seconds(j[2], False) for j in g))

        def work_group(group):
            cpu = free.get()
            try:
                for job in group:
                    work(job, cpu)
            finally:
                free.put(cpu)
        with ThreadPoolExecutor(parallel) as pool:
            list(pool.map(work_group, ordered_groups))
    else:
        with ThreadPoolExecutor(parallel) as pool:
            list(pool.map(work, jobs))
    results.close()
    manifest["finished_unix"] = time.time()
    manifest["binaries_unchanged"] = {role: cm.sha256(path) == manifest["binaries"][role]["sha256"]
                                      for role, path in binaries.items()}
    manifest["errors"] = [{"job": [j[0], j[1], list(j[2])], "error": e} for j, e in errors]
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    report(out)
    if errors or not all(manifest["binaries_unchanged"].values()):
        raise SystemExit(f"{len(errors)} runs failed or a binary changed; see manifest.json")


def load(out):
    with (out / "results.jsonl").open() as handle:
        return [json.loads(line) for line in handle if line.strip()]


def stats(values):
    values = [v for v in values if isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)]
    if not values:
        return None
    return {"n": len(values), "mean": statistics.mean(values),
            "sd": statistics.stdev(values) if len(values) > 1 else 0.0,
            "median": statistics.median(values), "min": min(values), "max": max(values)}


def fmt(value, digits=0):
    if value is None:
        return "–"
    if isinstance(value, float) and digits:
        return f"{value:.{digits}f}"
    return f"{value:,.0f}" if isinstance(value, (int, float)) else str(value)


def spread(s, digits=0):
    if not s:
        return "–"
    if s["n"] == 1:
        return fmt(s["mean"], digits)
    return f"{fmt(s['mean'], digits)} ±{fmt(s['sd'], digits or 1)} [{fmt(s['min'], digits)}–{fmt(s['max'], digits)}]"


def censored_completion(rows):
    """Whole-group completion: censored seeds are counted, never averaged as times."""
    values = [r.get("all_arrived_tick") for r in rows]
    done = [v for v in values if isinstance(v, int) and v >= 0]
    text = f"{len(done)}/{len(values)}"
    if done:
        text += f" med {statistics.median(done):,.0f}"
    return text


def report(out):
    rows = load(out)
    manifest = json.loads((out / "manifest.json").read_text())
    keys = list(CSV_LEAD)
    for row in rows:
        for key in row:
            if key not in keys and key != "command" and not isinstance(row[key], (list, dict)):
                keys.append(key)
    with (out / "results.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=keys, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)
    good = [r for r in rows if not r.get("error")]
    roles = list(manifest["binaries"])
    phase = manifest["phase"]
    group_key = lambda r: (r["scenario"], r["units_per_player"], r["players"], r["moving_percent"], r["ticks"])
    lines = [f"# Navigation {phase} matrix: {manifest.get('label', '')}", ""]
    for role, info in manifest["binaries"].items():
        lines.append(f"- **{role}**: `{info['path']}` sha256 `{info['sha256']}`, revision "
                     f"`{info.get('revision')}`, dirty {info.get('dirty')}")
    hw = manifest.get("hardware", {})
    lines += [f"- host: {hw.get('platform')} ({hw.get('logical_cpus')} cpus), cpus used {manifest.get('cpus')}, "
              f"{manifest.get('parallel_processes')} parallel process(es)",
              f"- runs: {len(rows)}, failed: {len(rows) - len(good)}; {manifest.get('measurement')}",
              f"- arrival rule: {manifest.get('arrival')}", ""]
    # Determinism: repeats of one binary must agree on every outcome field.
    nondet = []
    by_run = {}
    for r in good:
        by_run.setdefault((r["role"], r["mode"], group_key(r), r["seed"]), []).append(r)
    for key, samples in by_run.items():
        for field in OUTCOME_FIELDS:
            if field.startswith("tick_cpp") or field.endswith("_bytes"):
                continue
            if len({json.dumps(s.get(field)) for s in samples}) > 1:
                nondet.append((key, field))
    lines.append(f"Repeated-run deterministic outcome mismatches: **{len(nondet)}**" +
                 ("" if not nondet else " " + "; ".join(f"{k}:{f}" for k, f in nondet[:10])))
    lines.append("")
    modes = [m for m in MODES if any(r["mode"] == m for r in good)]
    if len(roles) == 2:
        a, b = roles
        lines += [f"Final state hashes, {a} vs {b} (Retail must match everywhere):", ""]
        for mode in modes:
            same = differ = 0
            for key in {k for (role, m, k, seed) in by_run if m == mode}:
                for seed in {s for (_, m, k, s) in by_run if m == mode and k == key}:
                    x, y = by_run.get((a, mode, key, seed)), by_run.get((b, mode, key, seed))
                    if x and y:
                        if x[0].get("hash") == y[0].get("hash"):
                            same += 1
                        else:
                            differ += 1
            lines.append(f"- {mode}: equal {same}, different {differ}")
        lines.append("")
    # Per-mode totals across every case (sum over seeds and scenarios).
    lines += ["## Per-mode totals (sum over all cases and seeds; first repeat)", ""]
    header = ["mode"] + [f"{field} ({role})" for field in ("crossed", "arrived", "groups done", "stalled unit-ticks",
                                                             "search work", "latency p95 med (received only)",
                                                             "requests outstanding at end")
                         for role in roles]
    lines += ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    for mode in modes:
        cells = [mode]
        for field in ("crossed_middle", "arrived_settled", "all_arrived_tick", "stalled_unit_ticks", "work", "lat",
                      "route_requests_still_pending"):
            for role in roles:
                sel = [r for r in good if r["mode"] == mode and r["role"] == role and r["repeat"] == 0]
                if field == "all_arrived_tick":
                    cells.append(f"{sum(1 for r in sel if r.get(field, -1) >= 0)}/{len(sel)}")
                elif field == "work":
                    cells.append(fmt(sum((r.get("retail_work_sum") or 0) for r in sel)))
                elif field == "lat":
                    values = [r.get("route_request_to_delivery_ticks_received_only_p95") for r in sel]
                    values = [v for v in values if isinstance(v, (int, float)) and v >= 0]
                    cells.append(fmt(statistics.median(values)) if values else "–")
                else:
                    cells.append(fmt(sum(r.get(field) or 0 for r in sel)))
        lines.append("| " + " | ".join(cells) + " |")
    lines.append("")
    if phase == "outcome":
        lines += ["## Per case (mean ±sd [min–max] across seeds; completion = seeds whose whole group "
                  "settled, median tick)", "",
                  "Latency = actual queued route request to delivery callback, simulation ticks, received-only "
                  "p95 (median across seeds); `out` = requests still outstanding at the end, `canc` = "
                  "cancelled+replaced+stale+cleared (sum across seeds).", ""]
        header = ["scenario", "units×players@moving", "ticks", "mode", "role", "crossed", "arrived",
                  "completion", "stalled unit-ticks", "path px", "search work", "deferred local",
                  "lat p50/p95/p99", "out/canc", "allocs/tick"]
        lines += ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
        groups = sorted({group_key(r) for r in good}, key=lambda k: (ALL_SCENARIOS.index(k[0]), k[4], k[2], k[1]))
        for key in groups:
            for mode in modes:
                for role in roles:
                    sel = [r for r in good if group_key(r) == key and r["mode"] == mode and r["role"] == role
                           and r["repeat"] == 0]
                    if not sel:
                        continue
                    def med(field):
                        values = [r.get(field) for r in sel]
                        values = [v for v in values if isinstance(v, (int, float)) and v >= 0]
                        return fmt(statistics.median(values)) if values else "–"
                    work = stats([(r.get("retail_work_sum") or 0) for r in sel])
                    canc = sum((r.get("route_request_cancelled") or 0) + (r.get("route_request_replaced") or 0) +
                               (r.get("route_request_stale") or 0) + (r.get("route_request_cleared") or 0)
                               for r in sel)
                    out_ = sum(r.get("route_requests_still_pending") or 0 for r in sel)
                    allocs = stats([(r.get("tick_cpp_allocation_calls") or 0) / max(1, r["ticks"]) for r in sel])
                    lines.append("| " + " | ".join([
                        key[0], f"{key[1]}×{key[2]}@{key[3]}", str(key[4]), mode, role,
                        spread(stats([r.get("crossed_middle") for r in sel])),
                        spread(stats([r.get("arrived_settled") for r in sel])),
                        censored_completion(sel),
                        spread(stats([r.get("stalled_unit_ticks") for r in sel])),
                        spread(stats([r.get("path_length_px") for r in sel])),
                        spread(work), spread(stats([r.get("local_deferred_work") for r in sel])),
                        f"{med('route_request_to_delivery_ticks_received_only_p50')}/"
                        f"{med('route_request_to_delivery_ticks_received_only_p95')}/"
                        f"{med('route_request_to_delivery_ticks_received_only_p99')}",
                        f"{fmt(out_)}/{fmt(canc)}", spread(allocs, 1)]) + " |")
    else:
        lines += ["## Timing per case (tick ms over repeats: median, mean ±sd [min–max]; "
                  "p95/p99 = median over repeats)", ""]
        header = ["scenario", "units×players@moving", "ticks", "seed", "mode"]
        for role in roles:
            header += [f"{role} mean ms", f"{role} p95/p99", f"{role} rss MiB"]
        if len(roles) == 2:
            header.append(f"{roles[1]}/{roles[0]} median")
        lines += ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
        groups = sorted({group_key(r) + (r["seed"],) for r in good},
                        key=lambda k: (ALL_SCENARIOS.index(k[0]), k[4], k[2], k[1], k[5]))
        for key in groups:
            for mode in modes:
                cells = [key[0], f"{key[1]}×{key[2]}@{key[3]}", str(key[4]), str(key[5]), mode]
                medians = []
                for role in roles:
                    sel = [r for r in good if group_key(r) + (r["seed"],) == key and r["mode"] == mode and
                           r["role"] == role]
                    s = stats([r["tick_ms_mean"] for r in sel])
                    if not s:
                        cells += ["–", "–", "–"]
                        medians.append(None)
                        continue
                    medians.append(s["median"])
                    p95 = statistics.median(r["tick_ms_p95"] for r in sel)
                    p99 = statistics.median(r["tick_ms_p99"] for r in sel)
                    rss = statistics.median(r.get("process_peak_rss_kib") or 0 for r in sel) / 1024
                    cells += [f"{s['median']:.3f} ({s['mean']:.3f} ±{s['sd']:.3f} [{s['min']:.3f}–{s['max']:.3f}], "
                              f"n={s['n']})", f"{p95:.3f}/{p99:.3f}", f"{rss:.0f}"]
                if len(roles) == 2:
                    cells.append(f"{medians[1] / medians[0]:.3f}" if all(medians) else "–")
                lines.append("| " + " | ".join(cells) + " |")
    lines.append("")
    (out / "summary.md").write_text("\n".join(lines))
    print(f"wrote {out / 'results.csv'} and {out / 'summary.md'}")


def combine(out, inputs):
    """One report from runs made separately, e.g. a baseline matrix published
    earlier and a candidate matrix run later with identical case lists."""
    manifests = [json.loads((d / "manifest.json").read_text()) for d in inputs]
    phases = {m["phase"] for m in manifests}
    if len(phases) != 1:
        raise SystemExit("cannot combine outcome and timing runs")
    if phases == {"timing"}:
        # Separate sessions are not paired measurements: different load,
        # thermal state, and possibly hardware. Run timing roles together.
        raise SystemExit("refusing to combine timing runs: run both binaries in one timing session")
    def case_set(d):
        return {(r.get("scenario"), r.get("units_per_player"), r.get("players"), r.get("moving_percent"),
                 r.get("ticks"), r.get("seed"), r.get("mode")) for r in load(d)}
    reference = case_set(inputs[0])
    for d in inputs[1:]:
        if case_set(d) != reference:
            raise SystemExit(f"{d} does not hold the same cases as {inputs[0]}")
    if len({json.dumps(m.get("hardware"), sort_keys=True) for m in manifests}) != 1:
        print("warning: inputs were recorded on different hardware", file=sys.stderr)
    binaries = {}
    for m in manifests:
        for role, info in m["binaries"].items():
            if role in binaries:
                raise SystemExit(f"role {role!r} appears in more than one input")
            binaries[role] = info
    out.mkdir(parents=True, exist_ok=True)
    merged = dict(manifests[0], binaries=binaries, label=" + ".join(m.get("label", "") for m in manifests),
                  combined_from=[str(d) for d in inputs])
    (out / "manifest.json").write_text(json.dumps(merged, indent=2) + "\n")
    with (out / "results.jsonl").open("w") as handle:
        for d in inputs:
            handle.write((d / "results.jsonl").read_text())
    report(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("run", "estimate"):
        p = sub.add_parser(name)
        p.add_argument("--phase", choices=("outcome", "timing"), required=True)
        p.add_argument("--binary", action="append", default=[], required=name == "run",
                       help="ROLE=PATH, e.g. baseline=/x/crowdbench-baseline (repeatable)")
        p.add_argument("--baseline-source", type=Path)
        p.add_argument("--baseline-build", type=Path)
        p.add_argument("--candidate-source", type=Path)
        p.add_argument("--candidate-build", type=Path)
        p.add_argument("--output", type=Path, required=name == "run")
        p.add_argument("--label", default="")
        p.add_argument("--cpus", default="2-23", help="outcome: pool of cpus, one process each; timing: ONE cpu")
        p.add_argument("--modes", nargs="+", choices=MODES, default=list(DEFAULT_MODES))
        p.add_argument("--scenarios", nargs="+", choices=ALL_SCENARIOS)
        p.add_argument("--populations", nargs="+", help="units:players:moving-percent")
        p.add_argument("--seeds", nargs="+", type=int)
        p.add_argument("--ticks", type=int, default=6000)
        p.add_argument("--long-ticks", type=int, help="default 12000 for outcome, 0 (off) for timing")
        p.add_argument("--long-scenarios", nargs="+", choices=ALL_SCENARIOS, default=list(LONG_SCENARIOS))
        p.add_argument("--long-population", default="2000:1:100")
        p.add_argument("--repeats", type=int)
        p.add_argument("--timeout", type=float, default=3600)
    p = sub.add_parser("report")
    p.add_argument("output", type=Path)
    p = sub.add_parser("combine", help="merge separate same-phase runs (distinct roles) into one before/after report")
    p.add_argument("output", type=Path)
    p.add_argument("inputs", nargs="+", type=Path)
    args = parser.parse_args()
    if args.command == "report":
        report(args.output)
        return
    if args.command == "combine":
        combine(args.output, args.inputs)
        return
    timing = args.phase == "timing"
    if args.scenarios is None:
        args.scenarios = list(TIMING_SCENARIOS if timing else SCENARIOS)
    if args.populations is None:
        args.populations = list(TIMING_POPULATIONS if timing else FULL_POPULATIONS)
    if args.seeds is None:
        args.seeds = [0] if timing else [0, 7, 42]
    if args.repeats is None:
        args.repeats = 3 if timing else 1
    if args.long_ticks is None:
        args.long_ticks = 0 if timing else 12000
    if timing and args.repeats < 3:
        parser.error("timing needs at least 3 repeats")
    for population in args.populations:
        parse_population(population)
    execute(args)


if __name__ == "__main__":
    main()
