#!/usr/bin/env python3
"""Do retail flyers land on other flyers? Multi-tick landings run natively.

Scenario extension of check_landing_site.py and check_air_occupancy.py: a
cluster of flyers hovers over one spot and keeps calling retail's own landing
predicate 509400 at its position. Everything that decides is native:

- an accepted flyer descends for a few ticks, still in flight mode 2, then
  enters mode 1 and is stamped into ground word +0 by 5066f0 (the stamp that
  51b370 / 4dafd2 perform when the mode changes);
- after every tick the persistent airborne grid is rebuilt with 507050(u,1)
  and 506c40 (51da6a..51dbbe), drawing owners with 535cc0;
- hovering flyers that are refused drift by a cell now and then.

Every native 509400 call is also replayed through our port
(retail_visual_test --landing-site, fed the native ground/air words), and
must agree. The script then reports how often two flyers end up LANDED with
overlapping footprints, and how each overlap arose: the later lander was
accepted while the earlier one was descending (a shared cell's owner was
redrawn to the later lander, hiding the descender), or the site was
unexplored (509400 runs no occupancy test at all).

    python3 tools/re/check_landing_overlap.py build-dbg/retail_visual_test [scenarios]
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP

p = Icd()
game, cells, explore, pool, typ, point, player = (HEAP + o for o in
    (0, 0x20000, 0x30000, 0x40000, 0x60000, 0x61000, 0x62000))
seed = 0


def put(a, v): p.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
def w16(a, v): p.uc.mem_write(a, struct.pack('<H', v & 0xffff))


def draw(uc, sp):
    global seed
    n = struct.unpack('<I', uc.mem_read(sp, 4))[0]
    seed = (seed * 214013 + 2531011) & 0xffffffff
    return 1, ((seed >> 16) & 32767) * n // 32768


p.hooks[0x535cc0] = draw
p.hooks[0x506650] = lambda uc, sp: (2, 0)   # sector-list relink: not part of the words
p.freeze_hooks()
put(0x62d55c, game)


def call(addr, args):
    eax, error = p.call(addr, args, timeout=0)
    assert error is None, error
    return eax


def overlap(a, b):
    return (a['x'] < b['x'] + b['f'] and b['x'] < a['x'] + a['f'] and
            a['z'] < b['z'] + b['f'] and b['z'] < a['z'] + a['f'])


rng = random.Random(0x509401)
scenarios = int(sys.argv[2]) if len(sys.argv) > 2 else 512
rows, expected = [], []
stats = {'landings': 0, 'pairs': 0, 'scen_overlap': 0, 'hidden_descender': 0, 'unexplored': 0, 'sentinel': 0,
         'onto_landed': 0, 'checks': 0, 'refused_by_landed': 0}
for scenario in range(scenarios):
    W, H = rng.randrange(12, 21), rng.randrange(12, 21)
    count = rng.randrange(2, 13)
    explored = rng.random() < 0.85
    seed = rng.getrandbits(32)
    words = (W >> 1) * ((H >> 1) + 4)
    p.uc.mem_write(game, bytes(0x1a000))
    put(game + 0x19e98, W); put(game + 0x19e9c, H); put(game + 0x19f04, cells)
    put(game + 0x19ef4, explore); put(game + 0x14e84, pool); put(game + 0x14e88, pool + count * 0x138)
    # Flat dry open map: only feature word +8 needs its 'none' value 0xffff.
    p.uc.mem_write(cells, (bytes(8) + b'\xff\xff' + bytes(4)) * (W * H))
    p.uc.mem_write(explore, struct.pack(f'<{words}H', *([0xffff if explored else 0] * words)))
    p.uc.mem_write(pool, bytes((count + 1) * 0x138))
    p.uc.mem_write(typ, bytes(0x270))
    w16(typ + 0x192, 10000); w16(typ + 0x194, -10000 & 0xffff)
    p.uc.mem_write(typ + 0x23c, bytes([255])); put(typ + 0x260, 0x800)
    cx, cz = W // 2 - 2, H // 2 - 2
    flyers = []
    for i in range(1, count + 1):
        a = pool + i * 0x138
        f = rng.choice((2, 2, 3))
        w16(a + 2, i); p.uc.mem_write(a + 0x78, struct.pack('<2h', f, f)); put(a + 0xb4, typ)
        put(a + 0xb8, player)   # owner record: 5066f0 reads [+0xb8] when it overwrites a live occupant
        p.uc.mem_write(a + 0x126, struct.pack('<2h', -9999, -9999))
        flyers.append({'id': i, 'a': a, 'f': f, 'x': cx + rng.randrange(-1, 2), 'z': cz + rng.randrange(-1, 2),
                       'mode': 2, 'state': 'hover', 'left': 0, 'accepted': None})

    def sync(b):
        p.uc.mem_write(b['a'] + 0x74, struct.pack('<2h', b['x'], b['z']))
        put(b['a'] + 0x68, (b['x'] * 16 + b['f'] * 8) << 16)
        put(b['a'] + 0x70, (b['z'] * 16 + b['f'] * 8) << 16)
        put(b['a'] + 0x130, 0x1000000 | b['mode'])

    def rebuild():
        for b in flyers: sync(b)
        for b in flyers: call(0x507050, (b['a'], 1))
        for b in flyers: call(0x506c40, (b['a'],))

    if not explored:
        # An unexplored site with a flyer already landed on it.
        b = flyers[0]; b['mode'] = 1; b['state'] = 'landed'; b['accepted'] = []; sync(b)
        call(0x5066f0, (b['a'],))
    rebuild()
    for tick in range(60):
        for b in flyers:
            if b['state'] == 'hover':
                x = (b['x'] * 16 + b['f'] * 8) << 16; z = (b['z'] * 16 + b['f'] * 8) << 16
                p.uc.mem_write(point, struct.pack('<iii', x, 0, z))
                raw = bytes(p.uc.mem_read(cells, W * H * 14))
                ok = call(0x509400, (b['a'], point)) & 1
                stats['checks'] += 1
                expected.append(ok)
                flat = [1] * count + [v for v in struct.unpack(f'<{words}H', bytes(p.uc.mem_read(explore, words * 2)))]
                for c in range(W * H):
                    g, air = struct.unpack_from('<HH', raw, c * 14)
                    flat += [g, air, 0, 0, 0xffff, 0, 0]
                rows.append(' '.join(map(str, [W, H, b['f'], b['f'], 10000, -10000, 255, 1, 0, 0, 0, x, z,
                                               b['id'], count, 0, words] + flat)))
                if ok:
                    under = [o for o in flyers if o is not b and overlap(o, b)]
                    b['accepted'] = [(o['id'], o['state']) for o in under]
                    foot = [struct.unpack_from('<H', raw, ((b['z'] + r) * W + b['x'] + c) * 14 + 2)[0]
                            for r in range(b['f']) for c in range(b['f'])]
                    b['sentinel'] = 0xffff in foot
                    if any(o['state'] == 'landed' for o in under): stats['onto_landed'] += 1
                    b['state'] = 'descend'; b['left'] = rng.randrange(3, 13)
                else:
                    if any(o['state'] == 'landed' and overlap(o, b) for o in flyers): stats['refused_by_landed'] += 1
                    if rng.random() < 0.25:
                        b['x'] = max(1, min(W - b['f'] - 2, b['x'] + rng.randrange(-1, 2)))
                        b['z'] = max(1, min(H - b['f'] - 2, b['z'] + rng.randrange(-1, 2)))
            elif b['state'] == 'descend':
                b['left'] -= 1
                if b['left'] <= 0:
                    # Mode 2 -> 1: 51b370/4dafd2 re-stamp through 5066f0.
                    b['mode'] = 1; b['state'] = 'landed'; sync(b)
                    call(0x5066f0, (b['a'],)); stats['landings'] += 1
        rebuild()
    landed = [b for b in flyers if b['state'] == 'landed']
    found = False
    for i, a in enumerate(landed):
        for b in landed[i + 1:]:
            if not overlap(a, b): continue
            found = True; stats['pairs'] += 1
            if not explored: stats['unexplored'] += 1
            else:
                # Whichever was accepted second saw the other descending.
                later, first = (a, b) if dict(a['accepted']).get(b['id']) == 'descend' else (b, a)
                if dict(later['accepted']).get(first['id']) == 'descend':
                    stats['hidden_descender'] += 1
                    if later['sentinel']: stats['sentinel'] += 1
    stats['scen_overlap'] += found
r = subprocess.run([sys.argv[1] if len(sys.argv) > 1 else 'build-dbg/retail_visual_test', '--landing-site'],
                   input='\n'.join(rows) + '\n', text=True, capture_output=True, check=True)
actual = [int(v) for v in r.stdout.split()]
assert len(actual) == len(expected), (len(actual), len(expected))
bad = [i for i, (a, e) in enumerate(zip(actual, expected)) if a != e]
assert not bad, (len(bad), bad[0], actual[bad[0]], expected[bad[0]])
print(f"PASS: {stats['checks']} native 509400 calls on natively maintained words match the port")
print(f"retail outcome over {scenarios} scenarios: {stats['landings']} landings; {stats['scen_overlap']} scenarios "
      f"end with overlapping LANDED flyers ({stats['pairs']} pairs: {stats['hidden_descender']} accepted while "
      f"the other was descending -- {stats['sentinel']} of them through 0xffff overflow cells, the rest "
      f"through an owner redraw -- and {stats['unexplored']} on unexplored sites); "
      f"{stats['onto_landed']} acceptances on top of an already landed flyer "
      f"(explored ones: refused {stats['refused_by_landed']} times)")
