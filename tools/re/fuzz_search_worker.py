#!/usr/bin/env python3
"""Randomized/adversarial differential test of the whole search lifecycle.

Extends check_search_worker.py (fixed 40x32 fixtures) to generated inputs:
random map sizes, mazes, one-cell gaps, diagonal-only (corner-cut) gaps,
slope/traffic/road bands, sealed goals, goals inside obstacles, blocked or
off-map starts, start == goal, and random budgets including budget exhaustion
across many ticks. Retail's real scheduler 0x416430 drives init 0x415170, the
tracer 0x4146e0, the phase-2 cost search 0x4142c0, retries and reconstruction
0x414450; only grid preparation, controller lookup, notification and delivery
sinks are substituted (exactly as in check_search_worker.py). Each scheduler
tick compares remaining budget, active state, retry, every cell flag and
direction, the ordered grade queries, the event stream, route flags and the
delivered waypoints.

    python3 tools/re/fuzz_search_worker.py build/retail_trace_test --cases 200 --seed 1
"""
import argparse
import random
import struct
import subprocess
import sys
import time

from emuphase import Phase, OBJ, GS
from check_cost_search import digest
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EIP


def maze(rng, w, h):
    g = [0] * (w * h)
    cw, ch = (w - 1) // 2, (h - 1) // 2
    if cw < 1 or ch < 1:
        return [6] * (w * h)
    seen = {(0, 0)}
    stack = [(0, 0)]
    g[1 * w + 1] = 6
    while stack:
        x, z = stack[-1]
        nb = [(x + dx, z + dz, dx, dz) for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1))
              if 0 <= x + dx < cw and 0 <= z + dz < ch and (x + dx, z + dz) not in seen]
        if not nb:
            stack.pop()
            continue
        nx, nz, dx, dz = rng.choice(nb)
        seen.add((nx, nz))
        g[(2 * z + 1 + dz) * w + 2 * x + 1 + dx] = 6
        g[(2 * nz + 1) * w + 2 * nx + 1] = 6
        stack.append((nx, nz))
    # A few loops so ties among equal-cost routes appear.
    for _ in range(rng.randrange(0, 4)):
        x, z = rng.randrange(1, w - 1), rng.randrange(1, h - 1)
        g[z * w + x] = 6
    return g


def generate(rng, index):
    kind = ('open', 'maze', 'gap', 'diag', 'noise', 'bands', 'sealed', 'goalwall',
            'startwall', 'offmap', 'same', 'adjacent', 'comb', 'spiral', 'bigmaze', 'bigcomb')[index % 16]
    w = rng.randrange(6, 48)
    h = rng.randrange(6, 40)
    if kind.startswith('big'):
        # Long winding routes: more than 64 corners overflow the
        # reconstruction ring (0x414450) and the navigator cap (0x4e4ea0),
        # and long trips exhaust the per-retry node budget.
        w, h = rng.randrange(48, 112), rng.randrange(40, 96)
        kind = kind[3:]
    g = [6] * (w * h)
    rp = lambda: (rng.randrange(w), rng.randrange(h))
    start, goal = rp(), rp()
    if kind == 'maze':
        g = maze(rng, w, h)
        opens = [(i % w, i // w) for i, v in enumerate(g) if v]
        start, goal = rng.choice(opens), rng.choice(opens)
    elif kind == 'gap':
        x = rng.randrange(2, w - 2)
        for z in range(h):
            g[z * w + x] = 0
        gap = rng.randrange(h)
        g[gap * w + x] = rng.choice((6, 5, 4, 7))
        start = (rng.randrange(0, x), rng.randrange(h))
        goal = (rng.randrange(x + 1, w), rng.randrange(h))
    elif kind == 'diag':
        # Staircase wall: cells touch only at corners, so the only crossing
        # is a diagonal step between two blocked orthogonal neighbours.
        off = rng.randrange(-h // 2, w // 2)
        for z in range(h):
            x = z + off
            if 0 <= x < w:
                g[z * w + x] = 0
        start = (0, h - 1)
        goal = (w - 1, 0)
    elif kind == 'noise':
        d = rng.choice(((0, 6), (0, 0, 6, 6, 6), (0, 4, 5, 6, 6, 7), (4, 5, 6, 7), (0, 5, 5, 6)))
        g = [rng.choice(d) for _ in g]
    elif kind == 'bands':
        for z in range(h):
            v = rng.choice((4, 5, 6, 7, 6))
            for x in range(w):
                g[z * w + x] = v
        if rng.random() < .5:
            cols = [rng.choice((4, 5, 6, 7, 6)) for _ in range(w)]
            g = [cols[i % w] if cols[i % w] != 6 else g[i] for i in range(w * h)]
    elif kind == 'sealed':
        gx, gz = rng.randrange(1, w - 1), rng.randrange(1, h - 1)
        for dx in (-1, 0, 1):
            for dz in (-1, 0, 1):
                if dx or dz:
                    g[(gz + dz) * w + gx + dx] = 0
        goal = (gx, gz)
        while start == goal or max(abs(start[0] - gx), abs(start[1] - gz)) <= 1:
            start = rp()
    elif kind == 'goalwall':
        x0, x1 = sorted(rng.sample(range(w), 2))
        z0, z1 = sorted(rng.sample(range(h), 2))
        for z in range(z0, z1 + 1):
            for x in range(x0, x1 + 1):
                g[z * w + x] = 0
        goal = (rng.randrange(x0, x1 + 1), rng.randrange(z0, z1 + 1))
    elif kind == 'startwall':
        g[start[1] * w + start[0]] = rng.choice((0, 0, 4, 5, 7))
        for _ in range(rng.randrange(0, w * h // 4)):
            g[rng.randrange(w * h)] = 0
    elif kind == 'offmap':
        if rng.random() < .5:
            goal = (rng.choice((-3, -1, w, w + 5)), rng.randrange(-2, h + 2))
        else:
            start = (rng.choice((-1, w)), rng.randrange(h))
    elif kind == 'same':
        goal = start
    elif kind == 'adjacent':
        goal = (min(w - 1, max(0, start[0] + rng.choice((-1, 0, 1)))),
                min(h - 1, max(0, start[1] + rng.choice((-1, 0, 1)))))
        if rng.random() < .5:
            g[goal[1] * w + goal[0]] = 0
    elif kind == 'comb':
        for x in range(2, w - 1, 3):
            top = rng.random() < .5
            for z in range(h - 2):
                g[(z + (2 if top else 0)) * w + x] = 0
        start, goal = (0, rng.randrange(h)), (w - 1, rng.randrange(h))
    elif kind == 'spiral':
        x0, z0, x1, z1 = 1, 1, w - 2, h - 2
        side = 0
        while x0 < x1 and z0 < z1:
            if side == 0:
                for x in range(x0, x1 + 1): g[z0 * w + x] = 0
                z0 += 2
            elif side == 1:
                for z in range(z0 - 2, z1 + 1): g[z * w + x1] = 0
                x1 -= 2
            elif side == 2:
                for x in range(x0, x1 + 3): g[z1 * w + x] = 0
                z1 -= 2
            else:
                for z in range(z0, z1 + 3): g[z * w + x0] = 0
                x0 += 2
            side = (side + 1) & 3
        start, goal = (0, 0), (w // 2, h // 2)
    if kind not in ('startwall',) and 0 <= start[0] < w and 0 <= start[1] < h:
        if kind != 'goalwall' or start != goal:
            g[start[1] * w + start[0]] = rng.choice((6, 6, 6, 7, 5))
    budget = rng.choice((1, 7, 9, 13, 37, 100, 503, 2000, 12000, rng.randrange(1, 5000)))
    exhaust = int(rng.random() < .15)
    return kind, w, h, start, goal, g, budget, exhaust


def run_retail(width, height, start, goal, grades, budget, exhaust, ticks=5000):
    p = Phase(width, height)
    unit = p.unit(*start)
    assert p.construct() is None
    p.plant_request(unit, start, goal)

    def write(address, value): p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))
    config = GS + 0x600000
    write(config + 8, config + 0x100); write(config + 0x10c, 4)
    p.uc.mem_write(GS + 0x3068, b'\x01\x00')
    player = GS + 0x2404
    write(player, 1); p.uc.mem_write(player + 0xea, b'\x01\x00')
    first = unit - 0x138
    write(player + 0x74, first); write(player + 0x78, first + 3 * 0x138)
    write(OBJ + 0x115, first); write(OBJ + 0x58, 0); write(OBJ + 0x225, budget)
    pending = [True]; events = []; queries = []; routes = []

    def lookup(uc, args): return 0, p.NAV if pending[0] else 0

    def prepare(uc, args):
        events.extend((2, struct.unpack('<I', uc.mem_read(args + 4, 4))[0])); return 2, 0

    def notify(uc, args):
        events.extend((1, struct.unpack('<I', uc.mem_read(args, 4))[0])); return 1, 0

    def receive(uc, args):
        address, count = struct.unpack('<II', uc.mem_read(args, 8))
        events.extend((3, count))
        routes.append(struct.unpack('<' + 'h' * (count * 2), uc.mem_read(address, count * 4)) if count else ())
        return 2, 0

    def finish(uc, args): events.append(4); pending[0] = False; return 1, 0

    def grade(uc, args):
        x, z, d = struct.unpack('<iii', uc.mem_read(args, 12)); queries.extend((x, z, d))
        return 3, grades[z * width + x] if 0 <= x < width and 0 <= z < height else 0
    vt = struct.unpack('<I', p.uc.mem_read(p.NAV, 4))[0]
    p.icd.hooks[struct.unpack('<I', p.uc.mem_read(vt + 0x18, 4))[0]] = lookup
    for address, hook in ((0x4e1ee0, prepare), (0x4e2470, notify), (0x4e4ea0, receive),
                          (0x4e2060, finish), (0x4139d0, grade)):
        p.icd.hooks[address] = hook
    if exhaust:
        p.uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, d: write(OBJ + 0xec, 0), begin=0x415ef3, end=0x415ef3)
    expected = []
    for tick in range(1, ticks + 1):
        write(GS + 0x19f44, tick); write(0x634674, int(pending[0]))
        events.clear(); queries.clear()
        _, error = p.icd.call(0x416430, (1,), ecx=OBJ)
        assert error is None and p.uc.reg_read(UC_X86_REG_EIP) == 0x6ffff000, error
        cells = bytes(p.uc.mem_read(p.get(0x1c), width * height * 4))
        flags = struct.unpack('<I', p.uc.mem_read(unit + 0x134, 4))[0] & 15
        route = routes[-1] if routes else ()
        values = (tick, p.get(0x165), int(p.get(0x58) != 0), p.get(0x1ad),
                  digest(cells[i] | cells[i + 1] << 8 for i in range(0, len(cells), 4)), digest(queries), digest(events),
                  flags, len(route) // 2, *route)
        expected.append(' '.join(map(str, values)))
        if not pending[0]:
            break
    return expected + ['END'], pending[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runner')
    parser.add_argument('--cases', type=int, default=140)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--only', type=int, default=-1)
    args = parser.parse_args()
    rng = random.Random(args.seed)
    total = ticks = 0
    kinds = {}
    t0 = time.time()
    failures = capped = 0
    for index in range(args.cases):
        kind, w, h, start, goal, g, budget, exhaust = generate(rng, index)
        if args.only >= 0 and index != args.only:
            continue
        expected, unfinished = run_retail(w, h, start, goal, g, budget, exhaust)
        if unfinished:
            print(f'skip {index} {kind}: retail did not finish in 5000 ticks')
            continue
        data = ' '.join(map(str, (w, h, *start, *goal, budget, exhaust))) + '\n' + ' '.join(map(str, g)) + '\n'
        actual = subprocess.run([args.runner, '--worker'], input=data, text=True,
                                capture_output=True, check=True).stdout.splitlines()
        if actual != expected:
            failures += 1
            diff = next(((i, a, b) for i, (a, b) in enumerate(zip(actual, expected)) if a != b),
                        (len(actual), len(expected)))
            print(f'FAIL case {index} kind={kind} {w}x{h} start={start} goal={goal} budget={budget} '
                  f'exhaust={exhaust}: first diff {diff}')
            continue
        total += 1
        ticks += len(expected) - 1
        last = expected[-2].split()
        if int(last[8]) == 64: capped += 1
        kinds[kind] = kinds.get(kind, 0) + 1
    print(f'{"PASS" if not failures else "FAIL"}: {total} randomized lifecycles ({ticks} ticks), '
          f'{failures} mismatches, {capped} 64-point routes, seed {args.seed}; kinds {sorted(kinds.items())} [{time.time()-t0:.0f}s]')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
