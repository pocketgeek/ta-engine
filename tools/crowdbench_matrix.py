#!/usr/bin/env python3
"""Run serialized crowdbench comparisons; keep the host otherwise idle for timings.

One units value is PER PLAYER. Population entries are players:moving-percent.
Each invocation records the immutable binary hash, exact commands, raw JSON,
errors, and run times. Repeated rounds reverse order to expose order effects.
This runner never builds, modifies the engine, or starts a public server.
"""
import argparse
import csv
import hashlib
import itertools
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time

SCENARIOS = (
    "open", "doors", "bridges", "maze", "opposingcolumns", "sharedgoal",
    "mixedfootprints", "exploration", "dynamicobstacle", "rapidreplacement",
    "unreachable", "recovery", "recovery-passive",
)
# Requirement-driven acceptance scenarios (tools/crowdbench_acceptance.h). Kept
# out of SCENARIOS so default matrices and their runtime estimates are unchanged.
ACCEPTANCE_SCENARIOS = ("jagged", "trapped", "crowdtrap", "singleunit", "groupdetour")
# Workload scenarios: churn (T7 I3) keeps hundreds of Legion groups live with
# staggered re-orders and a structure placed/removed every 20 ticks.
STRESS_SCENARIOS = ("churn",)
# W7 step 0 multi-command opposing scenarios (second stream a separate command 100 ticks after the first); also outside
# SCENARIOS so default matrices are unchanged. crossingcolumns needs one player.
W7_SCENARIOS = ("opposingdoors", "opposingbridges", "crossingcolumns")
ALL_SCENARIOS = SCENARIOS + ACCEPTANCE_SCENARIOS + STRESS_SCENARIOS + W7_SCENARIOS
# The frozen baseline has no Legion mode. (Retail+, Flowfield and Cooperative
# were removed on 2026-10-06.)
LEGACY_MODES = ("retail",)
MODES = LEGACY_MODES + ("legion",)
CASE_KEYS = ("build_role", "mode", "scenario", "units_per_player", "players",
             "moving_percent", "ticks", "seed", "turn_rate", "workers", "allocation_counting",
             "path_profiling", "latency_observation")
OUTCOME_KEYS = ("hash", "alive", "arrived_settled", "all_arrived_tick", "crossed_middle",
                "illegal_footprint_samples", "path_length_px", "stalled_unit_ticks",
                # 2026-10-08 keys (absent from older binaries: compared as None)
                "raw_terrain_contact_noprogress_unit_ticks", "age_waiting_no_progress_unit_ticks",
                "age_parked_no_progress_unit_ticks", "age_waiting_held_by_design_unit_ticks",
                "age_parked_held_by_design_unit_ticks", "cap8_moving_samples", "route_crawl_samples",
                "complete_outside_radius", "cross_gap_max")


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def command_output(command, cwd=None):
    result = subprocess.run(command, cwd=cwd, capture_output=True, check=True)
    return result.stdout


def provenance(binary, source_dir, build_dir):
    """Record the actual source tree; callers must build it before this capture.

    A revision alone is insufficient for an instrumented frozen baseline. Both
    tracked diffs and content hashes of untracked source files are retained.
    This records evidence, not a claim that git can reconstruct a binary.
    """
    result = {"binary": str(binary), "binary_sha256": sha256(binary),
              "binary_bytes": binary.stat().st_size,
              "binary_mtime_ns": binary.stat().st_mtime_ns,
              "captured_unix": time.time()}
    if source_dir is None:
        result["source_verified"] = False
        result["limitation"] = "source directory not supplied; executable provenance is incomplete"
    else:
        source_dir = source_dir.resolve(strict=True)
        result["source_directory"] = str(source_dir)
        try:
            git = ["git", "-C", str(source_dir)]
            result["revision"] = command_output(git + ["rev-parse", "HEAD"]).decode().strip()
            status = command_output(git + ["status", "--porcelain=v1", "--untracked-files=all"]).decode()
            diff = command_output(git + ["diff", "HEAD", "--binary"])
            result.update(dirty=bool(status), status_porcelain=status,
                          tracked_diff_sha256=hashlib.sha256(diff).hexdigest(),
                          tracked_diff=diff.decode(errors="replace"))
            names = command_output(git + ["ls-files", "-z", "--cached", "--others", "--exclude-standard"])
            files = {}
            for name in sorted(set(names.decode().split("\0")) - {""}):
                path = source_dir / name
                if path.is_file() and (name.startswith(("src/", "tools/", ".github/")) or
                                       path.suffix in (".cmake", ".txt") or name == "CMakeLists.txt"):
                    files[name] = sha256(path)
            result["source_files_sha256"] = files
            result["source_tree_sha256"] = hashlib.sha256(
                json.dumps(files, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
            result["source_verified"] = True
        except (OSError, subprocess.CalledProcessError) as error:
            result["source_verified"] = False
            result["limitation"] = f"could not inspect source checkout: {error}"
    settings = {}
    if build_dir is not None:
        build_dir = build_dir.resolve(strict=True)
        result["build_directory"] = str(build_dir)
        for relative in ("CMakeCache.txt", "compile_commands.json",
                         "CMakeFiles/crowdbench.dir/flags.make", "CMakeFiles/crowdbench.dir/link.txt"):
            path = build_dir / relative
            if path.is_file():
                # Preserve exact build commands and flags with their hashes.
                settings[relative] = {"sha256": sha256(path), "contents": path.read_text()}
        cache = settings.get("CMakeCache.txt", {}).get("contents", "")
        compiler = next((line.split("=", 1)[1] for line in cache.splitlines()
                         if line.startswith("CMAKE_CXX_COMPILER:FILEPATH=")), None)
        if compiler:
            try:
                result["compiler_version"] = command_output([compiler, "--version"]).decode()
            except (OSError, subprocess.CalledProcessError) as error:
                result["compiler_version_error"] = str(error)
    result["build_settings"] = settings
    return result


def hardware():
    result = {"platform": platform.platform(), "machine": platform.machine(),
              "processor": platform.processor(), "logical_cpus": os.cpu_count()}
    if hasattr(os, "sched_getaffinity"):
        result["cpu_affinity"] = sorted(os.sched_getaffinity(0))
    for name in ("cpuinfo", "meminfo"):
        path = Path("/proc") / name
        if path.exists():
            text = path.read_text()
            result[name] = (text.split("\n\n", 1)[0] if name == "cpuinfo" else
                            "\n".join(line for line in text.splitlines() if line.startswith("MemTotal:")))
    for command, key in ((["lscpu", "-J"], "lscpu"), (["sysctl", "-n", "machdep.cpu.brand_string"], "cpu_brand")):
        try:
            result[key] = command_output(command).decode().strip()
        except (OSError, subprocess.CalledProcessError):
            pass
    return result


def summarize(rows):
    groups = {}
    for row in rows:
        key = tuple(row.get(field) for field in CASE_KEYS)
        groups.setdefault(key, []).append(row)
    summaries = []
    for key, samples in groups.items():
        summary = dict(zip(CASE_KEYS, key))
        summary["runs"] = len(samples)
        summary["outcomes_deterministic"] = all(
            len({json.dumps(row.get(name), sort_keys=True) for row in samples}) == 1
            for name in OUTCOME_KEYS)
        summary["outcome_values"] = {name: sorted({str(row.get(name)) for row in samples})
                                     for name in OUTCOME_KEYS}
        metrics = {}
        for name in sorted(set.intersection(*(set(row) for row in samples))):
            values = [row[name] for row in samples]
            if name in CASE_KEYS or name == "round" or any(
                    isinstance(value, bool) or not isinstance(value, (int, float)) or
                    not math.isfinite(value) for value in values):
                continue
            # Censored (-1) completion/latency events are never averaged as
            # though they were successful negative-duration observations.
            censored = sum(value < 0 for value in values)
            observed = [value for value in values if value >= 0]
            metrics[name] = {"count": len(values), "censored": censored,
                             "mean": statistics.mean(observed) if observed else None,
                             "stdev": statistics.stdev(observed) if len(observed) > 1 else None,
                             "min": min(observed) if observed else None,
                             "max": max(observed) if observed else None,
                             "median": statistics.median(observed) if observed else None}
        summary["metrics"] = metrics
        summaries.append(summary)
    return summaries


def compare_traces(left, right):
    """Compare all ticks and controller transitions, not just the final hash."""
    for suffix in ("", ".units.csv"):
        a, b = Path(str(left) + suffix), Path(str(right) + suffix)
        with a.open(newline="") as first, b.open(newline="") as second:
            for line, (x, y) in enumerate(itertools.zip_longest(csv.reader(first), csv.reader(second)), 1):
                if x != y:
                    raise SystemExit(f"trace mismatch {suffix or 'ticks'} line {line}: {x!r} != {y!r}")
        print(f"identical: {a} / {b}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--reference-binary", type=Path,
                        help="interleave frozen reference cases with the candidate for matched timings")
    parser.add_argument("--reference-modes", nargs="+", default=list(LEGACY_MODES), choices=MODES)
    parser.add_argument("--source-dir", type=Path,
                        help="exact candidate source checkout used to build --binary")
    parser.add_argument("--reference-source-dir", type=Path,
                        help="frozen reference checkout, including common instrumentation overlay")
    parser.add_argument("--build-dir", type=Path, help="candidate CMake build directory")
    parser.add_argument("--reference-build-dir", type=Path, help="reference CMake build directory")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--label", default="unlabelled")
    parser.add_argument("--modes", nargs="+", default=list(MODES), choices=MODES)
    parser.add_argument("--units", nargs="+", type=int, default=[200, 500, 1000, 2000])
    parser.add_argument("--populations", nargs="+", default=["1:100", "4:25", "8:10", "4:100"],
                        help="players:moving-percent pairs, not total units")
    parser.add_argument("--scenarios", nargs="+", choices=ALL_SCENARIOS, default=["open"])
    parser.add_argument("--ticks", type=int, default=1200)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--seeds", nargs="+", type=int, default=[0])
    parser.add_argument("--turn-rate", type=int,
                        help="crowdbench --turn-rate for every run (default: the binary's own, 2500)")
    parser.add_argument("--timeout", type=float, default=600)
    parser.add_argument("--workers", action="store_true")
    parser.add_argument("--trace", action="store_true", help="every-tick traces; larger files and observer overhead")
    parser.add_argument("--allocations", action="store_true", help="separate diagnostic run; changes timing overhead")
    parser.add_argument("--profile", action="store_true", help="opt-in new-build search clock counters")
    parser.add_argument("--latency", action="store_true", help="separate request lifecycle diagnostic; adds allocation overhead")
    parser.add_argument("--compare-traces", nargs=2, type=Path)
    args = parser.parse_args()
    if args.compare_traces:
        compare_traces(*args.compare_traces)
        return
    if args.binary is None or args.output is None:
        parser.error("--binary and --output are required")
    binary = args.binary.resolve(strict=True)
    reference = args.reference_binary.resolve(strict=True) if args.reference_binary else None
    if args.rounds < 1 or args.ticks < 1:
        parser.error("rounds and ticks must be positive")
    if any(seed < 0 or seed > 0xffffffff for seed in args.seeds):
        parser.error("seeds must be unsigned 32-bit integers")
    if args.turn_rate is not None and not 1 <= args.turn_rate <= 32768:
        parser.error("turn-rate must be 1..32768")
    populations = []
    for value in args.populations:
        players, moving = map(int, value.split(":"))
        if not 1 <= players <= 8 or not 0 <= moving <= 100:
            parser.error("population needs players 1..8 and moving-percent 0..100")
        populations.append((players, moving))
    args.output.mkdir(parents=True, exist_ok=True)
    manifest_path = args.output / "manifest.json"
    if manifest_path.exists():
        parser.error("output already contains a manifest; choose a new directory")
    candidate_provenance = provenance(binary, args.source_dir, args.build_dir)
    reference_provenance = provenance(reference, args.reference_source_dir, args.reference_build_dir) if reference else None
    # Keep large build commands/source patches once in the evidence directory,
    # rather than rewriting them each time a run is appended to the manifest.
    for role, data in (("candidate", candidate_provenance), ("reference", reference_provenance)):
        if data:
            (args.output / f"{role}-provenance.json").write_text(json.dumps(data, indent=2) + "\n")
    manifest = {
        "schema": 2,
        "label": args.label, "binary": str(binary), "binary_sha256": sha256(binary),
        "hardware": hardware(), "started_unix": time.time(),
        "candidate_provenance": "candidate-provenance.json",
        "reference_provenance": "reference-provenance.json" if reference else None,
        "arguments": {key: str(value) if isinstance(value, Path) else value
                      for key, value in vars(args).items()},
        "measurement": "World.tick only; serial subprocesses; host quietness must be coordinated externally",
        "arrival": "live, empty orders, zero speed, legal full footprint, authored goal area, 30 unchanged ticks",
        "latency": "actual accepted queue request to route callback in simulation ticks and elapsed wall milliseconds (including intervening harness observation/events); received-only percentiles, cancellation counts and outstanding ages separately; duplicate retries do not reset the clock",
        "variability": "summary.json contains repeated-run mean, sample standard deviation, median, min/max and censored counts; deterministic outcome checks do not compare wall-clock timing",
        "diagnostic_timing": bool(args.latency or args.allocations or args.profile or args.trace),
        "runs": [],
    }
    if reference:
        manifest["reference_binary"] = str(reference)
        manifest["reference_binary_sha256"] = sha256(reference)
    variants = ([("reference", reference, mode) for mode in args.reference_modes] if reference else [])
    variants += [("candidate", binary, mode) for mode in args.modes]
    cases = list(itertools.product(args.scenarios, args.units, populations, args.seeds, variants))
    errors = 0
    rows = []
    with (args.output / "results.jsonl").open("w") as results:
        for repeat in range(args.rounds):
            ordered = cases if repeat % 2 == 0 else list(reversed(cases))
            for scenario, units, (players, moving), seed, (role, executable, mode) in ordered:
                role_prefix = "reference-" if role == "reference" else ""
                name = f"r{repeat}-{scenario}-{units}x{players}-m{moving}-s{seed}-{role_prefix}{mode}"
                command = [str(executable), "--scenario", scenario, "--units", str(units),
                           "--players", str(players), "--moving-percent", str(moving),
                           "--mode", mode, "--ticks", str(args.ticks), "--seed", str(seed)]
                if args.turn_rate is not None:
                    command += ["--turn-rate", str(args.turn_rate)]
                for flag in ("workers", "allocations", "profile", "latency"):
                    if getattr(args, flag):
                        command.append("--" + flag)
                if args.trace:
                    command += ["--trace", str((args.output / f"{name}.trace.csv").resolve())]
                print(f"start {name}", flush=True)
                record = {"name": name, "round": repeat, "build_role": role,
                          "command": command, "start_unix": time.time()}
                try:
                    with (args.output / f"{name}.stdout").open("w") as stdout, \
                            (args.output / f"{name}.stderr").open("w") as stderr:
                        result = subprocess.run(command, stdout=stdout, stderr=stderr, timeout=args.timeout)
                    record["returncode"] = result.returncode
                    if result.returncode:
                        raise RuntimeError(f"exit {result.returncode}")
                    lines = (args.output / f"{name}.stdout").read_text().splitlines()
                    data = json.loads(next(line for line in reversed(lines) if line.startswith("{")))
                    expected = {"mode": mode, "scenario": scenario, "units_per_player": units,
                                "players": players, "moving_percent": moving, "seed": seed,
                                "ticks": args.ticks, "workers": args.workers}
                    if args.turn_rate is not None:
                        expected["turn_rate"] = args.turn_rate
                    for key, value in expected.items():
                        if data.get(key) != value:
                            raise ValueError(f"benchmark command/metric mismatch: {key}={data.get(key)!r}, expected {value!r}")
                    if data.get("route_telemetry_unmatched", 0):
                        raise ValueError("unmatched route telemetry lifecycle events")
                    record["metrics"] = data
                    row = {"name": name, "round": repeat, "build_role": role, **data}
                    rows.append(row)
                    results.write(json.dumps(row) + "\n")
                    results.flush()
                    print(f"done {name}: {data['arrived_settled']}/{data['moving_units']} settled, "
                          f"mean {data['tick_ms_mean']:.3f} ms", flush=True)
                except (subprocess.TimeoutExpired, RuntimeError, ValueError, StopIteration) as error:
                    record["error"] = str(error)
                    errors += 1
                    print(f"FAILED {name}: {error}", flush=True)
                record["end_unix"] = time.time()
                manifest["runs"].append(record)
                manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    manifest["finished_unix"] = time.time()
    # Detect replacement/rebuilding of a binary while this matrix was running.
    manifest["binary_unchanged"] = sha256(binary) == manifest["binary_sha256"]
    if reference:
        manifest["reference_binary_unchanged"] = sha256(reference) == manifest["reference_binary_sha256"]
    if not manifest["binary_unchanged"] or not manifest.get("reference_binary_unchanged", True):
        errors += 1
        manifest["binary_error"] = "an executable changed during the matrix; timing comparison is invalid"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    summaries = summarize(rows)
    (args.output / "summary.json").write_text(json.dumps(summaries, indent=2) + "\n")
    nondeterministic = [row for row in summaries if not row["outcomes_deterministic"]]
    if nondeterministic:
        errors += len(nondeterministic)
        print(f"FAILED: {len(nondeterministic)} repeated cases had different deterministic outcomes", flush=True)
    if errors:
        raise SystemExit(f"{errors} cases failed; see manifest and stderr files")


if __name__ == "__main__":
    main()
