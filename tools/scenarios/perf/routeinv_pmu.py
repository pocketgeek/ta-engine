#!/usr/bin/env python3
"""routeinv_pmu.py OUT WARM -- sum routeinv_pmu.sh SIMPMU counters (cycles / instructions / L1-miss stall cycles /
LLC misses) per phase after the first WARM ticks of each run, and compare candidate with base and with
its own --no-route-index run."""
import json, sys
out, warm = sys.argv[1], int(sys.argv[2])
rows = {}
for name in ("base", "cand", "candoff"):
    tot = {}
    n = 0
    try:
        lines = open(f"{out}/{name}.pmu").read().splitlines()
    except OSError:
        continue
    for line in lines[warm:]:   # the first WARM ticks of the run are dropped (a situation's t starts at its clock)
        toks = line.split()
        d = dict(t.split("=", 1) for t in toks[1:])
        n += 1
        for k, v in d.items():
            if "/" in v:
                vals = [int(x) for x in v.split("/")]
                acc = tot.setdefault(k, [0, 0, 0, 0])
                for i in range(4):
                    acc[i] += vals[i]
    h = json.loads(open(f"{out}/{name}.json").readline())["hash"]["0"]
    rows[name] = (n, tot, h)
for name, (n, tot, h) in rows.items():
    print(f"{name:8} ticks={n} hash={h} " + " ".join(f"{k}: instr={tot[k][1]/1e6:.1f}M cyc={tot[k][0]/1e6:.1f}M" for k in ("tick", "nav", "movement") if k in tot))
if "base" in rows and "cand" in rows:
    b, c = rows["base"][1], rows["cand"][1]
    for k in ("tick", "nav"):
        print(f"cand/base - 1 {k}: instructions {100*(c[k][1]/b[k][1]-1):+.2f}%  ({(c[k][1]-b[k][1])/max(1,rows['cand'][0])/1e3:+.1f}k per tick)  cycles {100*(c[k][0]/b[k][0]-1):+.2f}%")
if "candoff" in rows and "cand" in rows:
    o, c = rows["candoff"][1], rows["cand"][1]
    for k in ("tick", "nav"):
        print(f"cand(on)/cand(off) - 1 {k}: instructions {100*(c[k][1]/o[k][1]-1):+.2f}%  ({(c[k][1]-o[k][1])/max(1,rows['cand'][0])/1e3:+.1f}k per tick)")
