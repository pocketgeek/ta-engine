#!/usr/bin/env python3
"""Compare AI behaviour between two sets of roster JSONL captures (retail vs tak).

  python3 tools/re/compare_ai.py --a ~/tak-tmp/retail-ai/runs/*/retail.jsonl \
      --b ~/tak-tmp/retail-ai/tak/*.jsonl [--label-a retail --label-b tak] \
      [--by-faction] [--json out.json]

Inputs are the shared schema written by tools/re/retail_ai_poll.py (retail) and
by a debug takserver with TAK_ROSTER_DUMP (tak; tools/re/tak_ai_run.py). Each AI
seat of each file is one observation (an AI-vs-AI file gives two). With --b
omitted it summarises one set.

Per observation it derives:
  build order   the first --order-length own units in placement order (a unit's
                first appearance; the starting units are excluded) and in
                completion order;
  timings       the first completion of every type, and the 1st/2nd/4th/7th
                lodestone (type name ending in "lode"), first static building
                that is not a lodestone, first mobile unit;
  composition   live finished units per type and per class at 3/6/10/15/20 min;
  economy       mana at each minute, the share of samples with mana under 5% of
                storage ("broke") and at 99% or more ("full");
  lodestones    finished lodestones at each minute;
  waves         with squad data (retail): strike squads gaining members, their
                launch ticks and sizes; for both engines a squad-free proxy,
                "sorties": the first tick a mobile unit is more than --sortie px
                from the start, and the most mobile units out that far at once;
  placement     distance from the start and angle relative to the direction of
                the nearest enemy start, for every static unit placed;
  outcome       which seats still own units at the end, and the game length.
A type is "mobile" if any of its units in the observation moved more than 8 px
between samples, otherwise "static"; lodestones are their own class.

Distribution summaries: n, median and IQR for each set, and the two-sample
Kolmogorov-Smirnov D with its asymptotic p-value (approximate below n of about
10 per side; a p-value under 0.05 means the two sets very likely differ).
Ticks are 30 per second in both engines.
"""
import argparse
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import statistics
import sys

MINUTES = (3, 6, 10, 15, 20)
TICKS_PER_MIN = 30 * 60


# ---- reading ---------------------------------------------------------------
def read_capture(path):
    meta, samples, end = {}, [], None
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue          # a run killed mid-write leaves a torn last line
            kind = record.get('kind', 'sample')
            if kind == 'meta':
                if not meta:
                    meta = record
            elif kind == 'end':
                end = record
            elif 'units' in record:
                samples.append(record)
    samples.sort(key=lambda s: s['tick'])
    return meta, samples, end


def faction(type_name):
    return type_name[:3]


# ---- one observation -------------------------------------------------------
class Observation:
    def __init__(self, path, meta, samples, end, player, args):
        self.path, self.player, self.args = path, player, args
        self.engine = meta.get('engine', '?')
        first = samples[0]
        own0 = [u for u in first['units'] if u[1] == player]
        self.start = (own0[0][4], own0[0][5]) if own0 else (0, 0)
        self.faction = faction(own0[0][2]) if own0 else '?'
        initial = {(u[0], u[2]) for u in first['units']}
        # Enemy starts: every other seat's first unit.
        self.enemy_starts = []
        for p in sorted({u[1] for u in first['units']} - {player}):
            unit = next(u for u in first['units'] if u[1] == p)
            self.enemy_starts.append((unit[4], unit[5]))
        self.length = samples[-1]['tick']
        self.end = end
        # Unit histories keyed by (id, type): a retail slot is reused after a death.
        placed, done, last_pos, moved = {}, {}, {}, defaultdict(bool)
        prev_remaining = {}
        for s in samples:
            for u in s['units']:
                key = (u[0], u[2])
                if key in last_pos:
                    x, z = last_pos[key]
                    if abs(u[4] - x) + abs(u[5] - z) > 8:
                        moved[u[2]] = True
                last_pos[key] = (u[4], u[5])
                if u[1] != player or key in initial:
                    continue
                if key not in placed:
                    placed[key] = (s['tick'], u[2], u[4], u[5])
                if key not in done and u[3] <= 0 and (prev_remaining.get(key, 1) > 0 or key in placed):
                    done[key] = s['tick']
                prev_remaining[key] = u[3]
        self.moved = moved
        self.placed = sorted(placed.items(), key=lambda kv: (kv[1][0], kv[0][0]))
        self.done = sorted(((t, key[1]) for key, t in done.items()))
        self.samples = samples
        self.squads = any('squads' in s for s in samples)

    def cls(self, type_name):
        if type_name.endswith('lode'):
            return 'lodestone'
        return 'mobile' if self.moved.get(type_name) else 'static'

    def at_minute(self, minute):
        tick = minute * TICKS_PER_MIN
        if tick > self.length + self.args.every:
            return None
        best = None
        for s in self.samples:
            if s['tick'] <= tick:
                best = s
            else:
                break
        return best

    def metrics(self):
        m = {}
        m['order_placed'] = [t for _, (_, t, _, _) in self.placed][:self.args.order_length]
        m['order_done'] = [t for _, t in self.done][:self.args.order_length]
        firsts = {}
        for tick, t in self.done:
            firsts.setdefault(t, tick)
        m['first_done'] = firsts
        lodes = [tick for tick, t in self.done if t.endswith('lode')]
        for k in (1, 2, 4, 7):
            m[f'lodestone_{k}'] = lodes[k - 1] if len(lodes) >= k else None
        statics = [tick for tick, t in self.done if self.cls(t) == 'static']
        mobiles = [tick for tick, t in self.done if self.cls(t) == 'mobile']
        m['first_static'] = statics[0] if statics else None
        m['first_mobile'] = mobiles[0] if mobiles else None
        comp, comp_cls, mana, lodecount = {}, {}, {}, {}
        for minute in MINUTES:
            s = self.at_minute(minute)
            if s is None:
                continue
            own = [u for u in s['units'] if u[1] == self.player and u[3] <= 0]
            comp[minute] = dict(Counter(u[2] for u in own))
            comp_cls[minute] = dict(Counter(self.cls(u[2]) for u in own))
            lodecount[minute] = sum(1 for u in own if u[2].endswith('lode'))
        for minute in range(1, int(self.length // TICKS_PER_MIN) + 1):
            s = self.at_minute(minute)
            pl = next((p for p in s['players'] if p['p'] == self.player), None) if s else None
            if pl:
                mana[minute] = pl['mana']
        m['composition'], m['composition_class'] = comp, comp_cls
        m['mana_by_minute'], m['lodestones_by_minute'] = mana, lodecount
        broke = full = total = 0
        for s in self.samples:
            pl = next((p for p in s['players'] if p['p'] == self.player), None)
            if not pl or pl['storage'] <= 0:
                continue
            total += 1
            broke += pl['mana'] < 0.05 * pl['storage']
            full += pl['mana'] >= 0.99 * pl['storage']
        m['broke_share'] = broke / total if total else None
        m['full_share'] = full / total if total else None
        # Sorties: mobile units far from the start.
        sortie, most = None, 0
        for s in self.samples:
            out = [u for u in s['units'] if u[1] == self.player and self.cls(u[2]) == 'mobile'
                   and math.hypot(u[4] - self.start[0], u[5] - self.start[1]) > self.args.sortie]
            if out and sortie is None:
                sortie = s['tick']
            most = max(most, len(out))
        m['first_sortie'], m['max_sortie'] = sortie, most
        # Waves from squad data: a strike slot that gains members.
        waves = []
        if self.squads:
            had = {}
            for s in self.samples:
                if 'squads' not in s:
                    continue
                mine = next((q for q in s['squads'] if q['p'] == self.player), None)
                now = {q['slot']: len(q['members']) for q in (mine['squads'] if mine else [])
                       if q['kind'] == 'strike'}
                for slot, n in now.items():
                    if n and not had.get(slot):
                        waves.append((s['tick'], n))
                had = now
        m['waves'] = waves
        m['first_wave'] = waves[0][0] if waves else None
        m['wave_count'] = len(waves) if self.squads else None
        m['wave_size_median'] = statistics.median(n for _, n in waves) if waves else None
        # Placement relative to the start, angle against the nearest enemy start.
        dist, ang = [], []
        enemy = min(self.enemy_starts, key=lambda e: math.hypot(e[0] - self.start[0], e[1] - self.start[1]),
                    default=None)
        toward = math.atan2(enemy[1] - self.start[1], enemy[0] - self.start[0]) if enemy else 0.0
        for _, (tick, t, x, z) in self.placed:
            if self.cls(t) == 'mobile':
                continue
            d = math.hypot(x - self.start[0], z - self.start[1])
            dist.append(d)
            if d > 1:
                a = math.degrees(math.atan2(z - self.start[1], x - self.start[0]) - toward)
                ang.append((a + 180) % 360 - 180)
        m['placement_distance'], m['placement_angle'] = dist, ang
        m['placement_distance_median'] = statistics.median(dist) if dist else None
        m['placement_toward_enemy_share'] = (sum(1 for a in ang if abs(a) < 90) / len(ang)) if ang else None
        last = self.samples[-1]
        alive = sorted({u[1] for u in last['units']})
        m['outcome'] = 'alive' if self.player in alive else 'dead'
        m['survivors'] = alive
        m['length'] = self.length
        m['end_reason'] = (self.end or {}).get('reason')
        return m


def observations(paths, args):
    out = []
    for path in paths:
        meta, samples, end = read_capture(path)
        if not samples:
            print(f'warning: {path}: no samples', file=sys.stderr)
            continue
        # (The 2026-10-22 scratch probes wrote retail's controller byte as "kind": 2 = AI.)
        is_ai = lambda p: p.get('controller', {2: 'ai'}.get(p.get('kind'))) == 'ai'
        seats = [p['p'] for p in samples[0]['players'] if is_ai(p)]
        # A retail seat handed to the AI after the first sample still reads human there.
        later = [p['p'] for p in samples[-1]['players'] if is_ai(p)]
        seats = sorted(set(seats) | set(later))
        if args.player is not None:
            seats = [args.player]
        for p in seats:
            if any(u[1] == p for u in samples[0]['units']):
                out.append(Observation(str(path), meta, samples, end, p, args))
    return out


def unit_events(samples):
    """(tick, NEW|DONE|GONE, player, type, id) from successive samples."""
    out, prev = [], {}
    for s in samples:
        now = {(u[0], u[2]): u for u in s['units']}
        for key, u in now.items():
            if key not in prev:
                out.append((s['tick'], 'NEW', u[1], u[2], u[0]))
            elif prev[key][3] > 0 and u[3] <= 0:
                out.append((s['tick'], 'DONE', u[1], u[2], u[0]))
        for key, u in prev.items():
            if key not in now:
                out.append((s['tick'], 'GONE', u[1], u[2], u[0]))
        prev = now
    return out


# ---- statistics ------------------------------------------------------------
def quantile(values, q):
    v = sorted(values)
    if not v:
        return None
    pos = (len(v) - 1) * q
    lo, hi = math.floor(pos), math.ceil(pos)
    return v[lo] + (v[hi] - v[lo]) * (pos - lo)


def ks_2samp(a, b):
    """Two-sample KS statistic and asymptotic p-value (Smirnov, with the
    Stephens small-sample correction)."""
    a, b = sorted(a), sorted(b)
    if not a or not b:
        return None, None
    i = j = 0
    d = 0.0
    while i < len(a) and j < len(b):
        x = min(a[i], b[j])
        while i < len(a) and a[i] == x:
            i += 1
        while j < len(b) and b[j] == x:
            j += 1
        d = max(d, abs(i / len(a) - j / len(b)))
    n = len(a) * len(b) / (len(a) + len(b))
    lam = (math.sqrt(n) + 0.12 + 0.11 / math.sqrt(n)) * d
    p = 2 * sum((-1) ** (k - 1) * math.exp(-2 * k * k * lam * lam) for k in range(1, 101))
    return d, min(1.0, max(0.0, p))


def summary(values):
    v = [x for x in values if x is not None]
    if not v:
        return {'n': 0}
    return {'n': len(v), 'missing': len(values) - len(v), 'median': quantile(v, 0.5),
            'q1': quantile(v, 0.25), 'q3': quantile(v, 0.75)}


def edit_distance(a, b):
    row = list(range(len(b) + 1))
    for i, x in enumerate(a, 1):
        prev, row[0] = row[0], i
        for j, y in enumerate(b, 1):
            cur = min(row[j] + 1, row[j - 1] + 1, prev + (x != y))
            prev, row[j] = row[j], cur
    return row[-1]


def modal_sequence(sequences, length):
    seq = []
    for k in range(length):
        c = Counter(s[k] for s in sequences if len(s) > k)
        if not c:
            break
        seq.append(c.most_common(1)[0][0])
    return seq


def mean_cross_distance(xs, ys, same=False):
    pairs = [(x, y) for i, x in enumerate(xs) for j, y in enumerate(ys) if not (same and j <= i)]
    return statistics.mean(edit_distance(x, y) for x, y in pairs) if pairs else None


# ---- report ----------------------------------------------------------------
SCALARS = ('lodestone_1', 'lodestone_2', 'lodestone_4', 'lodestone_7', 'first_static',
           'first_mobile', 'first_sortie', 'max_sortie', 'first_wave', 'wave_count',
           'wave_size_median', 'broke_share', 'full_share', 'placement_distance_median',
           'placement_toward_enemy_share', 'length')


def fmt(v, tick=False):
    if v is None:
        return '-'
    if tick:
        return f'{v / 1800:.2f}m'
    if isinstance(v, float):
        return f'{v:.0f}' if abs(v) >= 100 else f'{v:.3g}'
    return str(v)


def compare(groups, labels, args):
    report = {}
    ms = [[o.metrics() for o in g] for g in groups]
    tickish = lambda name: name.startswith(('lodestone_', 'first_')) or name == 'length'
    rows = []
    for name in SCALARS:
        cols = [summary([m[name] for m in g]) for g in ms]
        entry = {labels[i]: cols[i] for i in range(len(groups))}
        if len(groups) == 2:
            d, p = ks_2samp(*[[m[name] for m in g if m[name] is not None] for g in ms])
            entry['ks_d'], entry['ks_p'] = d, p
        report[name] = entry
        rows.append((name, cols, entry.get('ks_d'), entry.get('ks_p'), tickish(name)))
    for minute in MINUTES:
        for key in ('lodestones_by_minute', 'mana_by_minute'):
            vals = [[m[key].get(minute) for m in g] for g in ms]
            if not any(v is not None for g in vals for v in g):
                continue
            name = f'{key.split("_")[0]}@{minute}m'
            cols = [summary(v) for v in vals]
            entry = {labels[i]: cols[i] for i in range(len(groups))}
            if len(groups) == 2:
                entry['ks_d'], entry['ks_p'] = ks_2samp(*[[x for x in v if x is not None] for v in vals])
            report[name] = entry
            rows.append((name, cols, entry.get('ks_d'), entry.get('ks_p'), False))
        for cls in ('static', 'mobile'):
            vals = [[(m['composition_class'].get(minute) or {}).get(cls, 0)
                     if minute in m['composition_class'] else None for m in g] for g in ms]
            if not any(v is not None for g in vals for v in g):
                continue
            name = f'{cls}@{minute}m'
            cols = [summary(v) for v in vals]
            entry = {labels[i]: cols[i] for i in range(len(groups))}
            if len(groups) == 2:
                entry['ks_d'], entry['ks_p'] = ks_2samp(*[[x for x in v if x is not None] for v in vals])
            report[name] = entry
            rows.append((name, cols, entry.get('ks_d'), entry.get('ks_p'), False))
    # Build orders.
    orders = {}
    for i, g in enumerate(ms):
        placed = [m['order_placed'] for m in g]
        orders[labels[i]] = {'modal_placed': modal_sequence(placed, args.order_length),
                             'modal_done': modal_sequence([m['order_done'] for m in g], args.order_length),
                             'within_distance': mean_cross_distance(placed, placed, same=True),
                             'position_frequency': [dict(Counter(s[k] for s in placed if len(s) > k).most_common(4))
                                                    for k in range(args.order_length)]}
    if len(groups) == 2:
        orders['cross_distance'] = mean_cross_distance(*[[m['order_placed'] for m in g] for g in ms])
        orders['modal_distance'] = edit_distance(orders[labels[0]]['modal_placed'], orders[labels[1]]['modal_placed'])
    report['build_order'] = orders
    # Composition by type.
    comp = {}
    for minute in MINUTES:
        per = {}
        for i, g in enumerate(ms):
            have = [m['composition'][minute] for m in g if minute in m['composition']]
            if have:
                types = sorted({t for c in have for t in c})
                per[labels[i]] = {t: statistics.mean(c.get(t, 0) for c in have) for t in types}
        if per:
            comp[minute] = per
    report['composition_mean'] = comp
    report['outcomes'] = {labels[i]: dict(Counter(m['outcome'] for m in g)) for i, g in enumerate(ms)}
    report['observations'] = {labels[i]: [{'file': o.path, 'player': o.player, 'faction': o.faction,
                                           'engine': o.engine, 'length': o.length} for o in g]
                              for i, g in enumerate(groups)}
    return report, rows


def print_report(report, rows, labels, title=''):
    print(f'== {title or "all"} ==')
    for label in labels:
        obs = report['observations'][label]
        print(f'  {label}: {len(obs)} AI seat(s); factions {dict(Counter(o["faction"] for o in obs))}')
    head = f'{"metric":32}' + ''.join(f'{l + " median [IQR] (n)":>34}' for l in labels)
    if len(labels) == 2:
        head += f'{"KS D":>8}{"p":>8}'
    print(head)
    for name, cols, d, p, tick in rows:
        cells = []
        for c in cols:
            if not c.get('n'):
                cells.append(f'{"-":>34}')
            else:
                cells.append(f'{fmt(c["median"], tick)} [{fmt(c["q1"], tick)}..{fmt(c["q3"], tick)}] ({c["n"]})'.rjust(34))
        line = f'{name:32}' + ''.join(cells)
        if len(labels) == 2:
            line += f'{fmt(d):>8}{fmt(p):>8}'
        print(line)
    bo = report['build_order']
    for label in labels:
        print(f'  build order, {label} (modal, placed): {" ".join(bo[label]["modal_placed"])}')
        print(f'  build order, {label} (modal, finished): {" ".join(bo[label]["modal_done"])}')
        print(f'    mean edit distance within {label}: {fmt(bo[label]["within_distance"])}')
    if len(labels) == 2:
        print(f'  mean edit distance across sets: {fmt(bo["cross_distance"])}; '
              f'between modal orders: {bo["modal_distance"]}')
    for minute, per in report['composition_mean'].items():
        for label, types in per.items():
            top = sorted(types.items(), key=lambda kv: -kv[1])[:10]
            print(f'  {minute:>2}m {label}: ' + ', '.join(f'{t} {v:.1f}' for t, v in top))
    print(f'  outcomes: {report["outcomes"]}')
    print()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--a', nargs='+', required=True, type=Path, help='first set of JSONL files')
    parser.add_argument('--b', nargs='+', type=Path, help='second set of JSONL files')
    parser.add_argument('--label-a', default='A')
    parser.add_argument('--label-b', default='B')
    parser.add_argument('--player', type=int, help='analyse only this seat (default: every AI seat)')
    parser.add_argument('--by-faction', action='store_true', help='also report each faction separately')
    parser.add_argument('--order-length', type=int, default=15)
    parser.add_argument('--sortie', type=float, default=1200.0, help='sortie distance from the start, px')
    parser.add_argument('--every', type=int, default=30, help='sample cadence the captures used, ticks')
    parser.add_argument('--json', type=Path, help='also write the full report as JSON')
    parser.add_argument('--events', action='store_true',
                        help='print each file\'s unit events (placed, finished, gone) and stop')
    args = parser.parse_args()
    if args.events:
        for path in [*args.a, *(args.b or [])]:
            _, samples, end = read_capture(path)
            print(f'{path}: {len(samples)} samples, end {end}')
            for event in unit_events(samples)[sum(1 for _ in samples[0]['units']) if samples else 0:]:
                print('  %6d %-4s p%d %-10s #%d' % event)
        return 0
    groups = [observations(args.a, args)]
    labels = [args.label_a]
    if args.b:
        groups.append(observations(args.b, args))
        labels.append(args.label_b)
    if not all(groups):
        sys.exit('no AI seats found in one of the sets')
    full = {}
    report, rows = compare(groups, labels, args)
    print_report(report, rows, labels)
    full['all'] = report
    if args.by_faction:
        factions = sorted({o.faction for g in groups for o in g})
        for fac in factions:
            sub = [[o for o in g if o.faction == fac] for g in groups]
            if not all(sub):
                continue
            report, rows = compare(sub, labels, args)
            print_report(report, rows, labels, fac)
            full[fac] = report
    if args.json:
        args.json.write_text(json.dumps(full, indent=1, default=str))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
