#!/usr/bin/env python3
"""legion_cost -- the Legion work-scaling gate and the battle window tables.

Deterministic counters only (LegionNavigator::Stats through the scenario
runner's Work section); wall time is printed by the runner and never gated.

  legion_cost.py [scaling]   (default; the `legion_cost` ctest)
      Runs tools/scenarios/cost-corner.scn and cost-open.scn at N0 = 100, 200
      and 400 bodies (only the COUNT of group A is rewritten) and checks, per
      scenario:
        1. steady state: W(N) = total Legion work per member-tick over the
           steady window (cost-*.scn `# legion_cost steady A B`, all bodies
           still travelling); W(4*N0) / W(N0) <= 1.5. The whole run's W
           (settling included) is held to the same 1.5.
        2. slot assignment: slot_search_cells per assigned slot (every body of
           the selection is assigned a formation slot), N = 400 over N = 100,
           <= 2.2.
        3. upkeep per live group: (group_loop_iters + share_scan_iters) per
           (peak Legion groups x tick) over the steady window stays under UPKEEP_CAP, and 400 over 100
           stays <= UPKEEP_RATIO.
        4. every counter against tools/legion_cost_baseline.json.
      The full run is made with serial AND workers executions twice over
      (the runner fails unless they agree; the two JSON lines must be equal).
  legion_cost.py battle FILE.scn...
      The battle windows (`# legion_cost window NAME START END` in the file):
      the scenario is run to each window's end (a world run is a prefix of a
      longer one) and the window's work is the difference of the cumulative
      totals. Prints a table, checks two identical runs, and checks the
      window counters and the whole-run per-tick max and p99 against the
      baseline. battle-assault also requires its contact windows to spend at
      least 4x the field work per tick of its march windows (PF-02 bursts).
  Options: --runner PATH (legion_scenario), --scn-dir DIR, --baseline FILE,
      --strict (bands become eq: for hash-identical steps),
      --cumulative (battle: total, max and p99 at or below the W0 base, x1.00;
      the combat check of PLAN 3.0 from the W4 exit on),
      --update-baseline --reason TEXT (write new or changed entries, each
      carrying TEXT), --jobs N, --tmpdir DIR.
  Exit: 0 ok, 1 a gate failed, 2 usage / tooling error, 77 runner missing.

Baseline entries: {value, rule: eq|band, dir: le|ge, band, reason, since}.
`le` fails only when the key gets worse (one-sided, PLAN 3.0); a key that
improves by more than the band prints RATCHET. Every entry needs a reason; a
counter that moves and has no entry fails ("unbaselined"), so a step that adds
a counter class adds its entry with a declared reason.
"""
import argparse
import concurrent.futures as cf
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCALES = (100, 200, 400)
RATIO_STEADY = 1.5     # W(4N0)/W(N0)
RATIO_SLOTS = 2.2      # slot-assignment work per assigned slot, 100 -> 400
UPKEEP_RATIO = 1.2     # upkeep per live group-tick, 400 over 100 (measured 1.00)
UPKEEP_CAP = 0.04      # ... and absolute, iterations per group-tick (measured 0.0325)
BAND = 1.10
ASSAULT_FACTOR = 4.0   # contact field work per tick over march field work per tick

# Work classes that add up to total Legion work (tools/legion_observe.h, NavWork).
TOTAL_CLASSES = (
    "field_work", "trace_cells", "pass_scan_cells", "slot_search_cells", "group_loop_iters",
    "share_scan_iters", "aware_pairs", "held_rechecks", "lift_members_walked", "still_units_processed",
    "crowd_window_ring_cells", "crowd_settle_visits", "detour_cells", "softowner_lookups")
# Counters that are outcomes (checked by the observer's keys), not work.
OUTCOME_PREFIX = ("arrivals", "contact_arrivals", "mission_arrivals_", "completion_dist", "midroute",
                  "outside_area")
EQ_PREFIX = ("mission_legs_", "fields_started_by_kind_", "registrations")
BATTLE_COUNTERS = ("legion_total", "field_work", "slot_search_cells", "trace_cells", "pass_scan_cells",
                   "group_loop_iters", "aware_pairs", "fields_built")
# Sums the runner reports and this gate does not (tools/legion_observe.h): legion_all is legion_total
# plus the aware, lane and pinwheel steering work (2026-10-14 review, item 5); legion_total and its classes
# are gated.
REPORT_SUMS = ("legion_all",)


class Fail(Exception):
    pass


def comment_tags(path, tag):
    """`# legion_cost <tag> ...` lines of a .scn file, as token lists."""
    out = []
    for line in open(path, encoding="utf-8"):
        m = re.match(r"\s*#\s*legion_cost\s+" + tag + r"\s+(.*)$", line)
        if m:
            out.append(m.group(1).split())
    return out


class Runner:
    def __init__(self, exe, jobs):
        self.exe = exe
        self.jobs = jobs
        self.tmp = os.environ.get("TMPDIR", "/tmp")

    def run(self, scn, ticks=None, both_exec=False):
        cmd = [self.exe, scn, "--mode", "legion", "--offsets", "0", "--json",
               "--both-exec" if both_exec else "--serial"]
        if ticks:
            cmd += ["--ticks", str(ticks)]
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode == 77:
            raise Fail("77")
        if p.returncode != 0:
            raise Fail("runner failed (%d): %s\n%s" % (p.returncode, " ".join(cmd), p.stderr.strip()[-600:]))
        lines = [ln for ln in p.stdout.splitlines() if ln.startswith("{")]
        if not lines:
            raise Fail("no JSON from " + " ".join(cmd))
        d = json.loads(lines[-1])
        # Wall time (the runner's stderr) rides along under "_wall"; it is
        # reported only and is dropped before any comparison.
        d["_wall"] = {m.group(1): float(m.group(2)) for m in re.finditer(r"offset=0 (serial|workers) ([\d.]+) ms", p.stderr)}
        return d

    def many(self, jobs):
        """jobs: list of kwargs for run(); results in order."""
        with cf.ThreadPoolExecutor(max_workers=self.jobs) as ex:
            return list(ex.map(lambda kw: self.run(**kw), jobs))


def with_count(src, dst, n):
    text = open(src, encoding="utf-8").read()
    new, k = re.subn(r"^(group A \d+ \w+ )\d+", lambda m: m.group(1) + str(n), text, flags=re.M)
    if k != 1:
        raise Fail(src + ": expected exactly one `group A ... COUNT` line")
    open(dst, "w", encoding="utf-8").write(new)


def total(keys, name, suffix="total"):
    return int(keys.get("work.%s.%s" % (name, suffix), 0))


# ---- baseline ---------------------------------------------------------------

class Baseline:
    def __init__(self, path):
        self.path = path
        self.doc = {"version": 1, "entries": {}}
        if os.path.exists(path):
            self.doc = json.load(open(path, encoding="utf-8"))
        self.entries = self.doc["entries"]

    def reasons_ok(self):
        bad = [k for k, e in self.entries.items() if len(str(e.get("reason", "")).strip()) < 8]
        return bad

    def save(self):
        """One entry per line (diff-friendly); keys sorted."""
        items = sorted(self.entries.items())
        with open(self.path, "w", encoding="utf-8") as f:
            f.write('{\n "version": %d,\n "entries": {\n' % self.doc.get("version", 1))
            f.write(",\n".join("  %s: %s" % (json.dumps(k), json.dumps(e)) for k, e in items))
            f.write("\n }\n}\n")


def rule_for(key):
    leaf = key.split("/")[-1]
    name = re.sub(r"^work\.|\.total$", "", leaf)
    if leaf.endswith("region.goal.inside"):
        return "band", "ge"
    if leaf in ("commands", "units") or any(name.startswith(p) for p in EQ_PREFIX):
        return "eq", "le"
    return "band", "le"


def check_values(base, observed, strict, cumulative, update, reason, since, problems, notes):
    """observed: {key: int}. Compares with the baseline; collects problems."""
    for key, cur in sorted(observed.items()):
        e = base.entries.get(key)
        if e is None:
            if update:
                r, d = rule_for(key)
                base.entries[key] = {"value": cur, "rule": r, "dir": d, "band": BAND, "reason": reason,
                                     "since": since}
            elif cur:
                problems.append("unbaselined counter %s = %d (add an entry with a reason: --update-baseline "
                                "--reason ...)" % (key, cur))
            continue
        want = e["value"]
        rule = e.get("rule", "band")
        band = 1.0 if (strict or (cumulative and e.get("w0"))) else float(e.get("band", BAND))
        if cumulative and not e.get("w0"):
            continue
        if update and cur != want:
            e["value"] = cur
            e["reason"] = reason
            e["since"] = since
            continue
        if rule == "eq":
            if cur != want:
                problems.append("%s: %d != baseline %d (eq) -- %s" % (key, cur, want, e["reason"]))
            continue
        if e.get("dir", "le") == "le":
            if cur > want * band + 1e-9 and cur != want:
                problems.append("%s: %d > baseline %d x%.2f -- %s" % (key, cur, want, band, e["reason"]))
            elif want and cur * band < want - 1e-9:
                notes.append("RATCHET %s: %d vs baseline %d" % (key, cur, want))
        else:
            if cur * band < want - 1e-9 and cur != want:
                problems.append("%s: %d < baseline %d /x%.2f -- %s" % (key, cur, want, band, e["reason"]))


def counters_of(keys, prefix):
    """The baselined counters of one full run: every work.<c>.total that moved,
    the per-tick max and p99 of total Legion work, the group peak."""
    out = {}
    for k, v in keys.items():
        m = re.fullmatch(r"work\.(.+)\.total", k)
        if not m:
            continue
        name = m.group(1)
        if any(name.startswith(p) for p in OUTCOME_PREFIX) or name in REPORT_SUMS:
            continue
        out["%s/work.%s.total" % (prefix, name)] = int(v)
    for s in ("max", "p99"):
        out["%s/work.legion_total.%s" % (prefix, s)] = total(keys, "legion_total", s)
    out["%s/legion_groups_peak" % prefix] = int(keys.get("legion_groups_peak", 0))
    out["%s/commands" % prefix] = int(keys.get("commands", 0))
    return out


def missing_zero(base, observed, prefix, problems):
    """Baselined counters of this run that did not move now read 0: fine for
    `le`, an eq mismatch for `eq`."""
    for key, e in base.entries.items():
        if key.startswith(prefix + "/") and key not in observed and e.get("rule") == "eq" and e["value"]:
            problems.append("%s: now 0, baseline %d (eq) -- %s" % (key, e["value"], e["reason"]))


# ---- scaling ------------------------------------------------------------------

def scaling(args, base, rn):
    problems, notes, report = [], [], []
    observed = {}
    for scn_name in ("cost-corner", "cost-open"):
        src = os.path.join(args.scn_dir, scn_name + ".scn")
        if not os.path.exists(src):
            raise Fail("missing " + src)
        steady = comment_tags(src, "steady")
        if len(steady) != 1 or len(steady[0]) != 2:
            raise Fail(src + ": needs one `# legion_cost steady START END` line")
        t0, t1 = int(steady[0][0]), int(steady[0][1])
        full = None
        files = {}
        for n in SCALES:
            files[n] = os.path.join(args.tmpdir, "%s-%d.scn" % (scn_name, n))
            with_count(src, files[n], n)
        jobs = []
        for n in SCALES:
            jobs += [dict(scn=files[n], ticks=t0), dict(scn=files[n], ticks=t1),
                     dict(scn=files[n], both_exec=True), dict(scn=files[n], both_exec=True)]
        res = rn.many(jobs)
        W, Wfull, slots, upkeep = {}, {}, {}, {}
        for i, n in enumerate(SCALES):
            a, b, f1, f2 = (r["keys"] for r in res[i * 4:i * 4 + 4])
            ra, rb, rf1, rf2 = res[i * 4:i * 4 + 4]
            if not (rf1["serial_eq_workers"] and rf2["serial_eq_workers"]):
                problems.append("%s N=%d: serial != workers" % (scn_name, n))
            rf1.pop("_wall", None)
            rf2.pop("_wall", None)
            if rf1 != rf2:
                problems.append("%s N=%d: two identical runs differ (counters or hash not reproducible)"
                                % (scn_name, n))
            ticks_full = rf1["ticks"]
            W[n] = (total(b, "legion_total") - total(a, "legion_total")) / float(n * (t1 - t0))
            Wfull[n] = total(f1, "legion_total") / float(n * ticks_full)
            slots[n] = total(f1, "slot_search_cells") / float(n)
            groups = max(1, int(f1.get("legion_groups_peak", 1)))
            # Steady window only: the order-time registration scans are one-offs.
            up = lambda k: total(k, "group_loop_iters") + total(k, "share_scan_iters")
            upkeep[n] = (up(b) - up(a)) / float(groups * (t1 - t0))
            observed.update(counters_of(f1, "%s/N%d" % (scn_name, n)))
            observed["%s/N%d/region.goal.inside" % (scn_name, n)] = int(f1.get("region.goal.inside", 0))
        r_steady = W[400] / W[100] if W[100] else 0
        r_full = Wfull[400] / Wfull[100] if Wfull[100] else 0
        r_slots = slots[400] / slots[100] if slots[100] else 0
        r_up = upkeep[400] / upkeep[100] if upkeep[100] else 0
        report.append("%-11s W(N) steady [%d,%d): %s  W(4N0)/W(N0) = %.2f (<= %.1f)" % (
            scn_name, t0, t1, " ".join("%d:%.2f" % (n, W[n]) for n in SCALES), r_steady, RATIO_STEADY))
        report.append("%-11s W(N) whole run: %s  ratio %.2f (<= %.1f)" % (
            "", " ".join("%d:%.2f" % (n, Wfull[n]) for n in SCALES), r_full, RATIO_STEADY))
        report.append("%-11s slot cells per slot: %s  400/100 = %.2f (<= %.1f)" % (
            "", " ".join("%d:%.0f" % (n, slots[n]) for n in SCALES), r_slots, RATIO_SLOTS))
        report.append("%-11s upkeep per group-tick: %s  400/100 = %.2f (<= %.1f, cap %.3f)" % (
            "", " ".join("%d:%.4f" % (n, upkeep[n]) for n in SCALES), r_up, UPKEEP_RATIO, UPKEEP_CAP))
        if r_steady > RATIO_STEADY:
            problems.append("%s: steady W(400)/W(100) = %.2f > %.1f" % (scn_name, r_steady, RATIO_STEADY))
        if r_full > RATIO_STEADY:
            problems.append("%s: whole-run W(400)/W(100) = %.2f > %.1f" % (scn_name, r_full, RATIO_STEADY))
        if r_slots > RATIO_SLOTS:
            problems.append("%s: slot work per slot grows %.2fx from 100 to 400 bodies (> %.1f)" %
                            (scn_name, r_slots, RATIO_SLOTS))
        if r_up > UPKEEP_RATIO or max(upkeep.values()) > UPKEEP_CAP:
            problems.append("%s: upkeep per live group not bounded (%s)" % (
                scn_name, " ".join("%d:%.4f" % (n, upkeep[n]) for n in SCALES)))
        observed["%s/w_steady_x100_N400" % scn_name] = int(round(W[400] * 100))
        observed["%s/w_steady_x100_N100" % scn_name] = int(round(W[100] * 100))
    check_values(base, observed, args.strict, False, args.update_baseline, args.reason, args.since, problems, notes)
    for prefix in ("cost-corner", "cost-open"):
        missing_zero(base, observed, prefix, problems)
    return report, problems, notes


# ---- battle -------------------------------------------------------------------

def battle(args, base, rn, scn):
    name = os.path.splitext(os.path.basename(scn))[0]
    wins = [(w[0], int(w[1]), int(w[2])) for w in comment_tags(scn, "window")]
    if not wins:
        raise Fail(scn + ": needs `# legion_cost window NAME START END` lines")
    ends = sorted({e for _, _, e in wins} | {s for _, s, _ in wins if s})
    problems, notes, report = [], [], []

    def sweep():
        jobs = [dict(scn=scn, ticks=t) for t in ends] + [dict(scn=scn, both_exec=True)]
        return rn.many(jobs)

    r1 = sweep()
    r2 = sweep()
    wall = [r.pop("_wall", {}) for r in r1[-1:]]
    for r in r1[:-1] + r2:
        r.pop("_wall", None)
    r1[-1].pop("_wall", None)
    r2[-1].pop("_wall", None)
    if r1 != r2:
        problems.append("%s: two identical runs differ" % name)
    if not r1[-1]["serial_eq_workers"]:
        problems.append("%s: serial != workers" % name)
    cum = {t: r["keys"] for t, r in zip(ends, r1[:-1])}
    cum[0] = {}
    fullkeys = r1[-1]["keys"]
    full_ticks = r1[-1]["ticks"]
    observed = {}
    rows = {}
    for wname, s, e in wins:
        row = {}
        for c in BATTLE_COUNTERS:
            row[c] = total(cum[e], c) - total(cum[s], c) if s else total(cum[e], c)
        row["ticks"] = e - s
        rows[wname] = row
        for c in BATTLE_COUNTERS:
            observed["%s/%s/work.%s.total" % (name, wname, c)] = row[c]
    last = max(e for _, _, e in wins)
    if last > full_ticks:
        problems.append("%s: window end %d is past the run (%d ticks)" % (name, last, full_ticks))
    for s in ("max", "p99"):
        observed["%s/run/work.legion_total.%s" % (name, s)] = total(fullkeys, "legion_total", s)
    observed["%s/run/work.legion_total.total" % name] = total(fullkeys, "legion_total")
    dead = sum(int(v) for k, v in fullkeys.items() if re.fullmatch(r"g\.\w+\.dead", k))

    hdr = "%-12s %6s %14s %12s %14s %12s %10s" % ("window", "ticks", "legion_total", "field_work",
                                                   "slot_search", "trace_cells", "per tick")
    report.append("%s (%d units, %d ticks; wall time is the runner's, not gated)" % (name, r1[-1]["units"], full_ticks))
    report.append(hdr)
    for wname, s, e in wins:
        r = rows[wname]
        report.append("%-12s %6d %14d %12d %14d %12d %10.0f" % (
            wname, r["ticks"], r["legion_total"], r["field_work"], r["slot_search_cells"], r["trace_cells"],
            r["legion_total"] / float(r["ticks"])))
    if wall and wall[0]:
        report.append("wall (reported only): " + ", ".join("%s %.0f ms" % kv for kv in sorted(wall[0].items())))
    report.append("run: per-tick max %d  p99 %d  groups peak %s  dead %d" % (
        total(fullkeys, "legion_total", "max"), total(fullkeys, "legion_total", "p99"),
        fullkeys.get("legion_groups_peak"), dead))

    if name == "battle-assault":
        march = [w for w, _, _ in wins if w.startswith("march")]
        contact = [w for w, _, _ in wins if w.startswith("contact")]
        for c in contact:
            for m in march:
                rc = rows[c]["field_work"] / float(rows[c]["ticks"])
                rm = rows[m]["field_work"] / float(rows[m]["ticks"])
                ok = rc >= ASSAULT_FACTOR * rm
                report.append("field work per tick %s %.0f vs %s %.0f: %.1fx (>= %.0fx) %s" % (
                    c, rc, m, rm, rc / rm if rm else float("inf"), ASSAULT_FACTOR, "ok" if ok else "FAIL"))
                if not ok:
                    problems.append("%s: field work per tick in %s is %.1fx %s (< %.0fx)" % (
                        name, c, rc / rm if rm else 0, m, ASSAULT_FACTOR))
    check_values(base, observed, args.strict, args.cumulative, args.update_baseline, args.reason, args.since,
                 problems, notes)
    if args.cumulative:
        for key in sorted(base.entries):
            if key.startswith(name + "/") and base.entries[key].get("w0") and key not in observed:
                problems.append("cumulative: %s not produced" % key)
    return report, problems, notes


def mark_w0(base, scn_names):
    """The battle total / max / p99 entries are the W0 base the cumulative
    combat check reads (PLAN 3.0)."""
    for key, e in base.entries.items():
        for n in scn_names:
            if key.startswith(n + "/") and (key.endswith("work.legion_total.max") or
                                            key.endswith("work.legion_total.p99") or
                                            key.endswith("work.legion_total.total")):
                e["w0"] = True


def main():
    ap = argparse.ArgumentParser(description="Legion work-scaling gate and battle window tables")
    ap.add_argument("what", nargs="?", default="scaling", choices=("scaling", "battle"))
    ap.add_argument("files", nargs="*")
    ap.add_argument("--runner", default=os.environ.get("LEGION_SCENARIO", "build/legion_scenario"))
    ap.add_argument("--scn-dir", default=os.path.join(HERE, "scenarios"))
    ap.add_argument("--baseline", default=os.path.join(HERE, "legion_cost_baseline.json"))
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--cumulative", action="store_true")
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--reason", default="")
    ap.add_argument("--since", default="W0")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--tmpdir", default=os.environ.get("TMPDIR", "/tmp"))
    args = ap.parse_args()
    if args.update_baseline and len(args.reason.strip()) < 8:
        print("legion_cost: --update-baseline needs --reason TEXT (a baseline entry without a reason is refused)")
        return 2
    if not os.path.exists(args.runner):
        print("legion_cost: runner not found: %s" % args.runner)
        return 77 if os.environ.get("LEGION_COST_SKIP_MISSING") else 2
    os.makedirs(args.tmpdir, exist_ok=True)
    base = Baseline(args.baseline)
    rn = Runner(args.runner, args.jobs)
    bad = base.reasons_ok()
    if bad:
        print("legion_cost: baseline entries without a reason: %s" % ", ".join(bad[:5]))
        return 1
    try:
        if args.what == "scaling":
            report, problems, notes = scaling(args, base, rn)
        else:
            if not args.files:
                print("legion_cost battle: no .scn file")
                return 2
            report, problems, notes = [], [], []
            for f in args.files:
                r, p, n = battle(args, base, rn, f)
                report += r + [""]
                problems += p
                notes += n
            if args.update_baseline:
                mark_w0(base, [os.path.splitext(os.path.basename(f))[0] for f in args.files])
    except Fail as e:
        if str(e) == "77":
            print("legion_cost: scenario needs --data (skipped)")
            return 77
        print("legion_cost: %s" % e)
        return 2
    print("\n".join(report))
    for n in notes:
        print(n)
    if args.update_baseline:
        base.save()
        print("baseline written: %s (%d entries)" % (args.baseline, len(base.entries)))
        # gates other than the baseline still apply
        gate = [p for p in problems if "baseline" not in p]
        if gate:
            print("\n".join("FAIL " + p for p in gate))
            return 1
        return 0
    if problems:
        print("\n".join("FAIL " + p for p in problems))
        return 1
    print("legion_cost %s: ok" % args.what)
    return 0


if __name__ == "__main__":
    sys.exit(main())
