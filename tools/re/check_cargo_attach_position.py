#!/usr/bin/env python3
"""Compare World's attached-cargo position commit with retail's 4dad30.

4dad30's attached branch (4dad3e..4dadd2) asks 4dd250 for the host attach
point (host +0x68 plus the 4dd0f0 piece offset), holds a floater (type +0x260
bit 0x80000) at or above (waterline*0xffff + sea) << 16, and stores the point
with 51b3b0. This runs that segment natively for randomized hosts, heights,
floater flags, waterlines, sea levels and attach pieces (-1, as both pickup
handlers pass, and non-negative pieces on a host without a drawable, which
4dd0f0 also maps to a zero offset), and requires the same point from
tak::sim::retailAttachedPosition(retailAttachPoint(...)).
"""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EIP


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-dbg/transport_test')
    ap.add_argument('--cases', type=int, default=20000)
    ap.add_argument('--seed', type=int, default=0x4dd250)
    args = ap.parse_args()
    p = Icd()
    committed = []

    def commit(uc, sp):
        unit, x, y, z, _flags = struct.unpack('<Iiiii', uc.mem_read(sp, 20))
        committed.append((x, y, z))
        return 5, 0
    p.hooks[0x51b3b0] = commit
    p.freeze_hooks()
    # The orientation copy after 4dadd7 reads the host drawable; the position
    # commit is complete there, so the call deliberately stops at that point.
    p.uc.hook_add(UC_HOOK_CODE, lambda uc, *_: uc.emu_stop(), begin=0x4dadd7, end=0x4dadd7)
    game, host, cargo, kind, mover = HEAP, HEAP + 0x20000, HEAP + 0x21000, HEAP + 0x22000, HEAP + 0x23000
    for address in (game, host, cargo, kind, mover):
        p.uc.mem_write(address, bytes(0x1000))
    p.uc.mem_write(0x62d55c, struct.pack('<I', game))
    rng = random.Random(args.seed)
    rows = []
    edges = [0, 1, -1, 0x7fffffff, -0x80000000, 20 << 16, 21 << 16, 19 << 16, -(190 << 16)]
    for index in range(args.cases):
        if index < len(edges) * 4:
            hy = edges[index % len(edges)]
        else:
            hy = rng.randrange(-0x80000000, 0x80000000)
        rows.append((rng.randrange(-0x80000000, 0x80000000), hy,
                     rng.randrange(-0x80000000, 0x80000000), rng.randrange(2),
                     rng.randrange(256), rng.randrange(256),
                     -1 if rng.randrange(4) else rng.randrange(0, 127)))
    expected = []
    for hx, hy, hz, floater, waterline, sea, piece in rows:
        p.uc.mem_write(game + 0x19ef8, bytes((sea,)))
        p.uc.mem_write(host + 0x68, struct.pack('<iii', hx, hy, hz))
        p.uc.mem_write(host + 0xc0, struct.pack('<I', 0))
        p.uc.mem_write(cargo + 0xa8, struct.pack('<I', host))
        p.uc.mem_write(cargo + 0xb4, struct.pack('<I', kind))
        p.uc.mem_write(cargo + 0x112, struct.pack('<b', piece))
        p.uc.mem_write(kind + 0x260, struct.pack('<I', 0x80000 if floater else 0))
        p.uc.mem_write(kind + 0x248, bytes((waterline,)))
        committed.clear()
        _, error = p.call(0x4dad30, (cargo,), ecx=mover, allow_early_stop=True)
        if error:
            raise RuntimeError(error)
        if p.uc.reg_read(UC_X86_REG_EIP) != 0x4dadd7 or len(committed) != 1:
            raise AssertionError(('native position commit did not complete', committed))
        expected.append(committed[0])
    proc = subprocess.run([args.binary, '--attach-position'],
                          input=''.join(' '.join(map(str, row)) + '\n' for row in rows),
                          text=True, capture_output=True, check=True)
    actual = [tuple(map(int, line.split())) for line in proc.stdout.splitlines()]
    if len(actual) != len(expected):
        raise AssertionError(('result count', len(actual), len(expected)))
    for row, want, got in zip(rows, expected, actual):
        if want != got:
            raise AssertionError((row, want, got))
    clamped = sum(1 for row, got in zip(rows, expected) if got[1] != row[1])
    print(f'PASS: {len(rows)} attached-cargo position commits match retail 4dad30 '
          f'({clamped} floater clamps, {sum(row[6] >= 0 for row in rows)} non-negative pieces)')


if __name__ == '__main__':
    main()
