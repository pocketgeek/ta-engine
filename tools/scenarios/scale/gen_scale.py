#!/usr/bin/env python3
"""gen_scale.py -- write the Legion 10,000 scale suite's .scn files (phase 0).

    python3 tools/scenarios/scale/gen_scale.py [OUTDIR]     (default: this file's directory)

Deterministic: the same script writes the same files byte for byte (no clock, no random module; the one
permutation is a fixed LCG). The files are committed; rerun this only to change the suite, and say so in
the progress record. Every file says `probe scale` (tools/legion_scale.h) and `uplink 16` (the client's
512-command window over a 16-tick round trip, as tools/legion_issue_selection.h mirrors it), so command
delivery is measured through the same uplink a real client has.

Families (N = bodies; see the progress record for what each one stands for):
  one-N      one selection, one click across open ground; the map grows with N (approximately constant
             density), the gap between the army and its destination area is fixed: 100, 1000, 2500, 5000, 10000
  many-N     N/25 squads of 25 in a grid, each clicked to its own distant tile (a fixed permutation): many
             selections, independent destinations, low sharing; two allied players; map grows with N
  swap-N     constant 768 x 768 map, two allied armies of N/2 exchange sides head on (crowd-density stress
             at a constant map; opposing streams): 1000, 2500, 5000, 10000
  bridges-N  a river across the map with three bridges (4, 12 and 32 cells wide); two allied armies of N/2
             cross it in opposite directions (multiple passages, narrow bridge, crossing streams);
             bridges-1000-fight is the same on attack-move orders (fight legs take no swap)
  maze-N     a walled compound of rooms with doors; 85% go to its central room (room entrances, destination
             crowding), 10% to an island reached by one narrow bridge, 5% to a sealed box (unreachable)
  mixed-N    one Alt+1 formation of mixed footprints (1x1 .. 4x4), speeds, hovers and flyers across a lake
  attack-N   an armed army attacks a smaller armed enemy force that is walking away (moving target, attack
             rings, deaths)
  churn-N    one selection re-clicked four times (replacement), half of it stopped and re-clicked
             (cancellation), static features dropped along its way every 10 ticks (static churn), and a
             tenth sent into a sealed box (unreachable)
  idle-N     N-N/10 idle bodies standing in a block and N/10 movers sent through it (moving vs idle)
  scatter-N  stage 6: N bodies, each its own selection, clicked to N different distant destinations (unrelated
             routes, no sharing: the other extreme to one-10000's single objective): 2500, 10000
  sched-load continuous navigation load (stage 4): 48 squads of 10 re-clicked to new far tiles every 160 ticks
             and a rock walking across the map every 10 ticks, so field requests (first routes and stale
             refreshes) arrive faster than the quota serves them for the whole run: service fairness and the
             longest request age under load, not throughput (ctest legion_sched_load)
  room2door  stage 8: one order of two footprint classes into a walled room through two doors from two sides
  sched-mix  memory pressure (stage 4 fix round): 42 short-hop squads re-clicked every 120 ticks round 6
             squads crossing the map every 4800, under `navbytes` = two whole-map fields: the fit scan, the
             reservation and request promotion in the sim, and a request-age bound derived from that
             capacity (ctest legion_sched_mix)
  starve-1024  route requests on a big map (2026-11-15): a field allowance of eight whole-map fields, the ratio
             of 128 MiB to a 2048 x 2048 map. 80 one-body groups whose first window lies behind a wall, six
             waves of 30 from two allied players in turn to two points (later commands to a point another
             still walks to), a 1 x 1 feature dropped on one point and a 2 x 2 on the other (a covered and a
             buried goal), churn in a far corner (ctest legion_starve)
  cutoff-1024, cutoff-reopen, cutoff-toggle
             six one-body groups inside an enclosure ordered to a point outside; its one gap is closed at tick
             200, before any is through (cut off from their goal after the order); churn in a far corner, 40
             bodies of the other player walking outside. cutoff-1024: the gap stays closed. cutoff-reopen:
             it opens again at tick 1200. cutoff-toggle: it opens and closes every 150 ticks (ctests
             legion_cutoff_closed, legion_cutoff_reopen, legion_cutoff_toggle)
  armies14-2048  fourteen 60-body armies, one command each, cross a 2048 x 2048 map at the game's allowance
             while 60 one-body groups are ordered round a wall; churn in a far corner: more whole-map windows
             than the allowance holds (ctest legion_armies)
  seedcorpse-army  one 60-body army walks 300 cells on a 1024 x 1024 map; a 1 x 1 feature lands on its click
             cell at tick 300 (a covered goal under a crowd over the chain demand: its window is the whole
             map from then on); churn in a far corner (ctest legion_seedarmy)
"""
import math
import os
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
FOOT = 3          # the roster's largest footprint (2 and 3, cycled)
PITCH = FOOT + 1  # the runner's default spawn pitch for it
SPEED = 1.6       # the roster's slowest speed (px per tick)


def disc_cells(n, foot=FOOT):
    """The observer's packed-disc radius (cells) for n bodies of footprint foot."""
    return 16 * (foot + 1) * math.sqrt(n / math.pi) * 1.25 / 16


def side(n, pitch=PITCH):
    return int(math.ceil(math.sqrt(n))) * pitch


def ticks_for(cells):
    """Run length for a trip of `cells`: the slowest roster speed, 30% slack, 1500 ticks to settle."""
    return int(cells * 16 / SPEED * 1.3 + 1500) // 100 * 100


def header(name, ticks, w, h, players=1, teams=(), seed=7, weapons="off", uplink=True):
    lines = [f"scn 1", f"name {name}", f"ticks {ticks}", f"seed {seed}", f"players {players}"]
    lines += [f"team {p} {t}" for p, t in teams]
    lines += [f"weapons {weapons}"] + (["uplink 16"] if uplink else []) + ["probe scale", f"map flat {w} {h}"]
    return lines


def write(name, lines, doc):
    path = os.path.join(OUT, name + ".scn")
    with open(path, "w") as f:
        f.write("# " + doc.replace("\n", "\n# ") + "\n# Generated by gen_scale.py -- edit the generator, not this file.\n")
        f.write("\n".join(lines) + "\n")


def one(n):
    s = side(n)
    r = int(math.ceil(disc_cells(n)))
    h = max(s, 2 * r) + 64
    x0, z0 = 16, (h - s) // 2
    dest = x0 + s + 160 + r
    w = dest + r + 32
    L = header(f"one-{n}", ticks_for(dest + r - x0), w, h)
    L += ["type r roster", f"group A 0 r {n} rect {x0} {z0} {x0 + s} {z0 + s}",
          f"gate mid {x0 + s + 80} 0 {x0 + s + 80} {h}", f"at 2 move A {dest} {h // 2}"]
    write(f"one-{n}", L, f"one-{n}: one selection of {n} roster bodies, one click {160} cells past the army's front edge.")


def lcg_perm(k, seed=12345):
    """A fixed permutation of range(k) with no fixed point (a deterministic derangement)."""
    a = list(range(k))
    x = seed
    for i in range(k - 1, 0, -1):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        j = x % (i + 1)
        a[i], a[j] = a[j], a[i]
    for i in range(k):  # rotate fixed points away
        if a[i] == i:
            j = (i + 1) % k
            a[i], a[j] = a[j], a[i]
    return a


def many(n):
    squad, tile = 25, 40
    k = n // squad
    g = int(math.ceil(math.sqrt(k)))
    w = h = g * tile + 32
    perm = lcg_perm(k)
    # A squad's destination: its permuted tile, but far from home (at least a third of the map).
    L = header(f"many-{n}", ticks_for(int(w * 1.2)), w, h, players=2, teams=[(1, 0)])
    L.append("type r roster")
    orders = []
    for i in range(k):
        gx, gz = i % g, i // g
        x0, z0 = 16 + gx * tile + 10, 16 + gz * tile + 10
        L.append(f"group S{i} {i % 2} r {squad} rect {x0} {z0} {x0 + 20} {z0 + 20}")
        j = perm[i]
        if abs(j % g - gx) + abs(j // g - gz) < g // 3:
            j = (g - 1 - gx) + g * min(g - 1, (g - 1 - gz))  # the opposite tile
            if j >= k:
                j = k - 1 - i % max(1, g)
        dx, dz = 16 + (j % g) * tile + 20, 16 + (j // g) * tile + 20
        orders.append((2 + (i * 600) // k, i, dx, dz))
    for t, i, dx, dz in sorted(orders):
        L.append(f"at {t} move S{i} {dx} {dz}")
    write(f"many-{n}", L, f"many-{n}: {k} squads of {squad}, each clicked to its own distant tile over the first 600 ticks\n"
          "(two allied players): many selections, independent destinations, low sharing.")


def swap(n, w=768, h=768):
    half = n // 2
    s = side(half)
    z0 = (h - s) // 2
    L = header(f"swap-{n}", ticks_for(w), w, h, players=2, teams=[(1, 0)])
    L += ["type r roster", f"group A 0 r {half} rect 16 {z0} {16 + s} {z0 + s}",
          f"group B 1 r {half} rect {w - 16 - s} {z0} {w - 16} {z0 + s}",
          f"gate mid {w // 2} 0 {w // 2} {h}",
          f"at 2 move A {w - 16 - s // 2} {h // 2}", f"at 2 move B {16 + s // 2} {h // 2}"]
    write(f"swap-{n}", L, f"swap-{n}: constant {w} x {h} map, two allied armies of {half} exchange sides head on.")


def bridges(n, w=768, h=768, order="move"):
    name = f"bridges-{n}" + ("" if order == "move" else "-" + order)
    half = n // 2
    s = side(half)
    z0 = (h - s) // 2
    rx = w // 2 - 24
    L = header(name, ticks_for(w + 300), w, h, players=2, teams=[(1, 0)])
    # The river: deep water (height 20) from top to bottom, with three bridges left as ground.
    spans = [(96, 4), (360, 12), (600, 32)]
    z = 0
    for bz, bw in spans:
        L.append(f"height {rx} {z} 48 {bz - z} 20")
        z = bz + bw
    L.append(f"height {rx} {z} 48 {h - z} 20")
    L += ["type r roster", f"group A 0 r {half} rect 16 {z0} {16 + s} {z0 + s}",
          f"group B 1 r {half} rect {w - 16 - s} {z0} {w - 16} {z0 + s}"]
    for i, (bz, bw) in enumerate(spans):
        L.append(f"gate bridge{i} {rx} {bz} {rx + 48} {bz + bw}")
    L += [f"at 2 {order} A {w - 16 - s // 2} {h // 2}", f"at 2 {order} B {16 + s // 2} {h // 2}"]
    write(name, L, f"{name}: a 48-cell river with bridges 4, 12 and 32 cells wide; two allied armies of {half}\n"
          "cross it in opposite directions" + ("." if order == "move" else " on attack-move orders."))


def maze(n, w=1024, h=1024):
    a, isl, un = n * 85 // 100, n * 10 // 100, n - n * 85 // 100 - n * 10 // 100
    L = header(f"maze-{n}", ticks_for(w + 400), w, h)
    # The compound: an outer ring wall (x 512..960, z 288..736) with 6-cell doors in each side, and a
    # central room (x 640..832, z 416..608) with 8-cell doors; walls 2 cells thick.
    def box(x0, z0, x1, z1, door, doors):
        out = []
        cx, cz = (x0 + x1) // 2, (z0 + z1) // 2
        # top / bottom
        for zz in (z0, z1 - 2):
            if doors:
                out.append(f"wall {x0} {zz} {cx - door // 2 - x0} 2")
                out.append(f"wall {cx + door // 2} {zz} {x1 - cx - door // 2} 2")
            else:
                out.append(f"wall {x0} {zz} {x1 - x0} 2")
        for xx in (x0, x1 - 2):
            if doors:
                out.append(f"wall {xx} {z0} 2 {cz - door // 2 - z0}")
                out.append(f"wall {xx} {cz + door // 2} 2 {z1 - cz - door // 2}")
            else:
                out.append(f"wall {xx} {z0} 2 {z1 - z0}")
        return out
    L += box(512, 288, 960, 736, 6, True)
    L += box(640, 416, 832, 608, 8, True)
    # Inner partitions: two walls from the outer ring toward the room, each with a 6-cell gap (a small maze).
    L += [f"wall 560 400 2 120", f"wall 560 530 2 120", f"wall 880 380 2 140", f"wall 880 530 2 140"]
    # The island: land square (x 120..240, z 860..980) in a lake, one 6-cell bridge from the north.
    # A 20-cell moat round it (deep water); the north side leaves cells 177..182 as the bridge.
    L += ["height 100 840 77 20 20", "height 183 840 77 20 20", "height 100 980 160 20 20",
          "height 100 860 20 120 20", "height 240 860 20 120 20"]
    # The sealed box (unreachable): a closed wall ring at x 300..340, z 120..160.
    L += box(300, 120, 340, 160, 0, False)
    sa, si, su = side(a), side(isl), side(un)
    L += ["type r roster", f"group A 0 r {a} rect 16 {(h - sa) // 2} {16 + sa} {(h - sa) // 2 + sa}",
          f"group I 0 r {isl} rect 16 {16} {16 + si} {16 + si}",
          f"group U 0 r {un} rect {32 + si} 16 {32 + si + su} {16 + su}",
          "region room 640 416 832 608", "region island 120 860 240 980",
          "at 2 move A 736 512", "at 2 move I 180 920", "at 2 move U 320 140"]
    write(f"maze-{n}", L, f"maze-{n}: a walled compound with doors and a central room ({a} bodies), an island behind one\n"
          f"narrow bridge ({isl}) and a sealed box ({un}, unreachable).")


def mixed(n, w=None, h=None):
    s = side(n, 5)
    r = int(math.ceil(disc_cells(n, 4)))
    h = max(s, 2 * r) + 96
    lake_x0 = 16 + s + 40
    dest = lake_x0 + 120 + r
    w = dest + r + 32
    L = header(f"mixed-{n}", ticks_for(dest + r), w, h)
    # A lake (deep water) between the army and its destination, open ground round both ends.
    L.append(f"height {lake_x0} {h // 4} 120 {h // 2} 20")
    L += ["type t1 mover 1 2500 10 2.4", "type t2 mover 2 2000 10 2.0", "type t3 mover 3 1500 8 1.7",
          "type t4 mover 4 1000 6 1.4", "type hv hover 2 2000 10 2.2", "type fl flyer 2 2500 10 2.6"]
    parts = [("T1", "t1", 20), ("T2", "t2", 35), ("T3", "t3", 20), ("T4", "t4", 10), ("HV", "hv", 5), ("FL", "fl", 10)]
    z = (h - s) // 2
    names = []
    for name, t, pct in parts:
        k = n * pct // 100
        depth = max(5, int(math.ceil(k / (s / 5))) * 5)
        L.append(f"group {name} 0 {t} {k} rect 16 {z} {16 + s} {z + depth} pitch=5 squad=f1")
        z += depth
        names.append(name)
    L.append(f"at 2 move {','.join(names)} {dest} {h // 2}")
    write(f"mixed-{n}", L, f"mixed-{n}: one Alt+1 formation of footprints 1..4 (speeds 1.4..2.6), hovers and flyers, sent\n"
          "across a lake (hovers and flyers may cross it, walkers go round).")


def attack(n, w=None, h=None):
    e = n // 10
    s, se = side(n), side(e)
    h = max(s, 512) + 64
    w = s + 640
    L = header(f"attack-{n}", 6000, w, h, players=2, weapons="on")
    L += ["type r roster hp=300", "type e roster hp=1500",
          f"group A 0 r {n} rect 16 {(h - s) // 2} {16 + s} {(h - s) // 2 + s}",
          f"group E 1 e {e} rect {s + 200} {(h - se) // 2} {s + 200 + se} {(h - se) // 2 + se}",
          f"reach ring A E", "at 2 move E {} {}".format(w - 40, h // 2), "at 2 attack A @E"]
    write(f"attack-{n}", L, f"attack-{n}: {n} armed bodies attack {e} armed enemies that walk away (moving target,\n"
          "attack ring, deaths).")


def churn(n, w=None, h=None):
    s = side(n)
    r = int(math.ceil(disc_cells(n)))
    h = max(s, 2 * r) + 128
    w = s + 160 + 2 * r + 64
    x0, z0 = 16, (h - s) // 2
    un = n // 10
    a = n - un
    half = a // 2
    L = header(f"churn-{n}", ticks_for(w) + 2000, w, h)
    L += box_lines(w - 60, 16, w - 20, 56)
    L += ["type r roster", f"group A 0 r {half} rect {x0} {z0} {x0 + s} {z0 + s // 2}",
          f"group B 0 r {a - half} rect {x0} {z0 + s // 2} {x0 + s} {z0 + s}",
          f"group U 0 r {un} rect {x0} {max(0, z0 - side(un) - 8)} {x0 + side(un)} {max(side(un), z0 - 8)}"]
    mid = x0 + s + 80
    # A 4x4 rock placed and lifted again every 10 ticks from tick 300, stepping 8 cells down the route's
    # cross-section every two events (continuous localized churn).
    L.append(f"churn {mid} 16 4 4 every=10 walk=0,8,1 toggle from=300 until=3000")
    dest = (w - r - 32, h // 2)
    alt = [(w - r - 32, h // 3), (w - r - 32, 2 * h // 3), (x0 + s + 160 + r, h // 2), dest]
    L.append(f"at 2 move A,B {dest[0]} {dest[1]}")
    for i, (x, z) in enumerate(alt):
        L.append(f"at {200 + 200 * i} move A,B {x} {z}")
    L += [f"at 1200 stop B", f"at 1500 move B {dest[0]} {dest[1]}",
          f"at 2 move U {w - 40} 36"]
    write(f"churn-{n}", L, f"churn-{n}: {a} bodies re-clicked four times, half stopped and re-clicked, static features dropped\n"
          f"on their way; {un} sent into a sealed box.")


def sched_load(squads=48, squad=10, tile=64, every=160, ticks=4800):
    g = int(math.ceil(math.sqrt(squads)))
    w = h = g * tile + 32
    perm = lcg_perm(squads)
    L = header("sched-load", ticks, w, h, players=2, teams=[(1, 0)])
    L.append("type r roster")
    for i in range(squads):
        gx, gz = i % g, i // g
        x0, z0 = 16 + gx * tile + 20, 16 + gz * tile + 20
        L.append(f"group S{i} {i % 2} r {squad} rect {x0} {z0} {x0 + 16} {z0 + 16}")
    # A 4x4 rock placed and lifted every 10 ticks, walking down the map's middle column (static churn:
    # stale refreshes for the groups whose windows it touches).
    L.append(f"churn {w // 2} 16 4 4 every=10 walk=0,8,1 toggle from=100 until={ticks - 600}")
    orders = []
    for i in range(squads):
        # Re-clicked every `every` ticks (staggered by squad) to the next tile of its permutation cycle,
        # always across the map: every click is a new order, a new group and a new first route.
        j, t = i, 2 + (i * every) // squads
        while t < ticks - 900:
            j = perm[j]
            gx, gz = j % g, j // g
            if abs(gx - i % g) + abs(gz - i // g) < g // 2:
                gx, gz = g - 1 - gx, g - 1 - gz
            orders.append((t, i, 16 + gx * tile + tile // 2, 16 + gz * tile + tile // 2))
            t += every
    for t, i, dx, dz in sorted(orders):
        L.append(f"at {t} move S{i} {dx} {dz}")
    write("sched-load", L, f"sched-load: {squads} squads of {squad} re-clicked to far tiles every {every} ticks with a rock\n"
          "walking across the map: continuous navigation load (field requests faster than the quota serves).")


def sched_mix(squads=48, longs=6, squad=10, tile=64, hop=120, haul=4800, ticks=7200, maps=2, churn=False):
    """Memory-pressure mix: short hops (small field windows, a new request every `hop` ticks) round long hauls
    (whole-map windows) under a field allowance of two whole-map fields: the large requests wait for room the
    small ones keep taking unless the fit scan's reservation holds it for them. One footprint (one mobility
    plane), so a squad is one group with at most one request: tools/legion_sched_load_test.py derives the
    longest a request may wait from this file's numbers (the allowance, the map, the squads)."""
    g = int(math.ceil(math.sqrt(squads)))
    w = h = g * tile + 32
    L = header("sched-mix", ticks, w, h, players=2, teams=[(1, 0)])
    L.insert(L.index("probe scale") + 1, f"navbytes {maps * w * h * 4}")
    L.append("type m mover 2 2500 10 1.8")
    # The long hauls start on the map's rim (the corners and the edge middles) and cross to the opposite
    # rim tile, back and forth; every other tile holds a short-hop squad.
    rim = [(0, 0), (g - 1, g - 1), (g - 1, 0), (0, g - 1), (g // 2, 0), (g // 2, g - 1), (0, g // 2), (g - 1, g // 2)]
    tiles = rim[:longs] + [(i % g, i // g) for i in range(g * g) if (i % g, i // g) not in rim[:longs]]
    orders = []
    for i in range(squads):
        gx, gz = tiles[i]
        x0, z0 = 16 + gx * tile + 20, 16 + gz * tile + 20
        L.append(f"group S{i} {i % 2} m {squad} rect {x0} {z0} {x0 + 16} {z0 + 16}")
        home = (16 + gx * tile + tile // 2, 16 + gz * tile + tile // 2)
        if i < longs:
            away = (16 + (g - 1 - gx) * tile + tile // 2, 16 + (g - 1 - gz) * tile + tile // 2)
            every = haul
        else:
            nx = gx + 1 if gx + 1 < g else gx - 1   # the next tile along its row
            away = (16 + nx * tile + tile // 2, home[1])
            every = hop
        t, k = 2 + (i * every) // squads, 0
        while t < ticks - 900:
            dx, dz = away if k % 2 == 0 else home
            orders.append((t, i, dx, dz))
            t += every
            k += 1
    if churn:   # sched-load's walking rock: stale refreshes, so all four request classes compete
        L.append(f"churn {w // 2} 16 4 4 every=10 walk=0,8,1 toggle from=100 until={ticks - 600}")
    for t, i, dx, dz in sorted(orders):
        L.append(f"at {t} move S{i} {dx} {dz}")
    write("sched-mix", L, f"sched-mix: {squads - longs} squads of {squad} hopping one tile every {hop} ticks round {longs} crossing the\n"
          f"map every {haul}, the field allowance {maps} whole-map fields: large requests among a stream of small ones\n"
          "under memory pressure (fit scan, reservation, request promotion; ctest legion_sched_mix).")


def box_lines(x0, z0, x1, z1):
    return [f"wall {x0} {z0} {x1 - x0} 2", f"wall {x0} {z1 - 2} {x1 - x0} 2",
            f"wall {x0} {z0} 2 {z1 - z0}", f"wall {x1 - 2} {z0} 2 {z1 - z0}"]


def scatter(n):
    """Stage 6: the other extreme of the brief's pair (one-10000 is 10,000 bodies sharing one objective): n bodies,
    each its own selection clicked to its own distant destination over the first 600 ticks -- n unrelated routes. No
    `uplink`: n one-body orders through the 64-order window would make delivery (not navigation) the whole story; the
    runner's default 64 commands a tick is used, so delivery.* here is not the real path's."""
    pitch = 8
    g = int(math.ceil(math.sqrt(n)))
    w = h = g * pitch + 32
    perm = lcg_perm(n, seed=777)
    L = header(f"scatter-{n}", ticks_for(int(w * 1.2)), w, h, players=2, teams=[(1, 0)], uplink=False)
    L.append("type r roster")
    orders = []
    for i in range(n):
        gx, gz = i % g, i // g
        x0, z0 = 16 + gx * pitch + 2, 16 + gz * pitch + 2
        L.append(f"group B{i} {i % 2} r 1 rect {x0} {z0} {x0 + 4} {z0 + 4}")
        j = perm[i]
        if abs(j % g - gx) + abs(j // g - gz) < g // 3:
            j = (g - 1 - gx) + g * min(g - 1, (g - 1 - gz))
            if j >= n:
                j = n - 1 - i % max(1, g)
        dx, dz = 16 + (j % g) * pitch + 4, 16 + (j // g) * pitch + 4
        orders.append((2 + (i * 600) // n, i, dx, dz))
    for t, i, dx, dz in sorted(orders):
        L.append(f"at {t} move B{i} {dx} {dz}")
    write(f"scatter-{n}", L, f"scatter-{n}: {n} bodies, each its own selection clicked to its own distant destination over the\n"
          "first 600 ticks (two allied players): unrelated destinations, no sharing (stage 6's other extreme).")


def idle(n, w=None, h=None):
    m = n // 10
    k = n - m
    s, sm = side(k), side(m)
    h = s + 128
    w = s + 2 * sm + 256
    L = header(f"idle-{n}", ticks_for(w), w, h)
    xi = sm + 96
    L += ["type r roster", f"group I 0 r {k} rect {xi} 64 {xi + s} {64 + s}",
          f"group M 0 r {m} rect 16 {(h - sm) // 2} {16 + sm} {(h - sm) // 2 + sm}",
          f"at 2 move M {w - 16 - sm // 2} {h // 2}"]
    write(f"idle-{n}", L, f"idle-{n}: {m} movers sent through a block of {k} idle bodies (thousands idle, a thousand moving).")


def room2door():
    """Stage 8: one order of two footprint classes (two movement planes, two Legion groups, one destination area)
    approaching a walled room from two sides through two doors -- 2x2 bodies from the west through the west door,
    3x3 bodies from the south through the south door: each group's own entrance (multi-entrance, mixed footprints)."""
    L = ["scn 1", "name room2door", "ticks 9000", "seed 7", "probe scale", "map flat 320 320",
         "type a mover 2 2500 10 1.8", "type b mover 3 2500 10 1.6"]
    # The room: x 180..244, z 100..164, walls 2 thick; a west door at z 128..136, a south door at x 208..218.
    L += ["wall 180 100 64 2", "wall 180 162 28 2", "wall 218 162 26 2", "wall 180 100 2 28", "wall 180 136 2 28",
          "wall 242 100 2 64",
          "group A 0 a 60 rect 20 100 80 164", "group B 0 b 40 rect 180 240 244 290",
          "region room 182 102 242 162", "at 1 move A,B 212 132"]
    write("room2door", L, "room2door: one order, 2x2 bodies from the west and 3x3 from the south, into a walled room\n"
          "through two doors (stage 8: an entrance per group, on its own plane and field).")


def starve(size=1024, solos=80, ticks=5000):
    """Route requests on a big map: every source of a whole-component request the audited 2048 x 2048 game had,
    at an eighth of the allowance a whole-map field (docs/legion-pathfinding.md, sections 2 and 9)."""
    k = size / 1024.0

    def c(v):
        return int(round(v * k))
    L = ["scn 1", f"name starve-{size}", f"ticks {ticks}", "seed 7", "players 2", "team 1 0", "weapons off", "uplink 16",
         "probe scale", f"navbytes {8 * 4 * size * size}", f"map flat {size} {size}",
         f"wall {c(340)} {c(380)} 4 {c(240)}", f"wall {c(680)} {c(380)} 4 {c(240)}", "type m mover 2 2500 10 3.2"]
    cols = 8
    rows = (solos + cols - 1) // cols
    pos = []
    for i in range(solos):
        x, z = c(250) + (i % cols) * 8, c(420) + (i // cols) * max(4, c(160) // max(1, rows - 1))
        pos.append((x, z))
        L.append(f"group B{i} 0 m 1 cells {x},{z}")
    for w in range(6):
        x0, z0 = c(420), c(430) + w * 24
        L.append(f"group W{w} {w % 2} m 30 rect {x0} {z0} {x0 + 30} {z0 + 18} pitch=3")
    ax, az, bx, bz = c(900), c(500), c(900), c(700)
    L += [f"churn {c(880)} {c(60)} 2 2 every=7 walk=3,0,30 toggle",
          f"churn {ax} {az} 1 1 every=1000000 from=600", f"churn {bx - 1} {bz - 1} 2 2 every=1000000 from=900"]
    for i, (x, z) in enumerate(pos):
        L.append(f"at {10 + i * 10} move B{i} {c(372) + (i % cols) * 7} {z}")
    for w in range(6):
        L.append(f"at {30 + w * 150} fight W{w} {ax if w < 4 else bx} {az if w < 4 else bz}")
    write(f"starve-{size}", L, f"starve-{size}: route requests on a big map under a field allowance of eight whole-map fields (the\n"
          "ratio of 128 MiB to a 2048 x 2048 map). 80 one-body groups, each ordered across a long wall its first window\n"
          "(its own box plus 32 cells) lies behind; six waves of 30 of two allied players in turn, 150 ticks apart, to\n"
          "two far points past a second wall; a 1 x 1 feature dropped on the first point at tick 600 (a covered goal)\n"
          "and a 2 x 2 on the second at tick 900 (a buried goal); a static change every 7 ticks in a far corner.\n"
          "docs/legion-pathfinding.md, sections 2 and 9; ctest legion_starve.")


def cutoff(name, gap, ticks, doc, size=1024):
    """Bodies cut off from their goal after the order: an enclosure with one gap, six one-body groups inside ordered
    to a point outside, the gap closed by a feature at tick 200 (`gap`: the rest of that feature's churn line)."""
    L = ["scn 1", f"name {name}", f"ticks {ticks}", "seed 7", "players 2", "team 1 0", "weapons off", "uplink 16",
         "probe scale", f"navbytes {8 * 4 * size * size}", f"map flat {size} {size}",
         "wall 200 200 200 4", "wall 200 396 200 4", "wall 200 200 4 200", "wall 396 200 4 96", "wall 396 304 4 96",
         "type m mover 2 2500 10 3.2"]
    L += [f"group C{i} 0 m 1 cells {240 + i * 8},300" for i in range(6)]
    L += ["group O 1 m 40 rect 500 600 540 630 pitch=3", "churn 880 60 2 2 every=7 walk=3,0,30 toggle",
          f"churn 396 296 4 8 {gap}"]
    L += [f"at {10 + i * 5} move C{i} 800 300" for i in range(6)]
    L += ["at 20 move O 800 760", "at 1500 move O 520 620"]
    write(name, L, doc + "\nAn enclosure with one gap; six one-body groups inside are ordered to a point outside; a feature closes\n"
          "the gap at tick 200, before any is through. A static change every 7 ticks in a far corner; 40 bodies of the\n"
          "other player walk outside the whole time. docs/legion-pathfinding.md, section 2 (static changes and routes).")


def armies(n=14, size=2048):
    """More whole-map windows than the allowance holds: n armies of 60 (one command each: over the chain demand)
    cross the map, 60 one-body groups are ordered round a wall while they walk."""
    L = ["scn 1", f"name armies{n}-{size}", "ticks 5000", "seed 7", "players 2", "team 1 0", "weapons off", "uplink 16",
         "probe scale", f"map flat {size} {size}", "wall 1000 800 4 400", "type m mover 2 2500 10 3.2"]
    L += [f"group A{i} 0 m 60 rect 150 {150 + 125 * i} 180 {168 + 125 * i} pitch=3" for i in range(n)]
    L += [f"group B{j} 1 m 1 cells {900 + 8 * (j % 10)},{850 + 50 * (j // 10)}" for j in range(60)]
    L += ["churn 1900 60 2 2 every=7 walk=3,0,30 toggle"]
    L += [f"at {20 + 40 * i} move A{i} 1850 {1900 - 125 * i}" for i in range(n)]
    L += [f"at {700 + 10 * j} move B{j} {1040 + 7 * (j % 10)} {850 + 50 * (j // 10)}" for j in range(60)]
    write(f"armies{n}-{size}", L, f"armies{n}-{size}: {n} armies of 60, one command each, cross a {size} x {size} map at the game's field\n"
          "allowance; 60 one-body groups of the other player are ordered round a wall while they walk; a static change\n"
          "every 7 ticks in a far corner. Each army is over the chain demand and builds a window over most of the map:\n"
          "more than the allowance holds (docs/legion-pathfinding.md, Known weaknesses; ctest legion_armies).")


def seedarmy():
    """A covered goal under a crowd over the chain demand: the group keeps the window, and the window of a seed
    without a component is the whole map (fieldWindow), refreshed at every change anywhere."""
    L = ["scn 1", "name seedcorpse-army", "ticks 2500", "seed 7", "players 2", "team 1 0", "weapons off", "uplink 16",
         "probe scale", "map flat 1024 1024", "type m mover 2 2500 10 3.2", "group A 0 m 60 rect 300 500 330 518 pitch=3",
         "churn 60 60 2 2 every=7 walk=3,0,30 toggle", "churn 650 510 1 1 every=1000000 from=300", "at 20 move A 650 510"]
    write("seedcorpse-army", L, "seedcorpse-army: one 60-body army (one command) walks 300 cells on a 1024 x 1024 map; a 1 x 1 feature\n"
          "lands on its click cell at tick 300; a static change every 7 ticks in a far corner\n"
          "(docs/legion-pathfinding.md, Known weaknesses; ctest legion_seedarmy).")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for n in (100, 1000, 2500, 5000, 10000):
        one(n)
        many(n)
    for n in (1000, 2500, 5000, 10000):
        swap(n)
    bridges(1000, order="fight")
    for n in (1000, 10000):
        bridges(n)
        maze(n)
        mixed(n)
        attack(n)
        churn(n)
    idle(10000)
    for n in (400, 2500, 10000):   # 400: the ctest legion_traffic (stage 7)
        scatter(n)
    sched_load()
    sched_mix()
    room2door()
    starve()
    cutoff("cutoff-1024", "every=1000000 from=200", 4000,
           "cutoff-1024: bodies cut off from their goal after the order, for good (ctest legion_cutoff_closed).")
    cutoff("cutoff-reopen", "every=1000 from=200 toggle until=1300", 6000,
           "cutoff-reopen: bodies cut off from their goal after the order; the way opens again at tick 1200\n"
           "(ctest legion_cutoff_reopen).")
    cutoff("cutoff-toggle", "every=150 from=200 toggle", 6000,
           "cutoff-toggle: bodies cut off from their goal after the order; the way opens and closes every 150 ticks\n"
           "(ctest legion_cutoff_toggle).")
    armies()
    seedarmy()
