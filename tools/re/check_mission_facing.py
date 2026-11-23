#!/usr/bin/env python3
"""Check that a placed unit's Angle faces the way retail's own units face.

Retail runs natively:
  - 4c8ce1..4c8d64, the .ota [units] parse, for Angle 90, 180 and 270 (the
    word 514d02 later writes over the birth heading +0x7e);
  - 511990 on a type with orientation 0 and buildangle 0 (the rand 535cc0
    hooked to 0): the birth heading +0x7e of a freshly built unit;
  - the ground kernel 4d9ad0 (check_ground_travel.py's set-up) for a unit at
    the origin whose navigator points lie due east (+x) or due west (-x), at
    a turn rate that completes the turn in one call: the heading it settles
    on and the step it takes (+0x7e, mover +8/+0x10).

Then World answers the same: MissionScript::placedHeading through
`mission_orders_test --regions` (angle A), and World::steerGround on the same
kernel rows through `retail_motion_test --ground-travel`. The rule checked:
Angle 180 is the birth heading (0x8000), Angle 270 the heading of a unit
walking east, Angle 90 the heading of one walking west.

    python3 tools/re/check_mission_facing.py --orders build-o2/mission_orders_test \
        --motion build-o2/retail_motion_test
"""
import argparse
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW
import check_mission_regions as regions

PX = 65536


def walk(dx, headings):
    """4d9ad0 toward a point dx px away on x: (row, heading, speed, vx, vz) per start heading."""
    p = Icd(); p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    unit, mover, kind, nav, vt = HEAP, HEAP + 0x1000, HEAP + 0x2000, HEAP + 0x3000, HEAP + 0x4000
    def put(fmt, address, *values): p.uc.mem_write(address, struct.pack('<' + fmt, *values))
    def get(fmt, address): return struct.unpack('<' + fmt, p.uc.mem_read(address, struct.calcsize('<' + fmt)))
    put('I', unit + 8, mover); put('I', unit + 0xb4, kind); put('I', mover, nav); put('I', nav, vt)
    active, query, formation = HEAP + 0x5000, HEAP + 0x5004, HEAP + 0x5008
    put('I', vt + 0x14, active); put('I', vt + 0xc, query); put('I', vt + 0x34, formation)
    p.hooks[active] = lambda uc, a: (0, 1); p.hooks[formation] = lambda uc, a: (0, 0)
    p.hooks[0x56c640] = lambda uc, a: (8, 0)
    points = [(0, 0), (dx * PX, 0), (2 * dx * PX, 0)]
    def supply(uc, a):
        output, count = get('2I', a)
        put('i' * count * 3, output, *(v for x, z in points[:count] for v in (x, 0, z)))
        return 2, output
    p.hooks[query] = supply; p.freeze_hooks()
    speed, maximum, accel, brake, rate, road, water = PX, 2 * PX, PX // 4, PX // 4, 65535, 65536, 65536
    out = []
    for heading in headings:
        put('i', unit + 0x12b, maximum); put('i', kind + 0x162, maximum)
        put('ii', kind + 0x166, brake, accel); put('H', kind + 0x18e, rate)
        put('ii', kind + 0x16e, water, road); put('i', mover + 0x20, speed)
        put('H', mover + 0x36, 1); put('HH', unit + 0x7e, heading, 0)
        _, error = p.call(0x4d9ad0, (unit,), ecx=mover)
        assert not error, error
        vx, vy, vz = get('3i', mover + 8)
        row = ' '.join(map(str, [speed, maximum, accel, brake, rate, road, water, 0, 0, heading, 0, 0] +
                           [v for point in points for v in point]))
        out.append((row, get('H', unit + 0x7e)[0], get('i', mover + 0x20)[0], vx, vz))
    return out


def birth():
    """511990 with orientation 0 and buildangle 0: the new unit's +0x7e."""
    icd, put, get, cstr, st = regions.env()
    regions.settype(put, 7, 1, 1, True)
    put(regions.UNITS, '<H', 7)
    for a, n in ((0x535cc0, 1), (0x51a880, 2), (0x519b10, 1), (0x401000, 1), (0x50b4f0, 2), (0x4c66b0, 1)):
        icd.hooks[a] = (lambda n: lambda uc, ar: (n, 0))(n)
    _, err = icd.call(0x511990, (regions.UNITS, 160 * PX, 0, 160 * PX, 1))
    assert not err, err
    return get(regions.UNITS + 0x7e, '<H')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--orders', default='build-dbg/mission_orders_test')
    ap.add_argument('--motion', default='build-dbg/retail_motion_test')
    args = ap.parse_args()
    word = {a: regions.ota_record(0, 0, a)[3] & 0xffff for a in (90, 180, 270)}
    born = birth()
    starts = [0, 0x2000, 0x4000, 0x7fff, 0x8000, 0x8001, 0xc000, 0xe000, 0xffff]
    east, west = walk(200, starts), walk(-200, starts)
    for name, rows, sign in (('east', east, 1), ('west', west, -1)):
        for row, heading, speed, vx, vz in rows:
            assert speed > 0 and vz == 0 and (vx > 0 if sign > 0 else vx < 0), (name, row, heading, vx, vz)
        assert len({r[1] for r in rows}) == 1, (name, 'heading depends on the start', rows)
    retail = {'born': born, 'east': east[0][1], 'west': west[0][1]}
    # The rule: Angle 180 = birth heading, 270 = walking east, 90 = walking west.
    assert word[180] == retail['born'], ('Angle 180', hex(word[180]), 'birth', hex(born))
    assert word[270] == retail['east'], ('Angle 270', hex(word[270]), 'east', hex(retail['east']))
    assert word[90] == retail['west'], ('Angle 90', hex(word[90]), 'west', hex(retail['west']))

    ours = subprocess.run([args.orders, '--regions'], input='angle 90\nangle 180\nangle 270\n', text=True,
                          capture_output=True, check=True).stdout.split()
    assert list(map(int, ours)) == [word[90], word[180], word[270]], ('placedHeading', ours, word)
    rows = east + west
    got = subprocess.run([args.motion, '--ground-travel'], input=''.join(r[0] + '\n' for r in rows), text=True,
                         capture_output=True, check=True).stdout.splitlines()
    assert len(got) == len(rows), (len(got), len(rows))
    for (row, heading, speed, vx, vz), line in zip(rows, got):
        want = [heading, speed, vx, vz]
        assert list(map(int, line.split())) == want, ('World steerGround', row, 'retail', want, 'World', line)
    print(f'PASS: Angle 180 -> {word[180]:#06x} = birth heading {born:#06x}; Angle 270 -> {word[270]:#06x} = '
          f'walking east {retail["east"]:#06x}; Angle 90 -> {word[90]:#06x} = walking west {retail["west"]:#06x} '
          f'({len(rows)} kernel steps from {len(starts)} start headings; placedHeading and World steerGround agree)')


if __name__ == '__main__':
    main()
