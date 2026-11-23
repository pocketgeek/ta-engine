#!/usr/bin/env python3
"""Summarise featburn_timing.sh output: per workload, median over rounds of each build's
per-run statistics, and the candidate/base ratio of the medians. Usage: featburn_summary.py LOG"""
import sys, re, statistics as st
from collections import defaultdict
runs = defaultdict(lambda: defaultdict(list))
hashes = defaultdict(set)
for line in open(sys.argv[1]):
    if not line.startswith('round='): continue
    kv = dict(re.findall(r'(\w+)=(\S+)', line))
    key = (kv['map'], kv['scene'], kv['mode'])
    runs[key][kv['who']].append({k: float(kv[k]) for k in ('mean_ns', 'median_ns', 'p95_ns', 'p99_ns', 'max_ns')})
    hashes[key].add(kv['hash'])
cols = ('mean_ns', 'median_ns', 'p95_ns', 'p99_ns', 'max_ns')
print(f"{'map':14} {'scene':7} {'mode':8} {'n':>2} " + ' '.join(f"{c[:-3]+' b/c':>19}" for c in cols) + "  mean ratio  hash")
for key in sorted(runs):
    b, c = runs[key]['base'], runs[key]['cand']
    if not b or not c: continue
    med = lambda rs, k: st.median(r[k] for r in rs)
    cells = ' '.join(f"{med(b,k):9.0f}/{med(c,k):<9.0f}" for k in cols)
    ratio = med(c, 'mean_ns') / med(b, 'mean_ns')
    print(f"{key[0][:14]:14} {key[1]:7} {key[2]:8} {min(len(b),len(c)):>2} {cells}  {ratio:9.3f}  {'same' if len(hashes[key])==1 else 'DIFF'}")
