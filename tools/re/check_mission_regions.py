#!/usr/bin/env python3
"""Compare where a campaign mission puts its units, and which cells its
trigger regions hold, with retail.

Retail runs natively:
  - 4c8ce1..4c8d64, the .ota [units] parse: XPos/ZPos/Angle into a record
    (the TDF number lookup 543190 is hooked to hand back the values);
  - 514b40 on that record, through 5085c0's snap, up to its 512060 call
    (type lookup 5159f0 and 512060 itself hooked; 512060's args recorded);
  - 511990 on that position: the footprint-origin cell it stores (+0x74/+0x76);
  - 4d35c0's Create (the same 512060 hook) and SetTrigger / RemoveTrigger;
  - 4d33a0, the per-unit check, on units at given origin cells (the script
    thread start 56c640 hooked to record UnitCreated / TriggerHit calls).
The terrain sampler 507990 is hooked. MissionScript::placedPoint,
placedHeading, createPoint, footprintOrigin and retailTriggerRect/Circle answer
the same questions through `mission_orders_test --regions`.

    python3 tools/re/check_mission_regions.py --binary build-o2/mission_orders_test
"""
import argparse
import struct
import subprocess

from unicorn.x86_const import UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_ESI, UC_X86_REG_EBX
from emu import Icd, HEAP, STACK

GS = HEAP; SCEN = HEAP + 0x40000; TYPES = HEAP + 0x50000; STR = HEAP + 0x60000
REC = HEAP + 0x61000; UNITS = HEAP + 0x80000; BUMP = HEAP + 0x200000; SCRIPT = HEAP + 0x70000
UNIT_SIZE = 0x138


def env():
    icd = Icd(); uc = icd.uc
    st = {'bump': BUMP, 'calls': []}
    put = lambda a, fmt, *v: uc.mem_write(a, struct.pack(fmt, *v))
    get = lambda a, fmt='<I': struct.unpack(fmt, uc.mem_read(a, struct.calcsize(fmt)))[0]
    def cstr(a):
        b = bytes(uc.mem_read(a, 256)); return b[:b.index(0)].decode('latin1')
    put(0x62d55c, '<I', GS); put(GS + 0x175dc, '<I', SCEN); put(GS + 0x175c4, '<I', TYPES)
    def malloc(uc, a):
        n = get(a); p = st['bump']; st['bump'] += (n + 15) & ~15; uc.mem_write(p, b'\0' * n); return 0, p
    def thread(uc, a):
        v = [struct.unpack('<i', uc.mem_read(a + 4 * i, 4))[0] for i in range(8)]
        st['calls'].append((cstr(v[0]),) + tuple(v[4:4 + v[3]])); return 8, 0
    icd.hooks.update({
        0x4eb9e0: malloc, 0x4eba00: lambda uc, a: (0, 0), 0x56c640: thread,
        0x5d6267: lambda uc, a: (0, 1 if chr(get(a) & 0xff).isspace() else 0),
        0x5d5bb0: lambda uc, a: (0, 0 if cstr(get(a)).lower() == cstr(get(a + 4)).lower() else 1),
        0x507990: lambda uc, a: (2, 0)})
    put(GS + 0x306e, '<B', 0)
    return icd, put, get, cstr, st


def settype(put, tid, fx, fz, mobile):
    t = TYPES + tid * 0x2a4
    put(t + 0x126, '<hh', fx, fz); put(t + 0x24a, '<B', 1 if mobile else 0); put(t + 0x80, '<H', tid)
    put(t + 0x1be, '<I', 100); put(t + 0x22c, '<hh', 0, 0)
    return t


def ota_record(x, z, angle):
    """4c8ce1..4c8d64: the record's x, z (16.16) and angle word."""
    icd, put, get, cstr, st = env(); uc = icd.uc
    put(SCEN + 0xdc5, '<I', 16)
    vals = {'xpos': x, 'ypos': 0, 'zpos': z, 'angle': angle}
    icd.hooks[0x543190] = lambda uc, a: (2, vals.get(cstr(get(a)).lower(), get(a + 4)))
    frame = STACK + 0x80000
    put(frame - 0xc, '<I', SCEN)
    uc.reg_write(UC_X86_REG_EBP, frame); uc.reg_write(UC_X86_REG_ESP, frame - 0x400)
    uc.reg_write(UC_X86_REG_ESI, REC); uc.reg_write(UC_X86_REG_EBX, 0)
    uc.emu_start(0x4c8ce1, 0x4c8d68)
    return struct.unpack('<iiih', uc.mem_read(REC + 0xc, 14))


def place(rec, fx, fz, mobile):
    """514b40 on one record: the position it hands 512060."""
    icd, put, get, cstr, st = env(); uc = icd.uc
    t = settype(put, 7, fx, fz, mobile)
    put(SCEN + 0xdd5, '<I', 1); put(SCEN + 0xdd1, '<I', REC)
    uc.mem_write(STR, b'TESTUNIT\0')
    put(REC, '<III', STR, 0, 0); put(REC + 0xc, '<iiih', *rec); put(REC + 0x26, '<B', 1)
    row = GS + 0x2404
    put(row, '<I', 1); put(row + 0xea, '<BB', 1, 0)
    icd.hooks.update({0x44b6d0: lambda uc, a: (3, 0), 0x5159f0: lambda uc, a: (1, t),
                      0x401120: lambda uc, a: (1, 0)})
    def create(uc, a):
        st['create'] = [struct.unpack('<i', uc.mem_read(a + 4 * i, 4))[0] for i in range(10)]
        put(UNITS + 0xb4, '<I', t); return 10, UNITS
    icd.hooks[0x512060] = create
    _, err = icd.call(0x514b40, ())
    assert not err, err
    c = st['create']
    assert c[5:] == [1, 1, 0, 0, 1], c
    return c[2], c[4], get(UNITS + 0x7e, '<H')


def origin(x, z, fx, fz, mobile):
    """511990: the footprint-origin cell a new unit at x,z stores."""
    icd, put, get, cstr, st = env(); uc = icd.uc
    settype(put, 7, fx, fz, mobile)
    put(UNITS, '<H', 7)
    for a, n in ((0x535cc0, 1), (0x51a880, 2), (0x519b10, 1), (0x401000, 1), (0x50b4f0, 2), (0x4c66b0, 1)):
        icd.hooks[a] = (lambda n: lambda uc, ar: (n, 0))(n)
    _, err = icd.call(0x511990, (UNITS, x, 0, z, 1))
    assert not err, err
    return struct.unpack('<hh', uc.mem_read(UNITS + 0x74, 4))


def command(e, text, args):
    icd, put, get, cstr, st = e
    icd.uc.mem_write(STR, text.encode() + b'\0')
    for i, v in enumerate(args): put(STR + 0x100 + 4 * i, '<i', v)
    r, err = icd.call(0x4d35c0, (STR, len(args), STR + 0x100), ecx=SCRIPT)
    assert not err, err
    return r


def create(args, fx, fz, mobile):
    e = env(); icd, put, get, cstr, st = e
    t = settype(put, 9, fx, fz, mobile)
    icd.hooks[0x5159f0] = lambda uc, a: (1, t)
    def made(uc, a):
        st['create'] = [struct.unpack('<i', uc.mem_read(a + 4 * i, 4))[0] for i in range(10)]
        put(UNITS + 2, '<H', 1234); return 10, UNITS
    icd.hooks[0x512060] = made
    r = command(e, 'Create TESTUNIT', args)
    return r, st.get('create')


class Triggers:
    """One script state with regions armed; 4d33a0 on units at given cells."""
    def __init__(self):
        self.e = env(); icd, put, get, cstr, st = self.e
        put(GS + 0x14e84, '<II', UNITS, UNITS + 40 * UNIT_SIZE)

    def unit(self, i, ox, oz, flags=0x1000000, built=0.0, owner=0):
        icd, put, get, cstr, st = self.e
        u = UNITS + i * UNIT_SIZE
        put(u + 2, '<H', 100 + i); put(u + 0x74, '<hh', ox, oz); put(u + 0x130, '<I', flags)
        put(u + 0x108, '<f', built); put(u + 0xd4, '<I', 0); put(u + 0xfd, '<B', owner)
        return u

    def check(self, u):
        icd, put, get, cstr, st = self.e
        st['calls'].clear()
        _, err = icd.call(0x4d33a0, (u,), ecx=SCRIPT)
        assert not err, err
        return list(st['calls'])

    def hits(self, ox, oz):
        return {c[1] for c in self.check(self.unit(0, ox, oz)) if c[0] == 'TriggerHit'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='./build-dbg/mission_orders_test')
    args = parser.parse_args()
    queries, expect = [], []
    def ask(q, retail):
        queries.append(q); expect.append(retail)

    # [units]: parse, then 514b40's snap, then the origin cell 511990 stores.
    sizes = ((1, 1), (2, 2), (3, 3), (4, 4), (5, 5), (2, 3), (1, 4), (6, 18), (8, 20), (14, 4), (7, 12))
    for a in (0, 1, 90, 180, 270, 359, 360, -90, -1, 45, 721):
        ask(f'angle {a}', str(ota_record(0, 0, a)[3] & 0xffff))
    for mobile in (True, False):
        for fx, fz in sizes:
            for x, z in ((10, 20), (0, 0), (1, 7), (231, 222), (176, 243), (2047, 3)):
                rec = ota_record(x, z, 180)
                px, pz, _ = place(rec, fx, fz, mobile)
                ask(f'place {fx} {fz} {1 if mobile else 0} {x} {z}', f'{px} {pz}')
                ox, oz = origin(px, pz, fx, fz, mobile)
                ask(f'origin {px} {fx}', str(ox)); ask(f'origin {pz} {fz}', str(oz))
    # Create: the footprint corner at the cell, for both kinds.
    for mobile in (True, False):
        for fx, fz in sizes:
            for a in ((0, 10, 20), (2, 0, 0), (3, -1, 5), (1, 370, 225)):
                r, c = create(a, fx, fz, mobile)
                assert r == 1234 and c[0] & 0xff == a[0] and c[5:] == [1, 1, 0, 0, 1], (a, r, c)
                ask(f'create {fx} {fz} {a[1]} {a[2]}', f'{c[2]} {c[4]}')
    for a in ((), (1,), (1, 2, 3, 4), (1, 2, 3, 4, 5)):
        r, c = create(a, 1, 1, True)
        assert r == 0 and c is None, ('Create with', len(a), 'args made a unit')
    # Regions: rectangles (one inverted) and circles, every cell around them.
    t = Triggers()
    rects = {0: (10, 20, 12, 22), 1: (0, 0, 0, 0), 2: (5, 5, 3, 3), 3: (-3, -2, 4, 1), 4: (100, 7, 140, 9)}
    circles = {5: (30, 30, 4), 6: (0, 0, 0), 7: (10, 10, 7), 8: (50, 50, 12), 9: (-20, 60, 3)}
    for rid, r in rects.items(): assert command(t.e, 'SetTrigger', (rid,) + r) == rid
    for rid, c in circles.items(): assert command(t.e, 'SetTrigger', (rid,) + c) == rid
    cells = set()
    for x1, z1, x2, z2 in rects.values():
        for x in range(min(x1, x2) - 2, max(x1, x2) + 3):
            for z in range(min(z1, z2) - 2, max(z1, z2) + 3): cells.add((x, z))
    for cx, cz, r in circles.values():
        for x in range(cx - r - 2, cx + r + 3):
            for z in range(cz - r - 2, cz + r + 3): cells.add((x, z))
    for ox, oz in sorted(cells):
        got = t.hits(ox, oz)
        for rid, (x1, z1, x2, z2) in rects.items():
            ask(f'rect {x1} {z1} {x2} {z2} {ox} {oz}', '1' if rid in got else '0')
        for rid, (cx, cz, r) in circles.items():
            ask(f'circle {cx} {cz} {r} {ox} {oz}', '1' if rid in got else '0')

    # The latch and the slot rules, retail only (mission_orders_test checks ours).
    t = Triggers()
    assert command(t.e, 'SetTrigger', (3, 10, 20, 12, 22)) == 3
    u = t.unit(1, 11, 21, owner=2)
    assert t.check(u) == [('UnitCreated', 101, 2), ('TriggerHit', 3, 101, 2)], 'first check'
    assert t.check(u) == [], 'latched'
    assert command(t.e, 'SetTrigger', (3, 0, 0, 100, 100)) == 0, 'armed slot re-armed'
    command(t.e, 'RemoveTrigger', (3,))
    assert t.e[2](u + 0xd4) == 0x80000000, 'RemoveTrigger clears the region bit only'
    command(t.e, 'SetTrigger', (3, 10, 20, 12, 22))
    assert t.check(u) == [('TriggerHit', 3, 101, 2)], 'fires again after RemoveTrigger'
    for name, flags, built in (('dying', 0x1001000, 0.0), ('unfinished', 0x1000000, 0.5), ('dead', 0, 0.0)):
        assert t.check(t.unit(2, 11, 21, flags, built)) == [], name

    out = subprocess.run([args.binary, '--regions'], input='\n'.join(queries) + '\n', text=True,
                         capture_output=True, check=True).stdout.splitlines()
    assert len(out) == len(queries), (len(out), len(queries))
    bad = [(q, e, p) for q, e, p in zip(queries, expect, out) if e != p]
    if bad:
        raise AssertionError({'mismatches': len(bad), 'first': bad[:8]})
    print(f'PASS: {len(queries)} placement, Create, origin-cell and region answers match retail; '
          'latch, re-arm, RemoveTrigger and skipped units as retail')


if __name__ == '__main__':
    main()
