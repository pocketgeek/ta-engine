#!/usr/bin/env python3
"""Observe retail's initial navigator segment for supplied ground-goal handles.

Runs original 4e54e0 and its real goal-coordinate getter. Scheduler cancellation,
request submission and mission notifications are recorded boundary substitutes.
Also runs ordinary move validation/submission, mission construction and the
goal-controller installation chain from supplied fixed-point coordinates.
World comparison covers retained mission coordinates, goal origins and initial
navigator segments;
screen picking, packet decoding, full mission dispatch and physical movement remain
outside this check.
"""
import argparse
import struct
import subprocess

from emuphase import Phase, TYPE


def check(fx, fz, boat, goal, blocked):
    p = Phase(32, 32)
    unit = p.unit(6, 6)
    assert p.construct() is None
    p.plant_request(unit, (6, 6), goal)

    def put(address, value):
        p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))

    p.uc.mem_write(unit + 0x78, struct.pack('<hh', fx, fz))
    put(TYPE + 0x260, 0x80000 if boat else 0)
    p.uc.mem_write(TYPE + 0x192, struct.pack('<hh', 10000 if boat else 20,
                                           13 if boat else -10000))
    mission = p.HANDLE + 0x1000
    put(unit + 0x60, mission)
    start = (6 * 16 + fx * 8, 6 * 16 + fz * 8)
    put(unit + 0x68, start[0] << 16)
    put(unit + 0x70, start[1] << 16)
    if blocked:
        p.uc.mem_write(p.GMAP, b'\x00' * (32 * p.rows8 * 4))
    events = []

    def record(name):
        def hook(uc, args):
            events.append((name, struct.unpack('<I', uc.mem_read(args, 4))[0]))
            return 1, 0
        return hook

    p.icd.hooks[0x415f30] = record('cancel')
    p.icd.hooks[0x4e4f50] = record('request')
    p.icd.hooks[0x4e2470] = record('notify')
    p.icd.freeze_hooks()
    _, error = p.icd.call(0x4e54e0, args=(p.HANDLE,), ecx=p.NAV)
    assert error is None, error
    count = struct.unpack('<I', p.uc.mem_read(p.NAV + 0x10c, 4))[0]
    points = struct.unpack('<4h', p.uc.mem_read(p.NAV + 12, 8))
    expected = (*start, goal[0] * 16 + fx * 8, goal[1] * 16 + fz * 8)
    assert count == 2 and points == expected, (fx, fz, boat, goal, blocked, count, points)
    assert events == [('cancel', p.NAV), ('notify', 0x400), ('request', 1)], events


def check_mission(fx, fz, boat, position, blocked, command=False, reset=False):
    p = Phase(32, 32)
    unit = p.unit(6, 6)
    assert p.construct() is None
    p.plant_request(unit, (6, 6), (16, 16))

    def put(address, value):
        p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))

    p.uc.mem_write(unit + 0x78, struct.pack('<hh', fx, fz))
    put(TYPE + 0x260, 0x80000 if boat else 0)
    p.uc.mem_write(TYPE + 0x192, struct.pack('<hh', 10000 if boat else 20,
                                           13 if boat else -10000))
    start = (6 * 16 + fx * 8, 6 * 16 + fz * 8)
    put(unit + 0x68, start[0] << 16)
    put(unit + 0x70, start[1] << 16)
    if blocked and command:
        for z in range(12, 24):
            for x in range(12, 24):
                p._set_grade(x, z, 0)
    elif blocked:
        p.uc.mem_write(p.GMAP, b'\x00' * (32 * p.rows8 * 4))
    # Definition flags are controlled; no target entity or region is supplied.
    table = p._alloc(25 * 256)
    put(0x62db84, table)
    point = p._alloc(12)
    coordinates = struct.pack('<iii', position[0], 0, position[1])
    p.uc.mem_write(point, coordinates)
    if command:
        # A sorted definition fixture resolves the original Move_Ground name
        # lookup to kind 28. Other definitions are unused, empty-name entries.
        empty = p._alloc(1)
        for i in range(29):
            put(table + 25 * i + 21, empty if i < 28 else 0x615c4c)
        put(0x62db88, table + 29 * 25)
        put(TYPE + 0x264, 0x100)  # movement command enabled
        player = struct.unpack('<I', p.uc.mem_read(unit + 0xb8, 4))[0]
        put(player, 1)
        p.uc.mem_write(player + 0xea, b'\x01')
        result = p._alloc(4)
        _, error = p.icd.call(0x4de530, args=(result, 2, unit, 0, point, 0))
        assert error is None, error
        kind = p.uc.mem_read(result, 1)[0]
        assert kind == 28, kind
        assert bytes(p.uc.mem_read(point, 12)) == coordinates
        _, error = p.icd.call(0x4d78a0, args=(kind, 0, unit, 0, point, 0, 0, 0, 0, 0, 0))
        assert error is None, error
        mission = struct.unpack('<I', p.uc.mem_read(unit + 0x60, 4))[0]
        assert mission
    else:
        mission = p.HANDLE + 0x1000
        _, error = p.icd.call(0x4d6c40,
                         args=(28, 0, point, 0, 0, 0, 0, 0, 0, 0, 0, 0), ecx=mission)
        assert error is None, error
    assert bytes(p.uc.mem_read(mission + 0x22, 12)) == coordinates
    put(mission + 0xe, unit)
    put(unit + 0x60, mission)
    put(p.NAV + 4, 0)
    requests = []
    p.icd.hooks[0x415f30] = lambda uc, args: (1, 0)

    def request(uc, args):
        requests.append(struct.unpack('<I', uc.mem_read(args, 4))[0])
        return 1, 0

    p.icd.hooks[0x4e4f50] = request
    p.icd.freeze_hooks()
    # Real reset -> goal allocation/constructor -> installation -> navigator.
    _, error = p.icd.call(0x4d4da0, args=(mission + 0x22, 0), ecx=mission)
    assert error is None, error
    if reset:
        # The existing route ends elsewhere (for example after partial delivery).
        # The controller still owns the original mission coordinates.
        p.uc.mem_write(p.NAV + 0x10, struct.pack('<hh', 80, 96))
        _, error = p.icd.call(0x4d4da0, args=(mission + 0x22, 0), ecx=mission)
        assert error is None, error
    handle = struct.unpack('<I', p.uc.mem_read(mission + 0x6e, 4))[0]
    origin = tuple((value - foot * (8 << 16) + (8 << 16)) >> 20
                   for value, foot in zip(position, (fx, fz)))
    assert struct.unpack('<hh', p.uc.mem_read(handle + 8, 4)) == origin
    count = struct.unpack('<I', p.uc.mem_read(p.NAV + 0x10c, 4))[0]
    points = struct.unpack('<4h', p.uc.mem_read(p.NAV + 12, 8))
    expected = (*start, origin[0] * 16 + fx * 8, origin[1] * 16 + fz * 8)
    assert count == 2 and points == expected, (fx, fz, boat, position, blocked, count, points)
    assert requests == ([1, 0, 1] if reset else [1]), requests
    assert bytes(p.uc.mem_read(mission + 0x22, 12)) == coordinates
    return [*position, *origin, *(value * 65536 for value in points)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', help='also compare World mission coordinates, goal origins and initial segments')
    args = parser.parse_args()
    cases = 0
    missions = 0
    rows, expected = [], []
    for fx, fz in ((1, 1), (2, 2), (2, 3), (3, 2), (2, 6), (6, 2), (7, 4), (4, 7)):
        for boat in (False, True):
            for goal in ((0, 0), (16, 16), (26, 20), (31, 31)):
                for blocked in (False, True):
                    check(fx, fz, boat, goal, blocked)
                    cases += 1
            # Below/on/above a quantization boundary on each independent axis.
            for dx, dz in ((-1, -1), (0, 0), (1, 1), (-1, 1), (1, -1)):
                position = ((16 * 16 + (fx - 1) * 8) * 65536 + dx,
                            (20 * 16 + (fz - 1) * 8) * 65536 + dz)
                for blocked in (False, True):
                    check_mission(fx, fz, boat, position, blocked)
                    missions += 1
            for dx, dz in ((-4, -4), (0, 0), (4, 4), (-4, 4), (4, -4)):
                # Representable exactly by the World::order float interface.
                position = ((16 * 16 + (fx - 1) * 8) * 65536 + dx,
                            (20 * 16 + (fz - 1) * 8) * 65536 + dz)
                for blocked in (False, True):
                    for reset in (False, True):
                        expected.append(check_mission(fx, fz, boat, position, blocked, command=True, reset=reset))
                        rows.append([fx, fz, int(boat), *position, int(blocked), int(reset)])
    if args.binary:
        run = subprocess.run([args.binary, '--world-destination'],
                             input='\n'.join(' '.join(map(str, row)) for row in rows)+'\n',
                             text=True, capture_output=True, check=True)
        actual = [list(map(int, line.split())) for line in run.stdout.splitlines()]
        assert len(actual) == len(expected), (len(actual), len(expected))
        for row, want, got in zip(rows, expected, actual):
            assert want == got, {'input': row, 'retail': want, 'world': got}
    print(f'PASS: {cases} native destination setups retain supplied goal origins; '
          f'{missions} mission constructors/controller installations retain and '
          f'quantize supplied coordinates; {len(rows)} move-command validation/submissions'
          + (' match World mission coordinates/origins/segments' if args.binary else ' retain supplied coordinates'))


if __name__ == '__main__':
    main()
