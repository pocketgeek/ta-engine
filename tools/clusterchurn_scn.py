#!/usr/bin/env python3
"""clusterchurn_scn.py -- write the refreshPlane clustering workloads (perf follow-up J).

  tools/clusterchurn_scn.py OUTDIR

writes clusterchurn-{small,mid,edge,big}.scn: a flat 512x512 map, movers of footprints 1, 2 and 3 (three Legion
planes) ordered across the map and back, and toggling 2x2 blocking features (`churn ... every=1 toggle`) in
battle-like blobs. Every churn line changes the static map every tick (a place, then a lift that replaces it:
one or two dirty rectangles), so a tick's static sync hands refreshPlane about LINES..2*LINES rectangles:
small 4 lines in 4 far-apart blobs, mid 24 lines in 6 blobs (below the pairwise/bin threshold of 64),
edge 45 lines in 9 blobs (just above it), big 480 lines in 24 blobs (a battle's worth, below the 1024-rectangle dirty-list limit). Deterministic
(fixed seed); the committed tools/scenarios/perf/clusterchurn-*.scn files are this script's output
(kept out of tools/scenarios/*.scn, the baselined gate set).
"""
import random, sys, os

def write(path, name, blobs, per, seed):
    rnd = random.Random(seed)
    L = []
    L.append("scn 1")
    L.append(f"# perf follow-up J: refreshPlane clustering workload, {blobs} blobs x {per} toggling churn lines")
    L.append("# (tools/clusterchurn_scn.py). Movers of footprint 1, 2, 3 keep three Legion planes in use.")
    L.append(f"name {name}")
    L.append("ticks 600")
    L.append("seed 11")
    L.append("players 2")
    L.append("team 0 0")
    L.append("team 1 1")
    L.append("map flat 512 512")
    L.append("type a mover 1 2500 10 1.8")
    L.append("type b mover 2 2500 10 1.8")
    L.append("type c mover 3 2500 10 1.6")
    L.append("group A 0 a 12 rect 20 20 44 32")
    L.append("group B 0 b 12 rect 20 40 50 52")
    L.append("group C 0 c 12 rect 20 60 60 76")
    # blob centres away from the movers' start corner
    centres = []
    while len(centres) < blobs:
        x, z = rnd.randrange(90, 480), rnd.randrange(90, 480)
        if all(abs(x - a) > 40 or abs(z - b) > 40 for a, b in centres):
            centres.append((x, z))
    for (cx, cz) in centres:
        cells = set()
        while len(cells) < per:
            cells.add((cx + 3 * rnd.randrange(-6, 7), cz + 3 * rnd.randrange(-6, 7)))
        for (x, z) in sorted(cells):
            L.append(f"churn {x} {z} 2 2 every=1 toggle from={rnd.randrange(0, 2)}")
    L.append("at 1 move A 480 470")
    L.append("at 1 move B 470 480")
    L.append("at 1 move C 460 460")
    L.append("at 300 move A 30 30")
    L.append("at 300 move B 40 30")
    L.append("at 300 move C 30 40")
    with open(path, "w") as f:
        f.write("\n".join(L) + "\n")

out = sys.argv[1] if len(sys.argv) > 1 else "."
write(os.path.join(out, "clusterchurn-small.scn"), "clusterchurn-small", 4, 1, 1)
write(os.path.join(out, "clusterchurn-mid.scn"), "clusterchurn-mid", 6, 4, 2)
write(os.path.join(out, "clusterchurn-edge.scn"), "clusterchurn-edge", 9, 5, 4)
write(os.path.join(out, "clusterchurn-big.scn"), "clusterchurn-big", 24, 20, 3)
