#!/usr/bin/env python3
"""Compare MissionScript::orderPoint with the point retail's order parser
0x513fb0 builds for `m X Y`, `ma X Y`, `p X Y T`, `u X Y` and `a X Y` (and the
mobile `b TYPE N X Y`), and check that 4c8710 sets the scale it multiplies by.

Retail runs natively: the clause cut, the handlers' ftol(X * 65536.0) (5d3d54)
and the multiply by scenario +0xdc5. Hooked: sscanf (5d46f0; the CRT's float
scan needs the CRT initialised, so the hook reads " %f", " %d" and " %[set]" into
the same float/int slots), the mission constructor 4de530 (records its point and
kind), the queue push 4d78a0, and for `b` the type lookup and build-menu test.
The scale: 4c8710 runs on a fake TDF whose GlobalHeader has, or lacks, a
[Map Data] child, and is stopped right after its +0xdc5 store.

    python3 tools/re/check_mission_points.py --binary build-o2/mission_orders_test
"""
import argparse
import random
import re
import struct
import subprocess

from unicorn import UC_HOOK_CODE
from emu import Icd, HEAP

GS, SCEN, UNIT, TYPE, STR, OUT, TBL = (HEAP + n for n in (0, 0x40000, 0x50000, 0x51000, 0x60000, 0x61000, 0x62000))


def cstr(uc, a):
    b = bytes(uc.mem_read(a, 300))
    return b[:b.index(0)].decode('latin1')


def sscanf(uc, a):
    get = lambda x: struct.unpack('<I', uc.mem_read(x, 4))[0]
    src, fmt = cstr(uc, get(a)), cstr(uc, get(a + 4))
    argp, i, n = a + 8, 0, 0
    for conv in re.findall(r'%(\[[^\]]*\]|f|d)', fmt):
        while i < len(src) and src[i].isspace():
            i += 1
        if conv == 'f':
            m = re.match(r'[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?', src[i:])
            if m: uc.mem_write(get(argp), struct.pack('<f', float(m.group())))
        elif conv == 'd':
            m = re.match(r'[-+]?\d+', src[i:])
            if m: uc.mem_write(get(argp), struct.pack('<i', int(m.group())))
        else:
            m = re.match('[' + conv[1:-1] + ']+', src[i:])
            if m: uc.mem_write(get(argp), m.group().encode() + b'\0')
        if not m:
            break
        i += m.end(); argp += 4; n += 1
    return n if n else -1


def retail_point(clause, scale):
    """The (x, z) raw 16.16 point 0x513fb0 hands on for one clause."""
    icd = Icd(); uc = icd.uc
    put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    get = lambda a: struct.unpack('<I', uc.mem_read(a, 4))[0]
    geti = lambda a: struct.unpack('<i', uc.mem_read(a, 4))[0]
    put(0x62d55c, GS); put(GS + 0x175dc, SCEN); put(SCEN + 0xdc5, scale)
    put(UNIT + 0xb4, TYPE); put(UNIT + 8, 1)          # a mover, for b's MobileBuild
    uc.mem_write(STR, clause.encode() + b'\0'); uc.mem_write(OUT, b'\1')
    points = []
    def construct(uc, a):                             # 4de530(out, kind, unit, 0, &point, 0)
        pp = get(a + 16)
        if pp: points.append((geti(pp), geti(pp + 8)))
        return 6, OUT
    def push(uc, a):                                  # 4d78a0: b's point goes straight here
        pp = get(a + 16)
        if clause[0] == 'b' and 0x70000000 <= pp < 0x70100000: points.append((geti(pp), geti(pp + 8)))
        return 11, 0
    icd.hooks.update({0x4de530: construct, 0x4d78a0: push, 0x5d46f0: lambda uc, a: (0, sscanf(uc, a)),
                      0x4d4bf0: lambda uc, a: (1, 0), 0x515ac0: lambda uc, a: (1, 7),
                      0x519850: lambda uc, a: (2, 1)})
    _, error = icd.call(0x513fb0, (UNIT, STR, TBL))
    if error: raise AssertionError(error)
    assert len(points) == 1, (clause, points)
    return points[0]


def retail_scale(map_data):
    icd = Icd(); uc = icd.uc
    put = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    TDF, NODE, KIDS, N1, N2, BUF, S1, S2 = (HEAP + 0x70000 + 0x100 * i for i in range(8))
    put(TDF, NODE); put(TDF + 4, 0)
    uc.mem_write(S1, b'Schema 0\0'); uc.mem_write(S2, b'MAP DATA\0')
    put(N1, S1); put(N2, S2)
    kids = [N1, N2] if map_data else [N1]
    for i, k in enumerate(kids): put(KIDS + 4 * i, k)
    put(NODE + 8, KIDS); put(NODE + 0xc, KIDS + 4 * len(kids))
    put(SCEN + 0xdc5, 0xdeadbeef)
    uc.hook_add(UC_HOOK_CODE, lambda uc, a, s, u: uc.emu_stop(), begin=0x4c874e, end=0x4c874e)
    icd.call(0x4c8710, (0, TDF, BUF), ecx=SCEN, allow_early_stop=True)
    return struct.unpack('<I', uc.mem_read(SCEN + 0xdc5, 4))[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='./build-dbg/mission_orders_test')
    args = parser.parse_args()
    scales = {True: retail_scale(True), False: retail_scale(False)}
    assert scales == {True: 16, False: 1}, scales
    values = ['0', '1', '2', '68', '133', '511', '10.5', '3.25', '7.3', '9.99', '-1', '-2', '-0.3', '-7.7',
              '0.00001', '1e-6', '255.99999', '0.1', '1023.75']
    rng = random.Random(0x513fb0)
    values += ['%.*f' % (rng.choice((0, 1, 2, 4, 6)), rng.uniform(-50, 600)) for _ in range(40)]
    cases = []
    for scale in (16, 1):
        for form in ('m', 'ma', 'p', 'u', 'b', 'a'):
            for i, x in enumerate(values):
                z = values[(i * 7 + 3) % len(values)]
                clause = {'p': f'p {x} {z} 4', 'b': f'b HUT 1 {x} {z}'}.get(form, f'{form} {x} {z}')
                cases.append((clause, scale, x, z, retail_point(clause, scale)))
    feed = ''.join(f'{x} {s}\n{z} {s}\n' for _, s, x, z, _ in cases)
    out = subprocess.run([args.binary, '--points'], input=feed, text=True, capture_output=True,
                         check=True).stdout.split()
    port = [(int(out[2 * i]), int(out[2 * i + 1])) for i in range(len(cases))]
    bad = [(c[0], c[1], c[4], p) for c, p in zip(cases, port) if c[4] != p]
    if bad:
        raise AssertionError({'mismatches': len(bad), 'first': bad[:5]})
    print(f'PASS: +0xdc5 is 16 with [Map Data], 1 without; {len(cases)} m/ma/p/u/b/a points match retail '
          f'(16.16 raw, both scales)')


if __name__ == '__main__':
    main()
