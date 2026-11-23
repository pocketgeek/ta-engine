#!/usr/bin/env python3
"""Compare the flyer landing-site predicate with retail 509400.

Randomized maps exercise the unexplored shortcut (including footprints whose
coarse exploration index uses footprintX on both axes), map edges, feature
anchors/tails/markers and blocking bits, structure-yard flags, ground and
airborne occupant words (self, dead, out-of-pool and 0xffff owners), water for
flyers/floaters and the depth/slope limits.

    python3 tools/re/check_landing_site.py build-dbg/retail_visual_test [cases]
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP

p = Icd()
game, cells, explore, features, pool, typ, point = (HEAP + o for o in
    (0, 0x20000, 0x30000, 0x40000, 0x60000, 0x70000, 0x71000))
POOL = 6


def w32(a, v): p.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
def w16(a, v): p.uc.mem_write(a, struct.pack('<H', v & 0xffff))
def w8(a, v): p.uc.mem_write(a, bytes([v & 0xff]))


w32(0x62d55c, game)
rng = random.Random(0x509400)
cases = int(sys.argv[2]) if len(sys.argv) > 2 else 8192
rows, expected = [], []
stats = {'unexplored': 0, 'accepted': 0, 'refused': 0}
for case in range(cases):
    W, H = rng.randrange(6, 17), rng.randrange(6, 17)
    fx, fz = rng.randrange(1, 6), rng.randrange(1, 6)
    player = rng.randrange(10)
    sea = rng.choice((0, 20, 60, 100))
    maxwd = rng.choice((0, 10000, 20, -5, 300))
    minwd = rng.choice((-10000, 0, 13, -20))
    slope = rng.choice((255, 255, 10, 0))
    canfly = int(rng.random() < 0.8)
    floater = int(rng.random() < 0.2)
    self_id = rng.randrange(1, POOL + 1)
    alive = [0] + [int(rng.random() < 0.7) for _ in range(POOL)]
    alive[self_id] = 1
    nfeat = rng.randrange(0, 5)
    blocking = [int(rng.random() < 0.5) for _ in range(nfeat)]
    words = (W >> 1) * ((H >> 1) + 4)
    explored_all = rng.random() < 0.6
    ex = [((1 << player) if explored_all or rng.random() < 0.5 else 0) | (rng.getrandbits(16) & ~(1 << player))
          for _ in range(words)]
    # Bias the site to the map, with some off-edge origins and subcell offsets.
    x0 = rng.randrange(-1, W - fx + 1); z0 = rng.randrange(-1, H - fz + 1)
    x = ((x0 * 16 + fx * 8) << 16) + rng.randrange(-8 << 16, 8 << 16)
    z = ((z0 * 16 + fz * 8) << 16) + rng.randrange(-8 << 16, 8 << 16)
    dense = rng.random() < 0.7  # mostly clean cells so deep tests are reached
    cl = []
    for i in range(W * H):
        q = 0.04 if dense else 0.3
        ground = rng.randrange(0, POOL + 3) if rng.random() < q else (self_id if rng.random() < 0.05 else 0)
        r = rng.random()
        air = 0xffff if r < q / 3 else (rng.randrange(0, POOL + 3) if r < q else (self_id if r < q + 0.05 else 0))
        r = rng.random()
        back = 0
        if r < (0.7 if dense else 0.4):
            feature = 0xffff
        elif r < 0.85 and i > 0:
            feature = 0xfffe
            bz = rng.randrange(0, min(i // W, 3) + 1); bx = rng.randrange(0, min(i - bz * W, 3) + 1) if i - bz * W >= 0 else 0
            back = (bz, bx)
        elif r < 0.9:
            feature = rng.randrange(0xfffa, 0xfffe)
        else:
            feature = rng.randrange(0, nfeat + 2)
        yard = int(rng.random() < (0.03 if dense else 0.15))
        low = max(0, min(255, sea + rng.choice((-30, -5, 0, 1, 10, 40))))
        high = min(255, low + rng.choice((0, 0, 3, 12, 40)))
        cl.append([ground, air, high, low, feature, back, yard])
    # Tail anchors must resolve inside the map and to a bounded feature value.
    for i, c in enumerate(cl):
        if c[4] == 0xfffe:
            bz, bx = c[5]
            j = i - (bz * W + bx)
            if j < 0:
                c[5] = (0, 0); j = i
            if j == i:
                c[4] = 0xffff; c[5] = (0, 0); continue
            if cl[j][4] == 0xfffe or (nfeat <= cl[j][4] < 0xfffa):
                cl[j][4] = rng.randrange(0, nfeat) if nfeat else 0xffff
        else:
            c[5] = (0, 0)
    # Native memory.
    p.uc.mem_write(game, bytes(0x1a000))
    w32(game + 0x19e98, W); w32(game + 0x19e9c, H)
    w32(game + 0x19ef4, explore); w8(game + 0x19ef8, sea); w32(game + 0x19f04, cells)
    w32(game + 0x19ec0, nfeat); w32(game + 0x19edc, features)
    w32(game + 0x14e84, pool); w32(game + 0x14e88, pool + POOL * 0x138)
    p.uc.mem_write(explore, b''.join(struct.pack('<H', v) for v in ex))
    p.uc.mem_write(features, bytes(max(nfeat, 1) * 0x140))
    for i, b in enumerate(blocking):
        w32(features + i * 0x140 + 0x13c, 0x20 if b else rng.choice((0, 0x1f, 0x40)))
    p.uc.mem_write(pool, bytes((POOL + 1) * 0x138))
    for i in range(1, POOL + 1):
        w16(pool + i * 0x138 + 2, i)
        w32(pool + i * 0x138 + 0x130, (0x1000000 if alive[i] else 0) | rng.choice((1, 2)))
    me = pool + self_id * 0x138
    w16(me + 0x78, fx); w16(me + 0x7a, fz); w32(me + 0xb4, typ); w8(me + 0xfd, player)
    p.uc.mem_write(typ, bytes(0x270))
    w16(typ + 0x192, maxwd); w16(typ + 0x194, minwd); w8(typ + 0x23c, slope)
    w32(typ + 0x260, (0x800 if canfly else 0) | (0x200000 if floater else 0) | rng.choice((0, 0x1, 0x400)))
    buf = bytearray(W * H * 14)
    for i, (ground, air, high, low, feature, back, yard) in enumerate(cl):
        struct.pack_into('<HHxBBxHBBxB', buf, i * 14, ground, air, high, low, feature, back[0], back[1],
                         (0x10 if yard else 0) | rng.choice((0, 0x20, 0x01)))
    p.uc.mem_write(cells, bytes(buf))
    p.uc.mem_write(point, struct.pack('<iii', x, 0, z))
    eax, error = p.call(0x509400, (me, point), timeout=0)
    assert error is None, (case, error)
    result = eax & 1
    expected.append(result)
    stats['accepted' if result else 'refused'] += 1
    head = [W, H, fx, fz, maxwd, minwd, slope, canfly, floater, player, sea, x, z, self_id, POOL, nfeat, words]
    flat = alive[1:] + blocking + ex
    for ground, air, high, low, feature, back, yard in cl:
        flat += [ground, air, high, low, feature, back[0] * W + back[1], yard]
    rows.append(' '.join(map(str, head + flat)))
r = subprocess.run([sys.argv[1] if len(sys.argv) > 1 else 'build-dbg/retail_visual_test', '--landing-site'],
                   input='\n'.join(rows) + '\n', text=True, capture_output=True, check=True)
actual = [int(v) for v in r.stdout.split()]
assert len(actual) == len(expected), (len(actual), len(expected))
bad = [i for i, (a, e) in enumerate(zip(actual, expected)) if a != e]
assert not bad, (len(bad), bad[0], rows[bad[0]][:200], actual[bad[0]], expected[bad[0]])
print(f'PASS: {cases} native 509400 landing-site decisions match '
      f'({stats["accepted"]} accepted, {stats["refused"]} refused)')
