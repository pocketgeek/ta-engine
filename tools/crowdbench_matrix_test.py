#!/usr/bin/env python3
"""Regression checks for benchmark evidence, censoring, and run identity."""
import importlib.util
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

MODULE_PATH = Path(__file__).with_name("crowdbench_matrix.py")
SPEC = importlib.util.spec_from_file_location("crowdbench_matrix", MODULE_PATH)
matrix = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(matrix)


class MatrixTests(unittest.TestCase):
    def test_variability_and_censored_completion(self):
        rows = [dict(mode="legion", seed=7, hash="fixed", tick_ms_mean=value,
                     all_arrived_tick=-1, arrived_settled=4) for value in (1, 2, 3)]
        summary, = matrix.summarize(rows)
        self.assertTrue(summary["outcomes_deterministic"])
        self.assertEqual(summary["metrics"]["tick_ms_mean"]["mean"], 2)
        self.assertEqual(summary["metrics"]["tick_ms_mean"]["stdev"], 1)
        self.assertIsNone(summary["metrics"]["all_arrived_tick"]["mean"])
        self.assertEqual(summary["metrics"]["all_arrived_tick"]["censored"], 3)
        rows[-1]["hash"] = "different"
        self.assertFalse(matrix.summarize(rows)[0]["outcomes_deterministic"])

    def test_seed_and_diagnostic_runs_are_not_pooled(self):
        rows = [dict(mode="retail", seed=seed, latency_observation=enabled, tick_ms_mean=1)
                for seed in (0, 42) for enabled in (False, True)]
        self.assertEqual(len(matrix.summarize(rows)), 4)

    def test_source_provenance_includes_dirty_untracked_code(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            (root / "src").mkdir()
            source = root / "src" / "test.cpp"
            source.write_text("original\n")
            subprocess.run(["git", "-C", str(root), "add", "."], check=True)
            subprocess.run(["git", "-C", str(root), "-c", "user.name=Benchmark Test",
                            "-c", "user.email=benchmark@example.invalid", "commit", "-qm", "fixture"], check=True)
            binary = root / "binary"
            binary.write_bytes(b"frozen executable")
            first = matrix.provenance(binary, root, None)
            source.write_text("modified\n")
            (root / "src" / "new.h").write_text("new input\n")
            second = matrix.provenance(binary, root, None)
            self.assertTrue(second["dirty"])
            self.assertEqual(first["revision"], second["revision"])
            self.assertEqual(first["binary_sha256"], second["binary_sha256"])
            self.assertNotEqual(first["source_tree_sha256"], second["source_tree_sha256"])
            self.assertIn("src/new.h", second["source_files_sha256"])
            self.assertIn("modified", second["tracked_diff"])

    def test_runner_checks_identity_and_writes_both_modes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            binary = root / "fake-crowdbench"
            binary.write_bytes(b"immutable fake benchmark")
            run = subprocess.run
            def fake_run(command, **kwargs):
                if command[0] != str(binary.resolve()):
                    return run(command, **kwargs)
                args = dict(zip(command[1::2], command[2::2]))
                data = {"mode": args["--mode"], "scenario": args["--scenario"], "workers": False,
                        "units_per_player": int(args["--units"]), "players": int(args["--players"]),
                        "moving_percent": int(args["--moving-percent"]), "seed": int(args["--seed"]),
                        "ticks": int(args["--ticks"]), "arrived_settled": 0, "moving_units": 1,
                        "tick_ms_mean": 1, "hash": "deterministic", "all_arrived_tick": -1}
                kwargs["stdout"].write(json.dumps(data) + "\n")
                return subprocess.CompletedProcess(command, 0)
            out = root / "results"
            argv = [str(MODULE_PATH), "--binary", str(binary), "--output", str(out),
                    "--units", "1", "--populations", "1:100", "--ticks", "3",
                    "--rounds", "2", "--seeds", "0", "7"]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(
                    matrix.subprocess, "run", side_effect=fake_run), contextlib.redirect_stdout(io.StringIO()):
                matrix.main()
            manifest = json.loads((out / "manifest.json").read_text())
            self.assertEqual(len(manifest["runs"]), 8)
            self.assertTrue(manifest["binary_unchanged"])
            summaries = json.loads((out / "summary.json").read_text())
            self.assertEqual(len(summaries), 4)
            self.assertEqual({row["mode"] for row in summaries}, set(matrix.MODES))
            self.assertTrue(all(row["runs"] == 2 for row in summaries))

    def test_turn_rate_is_passed_through_and_checked(self):
        self.assertIn("churn", matrix.ALL_SCENARIOS)
        self.assertNotIn("churn", matrix.SCENARIOS)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            binary = root / "fake-crowdbench"
            binary.write_bytes(b"immutable fake benchmark")
            run = subprocess.run
            seen = []
            def fake_run(command, **kwargs):
                if command[0] != str(binary.resolve()):
                    return run(command, **kwargs)
                seen.append(command)
                args = dict(zip(command[1::2], command[2::2]))
                data = {"mode": args["--mode"], "scenario": args["--scenario"], "workers": False,
                        "units_per_player": int(args["--units"]), "players": int(args["--players"]),
                        "moving_percent": int(args["--moving-percent"]), "seed": int(args["--seed"]),
                        "ticks": int(args["--ticks"]), "turn_rate": int(args.get("--turn-rate", 2500)),
                        "arrived_settled": 0, "moving_units": 1, "tick_ms_mean": 1, "hash": "h",
                        "all_arrived_tick": -1}
                kwargs["stdout"].write(json.dumps(data) + "\n")
                return subprocess.CompletedProcess(command, 0)
            out = root / "results"
            argv = [str(MODULE_PATH), "--binary", str(binary), "--output", str(out), "--units", "1",
                    "--populations", "1:100", "--ticks", "3", "--rounds", "1", "--modes", "legion",
                    "--scenarios", "churn", "--turn-rate", "10000"]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(
                    matrix.subprocess, "run", side_effect=fake_run), contextlib.redirect_stdout(io.StringIO()):
                matrix.main()
            self.assertEqual(len(seen), 1)
            self.assertEqual(seen[0][seen[0].index("--turn-rate") + 1], "10000")
            summary, = json.loads((out / "summary.json").read_text())
            self.assertEqual(summary["turn_rate"], 10000)


SCREEN_PATH = Path(__file__).with_name("crowdbench_screen.py")
SCREEN_SPEC = importlib.util.spec_from_file_location("crowdbench_screen", SCREEN_PATH)
screen = importlib.util.module_from_spec(SCREEN_SPEC)
SCREEN_SPEC.loader.exec_module(screen)


class ScreenTests(unittest.TestCase):
    def args(self, *extra):
        parser_args = ["--binary", __file__, "--output", os.devnull] + list(extra)
        captured = {}
        def fake(binary, cases, cpus, jobs, timeout, progress=None):
            captured.update(cases=cases, cpus=cpus, jobs=jobs)
            return [], []
        with mock.patch.object(screen, "run_screen", side_effect=fake), \
                contextlib.redirect_stdout(io.StringIO()):
            screen.main(parser_args)
        return captured

    def test_case_list_matches_the_plan(self):
        captured = self.args("--cpus", "16-17,19")
        cases = captured["cases"]
        # 18 scenarios x 2 modes x {200x1, 500x4} + 6 x 2 modes x 2000x1 + churn 2000x1 x 2 modes
        self.assertEqual(len(screen.SCREEN_SCENARIOS), 18)
        self.assertIn("maze", screen.SCREEN_SCENARIOS)
        self.assertEqual(len(cases), 18 * 2 * 2 + 6 * 2 + 2)
        self.assertEqual(captured["cpus"], [16, 17, 19])
        self.assertEqual(captured["jobs"], 3)
        self.assertEqual({(c["units"], c["players"]) for c in cases if c["scenario"] == "churn"}, {(2000, 1)})
        self.assertTrue(all(c["turn_rate"] is None for c in cases))
        rated = self.args("--turn-rate", "10000", "-j", "2")["cases"]
        self.assertTrue(all(c["turn_rate"] == 10000 for c in rated))
        self.assertIn("--turn-rate", screen.command("crowdbench", rated[0]))

    def test_only_deterministic_keys_are_kept(self):
        row = {"hash": "abc", "tick_ms_mean": 1.0, "setup_ms": 2.0, "event_ms": 3.0, "process_peak_rss_kib": 9,
               "build_compiler": "gcc", "legion_bytes": 7, "search_execution_ns": 5, "arrived_settled": 4,
               "route_request_to_delivery_wall_ms_received_only_p50": 1.0, "legion_groups": 3,
               "tick_cpp_allocation_calls": 8, "allocation_counting": False, "route_crawl_samples": None}
        self.assertEqual(screen.deterministic(row), {"hash": "abc", "arrived_settled": 4, "legion_groups": 3,
                                                     "route_crawl_samples": None})

    def test_diff_counts_changes_and_exit_status(self):
        base = {"a": {"case": "a", "hash": "1", "arrived_settled": 10},
                "b": {"case": "b", "hash": "2"}}
        same = [{"case": "a", "hash": "1", "arrived_settled": 10, "new_key": 1}, {"case": "b", "hash": "2"}]
        out = io.StringIO()
        self.assertEqual(screen.diff(base, same, "base", out=out), 0)
        self.assertEqual(screen.diff(base, same, "base", out=out, strict_keys=True), 1)
        changed = [{"case": "a", "hash": "9", "arrived_settled": 12}]
        out = io.StringIO()
        self.assertEqual(screen.diff(base, changed, "base", out=out), 3)  # 2 keys + missing case b
        text = out.getvalue()
        self.assertIn("arrived_settled: 10 -> 12 (+2) (+20.0%)", text)
        self.assertIn("b: missing", text)

    def test_rows_are_in_case_order_whatever_the_parallelism(self):
        cases = [dict(scenario=s, units=u, players=1, mode="legion", seed=0, ticks=5, turn_rate=None)
                 for s, u in (("open", 200), ("doors", 2000), ("maze", 500))]
        def fake(binary, case, cpu, timeout):
            return {"case": case["scenario"], "hash": case["scenario"] * 2}, 0.0
        with mock.patch.object(screen, "run_case", side_effect=fake):
            serial, _ = screen.run_screen("x", cases, [], 1, 10)
            parallel, _ = screen.run_screen("x", cases, [0, 1, 2], 3, 10)
        self.assertEqual(serial, parallel)
        self.assertEqual([row["case"] for row in serial], ["open", "doors", "maze"])


BOARD_PATH = Path(__file__).with_name("legion_scoreboard.py")
BOARD_SPEC = importlib.util.spec_from_file_location("legion_scoreboard", BOARD_PATH)
board = importlib.util.module_from_spec(BOARD_SPEC)
BOARD_SPEC.loader.exec_module(board)


class ScoreboardTests(unittest.TestCase):
    def test_stalled_split_and_censored_arrival(self):
        common = dict(scenario="doors", units_per_player=200, players=1, moving_percent=100, ticks=6000)
        legion = dict(common, mode="legion", arrived_settled=171, age_waiting_held_by_design_unit_ticks=900,
                      age_parked_held_by_design_unit_ticks=100, age_waiting_no_progress_unit_ticks=5,
                      age_parked_no_progress_unit_ticks=0, arrival_tick_p95=-1, spin_unit_ticks=0)
        retail = dict(common, mode="retail", arrived_settled=38, age_waiting_held_by_design_unit_ticks=0,
                      age_parked_held_by_design_unit_ticks=0, age_waiting_no_progress_unit_ticks=300,
                      age_parked_no_progress_unit_ticks=200, arrival_tick_p95=4000, spin_unit_ticks=10)
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "rows.jsonl"
            path.write_text("\n".join(json.dumps(r) for r in (legion, retail)) + "\n")
            cases = board.load([path])
        case, = cases.values()
        self.assertEqual(case["legion"]["stalled_held_by_design"], [1000])
        self.assertEqual(case["legion"]["stalled_no_progress"], [5])
        self.assertEqual(case["retail"]["stalled_no_progress"], [500])
        text, losses = board.scoreboard(cases, 0.02)
        self.assertEqual(losses, 1)   # only p95: Legion never reaches it, Retail does
        self.assertIn("arrival_tick_p95 | never | 4,000 |", text)
        self.assertIn("## Acceptance rows", text)


FINAL_PATH = Path(__file__).with_name("navigation_final_matrix.py")
FINAL_SPEC = importlib.util.spec_from_file_location("navigation_final_matrix", FINAL_PATH)
final = importlib.util.module_from_spec(FINAL_SPEC)
FINAL_SPEC.loader.exec_module(final)


class FinalMatrixTests(unittest.TestCase):
    def fake(self, binaries, calls, broken=None):
        real = subprocess.run
        def run(command, **kwargs):
            if command[0] == "taskset":
                command = command[3:]
            if command[0] not in binaries:
                return real(command, **kwargs)  # provenance/hardware probes
            calls.append(command)
            args = dict(zip(command[1::2], command[2::2]))
            data = {"mode": args["--mode"], "scenario": args["--scenario"], "workers": False,
                    "units_per_player": int(args["--units"]), "players": int(args["--players"]),
                    "moving_percent": int(args["--moving-percent"]), "seed": int(args["--seed"]),
                    "ticks": int(args["--ticks"]), "arrived_settled": 1, "moving_units": 2,
                    "crossed_middle": 2, "tick_ms_mean": 1.0 + len(calls) % 3, "tick_ms_p95": 2.0,
                    "tick_ms_p99": 3.0, "hash": "same", "all_arrived_tick": -1,
                    "route_lifecycle_balanced": True, "build_ndebug": True, "build_optimized": True,
                    "route_request_to_delivery_ticks_received_only_p95": 5}
            if broken and broken in command:
                data["build_optimized"] = False
            return subprocess.CompletedProcess(command, 0, json.dumps(data) + "\n", "")
        return run

    def invoke(self, argv, binaries, calls, broken=None):
        with mock.patch.object(sys, "argv", [str(FINAL_PATH)] + argv), mock.patch.object(
                final.subprocess, "run", side_effect=self.fake(binaries, calls, broken)), \
                contextlib.redirect_stdout(io.StringIO()):
            final.main()

    def test_timing_alternates_pairs_and_reports_both_roles(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            a, b = root / "a", root / "b"
            a.write_bytes(b"baseline")
            b.write_bytes(b"candidate")
            calls = []
            out = root / "timing"
            self.invoke(["run", "--phase", "timing", "--binary", f"baseline={a}", "--binary", f"candidate={b}",
                         "--cpus", "0", "--scenarios", "open", "doors", "--populations", "10:1:100",
                         "--modes", "retail", "--seeds", "0", "--ticks", "5", "--output", str(out)],
                        {str(a.resolve()), str(b.resolve())}, calls)
            self.assertEqual(len(calls), 12)
            self.assertFalse(any("--latency" in c for c in calls))
            # Adjacent runs are one case's pair, and the leading binary alternates.
            firsts = [calls[i][0] for i in range(0, len(calls), 2)]
            self.assertTrue(all(calls[i][1:] == calls[i + 1][1:] for i in range(0, len(calls), 2)))
            self.assertIn(str(a.resolve()), firsts)
            self.assertIn(str(b.resolve()), firsts)
            # EVERY case must be led by each binary in some repeat; reversing
            # the case order must not cancel the alternation.
            leaders = {}
            for i in range(0, len(calls), 2):
                leaders.setdefault(tuple(calls[i][1:]), set()).add(calls[i][0])
            self.assertEqual(len(leaders), 2)
            self.assertTrue(all(len(v) == 2 for v in leaders.values()), leaders)
            summary = (out / "summary.md").read_text()
            self.assertIn("candidate/baseline median", summary)
            self.assertIn("Repeated-run deterministic outcome mismatches: **0**", summary)
            self.assertTrue((out / "results.csv").read_text().startswith("phase,role,mode"))

    def test_parallel_timing_keeps_each_case_on_one_cpu_and_combine_refuses_timing(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            a, b = root / "a", root / "b"
            a.write_bytes(b"baseline")
            b.write_bytes(b"candidate")
            calls = []
            out = root / "timing"
            self.invoke(["run", "--phase", "timing", "--binary", f"baseline={a}", "--binary", f"candidate={b}",
                         "--cpus", "0-1", "--scenarios", "open", "doors", "maze", "--populations", "10:1:100",
                         "--modes", "retail", "--seeds", "0", "--ticks", "5", "--output", str(out)],
                        {str(a.resolve()), str(b.resolve())}, calls)
            self.assertEqual(len(calls), 18)
            cpus = {}
            for row in final.load(out):
                cpus.setdefault(row["scenario"], set()).add(row["cpu"])
            self.assertTrue(all(len(v) == 1 for v in cpus.values()), cpus)
            with self.assertRaises(SystemExit):
                with contextlib.redirect_stdout(io.StringIO()):
                    final.combine(root / "merged", [out, out])

    def test_outcome_uses_diagnostics_and_rejects_unoptimized_binary(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            a = root / "a"
            a.write_bytes(b"one")
            calls = []
            with self.assertRaises(SystemExit):
                self.invoke(["run", "--phase", "outcome", "--binary", f"checkpoint={a}", "--cpus", "0-1",
                             "--scenarios", "doors", "maze", "--populations", "10:1:100", "--modes", "legion",
                             "--seeds", "0", "7", "--ticks", "5", "--long-ticks", "9", "--long-population",
                             "20:1:100", "--output", str(root / "outcome")],
                            {str(a.resolve())}, calls, broken="maze")
            self.assertEqual(len(calls), 8)  # 2 scenarios x 2 seeds, plus their long cases
            self.assertTrue(all("--latency" in c and "--allocations" in c for c in calls))
            manifest = json.loads((root / "outcome" / "manifest.json").read_text())
            self.assertEqual(len(manifest["errors"]), 4)
            self.assertIn("not an optimized", manifest["errors"][0]["error"])


if __name__ == "__main__":
    unittest.main()
