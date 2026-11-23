#!/usr/bin/env python3
"""legion_baseline_reasons -- the ctest of the Legion baseline gate.

  legion_baseline_reasons_test.py [BASELINE.json]

1. Fails (exit 1) when the committed baseline (default
   tools/scenarios/baseline.json) has an entry, an exceptions entry or an
   accepted_regressions entry without a reason, an exception without a cluster
   id, or an accepted regression without a limit (tools/legion_check.py
   validate_baseline).
2. Runs the gate's unit tests on synthetic JSON (tools/legion_check.py): an eq
   violation fails, a one-sided loss beyond the band fails, an improvement
   prints RATCHET and passes, a missing reason fails this very test, a
   floor-passing key that starts failing the floor fails inside its band, a
   declared tolerance licenses its loss only for its step and key, and so on.
"""
import contextlib
import copy
import io
import json
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import legion_check as lc  # noqa: E402
import legion_w9_census as w9c  # noqa: E402

S = "scn"


def entry(value, rule="band", dir="lower", band=1.10, **kw):
    e = {"value": value, "rule": rule, "dir": dir, "reason": "test base", "since": "W0"}
    if rule == "band":
        e["band"] = band
    e.update(kw)
    return e


def baseline(keys, mode="legion", **extra):
    doc = lc.empty_baseline()
    doc["entries"] = {S: {mode: keys}}
    doc.update(extra)
    return doc


def rec(keys, mode="legion", scn=S, **kw):
    r = {"scenario": scn, "mode": mode, "ticks": 100, "units": 10, "exec": "both", "window": 1,
         "offsets": [0, 1, -1], "hash": {"0": "a", "1": "b", "-1": "c"}, "serial_eq_workers": True,
         "keys": keys, "varying": {}}
    r.update(kw)
    return r


def run_check(doc, *recs, **kw):
    table = {(r["scenario"], r["mode"]): r for r in recs}
    return lc.check(doc, table, **kw)


class Gate(unittest.TestCase):
    def test_eq_violation_fails(self):
        doc = baseline({"spins": entry(0, "eq")})
        self.assertTrue(run_check(doc, rec({"spins": 0})).ok)
        r = run_check(doc, rec({"spins": 1}))
        self.assertFalse(r.ok)
        self.assertIn("eq: expected 0, got 1", r.fails[0])

    def test_hash_and_serial_workers_are_eq(self):
        doc = baseline({"hash@0": entry("a", "eq"), "serial_eq_workers": entry(True, "eq")})
        self.assertTrue(run_check(doc, rec({})).ok)
        self.assertFalse(run_check(doc, rec({}, hash={"0": "z"})).ok)
        self.assertFalse(run_check(doc, rec({}, serial_eq_workers=False)).ok)

    def test_one_sided_loss_beyond_band_fails_lower_and_higher(self):
        doc = baseline({"stop_go": entry(1000), "g.A.arrived": entry(100, dir="higher")})
        self.assertTrue(run_check(doc, rec({"stop_go": 1100, "g.A.arrived": 91})).ok)   # inside x1.10
        r = run_check(doc, rec({"stop_go": 1101, "g.A.arrived": 90}))
        self.assertEqual(len(r.fails), 2)

    def test_a_none_marker_is_not_a_loss_against_itself(self):
        # W5 step 0: a key that reads -1 ("none": no near-wall samples, no completions) must not fail or
        # ratchet against its own -1 base, lower or higher is better, and a real loss still fails.
        doc = baseline({"clearance_mean_x100": entry(-1), "g.A.arrived": entry(-1, dir="higher")})
        r = run_check(doc, rec({"clearance_mean_x100": -1, "g.A.arrived": -1}))
        self.assertTrue(r.ok)
        self.assertEqual(r.ratchets, [])
        self.assertFalse(run_check(doc, rec({"clearance_mean_x100": 5, "g.A.arrived": -1})).ok)
        self.assertFalse(run_check(doc, rec({"clearance_mean_x100": -1, "g.A.arrived": -5})).ok)

    def test_time_key_never_is_the_worst_value(self):
        doc = baseline({"g.A.t90": entry(500)})
        self.assertFalse(run_check(doc, rec({"g.A.t90": -1})).ok)
        doc = baseline({"g.A.t90": entry(-1)})
        r = run_check(doc, rec({"g.A.t90": 800}))
        self.assertTrue(r.ok)
        self.assertEqual(len(r.ratchets), 1)

    def test_improvement_prints_ratchet_and_passes(self):
        doc = baseline({"stop_go": entry(1000)})
        r = run_check(doc, rec({"stop_go": 800}))
        self.assertTrue(r.ok)
        self.assertEqual(r.ratchets, [("scn/legion/stop_go", 1000, 800)])
        self.assertIn("RATCHET scn/legion/stop_go 1000 -> 800", r.lines())
        # within the band is not a ratchet
        self.assertEqual(run_check(doc, rec({"stop_go": 950})).ratchets, [])

    def test_ratchet_raises_the_reference_and_cumulative_drift_is_caught(self):
        doc = baseline({"stop_go": entry(1000)})
        r = run_check(doc, rec({"stop_go": 800}))
        lc.apply_ratchet(doc, r, "improved by test", "2026-01-01T00:00:00Z")
        self.assertEqual(doc["references"][S]["legion"]["stop_go"], 800)
        self.assertEqual(doc["history"][0]["reason"], "improved by test")
        self.assertEqual(doc["history"][0]["ratchets"][0]["new"], 800)
        # head base is still 1000 (per step ok at 950) but the reference is 800:
        # 800 x1.10 = 880 < 950, so the cumulative test fails
        r = run_check(doc, rec({"stop_go": 950}))
        self.assertFalse(r.ok)
        self.assertIn("cumulative", r.fails[0])
        self.assertTrue(run_check(doc, rec({"stop_go": 880})).ok)

    def test_missing_key_fails_but_missing_work_counter_reads_zero(self):
        doc = baseline({"flips": entry(5), "work.field_work.total": entry(0)})
        r = run_check(doc, rec({}))
        self.assertEqual(len(r.fails), 1)
        self.assertIn("flips", r.fails[0])

    def test_work_counters_and_unbaselined_classes(self):
        doc = baseline({"work.field_work.total": entry(1000)})
        self.assertTrue(run_check(doc, rec({"work.field_work.total": 1100})).ok)
        self.assertFalse(run_check(doc, rec({"work.field_work.total": 1101})).ok)
        r = run_check(doc, rec({"work.field_work.total": 1000, "work.convoy_tests.total": 7}))
        self.assertIn("unbaselined", r.fails[0])
        # a new class carries a declared per-unit bound
        doc["entries"][S]["legion"]["work.convoy_tests.total"] = entry(2, "bound", per="unit")
        self.assertTrue(run_check(doc, rec({"work.field_work.total": 1000, "work.convoy_tests.total": 20})).ok)
        self.assertFalse(run_check(doc, rec({"work.field_work.total": 1000, "work.convoy_tests.total": 21})).ok)

    def test_reported_work_sums_are_never_baselined_or_gated(self):
        # work.legion_all (legion_total plus the aware and lane work) is report-only (tools/legion_observe.h)
        keys = {"work.field_work.total": 1000, "work.legion_all.total": 5000, "work.legion_all.max": 70}
        doc = lc.empty_baseline()
        lc.write_base(doc, {(S, "legion"): rec(keys)}, "step 0", "W0", ["*"], False, [])
        self.assertEqual(sorted(doc["entries"][S]["legion"]), ["work.field_work.total"])
        self.assertTrue(run_check(doc, rec(dict(keys, **{"work.legion_all.total": 9000}))).ok)
        # one a baseline still carries is skipped too
        doc["entries"][S]["legion"]["work.legion_all.total"] = entry(5000)
        self.assertTrue(run_check(doc, rec(dict(keys, **{"work.legion_all.total": 9000}))).ok)

    def test_cumulative_combat_is_x1_00_of_the_w0_base(self):
        scn = "battle-field-2x60"
        key = "work.legion_total.max"
        doc = lc.empty_baseline()
        doc["entries"] = {scn: {"legion": {key: entry(900, w0=800)}}}
        r = rec({key: 850}, scn=scn)
        self.assertTrue(run_check(doc, r).ok)                      # step band alone: fine
        out = run_check(doc, r, cumulative=True)
        self.assertFalse(out.ok)
        self.assertIn("cumulative combat", out.fails[0])
        self.assertTrue(run_check(doc, rec({key: 800}, scn=scn), cumulative=True).ok)

    def test_reasons_test_fails_on_a_missing_reason(self):
        doc = baseline({"spins": entry(0, "eq")})
        self.assertEqual(lc.validate_baseline(doc), [])
        for section, item in (("exceptions", {"scenario": S, "key": "g.A.t90", "cluster": "MV-11"}),
                              ("accepted_regressions", {"scenario": S, "mode": "legion", "key": "flips", "max": 9})):
            bad = copy.deepcopy(doc)
            bad[section].append(item)
            self.assertTrue(any("no reason" in p for p in lc.validate_baseline(bad)), section)
            bad[section][0]["reason"] = "  "
            self.assertTrue(any("no reason" in p for p in lc.validate_baseline(bad)), section)
            bad[section][0]["reason"] = "because"
            self.assertFalse(any("no reason" in p for p in lc.validate_baseline(bad)), section)
        bad = copy.deepcopy(doc)
        del bad["entries"][S]["legion"]["spins"]["reason"]
        self.assertTrue(any("no reason" in p for p in lc.validate_baseline(bad)))
        bad = copy.deepcopy(doc)
        bad["exceptions"].append({"scenario": S, "key": "g.A.t90", "cluster": "UNASSIGNED", "reason": "r"})
        self.assertTrue(any("cluster" in p for p in lc.validate_baseline(bad)))
        bad = copy.deepcopy(doc)
        bad["accepted_regressions"].append({"scenario": S, "mode": "legion", "key": "k", "reason": "r"})
        self.assertTrue(any("max or factor" in p for p in lc.validate_baseline(bad)))
        bad = baseline({"flips": entry(3, band=1.5)})
        self.assertTrue(any("band" in p for p in lc.validate_baseline(bad)))

    def test_reasons_cli_exits_nonzero(self):
        doc = baseline({"spins": entry(0, "eq")}, exceptions=[{"scenario": S, "key": "k", "cluster": "C"}])
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "b.json")
            json.dump(doc, open(p, "w"))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(lc.main(["validate", "--baseline", p]), 1)
                doc["exceptions"][0]["reason"] = "named"
                json.dump(doc, open(p, "w"))
                self.assertEqual(lc.main(["validate", "--baseline", p]), 0)

    # ---- the Retail floor
    def floor_doc(self, **extra):
        return baseline({"g.A.t90": entry(1000, band=1.20)}, **extra)

    def test_floor_passing_key_that_starts_failing_fails_inside_its_band(self):
        retail = rec({"g.A.t90": 1000}, mode="retail")
        ok = run_check(self.floor_doc(), rec({"g.A.t90": 1100}), retail)
        self.assertTrue(ok.ok, ok.fails)
        r = run_check(self.floor_doc(), rec({"g.A.t90": 1150}), retail)        # inside x1.20, over retail x1.10
        self.assertFalse(r.ok)
        self.assertEqual(len(r.fails), 1)
        self.assertIn("Retail floor", r.fails[0])

    def test_floor_higher_is_better_and_never(self):
        retail = rec({"g.A.arrived": 100}, mode="retail")
        doc = baseline({"g.A.arrived": entry(100, dir="higher", band=1.20)})
        self.assertTrue(run_check(doc, rec({"g.A.arrived": 91}), retail).ok)
        self.assertFalse(run_check(doc, rec({"g.A.arrived": 90}), retail).ok)
        retail = rec({"g.A.t90": -1}, mode="retail")                          # retail never finished
        self.assertTrue(run_check(baseline({"g.A.t90": entry(5000)}), rec({"g.A.t90": 5000}), retail).ok)
        retail = rec({"g.A.t90": 1000}, mode="retail")
        self.assertFalse(run_check(self.floor_doc(), rec({"g.A.t90": -1}), retail).ok)

    def test_floor_exception_may_fail_improve_and_clears(self):
        exc = [{"scenario": S, "key": "g.A.t90", "cluster": "MV-11", "reason": "known"}]
        retail = rec({"g.A.t90": 500}, mode="retail")
        doc = self.floor_doc(exceptions=exc)
        r = run_check(doc, rec({"g.A.t90": 1000}), retail)
        self.assertTrue(r.ok)                                                   # still failing, allowed
        self.assertEqual(r.exception_count, 1)
        r = run_check(doc, rec({"g.A.t90": 700}), retail)                      # improves, still above retail
        self.assertTrue(r.ok)
        self.assertEqual(r.ratchets, [("scn/legion/g.A.t90", 1000, 700)])
        self.assertEqual(r.cleared, [])
        r = run_check(doc, rec({"g.A.t90": 540}), retail)                      # now passes
        self.assertEqual(len(r.cleared), 1)
        lc.apply_ratchet(doc, r, "cleared", "2026-01-01T00:00:00Z")
        self.assertEqual(doc["exceptions"], [])
        self.assertEqual(doc["history"][0]["cleared_exceptions"], [S + "/g.A.t90"])
        # an exception still fails the outcome rule when it gets worse
        doc = self.floor_doc(exceptions=exc)
        self.assertFalse(run_check(doc, rec({"g.A.t90": 1300}), retail).ok)

    def test_ratchet_keeps_offset_spread_exceptions(self):
        exc = [{"scenario": S, "key": "g.A.t90", "cluster": "AR-11", "mode": "legion", "median5": True,
                "reason": "3-offset spread 900..1200 exceeds the band at W2s0; gated on median-of-5 offsets"}]
        retail = rec({"g.A.t90": 500}, mode="retail")
        doc = self.floor_doc(exceptions=exc)
        r = run_check(doc, rec({"g.A.t90": 540}, offsets=[0, 1, -1, 2, -2]), retail)  # passes the floor
        self.assertEqual(r.cleared, [])                                         # never was a floor exception
        lc.apply_ratchet(doc, r, "spread stays", "2026-01-01T00:00:00Z")
        self.assertEqual(doc["exceptions"], exc)
        self.assertEqual(doc["history"][0]["cleared_exceptions"], [])

    def test_ratchet_converts_the_spread_a_cleared_floor_exception_masked(self):
        # Ruling (h), W3 final exit: a floor exception also covers its key's offset spread (exception_for
        # reads it). When the ratchet clears it, that spread must stay excepted, or the next check fails.
        exc = [{"scenario": S, "key": "g.A.t90", "cluster": "MV-11", "reason": "known"},
               {"scenario": S, "key": "g.A.t50", "cluster": "MV-11", "reason": "known"}]
        doc = baseline({"g.A.t90": entry(1000, band=1.20), "g.A.t50": entry(1000, band=1.20)}, exceptions=exc)
        retail = rec({"g.A.t90": 500, "g.A.t50": 500}, mode="retail")
        five = [0, 1, -1, 2, -2]
        legion = rec({"g.A.t90": 540, "g.A.t50": 540}, offsets=five,
                     varying={"g.A.t90": [540, 900, 540, 400, 540]})   # spread 400..900; t50 steady
        r = run_check(doc, legion, retail)
        self.assertTrue(r.ok, r.fails)
        self.assertEqual(len(r.cleared), 1)                     # t90 passes the floor now; t50 is no floor key
        self.assertEqual(sorted(r.masked), [(S, "legion", "g.A.t90")])
        lc.apply_ratchet(doc, r, "cleared", "2026-01-01T00:00:00Z")
        self.assertEqual(doc["history"][0]["cleared_exceptions"], [S + "/g.A.t90"])
        spread = [x for x in doc["exceptions"] if x["key"] == "g.A.t90"]
        self.assertEqual(len(spread), 1)
        self.assertTrue(lc.is_spread_exception(spread[0]), spread[0])
        self.assertEqual((spread[0]["mode"], spread[0]["cluster"]), ("legion", "MV-11"))
        self.assertEqual(lc.validate_baseline(doc), [])
        r = run_check(doc, legion, retail)                      # the unmasked spread would fail here
        self.assertTrue(r.ok, r.fails)
        self.assertEqual(r.cleared, [])
        # a cleared floor exception whose key is steady leaves nothing behind
        doc = baseline({"g.A.t90": entry(1000, band=1.20)}, exceptions=[dict(exc[0])])
        r = run_check(doc, rec({"g.A.t90": 540}, offsets=five), retail)
        lc.apply_ratchet(doc, r, "cleared", "2026-01-01T00:00:00Z")
        self.assertEqual(doc["exceptions"], [])

    def test_one_body_groups_of_a_click_are_judged_at_click_level(self):
        # Ruling (f), W3 final exit: a one-body group inside a multi-body click (g.X.click_n > 1) is
        # report-only on arrived / t50 / t90 / done; the click's keys carry the band and the floor.
        doc = baseline({"g.a.t90": entry(1000), "g.a.arrived": entry(1, dir="higher"),
                        "click.a.t90": entry(1000), "g.p.t90": entry(1000)})
        lk = {"g.a.n": 1, "g.a.click_n": 24, "g.a.t90": -1, "g.a.arrived": 0, "click.a.n": 24,
              "click.a.t90": 1050, "g.p.n": 20, "g.p.t90": 1000}
        rk = {"g.a.n": 1, "g.a.click_n": 24, "g.a.t90": 900, "g.a.arrived": 1, "click.a.n": 24,
              "click.a.t90": 1000, "g.p.n": 20, "g.p.t90": 1000}
        r = run_check(doc, rec(lk), rec(rk, mode="retail"))
        self.assertTrue(r.ok, r.fails)                          # never / 0 on the body: report-only
        self.assertEqual(r.report_only, 2)                      # g.a.t90 and g.a.arrived on the floor
        self.assertTrue(lc.is_report_only("g.a.done", rec(lk)))
        self.assertFalse(lc.is_report_only("g.a.complete_n", rec(lk)))
        self.assertFalse(lc.is_report_only("g.p.t90", rec(lk)))      # twenty bodies: its own group
        # the click is gated: its band and the Retail floor
        r = run_check(doc, rec(dict(lk, **{"click.a.t90": 1300})), rec(rk, mode="retail"))
        self.assertFalse(r.ok)
        self.assertTrue(any("click.a.t90" in f and "Retail floor" in f for f in r.fails), r.fails)
        # a group alone (no click_n) keeps its floor
        alone = {k: v for k, v in lk.items() if k != "g.a.click_n"}
        r = run_check(doc, rec(alone), rec({k: v for k, v in rk.items() if k != "g.a.click_n"}, mode="retail"))
        self.assertFalse(r.ok)
        # a retake takes no entry and no floor exception for the per-body keys
        base = lc.empty_baseline()
        lc.write_base(base, {(S, "legion"): rec(lk), (S, "retail"): rec(rk, mode="retail")}, "step 0", "W0",
                      ["*"], False, [("*", "MV-12")])
        self.assertNotIn("g.a.t90", base["entries"][S]["legion"])
        self.assertIn("click.a.t90", base["entries"][S]["legion"])
        self.assertEqual(base["exceptions"], [])

    def test_offset_spread_exception_is_not_a_floor_exception(self):
        # Ruling (c), 2026-10-09: a spread exception gates the key on median-of-5; it never
        # licenses the Retail floor (it hid wall-4x50 and motion-cross floor failures).
        for reason in ("3-offset spread 900..1200 exceeds the band at W2s0; gated on median-of-5 offsets",
                       "offset spread 0..9 exceeds the 1.20 band at the W3-1 retake"):
            exc = [{"scenario": S, "key": "g.A.t90", "cluster": "offset-spread", "mode": "legion",
                    "median5": True, "reason": reason}]
            retail = rec({"g.A.t90": 500}, mode="retail")
            r = run_check(self.floor_doc(exceptions=exc), rec({"g.A.t90": 1000}, offsets=[0, 1, -1, 2, -2]),
                          retail)
            self.assertFalse(r.ok)
            self.assertEqual(r.exception_count, 0)
            self.assertTrue(any("Retail floor" in f for f in r.fails), r.fails)
        # beside a real floor exception for the same key, the floor exception is the one read
        both = [{"scenario": S, "key": "g.A.t90", "cluster": "offset-spread", "mode": "legion", "median5": True,
                 "reason": "offset spread 0..9 exceeds the band"},
                {"scenario": S, "key": "g.A.t90", "cluster": "MV-11", "reason": "known"}]
        r = run_check(self.floor_doc(exceptions=both), rec({"g.A.t90": 1000}, offsets=[0, 1, -1, 2, -2]),
                      rec({"g.A.t90": 500}, mode="retail"))
        self.assertTrue(r.ok, r.fails)
        self.assertEqual(r.exception_count, 1)
        # a retake does not take a spread exception for a floor exception either
        doc = lc.empty_baseline()
        doc["exceptions"] = [dict(both[0])]
        run = {(S, "legion"): rec({"g.A.t90": 1000}), (S, "retail"): rec({"g.A.t90": 500}, mode="retail")}
        lc.write_base(doc, run, "step 0", "W0", ["*"], False, [("*", "MV-11")])
        self.assertEqual(sorted(x["cluster"] for x in doc["exceptions"]), ["MV-11", "offset-spread"])

    def wide_rec(self, mode="legion", **keys):
        """An eleven-offset record: per-offset values in WIDE_OFFSETS order."""
        offs = list(lc.WIDE_OFFSETS)
        r = rec({k: v[0] for k, v in keys.items()}, mode=mode, offsets=offs,
                hash={str(o): "h" for o in offs}, varying={k: v for k, v in keys.items() if len(set(v)) > 1})
        return lc.regate(r)

    def test_small_count_keys_read_eleven_offsets_others_the_core_five(self):
        crossings = [0, 0, 1, 0, 1, 9, 8, 7, 9, 8, 9]     # core five median 0, eleven median 7
        flips = [5, 5, 5, 5, 5, 50, 50, 50, 50, 50, 50]     # core five 5, eleven 50
        r = self.wide_rec(**{"gate.top.crossings": crossings, "flips": flips, "g.A.n": [1] * 11,
                             "g.A.t90": [100, 100, 100, 100, 100, -1, -1, -1, -1, -1, -1],
                             "g.B.n": [40] * 11, "g.B.t90": [100, 100, 100, 100, 100, -1, -1, -1, -1, -1, -1]})
        self.assertTrue(r["wide"])
        self.assertEqual(r["keys"]["gate.top.crossings"], 7)
        self.assertEqual(r["keys"]["flips"], 5)
        self.assertEqual(r["keys"]["g.A.t90"], -1)        # one body: eleven offsets, six of them never
        self.assertEqual(r["keys"]["g.B.t90"], 100)       # forty bodies: the core five
        self.assertNotIn("flips", r["varying"])
        self.assertEqual(len(r["varying"]["gate.top.crossings"]), 11)
        doc = baseline({"gate.top.crossings": entry(7, band=1.20), "flips": entry(5)})
        self.assertTrue(run_check(doc, r).ok, run_check(doc, r).fails)    # its 0..9 spread needs no exception
        # five offsets are not enough for a gated small-count key
        five = rec({"gate.top.crossings": 0, "flips": 5}, offsets=[0, 1, -1, 2, -2])
        f = run_check(doc, lc.regate(five))
        self.assertFalse(f.ok)
        self.assertTrue(any("small-count" in x for x in f.fails), f.fails)
        # the Retail floor reads the eleven too (Retail 0..10 on corner crossings)
        retail = self.wide_rec(mode="retail", **{"gate.top.crossings": [0, 0, 0, 0, 0, 9, 9, 9, 9, 9, 9],
                                                 "flips": flips})
        self.assertEqual(retail["keys"]["gate.top.crossings"], 9)
        self.assertTrue(run_check(baseline({"gate.top.crossings": entry(7, band=1.20)}), r, retail).ok)

    def test_w8_1_gap_fixtures_judge_lane_crossings_per_file(self):
        # User decision W8-1 (2026-10-10): the passage gate takes a gap several abreast, so a GAP
        # fixture's gate.X.crossings is judged as crossings / files against Retail's crossings / files.
        def pair(scn, lcross, lfiles, rcross, rfiles, key="gate.top"):
            l = self.wide_rec(**{key + ".crossings": [lcross] * 11, key + ".files_x100": [lfiles] * 11})
            r = self.wide_rec(mode="retail", **{key + ".crossings": [rcross] * 11, key + ".files_x100": [rfiles] * 11})
            l["scenario"] = r["scenario"] = scn
            return l, r
        doc = lambda scn: dict(lc.empty_baseline(), entries={scn: {"legion": {}}})
        # 10 crossings over 2.5 abreast files (4 per file) against Retail 8 over 1.6 (5 per file): passes
        # per file though 10 > 8 x 1.1 raw
        l, r = pair("gap6", 10, 250, 8, 160)
        res = run_check(doc("gap6"), l, r)
        self.assertTrue(res.ok, res.fails)
        # the same counts on a fixture that is not a GAP fixture keep the raw floor
        l, r = pair("corner-8x56", 10, 250, 8, 160)
        res = run_check(doc("corner-8x56"), l, r)
        self.assertFalse(res.ok)
        self.assertTrue(any("Retail floor: legion 10" in f for f in res.fails), res.fails)
        # over the per-file bound it still fails, and says so
        l, r = pair("gapsweep", 30, 250, 8, 160, key="gate.g14")
        res = run_check(doc("gapsweep"), l, r)
        self.assertFalse(res.ok)
        self.assertTrue(any("per file (W8-1)" in f for f in res.fails), res.fails)
        # a mean under one file reads as one: 5 crossings, 0.4 files each side is 5 vs 5
        l, r = pair("gap8", 5, 40, 5, 40)
        self.assertTrue(run_check(doc("gap8"), l, r).ok)
        # per-file passes clear a floor exception, naming the per-file values
        l, r = pair("gap6", 10, 250, 8, 160)
        d = doc("gap6")
        d["exceptions"] = [{"scenario": "gap6", "key": "gate.top.crossings", "cluster": "MV-10",
                            "reason": "raw count over Retail", "median5": True}]
        res = run_check(d, l, r)
        self.assertTrue(res.ok, res.fails)
        self.assertEqual(len(res.cleared), 1)
        self.assertIn("per file (W8-1)", res.cleared[0])
        # without a files key on either side the raw floor stays
        l, r = pair("gap6", 10, 250, 8, 160)
        del l["keys"]["gate.top.files_x100"]
        self.assertFalse(run_check(doc("gap6"), l, r).ok)
        # a retake agrees with the check: a per-file pass takes no exception
        l, r = pair("gap6", 10, 250, 8, 160)
        d = lc.empty_baseline()
        lc.write_base(d, {("gap6", "legion"): l, ("gap6", "retail"): r}, "step 0", "W8", ["*"], False, [("*", "MV-02")])
        self.assertFalse(any(x["key"] == "gate.top.crossings" for x in d["exceptions"]), d["exceptions"])

    def test_w9_u1_tolerance_on_small_counts(self):
        # User decision W9-U1 (2026-10-10): on corner-1x448 gate.top.crossings and tail-corner380
        # wall_touch_near_permille the eleven-offset median passes when legion <= max(retail x1.1, retail + 1).
        def pair(scn, key, lvals, rvals):
            l = self.wide_rec(**{key: lvals})
            r = self.wide_rec(mode="retail", **{key: rvals})
            l["scenario"] = r["scenario"] = scn
            return l, r
        doc = lambda scn: dict(lc.empty_baseline(), entries={scn: {"legion": {}}})
        zero = [0] * 11
        # Retail 0: one event passes, two fail (x1.1 of 0 is 0 without U1)
        l, r = pair("corner-1x448", "gate.top.crossings", [1] * 11, zero)
        res = run_check(doc("corner-1x448"), l, r)
        self.assertTrue(res.ok, res.fails)
        self.assertTrue(any("W9-U1 tolerance" in i for i in res.info), res.info)
        l, r = pair("corner-1x448", "gate.top.crossings", [2] * 11, zero)
        res = run_check(doc("corner-1x448"), l, r)
        self.assertFalse(res.ok)
        self.assertTrue(any("Retail floor" in f for f in res.fails), res.fails)
        # the median of the eleven decides, not one offset: four of eleven at 4 events leave the median at 1
        l, r = pair("corner-1x448", "gate.top.crossings", [1, 1, 1, 1, 1, 1, 1, 4, 4, 4, 4], zero)
        self.assertTrue(run_check(doc("corner-1x448"), l, r).ok)
        # Retail 1: legion 2 passes (retail + 1), legion 3 fails
        l, r = pair("corner-1x448", "gate.top.crossings", [2] * 11, [1] * 11)
        self.assertTrue(run_check(doc("corner-1x448"), l, r).ok)
        l, r = pair("corner-1x448", "gate.top.crossings", [3] * 11, [1] * 11)
        self.assertFalse(run_check(doc("corner-1x448"), l, r).ok)
        # Retail 40: the plain x1.1 bound is wider than retail + 1 and still applies (44 passes, 46 fails)
        l, r = pair("corner-1x448", "gate.top.crossings", [44] * 11, [40] * 11)
        self.assertTrue(run_check(doc("corner-1x448"), l, r).ok)
        l, r = pair("corner-1x448", "gate.top.crossings", [46] * 11, [40] * 11)
        self.assertFalse(run_check(doc("corner-1x448"), l, r).ok)
        # tail-corner380 near-wall permille: Retail 1, legion 2 passes, legion 3 fails
        l, r = pair("tail-corner380", "wall_touch_near_permille", [2] * 11, [1] * 11)
        self.assertTrue(run_check(doc("tail-corner380"), l, r).ok)
        l, r = pair("tail-corner380", "wall_touch_near_permille", [3] * 11, [1] * 11)
        self.assertFalse(run_check(doc("tail-corner380"), l, r).ok)
        # the tolerance is for those keys only: another fixture's crossings, and another key, keep x1.1
        l, r = pair("corner-8x56", "gate.top.crossings", [1] * 11, zero)
        self.assertFalse(run_check(doc("corner-8x56"), l, r).ok)
        l, r = pair("tail-corner380", "wall_touch_permille", [2] * 11, [1] * 11)
        self.assertFalse(run_check(doc("tail-corner380"), l, r).ok)
        # a floor exception clears when the tolerance passes (and stays when it does not)
        l, r = pair("corner-1x448", "gate.top.crossings", [1] * 11, zero)
        d = doc("corner-1x448")
        d["exceptions"] = [{"scenario": "corner-1x448", "key": "gate.top.crossings", "cluster": "MV-02",
                            "reason": "lane crossings over Retail", "median5": True}]
        res = run_check(d, l, r)
        self.assertTrue(res.ok, res.fails)
        self.assertEqual(len(res.cleared), 1)

    def test_w9_step0_splits_are_report_only(self):
        # The wall-touch places, the still samples and the flip-site counts are instruments, not Retail-floor keys.
        for k in ("wall_touch_gate_permille", "wall_touch_ring_permille", "wall_touch_near_corner_permille",
                  "wall_touch_flat_permille", "wall_touch_still_permille", "gate.top.cross_pass", "gate.top.crossings_per_file_x100"):
            self.assertIsNone(lc.floor_dir(k), k)
        self.assertEqual(lc.floor_dir("wall_touch_permille"), "lower")
        self.assertEqual(lc.floor_dir("wall_touch_near_permille"), "lower")
        self.assertEqual(lc.floor_dir("gate.top.crossings"), "lower")
        legion = self.wide_rec(**{"wall_touch_gate_permille": [40] * 11, "wall_touch_permille": [10] * 11})
        retail = self.wide_rec(mode="retail", **{"wall_touch_gate_permille": [1] * 11, "wall_touch_permille": [10] * 11})
        res = run_check(dict(lc.empty_baseline(), entries={S: {"legion": {}}}), legion, retail)
        self.assertTrue(res.ok, res.fails)

    def test_w9_gap_per_file_key_is_the_median_of_per_offset_ratios(self):
        # W9 step 0: gate.X.crossings_per_file_x100 = crossings x 10000 / max(100, files_x100) at each offset,
        # medianed over the eleven -- not the ratio of the two medians.
        cross = [4, 4, 4, 4, 4, 4, 30, 30, 30, 30, 30]       # median 4
        files = [200, 200, 200, 200, 200, 200, 600, 600, 600, 600, 600]   # median 200
        l = self.wide_rec(**{"gate.top.crossings": cross, "gate.top.files_x100": files})
        l["scenario"] = "gap6"
        l2 = lc.regate(dict(l, keys=l["all_keys"], varying=l["all_varying"], scenario="gap6"))
        self.assertEqual(l2["keys"]["gate.top.crossings_per_file_x100"], 200)      # 6 offsets at 2.0, 5 at 0.5 -> median 2.0
        self.assertTrue(l2["wide"])
        # a mean under one file reads as one file: 5 crossings over 0.4 files is 5.0
        z = self.wide_rec(**{"gate.top.crossings": [5] * 11, "gate.top.files_x100": [40] * 11})
        z2 = lc.regate(dict(z, keys=z["all_keys"], varying=z["all_varying"], scenario="gap8"))
        self.assertEqual(z2["keys"]["gate.top.crossings_per_file_x100"], 500)
        # a fixture that is not a GAP fixture gets no such key
        n = self.wide_rec(**{"gate.top.crossings": [5] * 11, "gate.top.files_x100": [40] * 11})
        n2 = lc.regate(dict(n, keys=n["all_keys"], varying=n["all_varying"], scenario="corner-8x56"))
        self.assertNotIn("gate.top.crossings_per_file_x100", n2["keys"])
        # U1 on the per-file key: legion 1.4 per file vs Retail 0 passes, 2.5 vs 0 fails, and the verdict names it
        def pair(lcross, rcross, files=100):
            a = self.wide_rec(**{"gate.top.crossings": [lcross] * 11, "gate.top.files_x100": [files] * 11})
            b = self.wide_rec(mode="retail", **{"gate.top.crossings": [rcross] * 11, "gate.top.files_x100": [files] * 11})
            a["scenario"] = b["scenario"] = "gap6"
            return a, b
        doc = dict(lc.empty_baseline(), entries={"gap6": {"legion": {}}})
        a, b = pair(1, 0)
        res = run_check(doc, a, b)
        self.assertTrue(res.ok, res.fails)
        self.assertTrue(any(i.startswith("gap per file (W8-1) gap6/legion/gate.top.crossings") for i in res.info), res.info)
        a, b = pair(2, 0)
        res = run_check(doc, a, b)
        self.assertFalse(res.ok)
        self.assertTrue(any("per file (W8-1)" in f for f in res.fails), res.fails)

    def test_w9_census_gate_flags_a_moved_scenario_its_census_excludes(self):
        # W9 step 0: a step may move only the scenarios its predicates fire on (tools/legion_w9_census.py).
        census = {"a": {"b2_lane_differs": 3}, "b": {"b2_lane_differs": 0, "b3_fold_binds": 9}, "c": {}}
        self.assertEqual(w9c.covered(census, "B2"), {"a"})
        self.assertEqual(w9c.covered(census, "B3"), {"b"})
        base = {"a": "h1", "b": "h2", "c": "h3"}
        self.assertEqual(w9c.moved(census, "B2", base, dict(base, a="x")), [])           # a is covered
        self.assertEqual(w9c.moved(census, "B2", base, dict(base, a="x", b="y")), ["b"])   # b is not
        self.assertEqual(w9c.moved(census, "B2", base, base), [])
        # the committed census: the flowing controls read 0 for B2 / B3 fold / C1, and A2 / B1 fire nowhere at offset 0
        committed = w9c.load_census(w9c.DEFAULT)
        self.assertEqual(w9c.controls_clean(committed), [])
        self.assertEqual(w9c.controls_clean(dict(committed, uturn={"b3_fold_binds": 1})), ["uturn: b3_fold_binds = 1"])

    def test_contact_settled_floor_needs_ten_arrivals_in_both_modes(self):
        doc = baseline({"contact_settled_permille": entry(100, band=1.20)})
        few = {"contact_settled_permille": 100, "g.A.arrived": 3}
        retail = rec({"contact_settled_permille": 10, "g.A.arrived": 3}, mode="retail")
        self.assertTrue(run_check(doc, rec(few), retail).ok)                     # 3 arrive: noise, no floor
        many = {"contact_settled_permille": 100, "g.A.arrived": 6, "g.B.arrived": 6}
        retail = rec({"contact_settled_permille": 10, "g.A.arrived": 6, "g.B.arrived": 6}, mode="retail")
        r = run_check(doc, rec(many), retail)                                   # 12 arrive in both
        self.assertFalse(r.ok)
        self.assertIn("Retail floor", r.fails[0])
        few_retail = rec({"contact_settled_permille": 10, "g.A.arrived": 9, "g.B.arrived": 0}, mode="retail")
        r = run_check(doc, rec(dict(many, **{"g.A.arrived": 9, "g.B.arrived": 0})), few_retail)
        self.assertTrue(r.ok, r.fails)                                          # 9 arrive: no floor
        # the band still applies
        self.assertFalse(run_check(doc, rec(dict(few, contact_settled_permille=130)),
                                   rec({"contact_settled_permille": 10, "g.A.arrived": 3}, mode="retail")).ok)

    def test_median5_exception_needs_five_offsets(self):
        exc = [{"scenario": S, "key": "g.A.t90", "cluster": "MV-11", "reason": "noisy", "median5": True}]
        retail = rec({"g.A.t90": 500}, mode="retail")
        doc = self.floor_doc(exceptions=exc)
        self.assertFalse(run_check(doc, rec({"g.A.t90": 1000}), retail).ok)
        five = rec({"g.A.t90": 1000}, offsets=[0, 1, -1, 2, -2])
        self.assertTrue(run_check(doc, five, retail).ok)

    def test_spread_beyond_band_must_be_an_exception(self):
        doc = baseline({"flips": entry(100)})
        wide = rec({"flips": 100}, varying={"flips": [100, 140, 100]})
        r = run_check(doc, wide)
        self.assertFalse(r.ok)
        self.assertIn("spread", r.fails[0])
        doc["exceptions"].append({"scenario": S, "key": "flips", "cluster": "C", "reason": "noisy"})
        self.assertTrue(run_check(doc, wide).ok)
        self.assertTrue(run_check(baseline({"flips": entry(100)}),
                                  rec({"flips": 100}, varying={"flips": [100, 105, 100]})).ok)

    def test_retake_lists_spread_keys_as_median5_exceptions(self):
        doc = lc.empty_baseline()
        run = {(S, "legion"): rec({"flips": 100, "stable": 5}, varying={"flips": [100, 140, 100]})}
        lc.write_base(doc, run, "step 0", "W0", ["*"], False, [])
        self.assertEqual([(e["key"], e["median5"], e["cluster"]) for e in doc["exceptions"]],
                         [("flips", True, "offset-spread")])
        self.assertEqual(lc.validate_baseline(doc), [])
        table = dict(run)
        self.assertFalse(lc.check(doc, table).ok)                               # three offsets are not enough
        table[(S, "legion")] = rec({"flips": 100, "stable": 5}, varying={"flips": [100, 140, 100]},
                                   offsets=[0, 1, -1, 2, -2])
        self.assertTrue(lc.check(doc, table).ok)

    # ---- declared tolerances
    def tol_doc(self, **tol):
        a = {"scenario": S, "mode": "legion", "key": "spins_row", "step": "W5-MV06", "factor": 1.05,
             "reason": "declared tolerance"}
        a.update(tol)
        return baseline({"spins_row": entry(1000, band=1.0), "other": entry(1000, band=1.0)},
                        accepted_regressions=[a])

    def test_tolerance_licenses_only_its_step_and_key(self):
        doc = self.tol_doc()
        keys = {"spins_row": 1040, "other": 1000}
        r = run_check(doc, rec(keys), step="W5-MV06")
        self.assertTrue(r.ok, r.fails)
        self.assertEqual(len(r.licensed), 1)
        self.assertFalse(run_check(doc, rec(keys), step="W6-X").ok)            # another step
        self.assertFalse(run_check(doc, rec(keys)).ok)                         # no step named
        self.assertFalse(run_check(doc, rec({"spins_row": 1000, "other": 1040}), step="W5-MV06").ok)  # other key
        self.assertFalse(run_check(doc, rec({"spins_row": 1060, "other": 1000}), step="W5-MV06").ok)  # beyond x1.05

    def test_permanent_accepted_regression_with_absolute_max(self):
        doc = self.tol_doc(step=None, max=1200)
        del doc["accepted_regressions"][0]["step"], doc["accepted_regressions"][0]["factor"]
        self.assertTrue(run_check(doc, rec({"spins_row": 1200, "other": 1000})).ok)
        self.assertFalse(run_check(doc, rec({"spins_row": 1201, "other": 1000})).ok)

    def test_tolerance_never_licenses_a_floor_failure(self):
        doc = baseline({"g.A.t90": entry(1000, band=1.0)},
                       accepted_regressions=[{"scenario": S, "mode": "legion", "key": "g.A.t90", "step": "W5",
                                              "factor": 1.2, "reason": "declared"}])
        retail = rec({"g.A.t90": 1000}, mode="retail")
        self.assertTrue(run_check(doc, rec({"g.A.t90": 1100}), retail, step="W5").ok)
        r = run_check(doc, rec({"g.A.t90": 1150}), retail, step="W5")           # licensed by the tolerance, over the floor
        self.assertFalse(r.ok)
        self.assertEqual(len(r.licensed), 1)
        self.assertIn("Retail floor", r.fails[0])

    def test_missing_scenario_is_skipped_unless_required(self):
        doc = baseline({"flips": entry(1)})
        self.assertTrue(lc.check(doc, {}).ok)
        self.assertFalse(lc.check(doc, {}, require_all=True).ok)
        self.assertTrue(lc.check(doc, {}, skipped=[S], require_all=False).ok)


class Cli(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.TemporaryDirectory()
        self.addCleanup(self.d.cleanup)

    def path(self, name):
        return os.path.join(self.d.name, name)

    def write_run(self, name, *recs):
        with open(self.path(name), "w") as f:
            for r in recs:
                f.write(json.dumps(r) + "\n")
        return self.path(name)

    def call(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = lc.main(list(argv))
        return code, out.getvalue()

    def test_baseline_retake_check_and_ratchet_end_to_end(self):
        run = self.write_run("run.jsonl",
                             rec({"spins": 0, "g.A.t90": 900, "g.A.arrived": 40, "work.field_work.total": 500}),
                             rec({"spins": 4, "g.A.t90": 400, "g.A.arrived": 40}, mode="retail"))
        b = self.path("baseline.json")
        code, out = self.call("baseline", "--baseline", b, "--results", run, "--reason", "first base",
                              "--hashes", "--cluster", "scn/g.A.t90=MV-11")
        self.assertEqual(code, 0, out)
        doc = json.load(open(b))
        self.assertEqual(doc["entries"][S]["legion"]["spins"]["rule"], "eq")
        self.assertEqual(doc["entries"][S]["legion"]["hash@0"]["value"], "a")
        self.assertEqual(doc["entries"][S]["legion"]["g.A.arrived"]["dir"], "higher")
        self.assertEqual([(e["key"], e["cluster"]) for e in doc["exceptions"]], [("g.A.t90", "MV-11")])
        code, out = self.call("check", "--baseline", b, "--results", run)
        self.assertEqual(code, 0, out)
        self.assertIn("PASS", out)
        # an improvement: RATCHET printed, exit 0, nothing written without --ratchet
        better = self.write_run("better.jsonl",
                                rec({"spins": 0, "g.A.t90": 600, "g.A.arrived": 40, "work.field_work.total": 500}),
                                rec({"spins": 4, "g.A.t90": 400, "g.A.arrived": 40}, mode="retail"))
        code, out = self.call("check", "--baseline", b, "--results", better)
        self.assertEqual(code, 0, out)
        self.assertIn("RATCHET scn/legion/g.A.t90 900 -> 600", out)
        self.assertEqual(json.load(open(b)).get("history"), [])
        code, out = self.call("check", "--baseline", b, "--results", better, "--ratchet", "t90 faster",
                              "--date", "2026-10-08T00:00:00Z")
        self.assertEqual(code, 0, out)
        doc = json.load(open(b))
        self.assertEqual(doc["references"][S]["legion"]["g.A.t90"], 600)
        self.assertEqual(doc["history"][0]["when"], "2026-10-08T00:00:00Z")
        self.assertEqual(doc["history"][0]["reason"], "t90 faster")
        # a failing run does not ratchet
        worse_run = self.write_run("worse.jsonl", rec({"spins": 2, "g.A.t90": 600, "g.A.arrived": 40}),
                                   rec({"spins": 4, "g.A.t90": 400, "g.A.arrived": 40}, mode="retail"))
        code, out = self.call("check", "--baseline", b, "--results", worse_run, "--ratchet", "no")
        self.assertEqual(code, 1)
        self.assertIn("not ratcheting", out)
        self.assertEqual(len(json.load(open(b))["history"]), 1)

    def test_exit_table_marks_intended_and_incidental(self):
        old = self.write_run("old.jsonl", rec({"a": 100, "b": 100, "c": 100, "d": 0}))
        new = self.write_run("new.jsonl", rec({"a": 50, "b": 104, "c": 120, "d": 3}))
        code, out = self.call("exit-table", "--baseline", self.path("none.json"), "--results", new,
                              "--base", old, "--intended", "scn/legion/a")
        self.assertEqual(code, 0)
        self.assertIn("| scn/legion/a | 100 | 50 | -50.0% | intended |", out)
        self.assertIn("| scn/legion/c | 100 | 120 | +20.0% | incidental (within band) |", out)
        self.assertIn("| scn/legion/d | 0 | 3 | new | incidental (within band) |", out)
        self.assertNotIn("scn/legion/b", out)                                  # 4% is not a move
        self.assertIn("3 keys moved by more than 5%; 1 intended, 2 incidental", out)

    def test_exit_table_reads_a_spread_bound_at_its_median(self):
        base = self.path("base.json")
        json.dump({"entries": {S: {"legion": {
            "a": {"rule": "bound", "dir": "lower", "value": 663,
                  "reason": "W0: offset spread 1.21 exceeds band 1.20 ...: one-sided bound at median 552 x1.20"},
            "b": {"rule": "bound", "dir": "lower", "value": 36, "per": "unit", "reason": "declared bound"},
            "c": {"rule": "band", "dir": "lower", "value": 100, "reason": "r"}}}}}, open(base, "w"))
        new = self.write_run("new.jsonl", rec({"a": 552, "b": 9, "c": 130}))
        code, out = self.call("exit-table", "--baseline", self.path("none.json"), "--results", new, "--base", base)
        self.assertEqual(code, 0)
        self.assertNotIn("scn/legion/a", out)                                  # median 552 -> 552
        self.assertNotIn("scn/legion/b", out)                                  # a limit, not a base
        self.assertIn("| scn/legion/c | 100 | 130 | +30.0% |", out)
        self.assertIn("1 keys moved by more than 5%", out)

    def test_anchor_diff_prints_cumulative_drift_and_never_gates(self):
        anchor = self.path("anchor.json")
        json.dump({"entries": {S: {"legion": {"flips": 100, "g.A.arrived": 50, "same": 7}}}}, open(anchor, "w"))
        run = self.write_run("run.jsonl", rec({"flips": 130, "g.A.arrived": 60, "same": 7}))
        code, out = self.call("anchor", "--anchor", anchor, "--results", run)
        self.assertEqual(code, 0)
        self.assertIn("scn/legion/flips", out)
        self.assertIn("(+30.0%) worse", out)
        self.assertIn("(+20.0%) better", out)
        self.assertIn("2 of 3 common keys differ from the anchor; 1 worse, 1 better", out)

    def test_committed_baseline_is_valid(self):
        p = os.path.join(HERE, "scenarios", "baseline.json")
        if not os.path.exists(p):
            self.skipTest("no committed baseline")
        self.assertEqual(lc.validate_baseline(json.load(open(p))), [])


def main():
    path = None
    argv = sys.argv[1:]
    if argv and not argv[0].startswith("-"):
        path = argv.pop(0)
    if path is None:
        path = os.path.join(HERE, "scenarios", "baseline.json")
    bad = lc.validate_baseline(lc.load_json(path)) if os.path.exists(path) else ["no baseline at " + path]
    for b in bad:
        print("FAIL " + b)
    print("%s: %d baseline problems in %s" % ("FAIL" if bad else "ok", len(bad), path))
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    res = unittest.TextTestRunner(verbosity=1).run(suite)
    return 0 if not bad and res.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
