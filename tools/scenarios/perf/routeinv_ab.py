#!/usr/bin/env python3
"""routeinv_ab.py OUT WARM -- summarise routeinv_ab.sh runs: per run the mean / median / p95 / p99 / max and ticks over
33.3 ms of SIMPHASE tick and nav after WARM ticks, syncStatic and staticChanged ms from the last LPROBE line (cumulative
over the whole run), the ROUTEIDX counters and the final hash; then per concurrent pair the B/A ratios."""
import glob, json, os, re, statistics as st, sys

out, warm = sys.argv[1], int(sys.argv[2])


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, max(0, int(round(p / 100 * len(v) + 0.5)) - 1))]


def load(base):
    ph = []
    lprobe = {}
    ridx = {}
    for line in open(base + ".err", errors="replace"):
        if line.startswith("SIMPHASE"):
            d = dict(kv.split("=") for kv in line.split()[1:])
            ph.append({k: float(v.rstrip("ms")) for k, v in d.items()})
        elif line.startswith("LPROBE "):
            lprobe = {k: v for k, _, v in (t.partition("=") for t in line.split()[1:])}
        elif line.startswith("ROUTEIDX "):
            ridx = {k: v for k, _, v in (t.partition("=") for t in line.split()[1:]) if _}
    h = ""
    try:
        h = json.loads(open(base + ".json").readline())["hash"]["0"]
    except Exception:
        pass
    m = ph[warm:]
    s = {}
    for k in ("tick", "nav", "movement"):
        v = [r[k] for r in m]
        if v:
            s[k] = dict(mean=st.mean(v), med=st.median(v), p95=pct(v, 95), p99=pct(v, 99), max=max(v),
                        over=sum(1 for x in v if x > 1000.0 / 30))
    return dict(n=len(m), s=s, lp=lprobe, ri=ridx, hash=h)


runs = {}
for f in sorted(glob.glob(os.path.join(out, "r*-*-??.err"))):
    b = f[:-4]
    m = re.match(r"r(\d+)-([AB])-(\d\d)$", os.path.basename(b))
    runs[(m.group(1), m.group(2), m.group(3))] = load(b)
for (r, ab, c), x in sorted(runs.items()):
    t, n = x["s"].get("tick", {}), x["s"].get("nav", {})
    print(f"r{r} {ab} cores{c} n={x['n']} hash={x['hash']} tick mean={t.get('mean',0):.3f} med={t.get('med',0):.3f} "
          f"p95={t.get('p95',0):.3f} p99={t.get('p99',0):.3f} max={t.get('max',0):.3f} over33={t.get('over',0)} | nav mean={n.get('mean',0):.3f} "
          f"p99={n.get('p99',0):.3f} max={n.get('max',0):.3f} | changed_ms={x['lp'].get('changed_ms','-')} "
          f"calls={x['lp'].get('changed_calls','-')} sync_ms={x['lp'].get('sync_ms','-')} | idx={x['ri'].get('index','-')} "
          f"cand={x['ri'].get('candidates','-')} walk={x['ri'].get('walk_groups','-')} touched={x['ri'].get('touched','-')} "
          f"dec={x['ri'].get('decisions','-')}")
print("pairs (B/A - 1, %):")
for r in sorted({k[0] for k in runs}):
    for ca, cb in (("01", "23"), ("23", "01")):
        a, b = runs.get((r, "A", ca)), runs.get((r, "B", cb))
        if not a or not b:
            continue
        row = []
        for k in ("tick", "nav", "movement"):
            if k in a["s"] and k in b["s"] and a["s"][k]["mean"] > 0:
                row.append(f"{k} mean {100 * (b['s'][k]['mean'] / a['s'][k]['mean'] - 1):+.1f} "
                           f"p99 {100 * (b['s'][k]['p99'] / a['s'][k]['p99'] - 1):+.1f}")
        try:
            ca_ms, cb_ms = float(a["lp"]["changed_ms"]), float(b["lp"]["changed_ms"])
            row.append(f"changed_ms {ca_ms:.1f} -> {cb_ms:.1f}")
        except Exception:
            pass
        try:
            sa, sb = float(a["lp"]["sync_ms"]), float(b["lp"]["sync_ms"])
            row.append(f"sync_ms {sa:.1f} -> {sb:.1f}")
        except Exception:
            pass
        print(f"  r{r} A{ca}/B{cb}: " + "; ".join(row))
