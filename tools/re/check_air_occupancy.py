#!/usr/bin/env python3
"""Compare the persistent airborne occupant grid with retail over many ticks.

Each tick runs retail's own routines in retail's order: retirements through
507050(unit,0) (map removal 5066a0 from 512ae0), deaths that only clear the
live bit (their cells stay stale), then the end-of-update passes at
51da6a..51dbbe -- 507050(unit,1) for every live flyer, then 506c40. Bodies
wander, overlap densely, take off and land and cross map edges. Grid words,
the bounded-random 535cc0 draw count and the RNG state are compared after
every tick; unlike check_air_collision_grid.py the grid and the overlap lists
persist.

    python3 tools/re/check_air_occupancy.py build-dbg/retail_visual_test [scenarios]
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP

p = Icd()
game, cells, pool, typ = [HEAP + i * 0x10000 for i in range(4)]
seed = 0; calls = 0


def put(a, v): p.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))


def draw(uc, sp):
    global seed, calls
    n = struct.unpack('<I', uc.mem_read(sp, 4))[0]
    assert 1 <= n <= 7, n
    seed = (seed * 214013 + 2531011) & 0xffffffff; calls += 1
    return 1, ((seed >> 16) & 32767) * n // 32768


p.hooks[0x535cc0] = draw; p.freeze_hooks()
put(0x62d55c, game)
rng = random.Random(0x507050)
scenarios = int(sys.argv[2]) if len(sys.argv) > 2 else 256
rows, expected = [], []
totals = {'ticks': 0, 'draws': 0, 'retire': 0, 'silent': 0, 'sentinel': 0}


def call(addr, args):
    _, error = p.call(addr, args, timeout=0)
    assert error is None, error
    assert p.uc.reg_read(__import__('unicorn').x86_const.UC_X86_REG_EIP) == 0x6FFFF000


for scenario in range(scenarios):
    width = rng.randrange(8, 17); height = rng.randrange(8, 17)
    count = rng.randrange(2, 21); ticks = rng.randrange(8, 40)
    seed = rng.getrandbits(32); start = seed; calls = 0
    put(game + 0x19e98, width); put(game + 0x19e9c, height)
    put(game + 0x19f04, cells); put(game + 0x14e84, pool); put(game + 0x14e88, pool + count * 0x138)
    p.uc.mem_write(cells, bytes(width * height * 14)); p.uc.mem_write(pool, bytes((count + 1) * 0x138))
    p.uc.mem_write(typ, bytes(0x270)); put(typ + 0x260, 0x800)
    cluster = (rng.randrange(0, width), rng.randrange(0, height))
    spread = rng.choice((1, 2, 4, 16))
    bodies = []
    for i in range(1, count + 1):
        a = pool + i * 0x138
        fx, fz = rng.randrange(1, 5), rng.randrange(1, 5)
        p.uc.mem_write(a + 2, struct.pack('<H', i)); p.uc.mem_write(a + 0x78, struct.pack('<2h', fx, fz))
        put(a + 0xb4, typ)
        # The constructor leaves no recorded insertion origin.
        p.uc.mem_write(a + 0x126, struct.pack('<2h', -9999, -9999))
        bodies.append({'a': a, 'fx': fx, 'fz': fz, 'live': True,
                       'x': cluster[0] + rng.randrange(-spread, spread + 1),
                       'z': cluster[1] + rng.randrange(-spread, spread + 1), 'mode': 2})
    lines = [f'{width} {height} {count} {ticks} {start}']
    for tick in range(ticks):
        events = []
        for b in bodies:
            b['x'] += rng.randrange(-1, 2); b['z'] += rng.randrange(-1, 2)
            if rng.random() < 0.1: b['mode'] = 3 - b['mode']
            e = 0
            if b['live'] and tick > 0:
                r = rng.random()
                e = 1 if r < 0.04 else 2 if r < 0.06 else 0
            events.append(e)
            lines.append(f"{b['x']} {b['z']} {b['fx']} {b['fz']} {int(b['mode'] == 2)} {e}")
        for b in bodies:
            p.uc.mem_write(b['a'] + 0x74, struct.pack('<2h', b['x'], b['z']))
            put(b['a'] + 0x130, (0x1000000 if b['live'] else 0) | b['mode'])
        for b, e in zip(bodies, events):
            if not b['live'] or not e: continue
            if e == 1:
                call(0x507050, (b['a'], 0)); totals['retire'] += 1
            else:
                totals['silent'] += 1
            b['live'] = False; put(b['a'] + 0x130, b['mode'])
        for b in bodies:
            if b['live']: call(0x507050, (b['a'], 1))
        for b in bodies:
            if b['live']: call(0x506c40, (b['a'],))
        raw = bytes(p.uc.mem_read(cells, width * height * 14))
        words = [struct.unpack_from('<H', raw, i * 14 + 2)[0] for i in range(width * height)]
        totals['sentinel'] += words.count(0xffff)
        expected.append((seed, calls, *[-1 if v == 0xffff else v for v in words]))
        totals['ticks'] += 1
    totals['draws'] += calls
    rows.append('\n'.join(lines))
r = subprocess.run([sys.argv[1] if len(sys.argv) > 1 else 'build-dbg/retail_visual_test', '--air-occupancy'],
                   input='\n'.join(rows) + '\n', text=True, capture_output=True, check=True)
actual = [tuple(map(int, line.split())) for line in r.stdout.splitlines()]
assert len(actual) == len(expected), (len(actual), len(expected))
bad = next((i for i, (a, e) in enumerate(zip(actual, expected)) if a != e), None)
assert bad is None, (bad, actual[bad][:12], expected[bad][:12])
print(f"PASS: {scenarios} persistent airborne occupancy scenarios, {totals['ticks']} ticks match native "
      f"507050/506c40 ({totals['draws']} draws, {totals['retire']} retirements, {totals['silent']} silent deaths, "
      f"{totals['sentinel']} sentinel cells)")
