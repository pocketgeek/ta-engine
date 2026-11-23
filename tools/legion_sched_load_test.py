#!/usr/bin/env python3
"""legion_sched_load_test -- the field scheduler under continuous load (Legion 10,000 stage 4).

    legion_sched_load_test.py LEGION_SCENARIO SCN [--budget-div N] [--field-bytes N] [--ticks N]

Runs SCN in Legion, serial and workers, and checks SERVICE, not throughput. In every run: serial == workers,
and while memory is short (more than half the field allowance in use) no new build takes its kind past the
windows it can be served in (scale.sched.inflight_max.first <= kFirstInflight, .refresh <= kRefreshInflight,
measured at each such admission; many-2500.scn, where the per-class count alone let 78 refreshes into flight:
ctest legion_sched_inflight). The rest depends on the file:

tools/scenarios/scale/sched-load.scn (48 squads re-clicked to far tiles every 160 ticks and a rock walking
across the map, so field requests arrive faster than the quota serves them for the whole run):

  * serial == workers (the runner fails on any state or key mismatch);
  * the field quota really was saturated (the longest run of fully spent ticks), so the bounds below are
    measured under load;
  * every service class's longest gap between two slices of work for a job that asked for work
    (scale.sched.gap_max.*) within 3 x kAgeStep (age promotion to the top class) + one round-robin rotation of
    the largest possible top class (1,024 fields of kSlice each over the quota): 539 ticks;
  * every class's longest start-request wait (scale.sched.age_max.*) within the promotion time, the
    reservation age and one field tenure: 900 ticks (memory never runs short here: the quota is the limit);
  * no body waits longer than 60 ticks for a usable field (scale.wait.max, the frozen scale target);
  * movement goes on: most bodies arrive somewhere over the run (scale.moving.peak, scale.arrive.n > 0).
  With --budget-div N the registration, plane-build and awareness budgets are divided (over-budget paths);
  the service bounds still hold, and the run must really defer registrations and wait for plane builds
  (scale.sched.reg_deferred, plane_waits > 0).

A file with `navbytes`, or any file run with --field-bytes (tools/scenarios/scale/sched-mix.scn: a field
allowance of two whole-map fields, short-hop squads re-clicked every 120 ticks round squads crossing the map;
many-1000.scn --field-bytes 778752 --ticks 3000): memory, not the quota, is the limit, and large requests
compete with a stream of small ones. Checks:

  * the memory-pressure mechanisms ran in the sim: capacity refusals stepped over (fit_skips), reservations,
    request age promotion (and, unless --ticks shortens the run, shares admitted behind a stopped scan);
  * admission liveness: no run of consecutive ticks with a refusal for room and no new build longer than
    kFieldTenure + kStillScan + the longest build (below) -- every finished field, shared ones included, is a
    victim one tenure after it starts (scale.sched.admit_gap_max; many-1000 at two maps froze for ~1,800
    ticks when shared fields were never victims);
  * every class's longest start-request wait within a bound DERIVED FROM CAPACITY:

        steps(class) x kAgeStep + kReserveAge + rounds x (kFieldTenure + kStillScan)
        rounds = ceil(4 x request_cells_max / (A - 4 W H))

    A = the allowance (navbytes), 4 W H = one whole-map field (the largest request), request_cells_max = the
    most cells the start requests listed in one tick needed together (measured: the queue, in memory).
    Why: requests are served oldest first (a lower class rises one class per kAgeStep, steps(class) to the
    top); a request refused for room reserves after kReserveAge, and from then on nothing takes new room
    before it (shares take none). Every finished field is a victim kFieldTenure after it started (a share
    lands within kStillScan of the start, so sharing extends that by no more), so within one "round" of
    kFieldTenure + kStillScan ticks all the memory held at its start can be freed, which admits at least
    A - 4 W H of the queue's cells. That needs every build to finish within its tenure, checked from the
    file: kFirstInflight whole-map builds (8 W H relaxations + W H / kInitDiv initialisation each) at half
    the quota (the other half may pay plane debt) take at most kFieldTenure ticks.
    On sched-mix the bound fails if the reservation is removed (the large requests starve: 4,497 ticks) or
    requests are served by id instead of age (4,604), both measured.
  * movement goes on (scale.arrive.n > 0).

Exit 0 pass, 1 fail, 2 tooling.
"""
import json
import re
import subprocess
import sys

K_AGE_STEP, K_RESERVE, K_TENURE, K_STILL = 150, 150, 300, 30
K_SLICE, K_QUOTA, K_FIELDS, K_INIT_DIV = 32768, 384000, 1024, 16
K_FIRST_INFLIGHT, K_REFRESH_INFLIGHT = 24, 6
GAP_BOUND = 3 * K_AGE_STEP + (K_FIELDS * K_SLICE + K_QUOTA - 1) // K_QUOTA + 1
AGE_BOUND = 3 * K_AGE_STEP + K_RESERVE + K_TENURE
CLASSES = ("waiting", "first_route", "aware_repair", "stale_refresh")   # steps to the top: 0, 1, 2, 3


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    exe, scn = sys.argv[1], sys.argv[2]
    extra = sys.argv[3:]
    try:
        text = open(scn).read()
    except OSError as e:
        print("legion_sched_load_test:", e)
        return 2
    nb = re.search(r"^navbytes (\d+)", text, re.M)
    if "--field-bytes" in extra:
        nb = re.match(r"(\d+)", extra[extra.index("--field-bytes") + 1])
    service = "sched-load" in scn
    mp = re.search(r"^map flat (\d+) (\d+)", text, re.M)
    cmd = [exe, scn, "--mode", "legion", "--offsets", "0", "--both-exec", "--json", "--scale"] + extra
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stderr[-4000:])
        print("legion_sched_load_test: runner exit", p.returncode)
        return 1
    try:
        rec = json.loads(p.stdout.strip().splitlines()[-1])
    except Exception as e:
        print("legion_sched_load_test: no JSON line:", e)
        return 2
    k = rec["keys"]
    fails = []

    def need(ok, what):
        print("  %-86s %s" % (what, "ok" if ok else "FAIL"))
        if not ok:
            fails.append(what)

    def s(key):
        return k.get("scale.sched." + key, 0)

    need(rec.get("serial_eq_workers") is True, "serial == workers")
    need(s("inflight_max.first") <= K_FIRST_INFLIGHT,
         "first builds in flight within their windows: %d <= %d" % (s("inflight_max.first"), K_FIRST_INFLIGHT))
    need(s("inflight_max.refresh") <= K_REFRESH_INFLIGHT,
         "refreshes in flight within their windows: %d <= %d" % (s("inflight_max.refresh"), K_REFRESH_INFLIGHT))
    if nb:
        if not mp:
            print("legion_sched_load_test: a navbytes file needs `map flat W H`")
            return 2
        w, h = int(mp.group(1)), int(mp.group(2))
        whole = 4 * w * h
        allowance = max(int(nb.group(1)), 2 * whole)   # the navigator's floor: two whole-map fields
        build = (K_FIRST_INFLIGHT * (8 * w * h + w * h // K_INIT_DIV) + K_QUOTA // 2 - 1) // (K_QUOTA // 2)
        if build > K_TENURE:
            print("legion_sched_load_test: the bound needs builds within their tenure (%d > %d ticks)" % (build, K_TENURE))
            return 2
        need(s("fit_skips") > 0, "capacity refusals stepped over (fit_skips %d)" % s("fit_skips"))
        need(s("reservations") > 0, "reservations (%d ticks)" % s("reservations"))
        need(s("request_promotions") > 0, "request age promotion (%d request-ticks)" % s("request_promotions"))
        if "--ticks" not in extra:
            need(s("stopped_shares") > 0, "shares admitted behind a stopped scan (%d)" % s("stopped_shares"))
        gap = K_TENURE + K_STILL + build
        need(s("admit_gap_max") <= gap, "admission liveness: longest refusal run without a new build %d <= %d ticks"
             % (s("admit_gap_max"), gap))
        cells = s("request_cells_max")
        need(cells > 0, "the request queue was measured (request_cells_max %d)" % cells)
        rounds = -(-4 * cells // (allowance - whole))
        print("  (allowance %d B = %.2f whole-map fields; queue peak %d cells = %.2f fields; %d rounds of %d ticks; "
              "builds within %d ticks)" % (allowance, allowance / whole, cells, 4 * cells / whole, rounds,
                                           K_TENURE + K_STILL, build))
        for steps, c in enumerate(CLASSES):
            bound = steps * K_AGE_STEP + K_RESERVE + rounds * (K_TENURE + K_STILL)
            a = s("age_max." + c)
            need(a <= bound, "longest start-request wait, %s: %s <= %s (capacity bound)" % (c, a, bound))
        need(k.get("scale.arrive.n", 0) > 0, "bodies move and arrive (%d)" % k.get("scale.arrive.n", 0))
    elif service:
        peg = k.get("gauge.quota_peg_run_max", 0)
        need(peg >= 100, "the field quota was saturated (longest fully spent run %s ticks >= 100)" % peg)
        for c in CLASSES:
            g = s("gap_max." + c)
            need(g <= GAP_BOUND, "longest service gap, %s: %s <= %s" % (c, g, GAP_BOUND))
        for c in CLASSES:
            a = s("age_max." + c)
            need(a <= AGE_BOUND, "longest start-request wait, %s: %s <= %s" % (c, a, AGE_BOUND))
        wmax = k.get("scale.wait.max", 0)
        need(wmax <= 60, "longest wait for a usable field: %s <= 60 ticks" % wmax)
        need(k.get("scale.arrive.n", 0) > 0 and k.get("scale.moving.peak", 0) > 0, "bodies move and arrive")
        if "--budget-div" in extra:
            need(s("reg_deferred") > 0, "registrations deferred past the budget (%d)" % s("reg_deferred"))
            need(s("plane_waits") > 0, "registrations waiting for an incremental plane build (%d)" % s("plane_waits"))
    print("  (promotions %s, request promotions %s, fit skips %s, reservations %s, stopped shares %s, admissions %s, "
          "registrations deferred %s, longest registration wait %s)" % (
              s("promotions"), s("request_promotions"), s("fit_skips"), s("reservations"), s("stopped_shares"),
              s("admissions"), s("reg_deferred"), s("reg_wait_max")))
    if fails:
        print("legion_sched_load_test: %d FAILED" % len(fails))
        return 1
    print("legion_sched_load_test: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
