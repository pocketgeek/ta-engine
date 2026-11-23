#!/usr/bin/env python3
"""Generates the W5 step 0 fixtures (PLAN section 4, W5 step 0) into this directory.

  python3 tools/scenarios/gen_w5_fixtures.py [--check]

The lateblock size-sweep members are written to sweeps/ (not baselined: the sweep scripts and the nightly's
tools/scenarios/*.scn glob leave that directory alone).

The files are committed; --check regenerates them in memory and fails if a committed file differs
(the legion_w5_fixtures ctest). Edit this script, not the files.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SPEED = 117964          # raw 16.16 of 1.79999 px/tick: the fixtures' speed, for `spots` bodies of one speed
CELL = 16 * 65536       # raw 16.16 px of one cell
HEAD = ("scn 1\n")

files = {}


def put(name, text):
    files[name + ".scn"] = text


def cells(pts):
    return " ".join("%d,%d" % p for p in pts)


def lattice(x0, z0, cols, rows, dx, dz):
    return [(x0 + i * dx, z0 + j * dz) for j in range(rows) for i in range(cols)]


def header(name, lines, ticks, extra=""):
    out = HEAD
    for l in lines:
        out += "# " + l + "\n"
    out += "name %s\nticks %d\nseed 7\nplayers 2\nteam 0 0\nteam 1 1\nwanderers off\n" % (name, ticks)
    return out + extra


# ---- AR-06: attackring (the ar06bench rows) --------------------------------------------------------------
AR_COMMON = [
    "AR-06 (PLAN 3.4, W5 steps 1): Attack/Guard members behind their own front hold out of weapon range for ever.",
    "N attackers (an 8-column block) 110 cells west of ONE stationary enemy, all given the same Attack (or Guard /",
    "Fight) order; the observer's `reach` shape counts how many ever get a shot (reach.ring.ever_*), how many stand",
    "out of range for 600 ticks (out600), the damage dealt (a regeneration is never credited back) and the longest",
    "Legion Holding run out of reach (hold_out_max; hold0_max at field potential 0). Mechanism (audit AR-06): the",
    "shared field single-files the blob onto one row, only the head of the line is in range, and a shooter's",
    "braking is invisible to the settle rule. Source: the audit's attackring probe ($SCR/ar06/attackring.cpp).",
]


def attackring(name, n, cols, foot, rng, desc, target="mover", order="attack", guard_range=None, dmg=1,
               squad=False, extra_map="", ally=False, tx=130, tz=80, ticks=3000, marks="1200,2400", ax=20):
    rows = (n + cols - 1) // cols
    pitch = foot + 1
    z0 = tz - (rows * pitch) // 2
    t = header(name, AR_COMMON + desc, ticks)
    t += "map flat 200 160\n" if not extra_map else extra_map
    t += "type a mover %d 2500 10 1.8 gun=%d,%d,1.0\n" % (foot, rng, dmg)
    if target == "structure":
        t += "type tgt structure 4 4 hp=30000\n"
    else:
        t += "type tgt mover 2 2500 10 1.8 hp=30000\n"
    sq = " squad=f1" if squad else ""
    t += "group A 0 a %d rect %d %d %d %d pitch=%d weapons=on%s\n" % (n, ax, z0, ax + cols * pitch, z0 + rows * pitch, pitch, sq)
    if ally:
        t += "group T 0 tgt 1 cells %d,%d weapons=off\n" % (tx, tz)
    else:
        t += "group T 1 tgt 1 cells %d,%d weapons=off\n" % (tx, tz)
    if order == "fight":
        t += "probe claims\n"
    if guard_range:
        t += "reach ring A T range=%d marks=%s\n" % (guard_range, marks)
    else:
        t += "reach ring A T marks=%s\n" % marks
    if order == "attack":
        t += "at 1 attack A @T\n"
    elif order == "guard":
        t += "at 1 guard A @T\n"
    else:
        t += "at 1 fight A %d %d\n" % (tx, tz)
    return t


put("attackring-f1-64", attackring("attackring-f1-64", 64, 8, 1, 96, [
    "Row: 64 foot-1 attackers, range 96, vs a mobile target (audit: Legion 3/64 ever in range, Retail 44; PLAN: >= 35).",
]))
put("attackring-f2-64", attackring("attackring-f2-64", 64, 8, 2, 96, [
    "Row: 64 foot-2 attackers, range 96 (audit: Legion 4/64, Retail 14).",
]))
put("attackring-squad-64", attackring("attackring-squad-64", 64, 8, 1, 96, [
    "Row: the f1 row with the attackers in formation Alt+1 (audit: Legion 3/64, identical to the plain row).",
], squad=True))
put("attackring-f1-16", attackring("attackring-f1-16", 16, 8, 1, 96, [
    "Row: 16 foot-1 attackers (audit: Legion 1/16).",
]))
put("attackring-r200-64", attackring("attackring-r200-64", 64, 8, 1, 200, [
    "Row: 64 foot-1 attackers with range 200 (audit: Legion 3/64): a longer reach does not help the queue.",
]))
put("attackring-mob-40", attackring("attackring-mob-40", 40, 8, 1, 96, [
    "Row: 40 attackers vs a mobile target (PLAN: 4 -> >= 30 in reach, Retail 40).",
]))
put("attackring-mob-100", attackring("attackring-mob-100", 100, 10, 1, 96, [
    "Row: 100 attackers vs a mobile target (PLAN: 0 -> >= 40 in reach, Retail 52).",
]))
put("attackring-struct-60", attackring("attackring-struct-60", 60, 10, 1, 96, [
    "Row: 60 attackers vs a STRUCTURE (a 4x4 house, reach = range + 8*4 + 24 px). PLAN: in reach by tick 2400 2/60 -> >= 40/60",
    "(Retail 50); damage at 80 s (tick 2400) 1565 -> >= 15k with 10-damage shots.",
], target="structure", dmg=10))
put("attackring-fight-60", attackring("attackring-fight-60", 60, 10, 1, 96, [
    "Row: 60 attackers ordered to FIGHT (attack-move) to the target's spot; they auto-acquire it on the way",
    "(PLAN: 18 -> >= 40 in reach, Retail 54).",
], order="fight"))
put("attackring-guard-64", attackring("attackring-guard-64", 64, 8, 1, 96, [
    "Row: 64 attackers GUARD a stationary ally; `range=144` counts the guards within 9 cells of it",
    "(audit: Legion 13/64 within 6 cells, Retail 64/64; PLAN: >= 50). Protocol 303: a guard stands on",
    "Follow_Ground's ring (World::guardRing; 1x1 guard of a 2x2 ward: 80..128 px), not within 70 px, so the",
    "count reads the ring's outer edge plus a cell (range=96 before, which no ring guard can reach).",
], target="mover", order="guard", guard_range=144, ally=True))

# the wall ring: a closed ring of wall cells round the target; ranged attackers can stand within reach outside
# it but have no line of sight, so they hold at the field's goal (potential 0) without ever firing
wall_ring = ""
wall_ring += "map flat 200 160\n"
for (x, z, w, h) in [(126, 76, 8, 1), (126, 83, 8, 1), (126, 77, 1, 6), (133, 77, 1, 6)]:
    wall_ring += "wall %d %d %d %d\n" % (x, z, w, h)
put("attackring-wall-64", attackring("attackring-wall-64", 64, 8, 1, 96, [
    "Wall ring variant: the target stands inside a closed 8x8-cell ring of 1-cell walls (6x6 free inside). Attackers",
    "can come within its weapon distance on the outside but cannot see it. PLAN: no member Holding at field",
    "potential 0 for more than 60 ticks (reach.ring.hold0_max <= 60; 0 today because no reach seeds exist yet, so",
    "hold_out_max is the key that shows the base's hold); `ever` counts bodies within reach (a distance), not shooters.",
], extra_map=wall_ring))

# the river variant: the target is across a full-height deep river; range 200 shoots over it; the in-reach band
# on the near bank is a thin, long front
river_rows = []
for z in range(160):
    row = ["."] * 200
    for x in range(100, 106):
        row[x] = "~"
    river_rows.append("".join(row))
river = "map ascii\n" + "\n".join(river_rows) + "\nend\n"
put("attackring-river-64", attackring("attackring-river-64", 64, 8, 1, 200, [
    "River variant: a full-height 6-cell deep river (x 100-105) between the attackers and the target (x 110). Range 200",
    "shoots across it; the target is in another component. Only the bank strip within reach can fire (a long, thin front).",
], extra_map=river, tx=110, tz=80))

# ---- fight-retarget ---------------------------------------------------------------------------------------
t = header("fight-retarget", [
    "AR-06 / C18 (PLAN 3.4): a Fight group whose auto-acquired target dies mid-approach. 40 fighters march east to a",
    "point past a weak enemy (E, 3 bodies of 60 hp) that stands on their way. Each fighter that auto-acquires an E body",
    "leaves its convoy Point for the chase (its area slot is released); when E dies it resumes its Fight leg, re-joins",
    "the convoy's Point and claims a slot as a late joiner. PLAN gates: the claims invariant holds (`probe claims`:",
    "claims.bad_max == 0 -- overlapping, lost or dangling slot claims), every fighter resumes and settles",
    "(g.F.arrived == 40, g.F.done), and no fighter keeps a dead target's chase (reach.E: damage == the E bodies' 180 hp).",
], 3600)
t += "map flat 260 120\n"
t += "type f mover 2 2500 10 1.8 gun=96,10,1.0\n"
t += "type e mover 2 2500 10 1.8 hp=60\n"
t += "group F 0 f 40 cells %s weapons=on\n" % cells(lattice(14, 44, 8, 5, 3, 4))
t += "group E 1 e 3 cells 120,52 120,56 120,60 weapons=off\n"
t += "probe claims\nreach E F E marks=1200\nregion goal 200 40 240 80\n"
t += "at 1 fight F 230 60\n"
put("fight-retarget", t)

# ---- ring-crossing -----------------------------------------------------------------------------------------
t = header("ringcross", [
    "PLAN 3.4 gate 'ring-crossing' (W5 steps 1; W7 re-runs it): a Move group walks through a firing ring. 48 attackers",
    "ring a stationary enemy at the map centre (range 96, Attack order, from tick 1); at tick 1500 a 40-body Move group",
    "from the west is ordered to a point past the ring. W5's Engaged bodies never yield or part, so the crossing group must",
    "route round or through them; PLAN: the Move group arrives within +10% of the base (g.M.t90 / g.M.done, and the",
    "Retail floor), and the ring still fires (reach.ring.ever_end no worse than the base).",
], 5000)
t += "map flat 260 160\n"
t += "type a mover 1 2500 10 1.8 gun=96,1,1.0\n"
t += "type m mover 2 2500 10 1.8\n"
t += "type tgt mover 2 2500 10 1.8 hp=30000\n"
t += "group A 0 a 48 rect 100 70 124 94 pitch=2 weapons=on\n"
t += "group T 1 tgt 1 cells 160,80 weapons=off\n"
t += "group M 0 m 40 cells %s\n" % cells(lattice(14, 66, 8, 5, 3, 4))
t += "probe claims\nreach ring A T marks=1500\nregion goal 220 60 250 100\n"
t += "at 1 attack A @T\nat 1500 move M 235 80\n"
put("ringcross", t)

# ---- MV-05 lateblock ---------------------------------------------------------------------------------------
def lateblock(name, mode, bw, bh, desc):
    """mode: 0 no block, 1 the block stands before the order (order at tick 120), 2 the block is there from tick 0
    but not yet 'soft' when the order is given at tick 1 (the audit spawned it 150 ticks after the order)"""
    t = header(name, [
        "MV-05 (PLAN 3.4): a group's field is built before a block of idle same-player bodies becomes soft across its",
        "route. 40 movers cross a 260x120 map to (230,52); %s" % desc,
        "Soft changes never re-plan the field today, so the late block is met by the 12-cell local detour only: past",
        "a ~16-cell block the group parks at its face (audit: 27/40 by 9000 ticks, 13 holding, 2 for 17000+).",
        "PLAN: lateblock 16x16 27/40 -> >= 39/40 by 4500, 0 permanent.  Audit probe: legion_world_test probe_lateblock.",
    ], 9000)
    t += "map flat 260 120\ntype m mover 2 2500 10 1.8\n"
    t += "group A 0 m 40 cells %s\n" % cells(lattice(14, 44, 8, 5, 3, 4))
    if mode:
        t += "group B 0 m %d cells %s\n" % (bw * bh, cells(lattice(120, 60 - bh, bw, bh, 2, 2)))
    t += "probe claims\nregion goal 218 40 240 64\n"
    t += "at %d move A 230 52\n" % (120 if mode == 1 else 1)
    return t


put("lateblock-none", lateblock("lateblock-none", 0, 0, 0, "no block (mode 0): the reference."))
put("lateblock-before-16", lateblock("lateblock-before-16", 1, 16, 16,
    "a 16x16-body block (32 cells wide) stands from tick 0 and is soft when the order comes at tick 120 (mode 1)."))
put("lateblock-late-16", lateblock("lateblock-late-16", 2, 16, 16,
    "a 16x16-body block (32 cells wide) stands from tick 0 but is not yet soft when the order comes at tick 1 (mode 2)."))
for (bw, bh) in [(4, 4), (6, 6), (8, 8), (10, 10), (12, 12), (24, 20)]:
    put("sweeps/lateblock-late-%dx%d" % (bw, bh), lateblock("lateblock-late-%dx%d" % (bw, bh), 2, bw, bh,
        "a %dx%d-body block (%d cells wide), not yet soft at the order (mode 2); the size sweep." % (bw, bh, bw * 2)))
for (bw, bh) in [(8, 8), (12, 12), (24, 20)]:
    put("sweeps/lateblock-before-%dx%d" % (bw, bh), lateblock("lateblock-before-%dx%d" % (bw, bh), 1, bw, bh,
        "a %dx%d-body block stands soft before the order (mode 1); the size-sweep control." % (bw, bh)))

# ---- AR-07 chase / goalblock ---------------------------------------------------------------------------------
def spot(x, z, head=16384):
    return "%d,%d,%d,0,%d" % (x * CELL, z * CELL, head, SPEED)


def chase(name, n, desc, line=False, ticks=900):
    t = header(name, [
        "AR-07 (PLAN 3.4): a chase re-seeds its group every ~16 ticks. %s" % desc,
        "The target (T) walks east at the chasers' own speed (harvested-body `spots` with one speed, no spawn roll), so a",
        "chaser never closes in and the Attack order's goal moves all run. Today: one registration and one field per",
        "re-seed (audit, 1 chaser, 900 ticks: 54 registrations, 55 fields, 6.0M relaxations, 1.65 ms re-seed ticks).",
        "PLAN: registrations 54 -> 1; fields 55 -> <= 15; per-tick field relaxations (work.field_work.max) <= 40% of base.",
    ], ticks)
    t += "map flat 400 120\ntype c mover 2 2500 10 1.79999 gun=96,1,1.0\ntype t mover 2 2500 10 1.79999 hp=30000\n"
    if n == 1:
        t += "group C 0 c 1 spots %s weapons=on\n" % spot(30, 60)
    else:
        rows = (n + 9) // 10
        pts = [spot(24 + (i % 10) * 3, 50 + (i // 10) * 3) for i in range(n)]
        t += "group C 0 c %d spots %s weapons=on\n" % (n, " ".join(pts))
    t += "group T 1 t 1 spots %s weapons=off\n" % spot(70, 60)
    if line:
        t += "type w mover 2 2500 10 1.79999\n"
        t += "group W 0 w 15 cells %s\n" % cells([(70, 26 + 4 * i) for i in range(15)])
    t += "at 1 move T 390 60\nat 2 attack C @T\n"
    t += "reach chase C T marks=450\n"
    return t


put("chase-1", chase("chase-1", 1, "1 chaser (the audit's 54-registration case)."))
put("chase-50", chase("chase-50", 50, "50 chasers (audit: 2451 registrations in 50 shared groups, 51 fields, 9.3M relaxations)."))
put("chase-line", chase("chase-line", 1, "1 chaser with a 15-body line of idle friendly bodies across its way (audit: Retail stalls at the line, Legion rounds it with 0 holds).", line=True))


def goalblock(name, verb, bx, bz, desc):
    t = header(name, [
        "AR-07 (PLAN 3.4): a blocking feature lands %s at tick 300, with the group's field done and its slots claimed." % desc,
        "200 2x2 movers share one click, %s (audit: registrations +1, groups +1, fields +2 -- one refresh of the old" % verb,
        "group and one bounded field for the ONE re-registered member, because claimSlot re-points every goal to its",
        "own slot cell so only the member whose slot IS the blocked cell fails legal(goal)). PLAN: AR-07 `reseat()`",
        "re-seeds in place -- the work.registrations / fields_built / field_work totals after tick 300 fall.",
    ], 3000)
    t += "map flat 300 120\ntype m mover 2 2500 10 1.79999\n"
    t += "group A 0 m 200 rect 20 40 80 70 pitch=3\n"
    t += "probe claims\nregion goal 180 40 215 80\n"
    t += "churn %d %d 2 2 every=1 from=300 until=301\n" % (bx, bz)
    t += "at 1 %s A 200 60\n" % verb
    return t


put("goalblock-200", goalblock("goalblock-200", "move", 199, 59, "ON the shared goal cell"))
put("goalblock-fight-200", goalblock("goalblock-fight-200", "fight", 199, 59, "ON the shared goal cell (a Fight click)"))
put("goalblock-near-200", goalblock("goalblock-near-200", "move", 206, 59, "6 cells off the goal (the control)"))

# ---- MV-18 keelturn --------------------------------------------------------------------------------------------
for label, gx, gz in [("180", 20, 45), ("90", 80, 5), ("0", 140, 45)]:
    name = "keelturn-" + label
    t = header(name, [
        "MV-18 (PLAN 3.4, optional): a retail-like ship (verharp: maxvelocity 3.5, acceleration 0.35, turnrate 210, no",
        "turninplacerate) at (80,45) facing east is ordered to (%d,%d)%s. Retail turns it on an arc; Legion creeps at" % (
            gx, gz, {"180": ", directly behind it", "90": ", a quarter turn away", "0": ", straight ahead (the control)"}[label]),
        "1/8 speed facing away (audit: crawl 99 / sideways 78 of 405 ticks to arrive, vs Retail's arc). Keys: g.S.done, crawl_samples,",
        "sideways, backward. Audit probe: legion_world_test mv18.",
    ], 1500)
    t += "map flat 160 90\nheight 0 0 160 90 20\n"
    t += "type ship boat 2 210 0.35 3.5 depth=13 turninplace=0\n"
    t += "group S 0 ship 1 spots %s\n" % spot(80, 45)
    t += "region goal %d %d %d %d\n" % (gx - 4, gz - 4, gx + 4, gz + 4)
    t += "at 1 move S %d %d\n" % (gx, gz)
    put(name, t)

# ---- MV-07 staticblock ---------------------------------------------------------------------------------------------
def staticblock(name, owner, desc):
    t = header(name, [
        "MV-07 (PLAN 3.4, T2 A1): %s" % desc,
        "A 10x12 block of idle bodies (pitch 3, x 92-119, z 32-65) stands across the straight way of a 40-body group that",
        "is sent 140 cells east after 90 ticks. Open ground: half arrive by 1226, all by 1803. Today (audit): half 1603, all 3269",
        "(~1090 of the 1466 excess ticks are the goal-area arrival tail: members creep at speed ~21 of 163-197). PLAN:",
        "staticblock done <= 2500 (g.A.done), goal-area creep counter -70% (creep.goal.ticks).",
        "Source: legion_world_test staticblock.",
    ], 4000, extra="")
    t += "map flat 200 100\ntype m mover 2 2500 10 1.79999\n"
    if owner >= 0:
        t += "group B %d m 120 cells %s weapons=off\n" % (owner, cells(lattice(92, 32, 10, 12, 3, 3)))
    t += "group A 0 m 40 cells %s\n" % cells(lattice(20, 44, 8, 5, 3, 3))
    t += "region dest 150 40 170 60\ncreep goal A\n"
    t += "at 90 move A 160 50\n"
    return t


put("staticblock-own", staticblock("staticblock-own", 0, "the block belongs to the group's own player."))
put("staticblock-other", staticblock("staticblock-other", 1, "the block belongs to another player."))
put("staticblock-open", staticblock("staticblock-open", -1, "the reference with no block."))


def main():
    check = "--check" in sys.argv
    bad = 0
    for name, text in sorted(files.items()):
        path = os.path.join(HERE, name)
        if check:
            have = open(path).read() if os.path.exists(path) else None
            if have != text:
                print("differs: " + name)
                bad += 1
        else:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                f.write(text)
    if check:
        sys.exit(1 if bad else 0)
    print("wrote %d files" % len(files))


if __name__ == "__main__":
    main()
