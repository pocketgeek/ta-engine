#!/usr/bin/env python3
"""Compare the Classic brain's AI manager construction with retail's 0x40fed0.

Runs the original manager constructor -- its 98 squads, the base squads' quota
draws (0x40c560) and all three wave-size rolls (0x40c240 ground, 0x40c3c0 air,
0x40c490 naval) -- over random seeds and random player tables (who is active,
of which kind, allied, and how many units each has, which the ground roll reads
as E), and checks that ai::classicConstructSquads, given the same seed and E,
leaves the same squads: kind, clock, p0..p8 and active of every slot, the
assignment countdown and the final RNG seed. The only substitution is operator
new (0x4eb9e0), a bump allocator; the per-slot squad data is set up by retail's
own 0x50b340.

Observation only: reads YOUR OWN retail install from the gitignored assets/
directory; nothing from the binary is copied into the engine.

    python3 tools/re/check_ai_squad_ctor.py --binary build-o2/classic_ai_test
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP

GAME, PLAYERS, STRIDE = HEAP, 0x2404, 0x110
DATA = HEAP + 0x20000          # the player's 100 squad data records (0xc4 each)
ARENA = HEAP + 0x30000         # operator new
KINDS = {0x5ebb2c: 0, 0x5ebb50: 1, 0x5ebb60: 2, 0x5ebb70: 3, 0x5ebb1c: 4}   # Base Strike Backup Vtol Reserve


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-o2/classic_ai_test')
    ap.add_argument('--cases', type=int, default=3000)
    args = ap.parse_args()
    p = Icd()
    put = lambda a, v: p.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    get = lambda a: struct.unpack('<I', p.uc.mem_read(a, 4))[0]
    geti = lambda a: struct.unpack('<i', p.uc.mem_read(a, 4))[0]
    heap = [ARENA]

    def new(uc, argv):
        size = struct.unpack('<I', uc.mem_read(argv, 4))[0]
        at = heap[0]; heap[0] = (at + size + 15) & ~15
        uc.mem_write(at, bytes(size))
        return 0, at                     # cdecl: the caller pops
    p.hooks[0x4eb9e0] = new
    p.freeze_hooks()
    rng = random.Random(0x40fed0)
    rows, expected = [], []
    for case in range(args.cases):
        heap[0] = ARENA
        p.uc.mem_write(GAME, bytes(0x2404 + 10 * STRIDE + 0x100))
        put(0x62d55c, GAME)
        own = rng.randrange(10)
        player = GAME + PLAYERS + own * STRIDE
        enemy = 10
        records = []
        for i in range(10):
            rec = GAME + PLAYERS + i * STRIDE
            active = rng.random() < 0.8 or i == own
            kind = rng.choice([0, 1, 2, 2, 3, 4])
            if i == own and case % 7 == 0: kind = 2     # an AI seat: flag +0x1a5
            side = rng.choice([i, i, rng.randrange(11), 10])
            pop = rng.choice([0, 1, 5, 9, 10, 11, 25, 60, 150, 499, rng.randrange(65536)])
            put(rec, 1 if active else 0)
            p.uc.mem_write(rec + 0xea, bytes([kind, side]))
            p.uc.mem_write(rec + 0xe8, struct.pack('<H', pop))
            records.append((active, kind, side, pop))
        allied = [rng.random() < 0.3 for _ in range(11)]
        for side, a in enumerate(allied):
            p.uc.mem_write(player + 0xac + side, bytes([int(a)]))
        for active, kind, side, pop in records:
            if active and kind in (1, 2, 3) and side != 10 and not allied[side] and pop > enemy:
                enemy = pop
        put(player + 0x84, DATA)
        p.uc.mem_write(DATA, bytes(100 * 0xc4))
        for slot in range(100):
            _, error = p.call(0x50b340, (player, slot), ecx=DATA + slot * 0xc4)
            if error: raise RuntimeError(error)
        seed = rng.choice([1, 0x7ffffffe, rng.randrange(1, 0x7fffffff)])
        put(0x64186c, seed)
        manager = heap[0]; heap[0] += 0x1b0
        p.uc.mem_write(manager, bytes(0x1b0))
        _, error = p.call(0x40fed0, (player,), ecx=manager)
        if error: raise RuntimeError(error)
        row = [geti(manager + 5)]
        for slot in range(100):
            squad = get(manager + 0x11 + slot * 4)
            if not squad:
                row.append(-1); continue
            data = get(squad + 8)
            if data != DATA + slot * 0xc4 or get(squad + 4) != manager:
                raise AssertionError(('squad wiring', slot))
            row += [KINDS[get(squad)], get(squad + 12)]
            row += [geti(data + 0xc + 4 * k) for k in range(9)]
            row.append(get(data + 8))
        row.append(get(0x64186c))
        expected.append(row)
        rows.append('%d %d' % (seed, enemy))
    out = subprocess.run([args.binary, '--squad-ctor'], input='\n'.join(rows) + '\n',
                         text=True, capture_output=True, check=True).stdout
    actual = [list(map(int, line.split())) for line in out.splitlines()]
    bad = [i for i, (a, b) in enumerate(zip(actual, expected)) if a != b]
    if len(actual) != len(expected):
        raise AssertionError('row count %d vs %d' % (len(actual), len(expected)))
    if bad:
        i = bad[0]
        diff = [(k, a, b) for k, (a, b) in enumerate(zip(actual[i], expected[i])) if a != b][:8]
        raise AssertionError('%d of %d constructions differ; first: case %d (%s), (index, port, retail) %s'
                             % (len(bad), len(rows), i, rows[i], diff))
    print('PASS: %d manager constructions (98 squads, base quotas, ground/air/naval wave rolls, '
          'E from random player tables, final seed), 0 mismatches' % len(rows))


if __name__ == '__main__':
    main()
