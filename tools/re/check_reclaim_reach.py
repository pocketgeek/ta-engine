#!/usr/bin/env python3
"""Compare RECLAIM's reach and countdown with the original mission handler.

Reach: the real 0x406250 stage 0 resolves a random clicked cell of the target
(0x4930d0 over the real cell plane 0x50e600/0x50e660, tails walked back to the
anchor), tests it (0x496ea0) and installs the controller through the real
0x4d4e80 -> 0x4e3590. The controller's own acceptance (vt+0x10: 0x4e24a0 ->
0x4e39e0) and steering cell (vt+0x20: 0x4e38e0) are then queried for every
footprint-origin cell in a window three cells past the ring, with
builddistance varied (retail never reads it). The rows go to
`reclaimarea_test --reclaim-rect`, our retailReclaimRectangle +
RetailRectGoal.

Countdown: stage 1 (0x40638e) on the arrival event sets mission+0x4e; the
work stage 0x406529 is then woken until it returns 1, and once more as stage
4. The rows go to `reclaimarea_test --reclaim-countdown`, our
retailReclaimCountdown + retailReclaimWork.

Substituted host boundaries only: 0x4eb9e0 allocator, 0x56c5c0 script notify,
0x4d4d40 controller installer (captures the object), the stage-1 stance and
aim helpers 0x519b70/0x535cc0/0x511170/0x51b340/0x4d4ab0, 0x4eec20 (worker
particle), 0x4f5db0 (chatter), 0x51e4d0 (unit notify).
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build-o2/reclaimarea_test')
    args = parser.parse_args()
    p = Icd()
    G = HEAP; PLANE = HEAP+0x20000; DEFS = HEAP+0x30000
    UNIT = HEAP+0x40000; NAV = HEAP+0x41000; TYPE = HEAP+0x42000; MIS = HEAP+0x43000
    OBJS = HEAP+0x44000; OUT = HEAP+0x46000; ARENA = HEAP+0x100000
    W = H = 64

    def put(a, v): p.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    def get(a, fmt='<I'): return struct.unpack(fmt, p.uc.mem_read(a, struct.calcsize(fmt)))[0]
    def w16(a, v): p.uc.mem_write(a, struct.pack('<H', v & 0xffff))
    def w8(a, v): p.uc.mem_write(a, bytes([v & 0xff]))

    put(0x62d55c, G)
    put(G+0x19e98, W); put(G+0x19e9c, H); put(G+0x19f04, PLANE)
    put(G+0x19edc, DEFS); put(G+0x19ec0, 8); put(G+0x19e74, OBJS); put(G+0x19f44, 1000)
    brk = [ARENA]
    def alloc(uc, sp):
        n = get(sp); a = brk[0]; brk[0] += (n+15) & ~15
        p.uc.mem_write(a, b'\0'*n); return 0, a
    p.hooks[0x4eb9e0] = alloc
    installed = []
    def install(uc, sp):
        installed.append(get(sp)); return 1, 0
    p.hooks[0x4d4d40] = install
    p.hooks[0x56c5c0] = lambda uc, sp: (3, 0)
    p.hooks[0x4f5db0] = lambda uc, sp: (2, 0)
    p.hooks[0x51e4d0] = lambda uc, sp: (2, 0)
    p.hooks[0x519b70] = lambda uc, sp: (1, 0)
    p.hooks[0x535cc0] = lambda uc, sp: (1, 0)
    p.hooks[0x511170] = lambda uc, sp: (1, 0)
    p.hooks[0x51b340] = lambda uc, sp: (2, 0)
    p.hooks[0x4d4ab0] = lambda uc, sp: (3, 0)
    p.hooks[0x4eec20] = lambda uc, sp: (2, 0)
    p.freeze_hooks()

    def clear_plane():
        for i in range(W*H):
            c = PLANE+i*14
            p.uc.mem_write(c, b'\0'*14); w16(c+8, 0xffff); w8(c+7, 0xa)

    def place(ax, az, fw, fh, energy):
        d = DEFS+0x140
        p.uc.mem_write(d, b'\0'*0x140)
        w16(d+0xb0, fw); w16(d+0xb2, fh)
        p.uc.mem_write(d+0x128, struct.pack('<f', energy))
        w8(d+0x13c, 0x40)                       # reclaimable
        for dz in range(fh):
            for dx in range(fw):
                c = PLANE+((az+dz)*W+ax+dx)*14
                if dx == 0 and dz == 0:
                    w16(c+8, 1); w8(c+0xa, 0); w8(c+0xb, 0)
                else:
                    w16(c+8, 0xfffe); w8(c+0xa, dz); w8(c+0xb, dx)

    def unit(bw, bh, ox, oz, reach):
        p.uc.mem_write(UNIT, b'\0'*0x100); p.uc.mem_write(TYPE, b'\0'*0x400)
        put(UNIT+8, NAV); put(UNIT+0xb4, TYPE); put(UNIT+0xbc, HEAP+0x45000); put(UNIT+0xc0, HEAP+0x45100)
        w16(NAV+0x36, 0)
        put(TYPE+0x264, 0x800); put(TYPE+0x260, 0)
        w16(TYPE+0x230, reach); w16(TYPE+0x190, 0)
        w16(UNIT+0x74, ox); w16(UNIT+0x76, oz); w16(UNIT+0x78, bw); w16(UNIT+0x7a, bh)
        put(UNIT+0x68, (ox*16+bw*8) << 16); put(UNIT+0x70, (oz*16+bh*8) << 16)

    def mission(tx, tz, stage=0):
        p.uc.mem_write(MIS, b'\0'*0x80)
        put(MIS+0xe, UNIT); put(MIS+0x22, tx); put(MIS+0x26, 0); put(MIS+0x2a, tz)
        w8(MIS+5, stage)

    def run(events):
        v, err = p.call(0x406250, (UNIT, MIS, events))
        assert err is None, err
        return v

    rng = random.Random(0x406250)
    rows, expected = [], []
    for case in range(400):
        bw, bh = rng.randrange(1, 5), rng.randrange(1, 5)
        fw, fh = rng.randrange(1, 4), rng.randrange(1, 4)
        ax, az = rng.randrange(8, 40), rng.randrange(8, 40)
        clear_plane(); place(ax, az, fw, fh, 250.0)
        tx = ((ax+rng.randrange(fw))*16+rng.randrange(16)) << 16 | rng.randrange(65536)
        tz = ((az+rng.randrange(fh))*16+rng.randrange(16)) << 16 | rng.randrange(65536)
        unit(bw, bh, 0, 0, rng.choice([0, 16, 64, 500])); mission(tx, tz)
        installed.clear(); brk[0] = ARENA
        assert run(0) == 1 and get(MIS+6) == 0x700 and len(installed) == 1
        ctrl = installed[0]; vt = get(ctrl)
        assert vt == 0x5f2940
        for _ in range(12):
            ox = rng.randrange(ax-bw-3, ax+fw+4); oz = rng.randrange(az-bh-3, az+fh+4)
            w16(UNIT+0x74, ox); w16(UNIT+0x76, oz)
            w16(TYPE+0x230, rng.choice([0, 32, 128, 500]))
            got, err = p.call(get(vt+0x10), (UNIT,), ecx=ctrl); assert err is None
            _, err = p.call(get(vt+0x20), (OUT,), ecx=ctrl); assert err is None
            sx, sz = get(OUT, '<i'), get(OUT+8, '<i')
            assert sx % 65536 == 0 and sz % 65536 == 0
            cx, cz = ((sx >> 16)-bw*8)//16, ((sz >> 16)-bh*8)//16
            rows.append(f'{ax} {az} {fw} {fh} {bw} {bh} {ox} {oz}')
            expected.append(f'{int(bool(got & 0xff))} {cx} {cz}')
    result = subprocess.run([args.binary, '--reclaim-rect'], input='\n'.join(rows)+'\n',
                            text=True, capture_output=True, check=True)
    actual = result.stdout.split('\n')[:len(rows)]
    for row, wanted, got in zip(rows, expected, actual):
        assert wanted == got, (row, wanted, got)
    accepted = sum(e[0] == '1' for e in expected)
    assert 0 < accepted < len(expected)
    print(f'PASS: {len(rows)} original reclaim acceptance/steering queries over 400 controllers '
          f'({accepted} accepted), random builddistance and clicked cell')

    rows, expected = [], []
    energies = [0.0, 1.0, 2.0, 37.5, 75.0, 250.0, 525.0, 0.49, 1e-7, 3.75, 99.99]
    energies += [rng.choice([rng.uniform(0, 600), float(rng.randrange(700))]) for _ in range(40)]
    for energy in energies:
        energy = struct.unpack('<f', struct.pack('<f', energy))[0]
        clear_plane(); place(24, 26, 2, 2, energy)
        unit(2, 2, 22, 26, 0); mission((24*16+8) << 16, (26*16+8) << 16, 1)
        assert run(0x100) == 1
        countdown = get(MIS+0x4e, '<i')
        wakes = 0
        while True:
            w8(MIS+5, 3)
            v = run(1); wakes += 1
            if v != 2: break
            assert get(MIS+6) == 3
        assert v == 1
        w8(MIS+5, 4); assert run(1) == 1
        bits = struct.unpack('<I', struct.pack('<f', energy))[0]
        rows.append(f'{bits}')
        expected.append(f'{countdown} {wakes}')
    result = subprocess.run([args.binary, '--reclaim-countdown'], input='\n'.join(rows)+'\n',
                            text=True, capture_output=True, check=True)
    actual = result.stdout.split('\n')[:len(rows)]
    for row, wanted, got in zip(rows, expected, actual):
        assert wanted == got, (row, wanted, got)
    print(f'PASS: {len(rows)} original reclaim countdowns and work-stage wake counts')


if __name__ == '__main__':
    main()
