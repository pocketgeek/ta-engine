#!/usr/bin/env python3
"""Compare native navigator controller replacement with World mission resets.

Original 4e54e0 and the circle-goal constructor/getter run with independently
supplied route points, positions, footprints, mission flags and activation.
Scheduler cancellation/submission and mission notifications are substituted.
The comparison checks activation and retained/replaced point lists, not search
execution, physical movement or the entire mission dispatcher.
"""
import argparse
import random
import struct
import subprocess

from emuphase import Phase, TYPE


def fixtures():
    for fx, fz in ((1, 1), (2, 2), (2, 6), (6, 2)):
        for boat in (0, 1):
            for flags in (0, 0x400000, 0x10000000, 0x10400000):
                for active in (0, 1):
                    for count in (2, 3, 4, 64):
                        for offset in (0, 159, 160, 161, 256):
                            for fraction in (-1, 0, 1):
                                gx, gz = 426 * 65536 + 16384, 106 * 65536 + 32768
                                ax = ((gx - (fx - 1) * 524288) >> 20) * 16 + fx * 8
                                az = ((gz - (fz - 1) * 524288) >> 20) * 16 + fz * 8
                                route = [(80 + i, 96 + i) for i in range(count)]
                                route[-1] = (ax - offset, az)
                                yield [fx, fz, boat, flags, active, (ax - 320) * 65536 + fraction,
                                       az * 65536, gx, gz, count, *(v for point in route for v in point)]
    rng = random.Random(0x4e54e0)
    for _ in range(512):
        fx, fz = rng.randrange(1, 9), rng.randrange(1, 9)
        count = rng.choice((2, 3, 4, 64))
        # Full signed fixed-point arithmetic also exercises wrapped distance
        # subtraction and the signed doubled-distance comparison.
        raw = [rng.randrange(-2**31, 2**31) for _ in range(4)]
        route = [(rng.randrange(-32768, 32768), rng.randrange(-32768, 32768)) for _ in range(count)]
        yield [fx, fz, rng.randrange(2), rng.choice((0, 0x400000, 0x10000000, 0x10400000)),
               rng.randrange(2), *raw, count, *(v for point in route for v in point)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runner')
    args = parser.parse_args()
    p = Phase(32, 32)
    unit = p.unit(6, 6)
    assert p.construct() is None
    p.plant_request(unit, (6, 6), (26, 6))
    mission = p.HANDLE + 0x1000

    def put(address, value):
        p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))

    def get(address):
        return struct.unpack('<I', p.uc.mem_read(address, 4))[0]

    put(unit + 0x60, mission)
    p.icd.hooks[0x415f30] = lambda uc, a: (1, 0)
    p.icd.hooks[0x4e4f50] = lambda uc, a: (1, 0)
    p.icd.hooks[0x4e2470] = lambda uc, a: (1, 0)
    p.icd.freeze_hooks()
    rows, expected = [], []
    for row in fixtures():
        fx, fz, boat, flags, active, ux, uz, gx, gz, count, *points = row
        p.uc.mem_write(unit + 0x78, struct.pack('<hh', fx, fz))
        put(TYPE + 0x260, 0x80000 if boat else 0)
        put(unit + 0x68, ux); put(unit + 0x70, uz); put(mission + 0x5a, flags)
        _, error = p.icd.call(0x4e2500, args=(mission, gx, gz, 4), ecx=p.HANDLE)
        assert error is None, error
        put(p.NAV + 4, p.HANDLE); put(p.NAV + 0x10c, count)
        p.uc.mem_write(p.NAV + 0x114, bytes([active]))
        p.uc.mem_write(p.NAV + 12, struct.pack('<' + 'h' * len(points), *points))
        _, error = p.icd.call(0x4e54e0, args=(p.HANDLE,), ecx=p.NAV)
        assert error is None, error
        n = get(p.NAV + 0x10c)
        expected.append([p.uc.mem_read(p.NAV + 0x114, 1)[0] & 1, n,
                         *struct.unpack('<' + 'h' * (2*n), p.uc.mem_read(p.NAV + 12, 4*n))])
        rows.append(row)
    run = subprocess.run([args.runner, '--world-reset'],
                         input='\n'.join(' '.join(map(str, row)) for row in rows)+'\n',
                         text=True, capture_output=True, check=True)
    actual = [list(map(int, line.split())) for line in run.stdout.splitlines()]
    assert len(actual) == len(expected), (len(actual), len(expected))
    for index, (row, want, got) in enumerate(zip(rows, expected, actual)):
        assert want == got, {'case': index, 'input': row, 'retail': want, 'world': got}
    print(f'PASS: {len(rows)} World navigator resets match native activation and route points')


if __name__ == '__main__':
    main()
