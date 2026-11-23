#!/usr/bin/env python3
"""Who holds an overlapped ground cell? Retail's own stamp routines decide.

Random event sequences run the unmodified retail ground-word routines on a
24x24 map: placement and cell changes go through 5066f0 (after 506aa0 for a
move, as 4dafd2/4db001 do), and a body leaves the map through 5066a0, whose
506aa0 clears the cells it holds and, when the leaver had refused someone
(+0x130 bit 0x400000), restamps every refused body (bit 0x800000) over its old
rectangle through 5065b0 -> 5097f0 -> 5062d0. The sector lists 5097f0 walks
are real: 506650 links each body as 5066f0 places it.

All players are owner kind 1 or 2, as every seat is in a World match, so a
live occupant keeps its cell. The bodies are placed with no placement test
(spawns, unloads, landings), which is how footprints come to overlap.

After every event the native holders of all cells are recorded (a dead or
departed id reads as none). retail_replay_probe --ground-owner replays the
same events through World's handlers (groundStamp/groundUnstamp/groundRemove)
and World::searchBodyRect's owner of every cell must equal retail's; the
probe also reports how often the old rule (last covering unit in vector
order) disagrees, and fails if it never does.

Two references are printed for scale. A stateless rule, the covering body
whose last placement came first, misses the cases where a leaver hands a cell
to one of several refused bodies: 5097f0 restamps them sector by sector,
newest link first. And a control with owner kind 3 (a remote player, which
World never has) shows retail overwriting the holder instead.

    python3 tools/re/check_ground_owner.py --binary build-dbg/retail_replay_probe [scenarios]
"""
import argparse
import os
import random
import struct
import subprocess
import tempfile

from emu import Icd, HEAP

W = H = 24
SECTORS = 3                      # 8-cell sectors (position >> 23)
LIVE, REFUSING, REFUSED = 0x1000000, 0x400000, 0x800000

p = Icd()
game, cells, pool, typ, player, sectors, offmap = (HEAP + o for o in
    (0, 0x20000, 0x40000, 0x60000, 0x62000, 0x63000, 0x63800))
STRIDE = 0x138
MAX_UNITS = 12


def put(a, v): p.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
def put16(a, v): p.uc.mem_write(a, struct.pack('<H', v & 0xffff))
def get(a): return struct.unpack('<I', p.uc.mem_read(a, 4))[0]
def get16(a): return struct.unpack('<H', p.uc.mem_read(a, 2))[0]


# Search-cache notifications and the corner refresh are not ground-word state.
p.hooks[0x4e1e60] = lambda uc, sp: (1, 0)
p.hooks[0x4e1e20] = lambda uc, sp: (2, 0)
p.hooks[0x50ed60] = lambda uc, sp: (2, 0)
p.freeze_hooks()
put(0x62d55c, game)
put(game + 0x19e98, W); put(game + 0x19e9c, H); put(game + 0x19f04, cells)
put(game + 0x14e84, pool); put(game + 0x14e88, pool + MAX_UNITS * STRIDE)
put(game + 0x19f18, sectors); put(game + 0x19f1c, SECTORS); put(game + 0x19f20, SECTORS)
put(game + 0x19f30, offmap)
put(player, 1)


def call(addr, args):
    eax, error = p.call(addr, args, timeout=0)
    assert error is None, error
    return eax


def unit(i): return pool + i * STRIDE


def place(i, x0, z0, fx, fz):
    u = unit(i)
    put(u + 0x74, (x0 & 0xffff) | (z0 << 16)); put(u + 0x78, fx | (fz << 16))
    put(u + 0x68, (x0 * 16 + fx * 8) << 16); put(u + 0x70, (z0 * 16 + fz * 8) << 16)


def native_owners():
    out = []
    for c in range(W * H):
        i = get16(cells + c * 14)
        live = 0 < i < MAX_UNITS and get(unit(i) + 0x130) & LIVE and get(unit(i) + 0xa4) not in (0, offmap)
        out.append(i if live else 0)
    return out


def run(seed, steps, events, kind=1):
    rng = random.Random(seed)
    p.uc.mem_write(player + 0xea, bytes([kind]))
    p.uc.mem_write(cells, (bytes(8) + b'\xff\xff' + bytes(4)) * (W * H))
    p.uc.mem_write(pool, bytes(MAX_UNITS * STRIDE))
    p.uc.mem_write(sectors, bytes(SECTORS * SECTORS * 10))
    p.uc.mem_write(offmap, bytes(10))
    count = rng.randint(3, MAX_UNITS - 1)
    foot = {}
    for i in range(1, count + 1):
        u = unit(i)
        put16(u + 2, i); put(u + 0xb4, typ); put(u + 0xb8, player)
        foot[i] = (rng.randint(1, 3), rng.randint(1, 3))
    placed, key, serial = {}, {}, 0       # placed: id -> origin; key: World's stamp serial
    gone = set()
    mismatches = 0
    cluster = rng.randint(4, 14)          # bodies crowd round one spot
    for step in range(steps):
        free = [i for i in foot if i not in placed and i not in gone]
        if not free and not placed:
            break
        roll = rng.random()
        if free and (roll < 0.35 or not placed):
            i = rng.choice(free); fx, fz = foot[i]
            x0 = max(0, min(W - fx - 1, cluster + rng.randint(-2, 2)))
            z0 = max(0, min(H - fz - 1, cluster + rng.randint(-2, 2)))
            place(i, x0, z0, fx, fz); put(unit(i) + 0x130, LIVE | 1)
            call(0x5066f0, (unit(i),))
            placed[i] = (x0, z0); serial += 1; key[i] = serial
            events.append(f'place {i} {fx} {fz} {x0} {z0}')
        elif roll < 0.8:
            i = rng.choice(sorted(placed)); fx, fz = foot[i]; x0, z0 = placed[i]
            nx = max(0, min(W - fx - 1, x0 + rng.randint(-1, 1)))
            nz = max(0, min(H - fz - 1, z0 + rng.randint(-1, 1)))
            if (nx, nz) == (x0, z0):
                continue
            call(0x506aa0, (unit(i),))
            place(i, nx, nz, fx, fz)
            call(0x5066f0, (unit(i),))
            placed[i] = (nx, nz); serial += 1; key[i] = serial
            events.append(f'move {i} {nx} {nz}')
        elif roll < 0.92:
            i = rng.choice(sorted(placed))
            call(0x5066a0, (unit(i),))
            del placed[i]
            events.append(f'lift {i}')
        else:
            # Death retires the body from the ground word at once (World's
            # model; retail keeps a dying body stamped until 512ae0).
            i = rng.choice(sorted(placed))
            put(unit(i) + 0x130, get(unit(i) + 0x130) & ~LIVE)
            call(0x5066a0, (unit(i),))
            del placed[i]; gone.add(i)
            events.append(f'die {i}')
        expect = [0] * (W * H)
        for c in range(W * H):
            cx, cz = c % W, c // W
            best = None
            for i, (x0, z0) in placed.items():
                fx, fz = foot[i]
                if x0 <= cx < x0 + fx and z0 <= cz < z0 + fz and (best is None or key[i] < key[best]):
                    best = i
            expect[c] = best or 0
        native = native_owners()
        events.append('check ' + ' '.join(map(str, native)))
        if native != expect:
            mismatches += 1
    return mismatches


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('scenarios', nargs='?', type=int, default=400)
    ap.add_argument('--steps', type=int, default=40)
    ap.add_argument('--binary', required=True)
    args = ap.parse_args()
    events, stateless = [], 0
    for seed in range(args.scenarios):
        events.append(f'scenario {W} {H}')
        # Owner kinds 1 and 2 (this machine's human and AI) behave alike.
        stateless += run(seed, args.steps, events, kind=1 + seed % 2)
    checks = sum(1 for e in events if e.startswith('check'))
    print(f'retail 5066f0/506aa0/5066a0: {args.scenarios} scenarios, {checks} checks after each event')
    print(f'  reference: the stateless first-placed rule disagrees after {stateless}')
    control, scratch = 0, []
    for seed in range(args.scenarios // 4):
        control += run(seed, args.steps, scratch, kind=3)
    print(f'  control: with owner kind 3 that rule disagrees after {control} '
          f'of {sum(1 for e in scratch if e.startswith("check"))} checks (retail overwrites)')
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, 'events.txt')
        with open(path, 'w') as f:
            f.write('\n'.join(events) + '\n')
        out = subprocess.run([args.binary, '--ground-owner', path], capture_output=True, text=True)
    print(out.stdout.strip(), out.stderr.strip())
    return out.returncode if control else 1


if __name__ == '__main__':
    raise SystemExit(main())
