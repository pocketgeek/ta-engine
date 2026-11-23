#!/usr/bin/env python3
"""legion_check -- the Legion baseline gate (PLAN 3.0 / 3.8, T8-synthesis (5)).

All of the gating logic lives here. `legion_scenario --check B.json FILE.scn...`
runs the scenarios and hands their JSON lines to this script; the same lines
(`legion_scenario ... --json > run.jsonl`) can be checked offline.

  legion_check.py check      --baseline B --results RUN.jsonl... [--step ID]
                             [--cumulative] [--require-all] [--ratchet REASON]
  legion_check.py baseline   --baseline B --results RUN.jsonl... --reason TEXT
                             [--since ID] [--keys GLOB]... [--hashes]
                             [--cluster GLOB=CLUSTER]...      (step 0: retake the base)
  legion_check.py exit-table --baseline B --results RUN.jsonl... --base OLD
                             [--intended GLOB]... [--intended-file F]
  legion_check.py anchor     --anchor ANCHOR.json --results RUN.jsonl...
  legion_check.py validate   --baseline B                      (the reasons ctest)

Exit: 0 pass, 1 a gate failed, 2 usage / tooling error.

baseline.json
  {"version": 1,
   "entries": {SCENARIO: {MODE: {KEY: {value, dir, rule, band, reason, since,
                                       [per: "unit"], [w0: number],
                                       [phase_gate: {phases: [>= 10 values], reason}]}}}},
   "references": {SCENARIO: {MODE: {KEY: number}}},     # W0 base raised by ratchets
   "exceptions": [{scenario, key, cluster, reason, [mode], [median5: true]}],
   "accepted_regressions": [{scenario, mode, key, reason, [step], max | factor}],
   "history": [{when, reason, ratchets: [...], cleared_exceptions: [...]}]}
  dir     lower | higher  (which direction is better)
  rule    eq     the value must equal `value` (safety keys, hashes, serial==workers)
          band   one-sided: worse than `value` x band (per step) or than the
                 reference x band (cumulative) fails; better than the reference
                 by more than the band prints RATCHET and passes
          bound  an absolute declared bound (`per: unit` divides by the units)
  phase_gate  (cumulative combat keys only; lead ruling W4 (p)) the key's value at each of >= 10 stripe
          phases of a phase-sensitive change: the cumulative combat check passes a value above `w0` when
          the MEDIAN of the phases is at or below `w0` and the value is within the recorded range
  Time keys (.t50 .t90 .done .orders_done): -1 reads "never" = infinitely bad.
  KEYs: observer / work.* keys of the runner's "keys", `hash@OFFSET`, and
        `serial_eq_workers`.

Offsets (lead rulings W3 round 3, 2026-10-09): a run on the gate offsets (0,+-1..+-5, or a file's
`gateoffsets`; `legion_scenario --offsets gate`, the default of --check) is re-read here: the
small-count keys (crossings, wall touch, the t90 of a group under 10 bodies) on the median of all
its offsets, every other key on the core five 0,+-1,+-2. contact_settled's Retail floor is read
only where both modes deliver >= 10 units, and an offset-spread exception is never a floor
exception.

Click level (lead ruling W3 final exit (f)): a one-body observer group whose click one convoy
merged with others (aware-*, motion-*; the runner reports `g.G.click_n` for it and the convoy's
`click.C.n/arrived/t50/t90/done`) is judged at click level in both modes: its own arrived / t50 /
t90 / done are report-only (no entry, no band, no Retail floor); the click's keys carry them.
`apply_ratchet` keeps the offset spread a cleared Retail-floor exception had masked: it is
converted to a spread exception (ruling (h)).

Per-file lane crossings (user decision W8-1, 2026-10-10): for the GAP fixtures (`gap*`: gap6 .. gap20,
gapsweep) the Retail floor of `gate.X.crossings` compares legion crossings / files abreast with Retail's
crossings / files abreast (`gate.X.files_x100`, a mean under 1 read as 1), because the passage gate takes
a gap two or three abreast where Retail single-files. Corner and head-on fixtures keep the raw count.

W9-U1 (user, 2026-10-10): where Retail itself is 0 or 1, x1.1 of it is 0 or 1.1, below Retail's own spread
(0..10 on corner crossings, ruling (a)), so three keys are judged on the eleven-offset median with a
one-event tolerance, `legion <= max(retail x1.1, retail + 1)`: corner-1x448 `gate.top.crossings`,
tail-corner380 `wall_touch_near_permille`, and the GAP fixtures' per-file crossing key
(`gate.X.crossings_per_file_x100`, below). Every other Retail-floor key keeps legion <= retail x1.1.

The gap per-file key (W9 step 0, W8-1): for a GAP fixture `regate` derives `gate.X.crossings_per_file_x100` =
crossings x 10000 / max(100, files_x100) at EACH offset and takes the median of the eleven, the number the
W8-1 rule compares (a ratio of the per-offset medians used to stand in for it); the floor reads it, an INFO
line prints it for every GAP fixture, and it is a plain key for the exit table.

Precedence (PLAN 3.0): eq/safety, then the Retail floor, then a declared
tolerance or accepted regression, then the bands. A tolerance never licenses a
Retail-floor failure; one with a `step` licenses its loss only when `--step`
names that step, and only up to its `max` (absolute) or `factor` (x the base).
"""
import argparse
import datetime
import fnmatch
import json
import math
import re
import sys

INF = float("inf")
BAND = 1.10
FLOOR = 1.10
MAX_BAND = 1.20
TIME_SUFFIXES = (".t50", ".t90", ".done", ".orders_done")
COMBAT_SCENARIOS = ("battle-field*", "battle-assault*")
COMBAT_KEYS = ("work.legion_total.total", "work.legion_total.max", "work.legion_total.p99")
# Work sums reported, never gated (tools/legion_observe.h): legion_all is legion_total plus the aware, lane and
# pinwheel steering work the instruments report (2026-10-14 review, item 5). It needs no baseline entry, a
# retake (write_base) never takes one in, and check() skips one a baseline still carries.
REPORT_WORK = ("work.legion_all.",)
# The observer's mechanism attribution (tools/legion_observe.h, the pathfinding deep dive of 2026-11-02): the motion
# keys split by the mechanism of a body's last update (why.<mechanism>.ticks / moved_ticks / samples / stopped /
# statue / sideways / backward / reversals). Observation only, like REPORT_WORK: a plain `baseline` run never takes
# one in, and check() skips one a baseline still carries.
REPORT_WHY = ("why.",)
REPORT_KEYS = REPORT_WORK + REPORT_WHY      # reported by the runner, never baselined, never gated
SKIP_KEYS = ("members", "samples", "commands", "last_command_tick", "decision_samples",
             # W5 step 0: the claims audit's report-only classes (claims.bad_max, their sum, is the safety key)
             "claims.audits", "claims.overlaps_max", "claims.missing_max", "claims.dangling_max",
             "claims.orphans_max", "claims.orphans_end")
BAD_CLUSTERS = ("", "UNASSIGNED", "TODO", "?")
# Offsets (lead ruling W3 round 3 (a), 2026-10-09). Every key is gated on the median of the five
# core offsets, except the small-count keys: a handful of events decides them (corner crossings
# span 0..10 in Retail itself), so they are gated on the median of all eleven, in both modes and
# on the Retail floor. A run with the eleven offsets serves both: the other keys are re-read
# over the core five. A t90 is small-count when its group has fewer than 10 members (90% of
# them is then every one: the t90 is the last body's time). A scenario whose spawns the eleven
# put off the map runs its own `gateoffsets` (cost-open: -4..6; corner-8x56 and corner-1x448
# fill their map to 4 cells of two edges: -4..4): a record is wide with at least WIDE_MIN
# offsets, the core five among them, and its small-count keys are read over all of them.
CORE_OFFSETS = (0, 1, -1, 2, -2)
WIDE_OFFSETS = (0, 1, -1, 2, -2, 3, -3, 4, -4, 5, -5)
WIDE_MIN = 9
WIDE_KEYS = ("gate.*.crossings", "wall_touch_permille", "wall_touch_near_permille", "gate.*.crossings_per_file_x100")
WIDE_T90_MAX_N = 9
# Ruling (b): contact_settled is per arrived unit (W3-1), so its Retail floor is only read where
# at least this many units arrive in both modes (the motion-* scenarios deliver 1-3).
CONTACT_FLOOR_MIN_ARRIVED = 10
# Ruling (f): a one-body group's progress keys inside a multi-body click are report-only.
CLICK_REPORT_ONLY = (".arrived", ".t50", ".t90", ".done")


# ---------------------------------------------------------------- key classes
def is_time(key):
    # reach.NAME.first (W5 step 0): the first tick any attacker was in reach, -1 = never
    return key.endswith(TIME_SUFFIXES) or (key.startswith("reach.") and key.endswith(".first"))


def default_dir(key):
    if key.endswith((".arrived", ".flyers_landed", ".inside", ".complete_n", "approach.arrived_n")):
        return "higher"
    if is_reach_gain(key):
        return "higher"
    return "lower"


def is_reach_gain(key):
    """The `reach` shape's higher-is-better keys (W5 step 0): attackers that ever got a shot, the ones in
    reach at the end, the damage dealt and the targets destroyed."""
    return key.startswith("reach.") and (".ever_" in key or key.endswith((".now_end", ".targets_dead_end")) or
                                         ".damage_" in key)


def floor_dir(key):
    """Direction if `key` is a Retail-floor key (PLAN 3.0), else None."""
    if key.endswith(".arrived"):
        return "higher"
    if key.startswith("reach.") and (".ever_" in key or ".damage_" in key):
        return "higher"          # W5 step 0: the AR-06 rows (the army that gets a shot, the damage dealt)
    # wall_touch_permille / wall_touch_near_permille only: the W9 step 0 splits (wall_touch_{gate,ring,corner,flat}_permille,
    # the _near_ variants, wall_touch_still_permille) are report-only, like gate.X.cross_<site>.
    if (key.endswith(".t90") or key in ("spins", "parked_no_progress") or key.endswith(".crossings")
            or key in ("wall_touch_permille", "wall_touch_near_permille")
            or (key.startswith("contact_") and key.endswith("_permille"))):
        return "lower"
    return None


def is_wide(key, rec):
    """A small-count key, gated on the eleven offsets (ruling (a))."""
    if any(fnmatch.fnmatch(key, g) for g in WIDE_KEYS):
        return True
    if key.startswith(("g.", "click.")) and key.endswith(".t90"):
        n = rec.get("keys", {}).get(key[:-4] + ".n")
        return isinstance(n, int) and 0 < n <= WIDE_T90_MAX_N
    return False


def is_report_only(key, rec):
    """Ruling (f): a one-body group's arrived / t50 / t90 / done when the group belongs to a
    multi-body click (`g.G.click_n` > 1): judged at click level (`click.*`), report-only here."""
    if not key.startswith("g.") or not key.endswith(CLICK_REPORT_ONLY):
        return False
    g = key[:key.rindex(".")]
    keys = rec.get("all_keys", rec.get("keys", {}))
    n, cn = keys.get(g + ".n"), keys.get(g + ".click_n")
    return n == 1 and isinstance(cn, int) and cn > 1


def per_offset(rec, key):
    """{offset: value} of a key in a result record (a key that does not vary equals its median)."""
    offs = rec.get("offsets", [])
    vals = rec.get("varying", {}).get(key)
    if vals is None or len(vals) != len(offs):
        vals = [rec.get("keys", {}).get(key, 0)] * len(offs)
    return dict(zip(offs, vals))


def median_of(key, vals):
    """The runner's median: sorted, never (-1 on a time key) last, element (n-1)/2."""
    s = sorted(vals, key=lambda x: INF if is_time(key) and x < 0 else x)
    return s[(len(s) - 1) // 2]


def regate(rec):
    """Re-read a record on the gate's offsets: small-count keys over the eleven, the others over
    the core five. A record without the eleven is left as it is (`wide` stays False, and a gated
    small-count key fails on it); one without the core five too is read as given."""
    offs = rec.get("offsets", [])
    wide = list(offs)
    rec["wide"] = len(offs) >= WIDE_MIN and all(o in offs for o in CORE_OFFSETS)
    if not rec["wide"]:
        return rec
    keys, varying = {}, {}
    for k in rec.get("keys", {}):
        by = per_offset(rec, k)
        vals = [by[o] for o in (wide if is_wide(k, rec) else CORE_OFFSETS)]
        keys[k] = median_of(k, vals)
        if any(v != vals[0] for v in vals):
            varying[k] = vals
    rec["all_keys"], rec["all_varying"] = rec["keys"], rec.get("varying", {})
    rec["keys"], rec["varying"] = keys, varying
    add_per_file_keys(rec, wide)
    return rec


def add_per_file_keys(rec, offs):
    """W9 step 0 (user decision W8-1): for a GAP fixture, `gate.X.crossings_per_file_x100` = crossings x 10000 /
    max(100, files_x100) at each offset (a mean under one file reads as one), medianed over the eleven."""
    if not any(fnmatch.fnmatch(rec.get("scenario", ""), g) for g in GAP_SCENARIOS):
        return
    src = dict(rec, keys=rec["all_keys"], varying=rec["all_varying"])
    for k in sorted(rec["all_keys"]):
        m = re.fullmatch(r"gate\.(.+)\.crossings", k)
        fk = "gate.%s.files_x100" % m.group(1) if m else None
        if not m or fk not in rec["all_keys"]:
            continue
        cb, fb = per_offset(src, k), per_offset(src, fk)
        vals = [(cb[o] * 10000 + max(100, fb[o]) // 2) // max(100, fb[o]) for o in offs]
        pk = "gate.%s.crossings_per_file_x100" % m.group(1)
        rec["keys"][pk] = median_of(pk, vals)
        if any(v != vals[0] for v in vals):
            rec["varying"][pk] = vals


def arrived_median(rec):
    """Median over the core offsets of the units arrived at the end (sum of g.*.arrived)."""
    src = dict(rec, keys=rec.get("all_keys", rec.get("keys", {})), varying=rec.get("all_varying",
                                                                                  rec.get("varying", {})))
    tot = {}
    for k in src["keys"]:
        if k.startswith("g.") and k.endswith(".arrived"):
            for o, v in per_offset(src, k).items():
                tot[o] = tot.get(o, 0) + v
    core = [tot[o] for o in CORE_OFFSETS if o in tot] or list(tot.values()) or [0]
    return median_of("arrived", core)


def norm(key, x):
    """Comparable number; `never` (-1 on a time key) is infinitely bad."""
    if isinstance(x, bool) or not isinstance(x, (int, float)):
        return x
    if is_time(key) and x < 0:
        return INF
    return x


def worse(lower, cur, ref, band):
    """cur is worse than ref by more than band (one-sided). Equal or better is never worse, whatever the
    sign: a key's -1 (none: no near-wall samples, no completions) must not read as a loss against itself."""
    if (cur <= ref) if lower else (cur >= ref):
        return False
    if lower:
        return cur > ref * band + 1e-9 if ref != INF else False
    return cur * band + 1e-9 < ref


def better(lower, cur, ref, band):
    """cur is better than ref by more than band."""
    if (cur >= ref) if lower else (cur <= ref):
        return False
    if lower:
        return cur * band + 1e-9 < ref
    return cur > ref * band + 1e-9 if ref != INF else False


def fmt(x):
    if x == INF:
        return "never"
    if isinstance(x, float):
        return "%.4g" % x
    return str(x)


# --------------------------------------------------------------------- loading
def load_json(path):
    with open(path) as f:
        return json.load(f)


def load_results(paths):
    """JSON lines from the runner -> {(scenario, mode): record}; skipped lines
    are returned apart."""
    recs, skipped = {}, []
    for p in paths:
        lines = sys.stdin.read().splitlines() if p == "-" else open(p).read().splitlines()
        for ln in lines:
            ln = ln.strip()
            if not ln.startswith("{"):
                continue
            r = json.loads(ln)
            if "skipped" in r:
                skipped.append(r["scenario"])
                continue
            recs[(r["scenario"], r["mode"])] = regate(r)
    return recs, skipped


def lookup(rec, key):
    """(found, value) of a baseline key in a result record."""
    if key.startswith("hash@"):
        v = rec.get("hash", {}).get(key[5:])
        return v is not None, v
    if key == "serial_eq_workers":
        return "serial_eq_workers" in rec, rec.get("serial_eq_workers")
    keys = rec.get("keys", {})
    if key in keys:
        return True, keys[key]
    if key.startswith("work."):
        return True, 0           # the runner counts a missing work.* key as 0
    return False, None


def empty_baseline():
    return {"version": 1, "entries": {}, "references": {}, "exceptions": [],
            "accepted_regressions": [], "history": []}


def flatten(path):
    """{(scenario, mode, key): number} from a baseline (values), an anchor file
    ({"entries": {s: {m: {k: number|{value}}}}}) or runner JSON lines."""
    try:
        doc = load_json(path)
    except ValueError:
        doc = None
    out = {}
    if isinstance(doc, dict) and "entries" in doc:
        for s, modes in doc["entries"].items():
            for m, keys in modes.items():
                for k, e in keys.items():
                    v = e.get("value") if isinstance(e, dict) else e
                    if isinstance(e, dict) and e.get("rule") == "bound":
                        # A bound's value is a limit, not a measurement: a W0 spread bound
                        # stores its median x band ("bound at median N x1.20"), so the move is
                        # read against N; a declared bound (per unit, ...) has no base value.
                        mm = re.search(r"at median (-?[0-9.]+)", e.get("reason", ""))
                        if not mm:
                            continue
                        v = float(mm.group(1))
                        v = int(v) if v == int(v) else v
                    if isinstance(v, (int, float)) and not isinstance(v, bool):
                        out[(s, m, k)] = v
        return out
    recs, _ = load_results([path])
    for (s, m), r in recs.items():
        for k, v in r.get("keys", {}).items():
            out[(s, m, k)] = v
    return out


# ------------------------------------------------------------------ validation
def validate_baseline(doc):
    """Problems (strings) in a baseline document; empty = valid. The reasons
    ctest fails on any."""
    bad = []
    if not isinstance(doc, dict) or doc.get("version") != 1:
        return ["baseline: version must be 1"]

    def need_reason(where, e):
        if not isinstance(e.get("reason"), str) or not e["reason"].strip():
            bad.append("%s: no reason" % where)

    for s, modes in doc.get("entries", {}).items():
        for m, keys in modes.items():
            for k, e in keys.items():
                w = "entries %s/%s/%s" % (s, m, k)
                need_reason(w, e)
                if e.get("rule") not in ("eq", "band", "bound"):
                    bad.append("%s: rule must be eq|band|bound" % w)
                if e.get("rule") != "eq" and e.get("dir") not in ("lower", "higher"):
                    bad.append("%s: dir must be lower|higher" % w)
                if "value" not in e:
                    bad.append("%s: no value" % w)
                b = e.get("band", BAND)
                if e.get("rule") == "band" and not (1.0 <= b <= MAX_BAND):
                    bad.append("%s: band %s outside 1.0..%s" % (w, b, MAX_BAND))
    for i, e in enumerate(doc.get("exceptions", [])):
        w = "exceptions[%d] %s/%s" % (i, e.get("scenario"), e.get("key"))
        need_reason(w, e)
        if str(e.get("cluster", "")).strip() in BAD_CLUSTERS:
            bad.append("%s: no cluster id" % w)
        if not e.get("scenario") or not e.get("key"):
            bad.append("%s: scenario and key required" % w)
    for i, e in enumerate(doc.get("accepted_regressions", [])):
        w = "accepted_regressions[%d] %s/%s/%s" % (i, e.get("scenario"), e.get("mode"), e.get("key"))
        need_reason(w, e)
        if not e.get("scenario") or not e.get("key") or not e.get("mode"):
            bad.append("%s: scenario, mode and key required" % w)
        if "max" not in e and "factor" not in e:
            bad.append("%s: needs max or factor (a loss without a limit licenses anything)" % w)
    for i, e in enumerate(doc.get("history", [])):
        need_reason("history[%d]" % i, e)
    return bad


# ------------------------------------------------------------------------ check
class Report:
    def __init__(self):
        self.fails, self.ratchets, self.info, self.licensed = [], [], [], []
        self.cleared, self.exception_count = [], 0
        self.masked = {}        # (scn, mode, key) -> (lo, hi): a spread an exception covers (ruling (h))
        self.report_only = 0    # floor keys left to their click (ruling (f))
        self.checked = 0
        self.m5 = set()

    def fail(self, path, msg):
        self.fails.append("%s: %s" % (path, msg))

    @property
    def ok(self):
        return not self.fails

    def lines(self):
        out = ["FAIL %s" % f for f in self.fails]
        out += ["RATCHET %s %s -> %s" % (p, fmt(o), fmt(n)) for p, o, n in self.ratchets]
        out += ["LICENSED %s" % l for l in self.licensed]
        out += ["CLEARED exception %s (passes the Retail floor again; removed at the next ratchet)" % c
                for c in self.cleared]
        out += ["INFO %s" % i for i in self.info]
        out.append("%s: %d keys checked, %d failed, %d ratchets, %d licensed, %d floor exceptions, %d report-only"
                   % ("PASS" if self.ok else "FAIL", self.checked, len(self.fails), len(self.ratchets),
                      len(self.licensed), self.exception_count, self.report_only))
        return out


def find_license(doc, scn, mode, key, step):
    for a in doc.get("accepted_regressions", []):
        if not (fnmatch.fnmatch(scn, a.get("scenario", "")) and fnmatch.fnmatch(mode, a.get("mode", ""))
                and fnmatch.fnmatch(key, a.get("key", ""))):
            continue
        if a.get("step") is not None and a["step"] != step:
            continue
        yield a


def licensed_by(doc, scn, mode, key, step, lower, cur, base, ref):
    for a in find_license(doc, scn, mode, key, step):
        if "max" in a:
            lim = norm(key, a["max"])
            ok = cur <= lim if lower else cur >= lim
        else:
            f = a["factor"]
            anchor = max(base, ref) if lower else min(base, ref)
            ok = (cur <= anchor * f + 1e-9) if lower else (cur * f + 1e-9 >= anchor)
        if ok:
            return a
    return None


def judge(doc, rpt, scn, mode, key, e, rec, step):
    path = "%s/%s/%s" % (scn, mode, key)
    found, raw = lookup(rec, key)
    if not found:
        rpt.fail(path, "key missing from the run")
        return
    rpt.checked += 1
    rule = e.get("rule", "band")
    if rule == "eq":
        if raw != e["value"]:
            rpt.fail(path, "eq: expected %s, got %s" % (e["value"], raw))
        return
    lower = e.get("dir", default_dir(key)) == "lower"
    cur = norm(key, raw)
    base = norm(key, e["value"])
    if rule == "bound":
        if e.get("per") == "unit":
            cur = cur / max(1, rec.get("units", 1))
        if (cur > base + 1e-9) if lower else (cur + 1e-9 < base):
            rpt.fail(path, "bound %s %s, got %s%s" % ("<=" if lower else ">=", fmt(base), fmt(cur),
                                                      " per unit" if e.get("per") == "unit" else ""))
        return
    band = e.get("band", BAND)
    refraw = doc.get("references", {}).get(scn, {}).get(mode, {}).get(key, e["value"])
    ref = norm(key, refraw)
    step_bad = worse(lower, cur, base, band)
    cum_bad = worse(lower, cur, ref, band)
    if step_bad or cum_bad:
        lic = licensed_by(doc, scn, mode, key, step, lower, cur, base, ref)
        if lic:
            rpt.licensed.append("%s %s vs base %s / reference %s: %s" % (path, fmt(cur), fmt(base), fmt(ref),
                                                                       lic["reason"]))
            return
        which = []
        if step_bad:
            which.append("per-step vs head base %s x%.2f" % (fmt(base), band))
        if cum_bad:
            which.append("cumulative vs reference %s x%.2f" % (fmt(ref), band))
        rpt.fail(path, "%s than allowed (%s), got %s" % ("higher" if lower else "lower", "; ".join(which), fmt(cur)))
    elif better(lower, cur, ref, band):
        rpt.ratchets.append((path, ref, cur))


def spread_of(key, vals):
    """(lo, hi, hi/lo) of a key's per-offset values; the ratio is inf when lo is 0 or never."""
    v = [norm(key, x) for x in vals]
    lo, hi = min(v), max(v)
    if hi == lo:
        return lo, hi, 1.0
    return lo, hi, INF if lo <= 0 or hi == INF else hi / lo


def spread_check(doc, rpt, scn, mode, key, e, rec, exc):
    vals = rec.get("varying", {}).get(key)
    if not vals or key.startswith("work.") or e.get("rule") != "band":
        return
    if rec.get("wide") and is_wide(key, rec):
        return      # a small-count key is gated on the median of the gate offsets already (ruling (a))
    band = e.get("band", BAND)
    lo, hi, ratio = spread_of(key, vals)
    if ratio <= band:
        return
    if exc is not None:
        rpt.masked[(scn, mode, key)] = (lo, hi)
        return
    rpt.fail("%s/%s/%s" % (scn, mode, key),
             "offset spread %s..%s exceeds band %.2f: list it under exceptions (cluster, reason) and gate "
             "it on median-of-5" % (fmt(lo), fmt(hi), band))


def floor_passes(lower, legion, retail, tolerance=False):
    """legion <= retail x1.1 (lower is better) or legion x1.1 >= retail. `tolerance` (W9-U1, small counts where
    Retail is 0 or 1): legion <= max(retail x1.1, retail + 1)."""
    if lower and tolerance and retail != INF and legion <= retail + 1 + 1e-9:
        return True
    return not worse(True, legion, retail, FLOOR) if lower else legion * FLOOR + 1e-9 >= retail


# W9-U1 (user, 2026-10-10): the keys judged with the one-event tolerance, as (scenario glob, key glob).
U1_KEYS = (("corner-1x448", "gate.top.crossings"), ("tail-corner380", "wall_touch_near_permille"),
           ("gap*", "gate.*.crossings"))


def u1_applies(scn, key):
    return any(fnmatch.fnmatch(scn, s) and fnmatch.fnmatch(key, k) for s, k in U1_KEYS)


def need_median5(rpt, path, exc, rec):
    if exc.get("median5") and len(rec.get("offsets", [])) < 5 and path not in rpt.m5:
        rpt.m5.add(path)
        rpt.fail(path, "exception is gated on median-of-5 offsets; run has %d (pass --offsets with 5 values)"
                 % len(rec.get("offsets", [])))


def exception_for(doc, scn, mode, key):
    return next((x for x in doc.get("exceptions", [])
                 if x["scenario"] == scn and x["key"] == key and x.get("mode", mode) == mode), None)


def floor_exception_for(doc, scn, key):
    """The Retail-floor exception of a key. An offset-spread exception is not one (ruling (c),
    2026-10-09: counting it hid the wall-4x50 and motion-cross floor failures)."""
    return next((x for x in doc.get("exceptions", [])
                 if x["scenario"] == scn and x["key"] == key and x.get("mode", "legion") == "legion"
                 and not is_spread_exception(x)), None)


def floor_applies(key, lrec, rrec):
    """Ruling (b): contact_settled's floor only where both modes deliver >= 10 units."""
    if key == "contact_settled_permille":
        return min(arrived_median(lrec), arrived_median(rrec)) >= CONTACT_FLOOR_MIN_ARRIVED
    return True


def need_wide(rpt, path, key, rec):
    if not rec.get("wide") and is_wide(key, rec) and path not in rpt.m5:
        rpt.m5.add(path)
        rpt.fail(path, "small-count key is gated on the median of the gate offsets (%s, or the file's "
                       "gateoffsets, at least %d); run has %s" % (",".join(map(str, WIDE_OFFSETS)), WIDE_MIN,
                                                                rec.get("offsets", [])))


# W8-1 (user, 2026-10-10): the passage gate takes a gap two or three abreast (throughput), which a
# single-file Retail never does, so the lane swaps after the gap are judged PER FILE for the GAP
# fixtures: legion crossings / files against Retail's crossings / files, instead of the raw counts.
# Corner and head-on lane-crossing floors (W3-2 / W3-3) are not GAP fixtures and stay raw.
GAP_SCENARIOS = ("gap*",)


def per_file_floor(scn, key, lrec, rrec):
    """(legion per file, retail per file) for a `gate.X.crossings` key of a GAP fixture, else None.
    The files key is `gate.X.files_x100` (the observer's mean files abreast over the gate's samples,
    times 100); a mean under 1 is read as 1 (nobody abreast)."""
    if not any(fnmatch.fnmatch(scn, g) for g in GAP_SCENARIOS):
        return None
    m = re.fullmatch(r"gate\.(.+)\.crossings", key)
    if not m:
        return None
    fk = "gate.%s.files_x100" % m.group(1)
    lf, rf = lrec.get("keys", {}).get(fk), rrec.get("keys", {}).get(fk)
    lc_, rc_ = lrec.get("keys", {}).get(key), rrec.get("keys", {}).get(key)
    if any(isinstance(x, bool) or not isinstance(x, (int, float)) for x in (lf, rf, lc_, rc_)):
        return None
    pk = "gate.%s.crossings_per_file_x100" % m.group(1)
    lp, rp = lrec.get("keys", {}).get(pk), rrec.get("keys", {}).get(pk)
    if isinstance(lp, int) and isinstance(rp, int) and not isinstance(lp, bool) and not isinstance(rp, bool):
        return (lp / 100.0, rp / 100.0)      # the median of the per-offset ratios (W9 step 0)
    return (lc_ / max(1.0, lf / 100.0), rc_ / max(1.0, rf / 100.0))


def floor_pair(scn, key, lrec, rrec):
    """(legion, retail, per_file) as the Retail floor reads them: normalised keys, or the W8-1 per-file
    values for a GAP fixture's lane crossings."""
    pf = per_file_floor(scn, key, lrec, rrec)
    if pf is not None:
        return pf[0], pf[1], True
    return norm(key, lrec["keys"][key]), norm(key, rrec["keys"][key]), False


def floor_check(doc, rpt, recs):
    for (scn, mode), lrec in sorted(recs.items()):
        if mode != "legion" or (scn, "retail") not in recs:
            continue
        rrec = recs[(scn, "retail")]
        for key, lv in sorted(lrec.get("keys", {}).items()):
            fd = floor_dir(key)
            if fd is None or key not in rrec.get("keys", {}) or not floor_applies(key, lrec, rrec):
                continue
            if is_report_only(key, lrec) or is_report_only(key, rrec):
                rpt.report_only += 1
                continue
            lower = fd == "lower"
            lo, re_, per_file = floor_pair(scn, key, lrec, rrec)
            tol = u1_applies(scn, key)
            strict = floor_passes(lower, lo, re_)
            passes = floor_passes(lower, lo, re_, tol)
            pfx = " per file (W8-1)" if per_file else ""
            if per_file:
                rpt.info.append("gap per file (W8-1) %s/legion/%s: legion %s vs retail %s%s" % (
                    scn, key, fmt(lo), fmt(re_), "" if strict else " (above x1.1" + (", inside U1)" if passes else ")")))
            if passes and not strict:
                rpt.info.append("W9-U1 tolerance %s/legion/%s%s: legion %s <= max(retail x1.1, retail + 1) = %s" % (
                    scn, key, pfx, fmt(lo), fmt(max(re_ * FLOOR, re_ + 1))))
            exc = floor_exception_for(doc, scn, key)
            path = "%s/legion/%s" % (scn, key)
            need_wide(rpt, path, key, lrec)
            need_wide(rpt, "%s/retail/%s" % (scn, key), key, rrec)
            if exc:
                rpt.exception_count += 1
                if passes:
                    rpt.cleared.append("%s (legion %s vs retail %s%s, %s)" % (path, fmt(lo), fmt(re_), pfx, exc["cluster"]))
                else:
                    rpt.info.append("floor exception %s legion %s vs retail %s%s (%s)" % (path, fmt(lo), fmt(re_), pfx,
                                                                                         exc["cluster"]))
                need_median5(rpt, path, exc, lrec)
            elif not passes:
                rpt.fail(path, "Retail floor%s: legion %s vs retail %s (needs %s); a passing key may not start "
                               "failing the floor" % (pfx, fmt(lo), fmt(re_), "legion <= retail x%.1f" % FLOOR if lower
                                                      else "legion x%.1f >= retail" % FLOOR))


def phase_median(pg):
    v = sorted(pg["phases"])
    n = len(v)
    return v[n // 2] if n % 2 else (v[n // 2 - 1] + v[n // 2]) / 2.0


def phase_gate_ok(pg, w0, cur):
    """Lead ruling W4 (p): a phase-sensitive change (a striped scan whose residue shifts which tick a spike
    lands on) is judged on the MEDIAN over >= 10 stripe phases, not on the one phase a run happens to have.
    The entry's `phase_gate` {phases: [per-phase values], reason} passes when that median is at or below the
    W0 base and this run's value is one of the recorded phases' range (<= their maximum)."""
    ph = pg.get("phases") or []
    return len(ph) >= 10 and bool(pg.get("reason")) and phase_median(pg) <= w0 and cur <= max(ph)


def check(doc, recs, skipped=(), step=None, cumulative=False, require_all=False):
    rpt = Report()
    entries = doc.get("entries", {})
    for scn, modes in sorted(entries.items()):
        for mode, keys in sorted(modes.items()):
            rec = recs.get((scn, mode))
            if rec is None:
                if scn in skipped:
                    rpt.info.append("%s/%s skipped by the runner (needs data)" % (scn, mode))
                elif require_all:
                    rpt.fail("%s/%s" % (scn, mode), "no result in the run")
                continue
            for key, e in sorted(keys.items()):
                if is_report_only(key, rec) or key.startswith(REPORT_KEYS):
                    continue        # ruling (f): judged at click level; a reported work sum or why.* tag is never gated
                exc = exception_for(doc, scn, mode, key)
                if exc:
                    need_median5(rpt, "%s/%s/%s" % (scn, mode, key), exc, rec)
                if key in rec.get("keys", {}):
                    need_wide(rpt, "%s/%s/%s" % (scn, mode, key), key, rec)
                judge(doc, rpt, scn, mode, key, e, rec, step)
                spread_check(doc, rpt, scn, mode, key, e, rec, exc)
                if cumulative and key in COMBAT_KEYS and any(fnmatch.fnmatch(scn, g) for g in COMBAT_SCENARIOS):
                    w0 = e.get("w0")
                    w0 = e["value"] if isinstance(w0, bool) or w0 is None else w0
                    found, cur = lookup(rec, key)
                    pg = e.get("phase_gate")
                    if found and cur > w0 and pg and phase_gate_ok(pg, w0, cur):
                        rpt.info.append("%s/%s/%s: %s above the W0 base %s at this run's stripe phase; the "
                                        "median over %d phases is %s (ruling W4 (p))"
                                        % (scn, mode, key, cur, w0, len(pg["phases"]), phase_median(pg)))
                    elif found and cur > w0:
                        rpt.fail("%s/%s/%s" % (scn, mode, key),
                                 "cumulative combat cost: %s above the W0 base %s (x1.00, PLAN 3.0)" % (cur, w0))
            # a work counter that moved with no entry has no base to compare with
            for key, v in sorted(rec.get("keys", {}).items()):
                if key.startswith("work.") and v and key not in keys and not key.startswith(REPORT_WORK):
                    rpt.fail("%s/%s/%s" % (scn, mode, key),
                             "unbaselined work counter %s: a step that adds a class declares its per-unit bound" % v)
    for scn, mode in sorted(recs):
        if scn not in entries or mode not in entries[scn]:
            rpt.info.append("%s/%s has no baseline entries" % (scn, mode))
    floor_check(doc, rpt, recs)
    return rpt


def is_spread_exception(x):
    """An exception written for a key's offset spread (write_base / step-0 retakes), not for
    the Retail floor."""
    return bool(x.get("median5")) and "offset spread" in x.get("reason", "")[:24]


def apply_ratchet(doc, rpt, reason, when):
    refs = doc.setdefault("references", {})
    moved = []
    for path, old, new in rpt.ratchets:
        scn, mode, key = path.split("/", 2)
        refs.setdefault(scn, {}).setdefault(mode, {})[key] = -1 if new == INF else new
        moved.append({"key": path, "old": -1 if old == INF else old, "new": -1 if new == INF else new})
    cleared = []
    keep, converted = [], []
    for x in doc.get("exceptions", []):
        tag = "%s/legion/%s " % (x["scenario"], x["key"])
        # An offset-spread exception (median-of-5 gating) is not a floor exception: the key's
        # spread does not go away when its median passes the Retail floor, so it is kept.
        if is_spread_exception(x):
            keep.append(x)
            continue
        if any(c.startswith(tag) for c in rpt.cleared):
            cleared.append("%s/%s" % (x["scenario"], x["key"]))
            # Ruling (h): the floor exception may also have covered the key's offset spread in a
            # mode (check() records the spreads it masked). Clearing it would unmask that spread
            # and fail the next check, so it is converted to a spread exception for those modes.
            for (scn, mode, key), (lo, hi) in sorted(rpt.masked.items()):
                if scn != x["scenario"] or key != x["key"] or x.get("mode", mode) != mode:
                    continue
                converted.append({"scenario": scn, "key": key, "mode": mode, "cluster": x["cluster"],
                                  "median5": True,
                                  "reason": "offset spread %s..%s exceeds the band; kept as a spread exception "
                                            "when the ratchet cleared its Retail-floor exception (%s: %s)"
                                            % (fmt(lo), fmt(hi), when, x.get("reason", ""))})
        else:
            keep.append(x)
    # a spread another exception still covers needs no conversion
    for c in converted:
        if not any(k["scenario"] == c["scenario"] and k["key"] == c["key"] and k.get("mode", c["mode"]) == c["mode"]
                   for k in keep):
            keep.append(c)
    doc["exceptions"] = keep
    doc.setdefault("history", []).append({"when": when, "reason": reason, "ratchets": moved,
                                          "cleared_exceptions": cleared})
    return len(moved), len(cleared)


# ----------------------------------------------------------- baseline retake
def write_base(doc, recs, reason, since, patterns, hashes, clusters):
    """Step 0: take every (matching) key of the run as the head's base. Existing
    entries keep their rule, dir, band and reference; new ones get class
    defaults. Floor failures that are not yet exceptions are added with their
    cluster (`--cluster GLOB=CLUSTER`, default UNASSIGNED, which the reasons
    test rejects)."""
    entries = doc.setdefault("entries", {})
    added = changed = 0
    for (scn, mode), rec in sorted(recs.items()):
        slot = entries.setdefault(scn, {}).setdefault(mode, {})
        todo = {k: v for k, v in rec.get("keys", {}).items()
                if k not in SKIP_KEYS and not is_report_only(k, rec) and not k.startswith(REPORT_KEYS)
                and any(fnmatch.fnmatch(k, p) for p in patterns)}
        if hashes:
            for off, h in rec.get("hash", {}).items():
                todo["hash@" + off] = h
            if rec.get("serial_eq_workers") is not None:
                todo["serial_eq_workers"] = rec["serial_eq_workers"]
        for k, v in todo.items():
            e = slot.get(k)
            if e is None:
                if k.startswith("hash@") or k == "serial_eq_workers":
                    e = {"rule": "eq", "dir": "lower"}
                elif k.startswith("work."):
                    e = {"rule": "band", "dir": "lower", "band": BAND}
                elif k == "spins" and v == 0 or k.endswith("illegal_overlap_ticks") and v == 0 or k == "claims.bad_max":
                    e = {"rule": "eq", "dir": "lower"}
                else:
                    e = {"rule": "band", "dir": default_dir(k), "band": BAND}
                e["reason"] = reason
                e["since"] = since
                slot[k] = e
                added += 1
            elif e.get("value") != v:
                changed += 1
            e["value"] = v
        for k, vals in rec.get("varying", {}).items():
            e = slot.get(k)
            if e is None or e.get("rule") != "band" or k.startswith("work."):
                continue
            lo, hi, ratio = spread_of(k, vals)
            if ratio <= e.get("band", BAND):
                continue
            exc = exception_for(doc, scn, mode, k)
            if exc is None:
                cluster = next((c for g, c in clusters if fnmatch.fnmatch("%s/%s" % (scn, k), g)), "offset-spread")
                doc.setdefault("exceptions", []).append(
                    {"scenario": scn, "key": k, "mode": mode, "cluster": cluster, "median5": True,
                     "reason": "3-offset spread %s..%s exceeds the band at %s; gated on median-of-5 offsets"
                               % (fmt(lo), fmt(hi), since)})
            else:
                exc["median5"] = True
        if mode == "legion" and (scn, "retail") in recs:
            for k, v in rec.get("keys", {}).items():
                fd = floor_dir(k)
                rv = recs[(scn, "retail")].get("keys", {}).get(k)
                if fd is None or rv is None:
                    continue
                if not floor_applies(k, rec, recs[(scn, "retail")]):
                    continue
                if is_report_only(k, rec) or is_report_only(k, recs[(scn, "retail")]):
                    continue
                lo, rr, _pf = floor_pair(scn, k, rec, recs[(scn, "retail")])
                ok = floor_passes(fd == "lower", lo, rr)
                if ok or floor_exception_for(doc, scn, k):
                    continue
                cluster = next((c for g, c in clusters if fnmatch.fnmatch("%s/%s" % (scn, k), g)), "UNASSIGNED")
                doc["exceptions"].append({"scenario": scn, "key": k, "cluster": cluster,
                                          "reason": "fails legion <= retail x%.1f at %s: legion %s vs retail %s"
                                                    % (FLOOR, since, fmt(lo), fmt(rr))})
    return added, changed


# ----------------------------------------------------- exit table / anchor diff
def pct(old, new):
    if old == new:
        return 0.0
    if old == 0:
        return INF
    return (new - old) / abs(old) * 100.0


def exit_table(old, new, intended):
    rows = []
    for k in sorted(set(old) & set(new)):
        o, n = old[k], new[k]
        p = pct(o, n)
        if abs(p) > 5.0:
            path = "%s/%s/%s" % k
            mark = "intended" if any(fnmatch.fnmatch(path, g) for g in intended) else "incidental (within band)"
            rows.append((path, o, n, p, mark))
    return rows


def run_exit_table(args, recs):
    old = flatten(args.base)
    new = {}
    for (s, m), r in recs.items():
        for k, v in r.get("keys", {}).items():
            new[(s, m, k)] = v
    intended = list(args.intended)
    if args.intended_file:
        intended += [l.strip() for l in open(args.intended_file) if l.strip() and not l.startswith("#")]
    rows = exit_table(old, new, intended)
    print("| key | old | new | change | kind |")
    print("|---|---|---|---|---|")
    for path, o, n, p, mark in rows:
        print("| %s | %s | %s | %s | %s |" % (path, o, n, "new" if p == INF else "%+.1f%%" % p, mark))
    print("%d keys moved by more than 5%%; %d intended, %d incidental"
          % (len(rows), sum(r[4] == "intended" for r in rows), sum(r[4] != "intended" for r in rows)))
    return 0


def run_anchor(args, recs):
    anchor = flatten(args.anchor)
    cur = {}
    for (s, m), r in recs.items():
        for k, v in r.get("keys", {}).items():
            cur[(s, m, k)] = v
    drift = []
    for k in sorted(set(anchor) & set(cur)):
        a, c = anchor[k], cur[k]
        if a == c:
            continue
        lower = default_dir(k[2]) == "lower"
        p = pct(a, c)
        worseish = (c > a) == lower
        drift.append(("%s/%s/%s" % k, a, c, p, worseish))
    drift.sort(key=lambda d: -(abs(d[3]) if d[3] != INF else 1e18))
    print("anchor drift (frozen %s; report only, never gates)" % args.anchor)
    for path, a, c, p, w in drift:
        print("  %-60s %s -> %s  (%s) %s" % (path, a, c, "new" if p == INF else "%+.1f%%" % p,
                                              "worse" if w else "better"))
    print("%d of %d common keys differ from the anchor; %d worse, %d better"
          % (len(drift), len(set(anchor) & set(cur)), sum(d[4] for d in drift), sum(not d[4] for d in drift)))
    return 0


# --------------------------------------------------------------------------- cli
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=("check", "baseline", "exit-table", "anchor", "validate"))
    ap.add_argument("--baseline")
    ap.add_argument("--results", nargs="+", default=[])
    ap.add_argument("--step")
    ap.add_argument("--cumulative", action="store_true")
    ap.add_argument("--require-all", action="store_true")
    ap.add_argument("--ratchet", metavar="REASON")
    ap.add_argument("--date", help="history timestamp (default now; tests pin it)")
    ap.add_argument("--reason")
    ap.add_argument("--since", default="W0")
    ap.add_argument("--keys", action="append", default=None)
    ap.add_argument("--hashes", action="store_true")
    ap.add_argument("--cluster", action="append", default=[])
    ap.add_argument("--base")
    ap.add_argument("--intended", action="append", default=[])
    ap.add_argument("--intended-file")
    ap.add_argument("--anchor")
    args = ap.parse_args(argv)

    def need(v, name):
        if not v:
            print("legion_check: %s needs %s" % (args.cmd, name), file=sys.stderr)
            sys.exit(2)

    if args.cmd == "anchor":
        need(args.anchor, "--anchor")
        recs, _ = load_results(args.results)
        return run_anchor(args, recs)
    if args.cmd != "exit-table":
        need(args.baseline, "--baseline")
    if args.cmd == "validate":
        doc = load_json(args.baseline)
        bad = validate_baseline(doc)
        for b in bad:
            print("FAIL " + b)
        print("%s: %d problems" % ("PASS" if not bad else "FAIL", len(bad)))
        return 1 if bad else 0
    need(args.results, "--results")
    recs, skipped = load_results(args.results)
    if args.cmd == "exit-table":
        need(args.base, "--base")
        return run_exit_table(args, recs)
    try:
        doc = load_json(args.baseline)
    except FileNotFoundError:
        if args.cmd != "baseline":
            raise
        doc = empty_baseline()
    if args.cmd == "baseline":
        need(args.reason, "--reason")
        clusters = []
        for c in args.cluster:
            g, _, name = c.partition("=")
            clusters.append((g, name))
        added, changed = write_base(doc, recs, args.reason, args.since, args.keys or ["*"], args.hashes, clusters)
        with open(args.baseline, "w") as f:
            json.dump(doc, f, indent=1, sort_keys=True)
            f.write("\n")
        bad = validate_baseline(doc)
        print("baseline %s: %d entries added, %d values changed" % (args.baseline, added, changed))
        for b in bad:
            print("FAIL " + b)
        return 1 if bad else 0

    bad = validate_baseline(doc)
    if bad:
        for b in bad:
            print("FAIL " + b)
        return 1
    rpt = check(doc, recs, skipped, args.step, args.cumulative, args.require_all)
    for ln in rpt.lines():
        print(ln)
    if args.ratchet is not None:
        if not rpt.ok:
            print("not ratcheting: the check failed")
            return 1
        if not args.ratchet.strip():
            print("legion_check: --ratchet needs a reason", file=sys.stderr)
            return 2
        when = args.date or datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        n, c = apply_ratchet(doc, rpt, args.ratchet, when)
        with open(args.baseline, "w") as f:
            json.dump(doc, f, indent=1, sort_keys=True)
            f.write("\n")
        print("ratcheted %d references, cleared %d exceptions; history line written" % (n, c))
    return 0 if rpt.ok else 1


if __name__ == "__main__":
    sys.exit(main())
