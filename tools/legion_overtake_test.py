#!/usr/bin/env python3
"""legion_overtake_test -- overtaking a slower ally (2026-11-15).

    legion_overtake_test.py LEGION_SCENARIO SCENARIO_DIR

A single-file column of 12 bodies walks north, slow 2x2 bodies (speed 1.15, an Aramon Archer's) and fast 3x3 ones
(2.9, an Aramon Knight's) mixed, each on an order of its own, as the computer player sends its army. Before, a fast
body behind a slow one held, walked a cell when the slow one had walked one and held again, all the way: flow-around
takes one cell, and a 3x3 body behind a 2x2 one needs two or three cells sideways before its lane is clear. It now
steps aside and passes where there is room (overtakeRoute in src/sim/legion.cpp). All asset-free, all offsets
spawn inside the walls.

  overtake-field (open ground, attack-move orders), offsets 0, +-1..3. Parent 870296cf: fast bodies' last arrival
    1,143..1,687 (over 1,200 at 4 of 7 offsets), 70 stops in all; now 1,128..1,148, none. Checked: the last fast
    body done by tick 1,260 at every offset (1,200 until 2026-11-18), at most 10 fast stops in all, at least one overtake.
  overtake-road (a road 8 cells wide, move orders), offsets 0, +-1..3. Parent: fast bodies' mean arrival median
    1,674 (1,533..2,182), 1,558 stops; now median 1,064 (992..1,575), 173 stops. Checked: that median at most 1,300,
    at most 400 fast stops in all.
  overtake-corridor (4 cells wide: no room to pass, attack-move orders), offsets 0, +-1, 2. Parent: fast mean
    2,178..2,230, slow bodies' last 2,709..2,761; now 2,117..2,171 and 2,682..2,734. Checked: no worse -- the fast
    mean at most 2,250 and the slow bodies' last at most 2,780 at every offset.
  overtake-patrol, -attack, -guard (overtake-field's column on patrol, attack and guard orders; 2026-11-19), offsets
    0, +-1..3. Main 6fad01ac passed on move and fight legs only: fast stops 706 / 786 / 2,674 in all; now 66 / 12 / 7.
    Checked: at least one overtake each, at most 150 / 60 / 60 fast stops.
Every body arrives in the first three, and the slow bodies' last arrival is no later than tick 2,800 on field and road
(parent 2,640..2,784): nobody pays for a pass.

The same columns sent with ONE right-click to one point (overtake-click-*, as a player sends a mixed selection), and
one-click fixtures of 10-48 bodies: single-file columns (-col-*, -field, -road, -corridor), loose columns two abreast
(-loose-*), bunches wider than deep (-bunch-*) sent 300, 600 and 1,000 px; knights at the rear of a block
(-rear-200/240/300/500); three speeds (-mid-300/320, -mix3-1000, -rear-600: knights behind the swordsmen behind the
mid bodies, sent 583 px); knights in front (-front-500); the larger blocks (-blob
32 bodies, -choke 40 through a 12-cell gap, -block 48 standing 8 abreast); and the point just past a narrowing the click
comes through (-gate: 36 bodies of three speeds, knights at the rear, through a 12-cell gap in a wall to a point 10
cells past it; -bridge: the same over a 12-cell land bridge across water; -gateangle: 40 bodies through a gap they
come at aslant, to a point 6 cells past it); a point 4 cells from a wall (-wallside: 30 bodies, knights at the rear).
Since the arrival layout (2026-11-16, arrivalLayout in src/sim/legion.cpp) such a click of several speed groups and
at most 48 members gives each body its own spot, fast rows in front, its fast bodies pass slower ones of the click
anywhere on the way, a slower body steps aside for a faster one with no lane round it (laidGiveWay) and, just outside
the click's area, slows while a faster one passes it (laidPace). Rows that would reach back onto a narrowing behind
the point, or a three-speed click's rows that stand against terrain, give the formation's own shape instead
(FALLBACK: -gate, -bridge, -gateangle, -wallside). Owner (2026-11-16): "fast units even if it delays the slow units". Checked:
  - the columns' and the blob's per-offset limits on the fast bodies, as on main f3a4c16e (clicks()): field and road
    fast mean at most 1,050 at every offset and at most 60 / 150 fast stops; corridor fast mean at most 2,250; the
    blob's last fast body at most 2,150;
  - per fixture over its offsets (0, +-1..6; the road and field columns, the blob 0, +-1..3, the corridor 0, +-1, 2),
    each body's arrival its settle tick (--dump-units), against main f3a4c16e (CLICKS):
    - every body settles (a body standing on its own spot when the order comes counts as arrived at once);
    - no body moves more than 25 px after it settles (one yield; no wandering, no re-shuffling);
    - no two bodies settle on each other's cells;
    - lanes: no more pairs of bodies of one kind swap sides across the way between start and end (each more than
      16 px apart across the way, from the members' starting centroid toward the point) than on main (CROSS);
    - each body against its own slot: at most 5% of the bodies, and 8% of the fast ones, settle more than 24 px
      from it (not where the layout gives the formation's own shape: FALLBACK);
    - each faster body against the frontmost body of every slower group: at most 6% of the faster bodies stand more
      than a row (48 px) behind that of any slower group, along the way in from the members' starting centroid (not in
      the corridor, 4 cells wide: one file; nor FALLBACK);
    - the fast group's median over the offsets of its mean arrival is earlier than main's, except where the fast
      bodies start at the rear or in the middle of a short order (SHORT_REAR: they walk round the slow ones to the front
      rows and come in later -- the owner accepted it), and no later than main's where the layout gives the
      formation's own shape (FALLBACK). Slow and mid groups may arrive later; their numbers are printed.
  The owner signed off (2026-11-16) on field and road coming in level with main (LEVEL, at most 1% later) and on
  knights starting behind slower rows settling short of their spots (REAR_START: per-fixture bars, measured).
Exit 0 pass, 1 fail, 2 tooling.
"""
import concurrent.futures
import json
import os
import statistics
import subprocess
import sys


def run(exe, scenarios, name, offsets):
    scn = os.path.join(scenarios, name + ".scn")
    p = subprocess.run([exe, scn, "--mode", "legion", "--offsets", offsets, "--serial", "--json"],
                       capture_output=True, text=True)
    if p.returncode != 0:
        print(p.stdout[-2000:], p.stderr[-4000:])
        return None
    rows = [json.loads(ln) for ln in p.stdout.splitlines() if ln.startswith("{")]
    if not rows:
        return None
    keys, varying, offs = rows[0]["keys"], rows[0].get("varying", {}), rows[0]["offsets"]
    groups = sorted({k.split(".")[1] for k in keys if k.startswith("g.") and k.endswith(".n")})

    def per(key):
        return varying.get(key, [keys.get(key, 0)] * len(offs))   # no overtakes key before the rule

    out = []
    for i, off in enumerate(offs):
        r = {"offset": off, "overtakes": per("work.overtakes.total")[i],
             "click_overtakes": per("work.click_overtakes.total")[i]}
        for kind in "FS":
            mine = [g for g in groups if g.startswith(kind)]
            done = [per(f"g.{g}.done")[i] for g in mine]
            r[kind + "done"] = done
            r[kind + "stops"] = sum(per(f"g.{g}.stops")[i] for g in mine)
        out.append(r)
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    exe, scenarios = sys.argv[1], sys.argv[2]
    bad = []
    runs = {}
    for name, offsets in (("overtake-field", "0,1,-1,2,-2,3,-3"), ("overtake-road", "0,1,-1,2,-2,3,-3"),
                          ("overtake-corridor", "0,1,-1,2")):
        rows = run(exe, scenarios, name, offsets)
        if rows is None:
            return 2
        runs[name] = rows
        for r in rows:
            fmean = statistics.mean(r["Fdone"]) if min(r["Fdone"]) >= 0 else -1
            print(f"{name} offset {r['offset']:+d}: fast done {r['Fdone']} mean {fmean:.0f} stops {r['Fstops']}; "
                  f"slow last {max(r['Sdone'])} stops {r['Sstops']}; overtakes {r['overtakes']}")
            if min(r["Fdone"] + r["Sdone"]) < 0:
                bad.append(f"{name} offset {r['offset']}: a body never arrived")
    # Patrol, attack and guard legs pass too (2026-11-19, owner: "try all three"): overtake-field's column on those
    # orders. Before, only move and fight legs passed: a fast body kept station behind a slow one all the way.
    # Fast stops over the 7 offsets, main 6fad01ac -> now: patrol 706 -> 66, attack 786 -> 12, guard 2,674 -> 7.
    # Checked: at least one overtake and at most the bar's fast stops in all.
    for name, most in (("overtake-patrol", 150), ("overtake-attack", 60), ("overtake-guard", 60)):
        rows = run(exe, scenarios, name, "0,1,-1,2,-2,3,-3")
        if rows is None:
            return 2
        stops, passes = sum(r["Fstops"] for r in rows), sum(r["overtakes"] for r in rows)
        print(f"{name}: fast stops {stops}, overtakes {passes}")
        if passes < 1:
            bad.append(f"{name}: no overtake")
        if stops > most:
            bad.append(f"{name}: {stops} fast stops (at most {most})")
    if bad:
        for b in bad:
            print("FAIL:", b)
        return 1
    field, road, corridor = runs["overtake-field"], runs["overtake-road"], runs["overtake-corridor"]
    for r in field:
        # 1,260 since 2026-11-18 (owner: "land them"): a fast body that would keep station behind a slower one
        # passes it instead (drive's follow rule asks overtakeRoute), and at offset +1 the last knight's pass
        # comes later (1,253).
        if max(r["Fdone"]) > 1260:
            bad.append(f"overtake-field offset {r['offset']}: last fast body done at {max(r['Fdone'])} (at most 1,260)")
    if sum(r["Fstops"] for r in field) > 10:
        bad.append(f"overtake-field: {sum(r['Fstops'] for r in field)} fast stops (at most 10)")
    if sum(r["overtakes"] for r in field) < 1:
        bad.append("overtake-field: no overtake")
    median = statistics.median(statistics.mean(r["Fdone"]) for r in road)
    if median > 1300:
        bad.append(f"overtake-road: median fast mean arrival {median:.0f} (at most 1,300)")
    if sum(r["Fstops"] for r in road) > 400:
        bad.append(f"overtake-road: {sum(r['Fstops'] for r in road)} fast stops (at most 400)")
    for name, rows in (("overtake-field", field), ("overtake-road", road)):
        for r in rows:
            if max(r["Sdone"]) > 2800:
                bad.append(f"{name} offset {r['offset']}: last slow body done at {max(r['Sdone'])} (at most 2,800)")
    for r in corridor:
        if statistics.mean(r["Fdone"]) > 2250:
            bad.append(f"overtake-corridor offset {r['offset']}: fast mean {statistics.mean(r['Fdone']):.0f} (at most 2,250)")
        if max(r["Sdone"]) > 2780:
            bad.append(f"overtake-corridor offset {r['offset']}: last slow body {max(r['Sdone'])} (at most 2,780)")
    bad += clicks(exe, scenarios)
    bad += layout_clicks(exe, scenarios)
    for b in bad:
        print("FAIL:", b)
    return 1 if bad else 0


def clicks(exe, scenarios):
    """The one-click columns' and the blob's per-offset limits on the fast bodies, as on main f3a4c16e (the slow ones'
    limits went with the owner's decision: they may arrive later); returns the failures."""
    bad = []
    runs = {}
    for name, offsets in (("overtake-click-field", "0,1,-1,2,-2,3,-3"), ("overtake-click-road", "0,1,-1,2,-2,3,-3"),
                          ("overtake-click-corridor", "0,1,-1,2"), ("overtake-click-blob", "0,1,-1,2,-2,3,-3")):
        rows = run(exe, scenarios, name, offsets)
        if rows is None:
            return [f"{name}: legion_scenario failed"]
        runs[name] = rows
        for r in rows:
            if min(r["Fdone"]) < 0:
                bad.append(f"{name} offset {r['offset']}: a fast body never arrived")
    if bad:
        return bad
    for name, (fmax, smax) in {"overtake-click-field": (1050, 60), "overtake-click-road": (1050, 150)}.items():
        rows = runs[name]
        for r in rows:
            if statistics.mean(r["Fdone"]) > fmax:
                bad.append(f"{name} offset {r['offset']}: fast mean {statistics.mean(r['Fdone']):.0f} (at most {fmax:,})")
        if sum(r["Fstops"] for r in rows) > smax:
            bad.append(f"{name}: {sum(r['Fstops'] for r in rows)} fast stops (at most {smax})")
        print(f"{name}: fast means {[round(statistics.mean(r['Fdone'])) for r in rows]}, fast stops "
              f"{sum(r['Fstops'] for r in rows)}")
    for r in runs["overtake-click-corridor"]:
        if statistics.mean(r["Fdone"]) > 2250:
            bad.append(f"overtake-click-corridor offset {r['offset']}: fast mean "
                       f"{statistics.mean(r['Fdone']):.0f} (at most 2,250)")
    # The blob's last knight at most 2,150 (main's cap) at every offset.
    bad += [f"overtake-click-blob offset {r['offset']}: fast done {max(r['Fdone'])} (at most 2,150)"
            for r in runs["overtake-click-blob"] if max(r["Fdone"]) > 2150]
    print(f"overtake-click-blob: last fast body {[max(r['Fdone']) for r in runs['overtake-click-blob']]}")
    return bad


# One right-click per fixture (overtake-click-<name>), each speed group's numbers on main f3a4c16e: (median over the
# offsets of the group's mean arrival, ... of its t90, bodies unsettled over the offsets). Each body's arrival is its
# settle tick (--dump-units).
ALL_OFFSETS = (0, 1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 6, -6)
SEVEN = (0, 1, -1, 2, -2, 3, -3)   # the road's and the field's columns spawn in the walls past +-3
CLICKS = {
    "col-300": (ALL_OFFSETS, {"fast": (168, 303, 0), "slow": (301, 446, 0)}),
    "col-600": (ALL_OFFSETS, {"fast": (312, 596, 0), "slow": (627, 803, 0)}),
    "col-1000": (ALL_OFFSETS, {"fast": (432, 635, 0), "slow": (991, 1175, 0)}),
    "loose-300": (ALL_OFFSETS, {"fast": (218, 339, 0), "slow": (295, 350, 0)}),
    "loose-600": (ALL_OFFSETS, {"fast": (417, 680, 0), "slow": (608, 751, 0)}),
    "loose-1000": (ALL_OFFSETS, {"fast": (596, 947, 0), "slow": (1049, 1265, 0)}),
    "bunch-300": (ALL_OFFSETS, {"fast": (185, 272, 0), "slow": (316, 374, 0)}),
    "bunch-600": (ALL_OFFSETS, {"fast": (354, 565, 0), "slow": (612, 841, 0)}),
    "bunch-1000": (ALL_OFFSETS, {"fast": (465, 631, 0), "slow": (1010, 1094, 0)}),
    "rear-200": (ALL_OFFSETS, {"fast": (151, 168, 0), "slow": (176, 203, 0)}),
    "rear-300": (ALL_OFFSETS, {"fast": (259, 325, 0), "slow": (299, 343, 0)}),
    "rear-500": (ALL_OFFSETS, {"fast": (411, 438, 0), "slow": (416, 478, 0)}),
    "mid-300": (ALL_OFFSETS, {"fast": (224, 265, 0), "mid": (252, 285, 0), "slow": (264, 301, 0)}),
    "mid-320": (ALL_OFFSETS, {"fast": (260, 309, 0), "mid": (228, 248, 0), "slow": (298, 405, 0)}),
    "front-500": (ALL_OFFSETS, {"fast": (200, 211, 0), "slow": (562, 574, 0)}),
    "mix3-1000": (ALL_OFFSETS, {"fast": (660, 735, 0), "mid": (694, 726, 0), "slow": (842, 1078, 0)}),
    "field": (SEVEN, {"fast": (928, 1019, 0), "slow": (2396, 2641, 0)}),
    "road": (SEVEN, {"fast": (959, 1068, 0), "slow": (2334, 2462, 0)}),
    "corridor": ((0, 1, -1, 2), {"fast": (2190, 2574, 0), "slow": (2402, 2708, 0)}),
    "blob": (SEVEN, {"fast": (1933, 2015, 0), "slow": (2039, 2121, 0)}),
    "choke": (ALL_OFFSETS, {"fast": (1640, 2120, 0), "mid": (2079, 2345, 0), "slow": (2228, 2550, 0)}),
    "block": (ALL_OFFSETS, {"fast": (1035, 1998, 0), "mid": (2083, 2330, 0), "slow": (2265, 2660, 0)}),
    "gate": (ALL_OFFSETS, {"fast": (712, 894, 0), "mid": (582, 824, 0), "slow": (1006, 1269, 0)}),
    "bridge": (ALL_OFFSETS, {"fast": (798, 1050, 0), "mid": (688, 836, 0), "slow": (1122, 1297, 0)}),
    "gateangle": (ALL_OFFSETS, {"fast": (650, 860, 0), "mid": (724, 987, 0), "slow": (1142, 1490, 0)}),
    "wallside": (ALL_OFFSETS, {"fast": (652, 911, 0), "mid": (841, 1040, 0), "slow": (860, 979, 0)}),
    "rear-600": (ALL_OFFSETS, {"fast": (333, 499, 0), "mid": (314, 353, 0), "slow": (680, 927, 0)}),
    "rear-240": (ALL_OFFSETS, {"fast": (174, 246, 0), "slow": (272, 504, 0)})}
# Fast bodies starting at the rear or in the middle of a short order walk round the slower ones to the front rows and
# come in later than on main, where they stood behind them (owner, 2026-11-16: "for short moves them arriving later
# is kinda expected"). Their numbers are printed; every other check holds for them. rear-600 (583 px) joined them
# on 2026-11-18 (owner: "land them"): with followers passing instead of keeping station its knights' mean median
# came to 352 (main 333).
SHORT_REAR = {"rear-200", "rear-300", "mid-300", "mid-320", "rear-240", "rear-600"}
# Where the layout gives the formation's own shape (arrivalLayout): rows that would reach back onto a narrowing the
# click comes through (-gate, -bridge: the point just past it) or, in a click of three speed groups, stand against
# terrain (-gateangle: a gap approached aslant; -wallside: a point 4 cells from a wall). No spot of the layout there, so the slot and depth-order checks do
# not apply; the fast bodies arrive no later than on main, and every other check holds.
FALLBACK = {"gate", "bridge", "gateangle", "wallside"}
# Signed off by the owner on 2026-11-16 after the last verification round ("land with sign-off"):
#  - LEVEL: on the 2,300 px single-file columns (field, road) every knight passes the swordsmen early and arrives at
#    full speed on main too, so the fast group comes in level with main: it may be at most 1% later there.
#  - REAR_START: knights starting behind slower rows are held behind them as those rows fill and settle up to ~180 px
#    short of their own spots (rear-500, rear-600, rear-240). Per fixture: % of the bodies, % of the fast ones off
#    their slot, % of the faster ones a row behind a slower front, and lanes crossed (rear-500: the held knights'
#    re-choices and a swordsman column rounding off the way's slant cross 6 pairs where main crosses none).
#    Measured at ec6568be: rear-500 off 8/182 (fast 6/78), lanes 6; rear-600 14/312 (11/104); rear-240 36/312
#    (18/104), behind 7/104.
LEVEL = {"field", "road"}
LEVEL_PCT = 1
REAR_START = {"rear-500": (5, 10, 6, 8), "rear-600": (6, 14, 6, 59), "rear-240": (14, 22, 9, 56)}
# Pairs of bodies of one kind that swap sides across the way (lanes crossed) on main f3a4c16e, over the fixture's
# offsets: a fixture may have no more.
CROSS = {"col-300": 37, "col-600": 35, "col-1000": 16, "loose-300": 15, "loose-600": 25, "loose-1000": 36,
         "bunch-300": 4, "bunch-600": 7, "bunch-1000": 18, "rear-200": 0, "rear-300": 7, "rear-500": 0, "mid-300": 9,
         "mid-320": 2, "front-500": 0, "mix3-1000": 4, "field": 0, "road": 0, "corridor": 0, "blob": 6, "choke": 352,
         "block": 280, "gate": 278, "bridge": 420, "gateangle": 1118, "wallside": 189, "rear-600": 59, "rear-240": 56}
# A corridor 4 cells wide holds one file: no body can reach a spot past another, so the layout's order is not checked.
ONE_FILE = {"corridor"}
SLOT = 24          # px a body may settle from its slot
OFF_SLOT_PCT = 5   # % of the bodies that may settle farther from their slot ...
OFF_FAST_PCT = 8   # ... and of the fast ones (knights from the back of a short order walled out of their front row by
                   # knights that came in first and stood in the row behind it: 3-4 of 52 at 13 offsets)
ROW = 48           # px a faster body may stand behind the frontmost body of a slower group (a row)
BEHIND_PCT = 6     # % of the faster bodies that may stand farther behind
SPEED = {"fast": 0, "mid": 1, "slow": 2}
UNSETTLED = 6000   # an unsettled body counts as arriving at this tick
WANDER = 25        # px a settled body may move after it settles: one yield, a cell's diagonal from where it stood
                   # (22.6 px from a cell's centre, a pixel more from a body that settled a pixel off it)


def trace(exe, scn, offset):
    """One offset: per unit id its settle tick (the last tick it stood with no Legion leg after one; a body standing on
    its own spot when the order came never takes a leg and counts as arrived at the order's tick), start, end, its
    point, its slot (the last one it held), and how far it moved after it settled; plus the passes within the click.
    None on a tooling failure."""
    p = subprocess.run([exe, scn, "--mode", "legion", "--offsets", str(offset), "--serial", "--json", "--dump-units",
                        "1"], capture_output=True, text=True)
    rows = [json.loads(ln) for ln in p.stdout.splitlines() if ln.startswith("{")]
    if p.returncode != 0 or not rows:
        return None
    units = {}
    for ln in p.stderr.splitlines():
        if not ln.startswith("DUMP "):
            continue
        f = dict(kv.split("=", 1) for kv in ln.split()[1:])
        i, t, x, z = int(f["id"]), int(f["t"]), int(f["x"]), int(f["z"])
        u = units.setdefault(i, {"start": (x, z), "member": False, "ever": False, "settle": None, "at": None,
                                 "wander": 0.0, "point": None, "slot": None})
        if f["st"] != "0" or f["g"] != "0":
            u["member"] = u["ever"] = True
            u["settle"] = u["at"] = None
            if int(f["gx"]) >= 0:
                u["point"] = (int(f["gx"]), int(f["gz"]))
            if int(f["sx"]) >= 0:
                u["slot"] = (int(f["sx"]), int(f["sz"]))
        else:
            if u["member"]:
                u["settle"], u["at"] = t, (x, z)
            u["member"] = False
            if u["at"]:
                u["wander"] = max(u["wander"], ((x - u["at"][0]) ** 2 + (z - u["at"][1]) ** 2) ** 0.5)
        u["end"] = (x, z)
    for u in units.values():
        if not u["ever"] and u["end"] == u["start"]:
            u["settle"], u["at"] = 5, u["start"]
    return units, rows[0]["keys"].get("work.click_overtakes.total", 0)


def layout_clicks(exe, scenarios):
    """The one-click fixtures under the arrival layout; returns the failures."""
    def t90(v):
        v = sorted(v)
        return v[min(len(v) - 1, (len(v) * 9 + 9) // 10 - 1)]
    jobs = {}
    with concurrent.futures.ThreadPoolExecutor(max(1, min(8, os.cpu_count() or 1))) as pool:
        for name, (offsets, _) in CLICKS.items():
            scn = os.path.join(scenarios, f"overtake-click-{name}.scn")
            for off in offsets:
                jobs[(name, off)] = pool.submit(trace, exe, scn, off)
    bad = []
    for name, (offsets, main) in CLICKS.items():
        # Unit ids follow the scenario's group lines; the type names are the speed groups.
        kinds, foot = [], {}
        for ln in open(os.path.join(scenarios, f"overtake-click-{name}.scn")):
            w = ln.split()
            if w and w[0] == "type":
                foot[w[1]] = int(w[3])
            if w and w[0] == "group":
                kinds += [w[3]] * int(w[4])
        runs = [jobs[(name, off)].result() for off in offsets]
        if any(r is None for r in runs):
            bad.append(f"overtake-click-{name}: legion_scenario failed")
            continue
        passes = sum(r[1] for r in runs)
        wander = overlap = behind = off_slot = off_fast = crossed = 0
        worst = 0.0
        for off, (units, _) in zip(offsets, runs):
            wander += sum(u["wander"] > WANDER for u in units.values())
            # The cells each body covers where it settled: footprintOrigin, floor((centre - (size-1)*8) / 16).
            cells = [((units[i + 1]["end"][0] - (foot[k] - 1) * 8) // 16, (units[i + 1]["end"][1] - (foot[k] - 1) * 8) // 16,
                      foot[k]) for i, k in enumerate(kinds)]
            for a in range(len(cells)):
                for b in range(a + 1, len(cells)):
                    (xa, za, fa), (xb, zb, fb) = cells[a], cells[b]
                    overlap += xa < xb + fb and xb < xa + fa and za < zb + fb and zb < za + fa
            # Each body against its own slot: settled more than SLOT px from it is off the layout.
            for i, k in enumerate(kinds):
                u = units[i + 1]
                if u["slot"] and ((u["end"][0] - u["slot"][0]) ** 2 + (u["end"][1] - u["slot"][1]) ** 2) ** 0.5 > SLOT:
                    off_slot += 1
                    off_fast += k == "fast"
            # Depth along the way in: from the members' starting centroid toward the point. Each body of a faster
            # group against the frontmost body of every slower one: more than a row (ROW px) behind it is out of order.
            point = next((units[i + 1]["point"] for i in range(len(kinds)) if units[i + 1]["point"]), None)
            if not point:
                continue
            cx = statistics.mean(units[i + 1]["start"][0] for i in range(len(kinds)))
            cz = statistics.mean(units[i + 1]["start"][1] for i in range(len(kinds)))
            ax, az = point[0] - cx, point[1] - cz
            length = (ax * ax + az * az) ** 0.5 or 1

            def across(p):
                return ((p[0] - cx) * az - (p[1] - cz) * ax) / length
            # Lanes: two bodies of one kind more than 16 px apart across the way at the start and at the end, on
            # opposite sides of each other.
            for a in range(len(kinds)):
                for b in range(a + 1, len(kinds)):
                    if kinds[a] != kinds[b]:
                        continue
                    s0 = across(units[a + 1]["start"]) - across(units[b + 1]["start"])
                    s1 = across(units[a + 1]["end"]) - across(units[b + 1]["end"])
                    crossed += abs(s0) > 16 and abs(s1) > 16 and (s0 > 0) != (s1 > 0)
            if name in ONE_FILE or name in FALLBACK:
                continue
            depth = {}
            for i, k in enumerate(kinds):
                x, z = units[i + 1]["end"]
                depth.setdefault(k, []).append(((x - cx) * ax + (z - cz) * az) / length)
            # Each faster body once: more than a row behind the frontmost body of any slower group.
            for a in depth:
                fronts = [max(depth[b]) for b in depth if SPEED[a] < SPEED[b]]
                if fronts:
                    front = max(fronts)
                    behind += sum(d < front - ROW for d in depth[a])
                    worst = max(worst, max(front - d for d in depth[a]))
        bodies = len(kinds) * len(offsets)
        faster = sum(k != "slow" for k in kinds) * len(offsets)
        fasts = sum(k == "fast" for k in kinds) * len(offsets)
        line = (f"overtake-click-{name}: passes within the click {passes}, wandering {wander}, overlaps {overlap}, "
                f"off their slot {off_slot}/{bodies} (fast {off_fast}/{fasts}), behind a slower group's front "
                f"{behind}/{faster} (worst {worst:.0f} px), lanes crossed {crossed} (main {CROSS[name]})")
        for kind, (mm, mt, mu) in main.items():
            ids = [i + 1 for i, k in enumerate(kinds) if k == kind]
            means, t90s, unsettled = [], [], 0
            for units, _ in runs:
                v = [units[i]["settle"] for i in ids]
                unsettled += sum(x is None for x in v)
                v = [UNSETTLED if x is None else x for x in v]
                means.append(statistics.mean(v))
                t90s.append(t90(v))
            m, t = statistics.median(means), statistics.median(t90s)
            line += (f"; {kind} mean {m:.0f} (main {mm}, {(m / mm - 1) * 100:+.1f}%) t90 {t:.0f} (main {mt}, "
                     f"{(t / mt - 1) * 100:+.1f}%)" + (f" unsettled {unsettled}" if unsettled else ""))
            if unsettled > mu:
                bad.append(f"overtake-click-{name}: {unsettled} {kind} bodies never settle (main {mu})")
            if kind == "fast" and name in FALLBACK and round(m) > mm:
                bad.append(f"overtake-click-{name}: fast mean arrival median {m:.0f}, later than main's {mm}")
            elif kind == "fast" and name in LEVEL and m > mm * (1 + LEVEL_PCT / 100):
                bad.append(f"overtake-click-{name}: fast mean arrival median {m:.0f}, more than {LEVEL_PCT}% later "
                           f"than main's {mm}")
            elif kind == "fast" and name not in SHORT_REAR and name not in FALLBACK and name not in LEVEL and m >= mm:
                bad.append(f"overtake-click-{name}: fast mean arrival median {m:.0f}, no earlier than main's {mm}")
        print(line)
        if wander:
            bad.append(f"overtake-click-{name}: {wander} bodies move more than {WANDER} px after settling")
        if overlap:
            bad.append(f"overtake-click-{name}: {overlap} pairs of bodies settled on each other")
        slot_pct, fast_pct, behind_pct, cross = REAR_START.get(name, (OFF_SLOT_PCT, OFF_FAST_PCT, BEHIND_PCT,
                                                                      CROSS[name]))
        if crossed > cross:
            bad.append(f"overtake-click-{name}: {crossed} pairs of bodies crossed lanes (main {CROSS[name]}, at most {cross})")
        if name not in FALLBACK and (off_slot * 100 > bodies * slot_pct or off_fast * 100 > fasts * fast_pct):
            bad.append(f"overtake-click-{name}: {off_slot} of {bodies} bodies ({off_fast} of {fasts} fast) settle more "
                       f"than {SLOT} px from their slot (at most {slot_pct}%, fast {fast_pct}%)")
        if behind * 100 > faster * behind_pct:
            bad.append(f"overtake-click-{name}: {behind} of {faster} faster bodies settle more than {ROW} px behind a "
                       f"slower group's frontmost body (at most {behind_pct}%)")
    return bad


if __name__ == "__main__":
    sys.exit(main())
