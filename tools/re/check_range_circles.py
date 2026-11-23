#!/usr/bin/env python3
"""Check client/orderoverlay.h against retail's Shift range circles.

Part 1 runs 0x4d6530 (which circles a unit gets) with its ring drawer 0x4d5c40
replaced by a recorder, over random unit/type states, and compares each
(radius, colour) list with range_circles_test --rows.

Part 2 runs the ring drawer 0x4d5c40 itself on flat ground with a stand-in
draw device and compares its vertex count and ring points with the port's
segment count and point offsets (within one world unit: retail reads a 512-entry
sine table, the port uses float trig).

    python3 tools/re/check_range_circles.py --binary build-o2/range_circles_test
"""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP

# Unused alignment padding after 0x4d6530 / 0x4d6880: stand-in device methods.
STUB_BEGIN, STUB_DRAW, STUB_NOP = 0x4d6626, 0x4d6627, 0x4d6628


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-o2/range_circles_test')
    args = ap.parse_args()
    p = Icd()
    game, unit, kind, weapon, mission, ctx, device, vtbl = [HEAP + n * 0x20000 for n in range(8)]
    put = lambda a, v: p.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    put16 = lambda a, v: p.uc.mem_write(a, struct.pack('<H', v & 0xffff))
    put(0x62d55c, game)
    put(mission + 0xe, unit)
    put(unit + 0xb4, kind)

    rings = []
    def ring(uc, a):
        r, colour = struct.unpack('<iI', uc.mem_read(a + 8, 8))
        rings.append((r, colour))
        return 6, 0
    p.hooks[0x4d5c40] = ring

    rng = random.Random(0x4d6530)
    rows, expected = [], []
    for i in range(4000):
        cloak = rng.choice([0, 0, 1, 50, 80, 100, -5, 0x7fff, rng.randrange(65536)])
        cloaked = rng.random() < 0.6
        kamikaze = rng.random() < 0.5
        explode = rng.random() < 0.8
        aoe = rng.choice([0, 10, 16, 17, 203, 320, rng.randrange(65536)])
        kdist = rng.choice([0, 64, 200, rng.randrange(65536)])
        sight = rng.choice([0, 100, 200, rng.randrange(65536)])
        mobile = rng.random() < 0.7
        tick = rng.randrange(2 ** 32)
        put16(kind + 0x22a, cloak); put(unit + 0x114, 4 if cloaked else 0)
        put(kind + 0x260, (0x10000000 if kamikaze else 0) | rng.choice([0, 0x800, 0xefffffff]))
        put(kind + 0x1b6, weapon if explode else 0); put16(weapon + 0x8a, aoe)
        put16(kind + 0x236, kdist); put16(kind + 0x226, sight)
        put(unit + 8, 0x1234 if mobile else 0); put(game + 0x19f44, tick)
        rings.clear()
        _, err = p.call(0x4d6530, (ctx, mission))
        if err: raise SystemExit(err)
        rows.append(f'{cloak} {int(cloaked)} {int(kamikaze)} {int(explode)} {aoe} {kdist} {sight} {int(mobile)} {tick}')
        expected.append(' '.join(f'{r}:{c:08x}' for r, c in rings))
    out = subprocess.run([args.binary, '--rows'], input='\n'.join(rows) + '\n',
                         capture_output=True, text=True, check=True).stdout.splitlines()
    bad = [(r, e, o) for r, e, o in zip(rows, expected, out) if e != o]
    if len(out) != len(rows) or bad:
        for r, e, o in bad[:10]: print('MISMATCH', r, 'icd', repr(e), 'port', repr(o))
        raise SystemExit(f'0x4d6530: {len(bad)} of {len(rows)} rows differ')
    print(f'0x4d6530: {len(rows)} unit states match')

    # ---- part 2: the ring itself ------------------------------------------------
    del p.hooks[0x4d5c40]
    heap = [HEAP + 0x200000]
    def alloc(uc, a):
        n = struct.unpack('<I', uc.mem_read(a, 4))[0]
        at = heap[0]; heap[0] += (n + 15) & ~15
        return 0, at   # cdecl: the caller pops
    p.hooks[0x4eb9e0] = alloc
    p.hooks[0x4eba00] = lambda uc, a: (0, 0)
    p.hooks[0x4240d0] = lambda uc, a: (4, 0)          # element constructor loop
    ground = [0]
    p.hooks[0x511170] = lambda uc, a: (1, ground[0])
    p.hooks[0x540150] = lambda uc, a: (3, 1)          # every point on screen
    p.hooks[0x5ac2f0] = lambda uc, a: (0, device)
    p.hooks[0x5ac370] = lambda uc, a: (0, device)
    p.hooks[0x5ac3a0] = lambda uc, a: (0, 0)
    put(device, vtbl); put(vtbl + 0x6c, STUB_BEGIN); put(vtbl + 0x64, STUB_DRAW)
    p.hooks[STUB_BEGIN] = lambda uc, a: (2, 0)
    drawn = []
    def draw(uc, a):
        prim, buf, count = struct.unpack('<III', uc.mem_read(a, 12))
        verts = [struct.unpack('<ff', uc.mem_read(buf + 0x20 * k, 8)) for k in range(count)]
        colours = {struct.unpack('<I', uc.mem_read(buf + 0x20 * k + 0x10, 4))[0] for k in range(count)}
        drawn.append((prim, verts, colours))
        return 3, 0
    p.hooks[STUB_DRAW] = draw
    put(ctx + 0x2c, 0); put(ctx + 0x30, 0)
    pos = HEAP + 0x1f0000
    cases = []
    for r in [1, 2, 7, 8, 9, 13, 50, 64, 80, 100, 101, 160, 203, 320, 1000]:
        cx, cz = 2000, 3000
        p.uc.mem_write(pos, struct.pack('<iii', cx << 16, 0, cz << 16))
        drawn.clear(); heap[0] = HEAP + 0x200000
        _, err = p.call(0x4d5c40, (ctx, pos, r, -1, 0, 0))
        if err: raise SystemExit(err)
        cases.append((r, drawn[0] if drawn else None, cx, cz))
    out = subprocess.run([args.binary, '--ring'], input='\n'.join(str(c[0]) for c in cases) + '\n',
                         capture_output=True, text=True, check=True).stdout.splitlines()
    worst = 0.0
    for (r, d, cx, cz), line in zip(cases, out):
        pts = [tuple(map(float, xy.split(','))) for xy in line.split()[1:]]
        n = int(line.split()[0])
        if d is None:
            if n: raise SystemExit(f'radius {r}: icd draws nothing, port {n} segments')
            continue
        prim, verts, colours = d
        if prim != 2 or colours != {0xffffffff}:
            raise SystemExit(f'radius {r}: primitive {prim} colours {colours}')
        if len(verts) != 2 * n:
            raise SystemExit(f'radius {r}: icd {len(verts)} vertices, port {n} segments')
        for k in range(n):   # segment k runs from point k to point k+1
            for end in (0, 1):
                vx, vy = verts[2 * k + end]
                ox, oz = pts[k + end]
                # screen y = z - height/2 on flat ground (height 0)
                worst = max(worst, abs(vx - int(cx + ox)), abs(vy - int(cz + oz)))
    if worst > 1.0:
        raise SystemExit(f'0x4d5c40: ring points differ by up to {worst}')
    print(f'0x4d5c40: {len(cases)} radii match (segments exact, points within {worst:.0f})')


if __name__ == '__main__':
    main()
