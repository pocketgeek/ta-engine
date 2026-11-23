#!/usr/bin/env python3
"""Compare reconstruction (414450) on controlled valid parent chains.

Checks traffic/partial/detour flags and the 64-corner ring, including the start.
World coordinates use the fixture's one-cell footprint on both sides.
"""
import argparse
import struct
import subprocess

from emuphase import Phase, OBJ
from unicorn.x86_const import UC_X86_REG_EIP


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runner')
    args = parser.parse_args()
    fixtures, expected = [], []
    paths = [[(2, 8)], [(x, 8) for x in range(2, 30)], [(x, x) for x in range(2, 14)]]
    for length in (15, 32, 33, 63, 64, 100, 150):
        points = [(2, 8)]
        for x in range(3, length + 3):
            z = points[-1][1]
            points.extend(((x, z), (x, 9 if z == 8 else 8)))
        paths.append(points)
    directions = {(0,-1):0, (-1,-1):1, (-1,0):2, (-1,1):3,
                  (0,1):4, (1,1):5, (1,0):6, (1,-1):7}
    for points in paths:
        for partial in (0, 19):
            for traffic in ((), (1,), (1,2), tuple(range(1, len(points)))):
                width, height = 160, 16
                p = Phase(width, height)
                unit = p.unit(*points[0])
                assert p.construct() is None
                p.plant_request(unit, points[0], points[-1])
                assert p.init()[1] is None
                # A coincident start/goal completes init and releases the grid.
                # This fixture invokes reconstruction directly on a planted chain.
                p.uc.mem_write(OBJ+0x6c, struct.pack('<I', p.GRID))
                p.uc.mem_write(OBJ+0x58, struct.pack('<I', unit))
                plane = [(0,0)] * (width * height)
                for i in range(1, len(points)):
                    x, z = points[i]; px, pz = points[i-1]
                    flags = 8 | (64 if i in traffic else 0)
                    direction = directions[x-px, z-pz]
                    plane[z*width+x] = flags, direction
                    p.uc.mem_write(p.get(0x1c)+(z*width+x)*4, bytes((flags,direction,0,0)))
                p.uc.mem_write(OBJ+0x34, struct.pack('<hh', *points[-1]))
                p.uc.mem_write(OBJ+0x40, struct.pack('<I', partial))
                routes = []
                def receive(uc, args):
                    address, count = struct.unpack('<II', uc.mem_read(args,8))
                    routes.append((count, struct.unpack('<'+'h'*(count*2), uc.mem_read(address,count*4))))
                    return 2,0
                p.icd.hooks[0x4e4ea0] = receive
                _, error = p.icd.call(0x414450, (0,), ecx=OBJ)
                assert error is None and p.uc.reg_read(UC_X86_REG_EIP) == 0x6ffff000, error
                assert len(routes) == 1
                flags = struct.unpack('<I', p.uc.mem_read(unit+0x134,4))[0] & 15
                expected.append(' '.join(map(str, (flags, routes[0][0], *routes[0][1]))))
                fixtures.append(' '.join(map(str, (width,height,*points[0],*points[-1],partial,8))))
                fixtures.extend(f'{flags} {direction}' for flags,direction in plane)
    actual = subprocess.run([args.runner,'--route'], input='\n'.join(fixtures)+'\n',
                            text=True,capture_output=True,check=True).stdout.splitlines()
    assert actual == expected, next(((i,a,b) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),
                                   (len(actual),len(expected)))
    print(f'PASS: {len(expected)} route reconstructions; world waypoints and traffic/partial/detour flags match')


if __name__ == '__main__': main()
