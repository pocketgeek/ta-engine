#!/usr/bin/env python3
"""Check the Classic AI's tables against retail's captured PrintWeights dumps,
without running retail.

Retail's `+PrintWeights` debug print (0x413030) writes one row per unit type:
`<limit|n/a> - <dynamic priority> = <profile preference> - '<UNIT> :<name>'`.
The harness (retail_ai_harness.py --printweights) captured such dumps once,
with a memory poll of the AI's build manager every few ticks (manager.jsonl:
tick, game RNG seed, store, income history, population, unit counts and the
priority table). This script reads those captures read-only and checks, per
dump:

  1. pref and limit: every row equals the Classic profile baked from the same
     map's aiprofile (exact).
  2. priority: it finds the polls on either side of the refresh that put the
     dump's table in place, and runs our port (classic_ai_test --printweights:
     the brain's type facts, profile limits and priority pass) over retail's
     table, in retail's order, on that state, from the earlier poll's seed
     advanced k draws (the sim's draws between the poll and the refresh are
     unknown), k = 0..--search. A k that reproduces every row is an exact
     match. A table already in place at the first poll is tried as the
     manager's creation pass (no limits, no units, no store) and as an early
     refresh, from the harness's poke seeds.
  3. retail's own priority routine 0x410810, emulated over the same table
     and state, against the port: at the matching seed and on --seeds random
     seeds (every row, the final seed and the draw count must agree). This
     part needs no capture to be reproducible.

    python3 tools/re/check_ai_printweights.py --binary build-o2/classic_ai_test \\
        --data assets/game [--dumps ~/tak-tmp/retail-ai/baseline/printweights]

Exit status 0 only when every row's pref and limit match, every dump is
reproduced and the emulation agrees everywhere; --report prints how each dump
was reproduced and every row that differs.
"""
import argparse
import json
import os
import random
import re
import struct
import subprocess
import sys
import zlib
from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_FPCW

ROW = re.compile(r"^\s*(n/a|-?\d+)\s+-\s+(-?\d+)\s+=\s+(-?\d+)\s+-\s+'([^\t']+)")
TYPE_SIZE = 676   # retail's unit-type record (0x2a4)
AUTOSTOP_SEED = 1234   # -AutoStop reseeds with it before every tick


def step(seed, k):
    for _ in range(k):
        seed = seed * 16807 % 0x7fffffff
    return seed


def parse_dump(path):
    rows = []
    for line in open(path, encoding='latin-1'):
        m = ROW.match(line)
        if m:
            limit = -1 if m.group(1) == 'n/a' else int(m.group(1))
            rows.append((m.group(4).strip().lower(), limit, int(m.group(2)), int(m.group(3))))
    return rows


class Port:
    """classic_ai_test --printweights: our brain's view of retail's table."""
    def __init__(self, binary, data):
        self.binary, self.data = binary, data

    def run(self, lines):
        proc = subprocess.run([self.binary, '--printweights', self.data], input='\n'.join(lines)+'\n',
                              text=True, capture_output=True, check=True)
        return proc.stdout.splitlines()

    def facts(self, names):
        out = {}
        for name, line in zip(names, self.run([f'facts {n}' for n in names])):
            v = list(map(int, line.split()))
            out[name] = None if not v[0] else dict(flags=v[1], secondary=v[2], water=v[3],
                                                    cost=struct.unpack('<f', struct.pack('<I', v[4]))[0],
                                                    cost_bits=v[4], pref=v[5], limit=v[6])
        return out

    def passes(self, state_line, seeds, creation=False):
        out = []
        for line in self.run([state_line, f'creation {int(creation)}']+[f'seed {s}' for s in seeds]):
            v = list(map(int, line.split()))
            out.append((v[:-2], v[-2], v[-1]))
        return out


def state_line(names, counts, pop, stored, capacity, hist):
    parts = [f'state {pop} {stored!r} {capacity!r} {len(names)}']
    for n in names:
        c = counts.get(n, [0, 0])
        parts.append(f'{n} {c[0]} {c[1]}')
    for income, usage in hist:
        parts.append(f'{income!r} {usage!r}')
    return ' '.join(parts)


def transitions(entry, names, want):
    """Pairs of manager polls (before, after) around each change of the AI
    seat's priority table into the dump's, latest first, up to the dump."""
    path = os.path.join(os.path.dirname(entry['dump']), 'manager.jsonl')
    lines = [json.loads(l) for l in open(path)]
    order = lines[0]['types']
    pos = [order.index(n) if n in order else None for n in names]
    seat = json.load(open(entry['state']))['seat']
    polls = []
    for x in lines[1:]:
        for st in x['seats']:
            if st['p'] == seat:
                st = dict(st, tick=x['tick'], rng=x['rng'])
                st['table'] = [st['prio'][i] if i is not None else None for i in pos]
                polls.append(st)
    tick = json.load(open(entry['state']))['dump']['gametime_tick']
    out = []
    for a, b in zip(polls, polls[1:]):
        if b['tick'] > tick + 60:
            break
        if b['table'] == want and a['table'] != want:
            out.append((a, b))
    return out[::-1]


class Retail:
    """0x410810 over a whole table, on the state a dump implies."""
    def __init__(self):
        p = self.p = Icd()
        p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        self.game, self.manager, self.owner, self.resource, self.types, self.arrays = \
            [HEAP+n*0x30000 for n in range(6)]
        self.put(0x62d55c, self.game)
        self.put(self.game+0x175c4, self.types)
        self.put(self.manager, self.owner)
        self.put(self.owner+0x10c, self.resource)
        # The priority-combat value (0x4107e0) is not part of this check.
        p.hooks[0x4107e0] = lambda uc, sp: (1, 0)
        p.freeze_hooks()
        self.calls = []

        def observe(uc, address, size, user):
            sp = uc.reg_read(UC_X86_REG_ESP)
            self.calls.append(struct.unpack('<i', uc.mem_read(sp+4, 4))[0])
        p.uc.hook_add(UC_HOOK_CODE, observe, begin=0x535cc0, end=0x535cc0)

    def put(self, a, v):
        self.p.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))

    def get(self, a):
        return struct.unpack('<I', self.p.uc.mem_read(a, 4))[0]

    def run(self, facts, counts, pop, stored, capacity, hist, seed):
        """facts: per row (index 1..n) a facts dict; counts: per row (count, completed)."""
        uc, a, n = self.p.uc, self.arrays, len(facts)+1
        prio, combat, count, done, desired = a, a+0x400, a+0x800, a+0xc00, a+0x1000
        uc.mem_write(self.types, bytes(TYPE_SIZE*n))
        uc.mem_write(a, bytes(0x2000))
        self.put(self.game+0x175b8, n)
        for offset, base in ((0x65, prio), (0xa1, combat)):
            self.put(self.manager+offset+4, base)
            self.put(self.manager+offset+8, base+n)
            self.put(self.manager+offset+12, base+n)
        for offset, base in ((0x85, count), (0x95, done), (0xe5, desired)):
            self.put(self.manager+offset, base)
        for i, (f, (c, d)) in enumerate(zip(facts, counts), start=1):
            t = self.types+TYPE_SIZE*i
            self.put(t+0x260, f['flags'])
            self.put(t+0x264, f['secondary'])
            uc.mem_write(t+0x194, struct.pack('<h', f['water']))
            self.put(t+0x20e, f['cost_bits'])
            uc.mem_write(count+2*i, struct.pack('<h', c))
            uc.mem_write(done+2*i, struct.pack('<h', d))
            self.put(desired+4*i, f['limit'])
        uc.mem_write(self.owner+0xe8, struct.pack('<H', pop))
        uc.mem_write(self.resource, struct.pack('<ff', stored, capacity))
        for i, (income, usage) in enumerate(hist):
            uc.mem_write(self.resource+0x28+12*i+4, struct.pack('<ff', income, usage))
        self.put(0x64186c, seed)
        self.calls.clear()
        _, error = self.p.call(0x410810, ecx=self.manager)
        if error:
            raise RuntimeError(error)
        out = [b if b < 128 else b-256 for b in uc.mem_read(prio+1, n-1)]
        return out, self.get(0x64186c), sum(1 for c in self.calls if c >= 2)


def check_dump(key, entry, port, retail, args, report):
    rows = parse_dump(entry['dump'])
    names = [r[0] for r in rows]
    facts = port.facts(names)
    result = dict(key=key, rows=len(rows), pref_ok=0, limit_ok=0, prio_ok=0, mismatches=[], k=None)
    for name, limit, prio, pref in rows:
        f = facts[name]
        if f is None:
            result['mismatches'].append((name, 'type', 'missing from our catalogue'))
            continue
        result['pref_ok'] += f['pref'] == pref
        result['limit_ok'] += f['limit'] == limit
        if f['pref'] != pref:
            result['mismatches'].append((name, 'pref', f'retail {pref}, ours {f["pref"]}'))
        if f['limit'] != limit:
            result['mismatches'].append((name, 'limit', f'retail {limit}, ours {f["limit"]}'))

    want = [r[2] for r in rows]
    tries = []   # (state, counts, start seed, creation pass)
    for before, after in transitions(entry, names, want):
        # The refresh came between two polls: try the earlier poll's state and
        # then the later one's (counts or the store may move in between), each
        # from the earlier poll's seed.
        for st in (before, after):
            tries.append((st, st['counts'], before['rng'], False))
    if not tries:
        # The table was in place at the first poll: the manager's creation pass
        # at game start (0x4102f0 -> 0x410810: empty caches, the profile not
        # read yet so every limit is -1, the player's store not set up) or an
        # early refresh; from retail's -AutoStop seed or the harness's tick-1 poke.
        after = json.load(open(entry['state']))['state_after_refresh']
        meta = json.load(open(os.path.join(os.path.dirname(entry['dump']), 'meta.json')))
        seeds = [AUTOSTOP_SEED, meta['poke']['rng_before'], meta['poke']['rng_after']]
        empty = dict(after, mana=0.0, storage=0.0, hist=[[0.0, 0.0]]*30)
        tries = [(dict(st, pop=pop), counts, s, creation) for s in seeds
                 for creation, st, counts in ((True, empty, {}), (False, after, after['counts']))
                 for pop in sorted({0, 1, after['pop']})]
    best = None
    for st, counts, start, creation in tries:
        line = state_line(names, counts, st['pop'], st['mana'], st['storage'], st['hist'])
        seeds, s = [], start
        for _ in range(args.search+1):
            seeds.append(s)
            s = s*16807 % 0x7fffffff
        for k, (got, final, draws) in enumerate(port.passes(line, seeds, creation)):
            hits = sum(g == w for g, w in zip(got, want))
            if best is None or hits > best[0]:
                best = (hits, k, seeds[k], got, st, counts, line, creation, start)
            if hits == len(want):
                break
        if best[0] == len(want):
            break
    hits, k, seed, got, st, counts, line, creation, start = best
    result['prio_ok'] = hits
    result['k'] = k if hits == len(want) else None
    result['how'] = (f"{'creation pass' if creation else 'refresh'} after tick {st['tick']}, "
                     f"pop {st['pop']}, seed {start} + {k} draws")
    for (name, _, prio, _), g in zip(rows, got):
        if g != prio:
            f = facts[name]
            fixed = f is not None and f['cost'] <= 500.0
            result['mismatches'].append((name, 'priority',
                f'retail {prio}, ours {g} at the closest seed (k={k}); '
                + ('no draw in this row: a facts/state difference' if fixed else 'row draws RNG')))

    # Part 3: retail's routine against the port on this state.
    if all(facts[n] is not None for n in names):
        tfacts = [dict(facts[n], limit=-1) if creation else facts[n] for n in names]
        tcounts = [tuple(counts.get(n, [0, 0])) for n in names]
        rng = random.Random(zlib.crc32(key.encode()))
        probe = [seed]+[rng.randrange(1, 0x7fffffff) for _ in range(args.seeds)]
        ours = port.passes(line, probe, creation)
        emu_bad = 0
        for s, (pg, pf, pd) in zip(probe, ours):
            rg, rf, rd = retail.run(tfacts, tcounts, st['pop'], st['mana'], st['storage'], st['hist'], s)
            if (rg, rf, rd) != (pg, pf, pd):
                emu_bad += 1
                if report:
                    print(f'  {key}: seed {s}: emulated {rg} {rf} {rd} vs port {pg} {pf} {pd}')
        result['emu_cases'] = len(probe)
        result['emu_bad'] = emu_bad
    else:
        result['emu_cases'], result['emu_bad'] = 0, 0
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--binary', default='build-o2/classic_ai_test')
    ap.add_argument('--data', default='assets/game')
    ap.add_argument('--dumps', default=os.path.expanduser('~/tak-tmp/retail-ai/baseline/printweights'))
    ap.add_argument('--search', type=int, default=20000, help='seed offsets tried per dump')
    ap.add_argument('--seeds', type=int, default=64, help='random seeds for the emulation check')
    ap.add_argument('--report', action='store_true', help='print every differing row')
    ap.add_argument('--only', help='check one index.json key')
    args = ap.parse_args()
    index = json.load(open(os.path.join(args.dumps, 'index.json')))
    port, retail = Port(args.binary, args.data), Retail()
    total = dict(rows=0, pref_ok=0, limit_ok=0, prio_ok=0, emu_cases=0, emu_bad=0, exact=0, dumps=0)
    failed = False
    for key, entry in index.items():
        if args.only and key != args.only:
            continue
        r = check_dump(key, entry, port, retail, args, args.report)
        total['dumps'] += 1
        for f in ('rows', 'pref_ok', 'limit_ok', 'prio_ok', 'emu_cases', 'emu_bad'):
            total[f] += r[f]
        total['exact'] += r['k'] is not None
        print(f"{key:14s} rows {r['rows']:3d}  pref {r['pref_ok']:3d}  limit {r['limit_ok']:3d}  "
              f"priority {r['prio_ok']:3d}  {'exact at k=%d' % r['k'] if r['k'] is not None else 'NOT reproduced'}  "
              f"emulation {r['emu_cases']-r['emu_bad']}/{r['emu_cases']}")
        if args.report:
            print(f"    {r['how']}")
            for m in r['mismatches']:
                print(f'    {m[0]:10s} {m[1]:8s} {m[2]}')
        failed |= (r['pref_ok'] != r['rows'] or r['limit_ok'] != r['rows'] or r['k'] is None
                   or r['emu_bad'] != 0 or r['emu_cases'] == 0)
    print(f"{'PASS' if not failed else 'FAIL'}: {total['dumps']} dumps, {total['rows']} rows: "
          f"pref {total['pref_ok']}, limit {total['limit_ok']}, priority {total['prio_ok']} equal; "
          f"{total['exact']} dumps reproduced exactly; emulated 0x410810 = port on "
          f"{total['emu_cases']-total['emu_bad']}/{total['emu_cases']} table passes")
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
