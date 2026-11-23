#!/usr/bin/env python3
"""Compare circle, rectangle and ring controller queries with retail code."""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_EIP


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runner')
    args = parser.parse_args()
    p = Icd()
    rng = random.Random(0x4e3450)
    fixtures = []
    expected = []
    for shape in range(3):
        for _ in range(1000):
            x, z = rng.randrange(-20, 21), rng.randrange(-20, 21)
            a, b = rng.randrange(-10, 11), rng.randrange(-10, 11)
            c, d, e = rng.randrange(0, 400), rng.randrange(0, 500), rng.randrange(0, 900)
            if shape == 1:
                a, b = sorted((a, b))
                c, d = sorted((rng.randrange(-10, 11), rng.randrange(-10, 11)))
                fields = struct.pack('<iiii', a, b, c, d)
                functions = (0x4e39e0, 0x4e3a30)
            elif shape == 0:
                fields = struct.pack('<hhii', a, b, c, d)
                functions = (0x4e27e0, 0x4e2890)
            else:
                fields = struct.pack('<hhiii', a, b, c, d, e)
                functions = (0x4e3450, 0x4e34e0)
            p.uc.mem_write(HEAP+8, fields)
            results = []
            for fn in functions:
                value, error = p.call(fn, (x, z), ecx=HEAP)
                assert error is None and p.uc.reg_read(UC_X86_REG_EIP) == 0x6ffff000, error
                results.append(value)
            fixtures.append(' '.join(map(str, (shape, a, b, c, d, e, x, z))))
            # Real vector code, with enough reserved space to avoid allocator
            # substitution. Ring enumeration also follows a mission to its unit.
            vec, storage, mission, unit = HEAP+0x100, HEAP+0x1000, HEAP+0x200, HEAP+0x400
            p.uc.mem_write(vec, struct.pack('<IIII', 0, storage, storage, storage+4096))
            p.uc.mem_write(HEAP+4, struct.pack('<I', mission))
            p.uc.mem_write(mission+0xe, struct.pack('<I', unit))
            p.uc.mem_write(unit+0x78, struct.pack('<hh', rng.randrange(1, 9), rng.randrange(1, 9)))
            _, error = p.call((0x4e2670, 0x4e36b0, 0x4e2a70)[shape], (vec,), ecx=HEAP)
            assert error is None and p.uc.reg_read(UC_X86_REG_EIP) == 0x6ffff000, error
            end = struct.unpack('<I', p.uc.mem_read(vec+8, 4))[0]
            assert storage <= end <= storage+4096
            points = struct.unpack('<'+'h'*((end-storage)//2), p.uc.mem_read(storage, end-storage))
            expected.append(' '.join(map(str, (results[0] & 255, results[1], *points))))
    actual = subprocess.run([args.runner, '--goal'], input='\n'.join(fixtures)+'\n',
                            text=True, capture_output=True, check=True).stdout.splitlines()
    assert actual == expected, next(((i, a, b, fixtures[i]) for i, (a, b)
                                    in enumerate(zip(actual, expected)) if a != b),
                                   (len(actual), len(expected)))
    print(f'PASS: {len(expected)} controller acceptance, distance and ordered goal lists match retail')

    radii = [-2147483648, -65536, -30001, -1, 0, 4, 11, 12, 16, 68, 30000, 30001, 2147483647]
    radii += [rng.randrange(-2147483648, 2147483648) for _ in range(1000)]
    mission, unit = HEAP+0x200, HEAP+0x400
    p.uc.mem_write(mission+0xe, struct.pack('<I', unit))
    p.uc.mem_write(unit+0x78, struct.pack('<hh', 2, 2))
    expected = []
    for radius in radii:
        _, error = p.call(0x4e2500, (mission, 0, 0, radius), ecx=HEAP)
        assert error is None and p.uc.reg_read(UC_X86_REG_EIP) == 0x6ffff000, error
        expected.append(str(struct.unpack('<i', p.uc.mem_read(HEAP+0x10, 4))[0]))
    actual = subprocess.run([args.runner, '--circle-radius'], input='\n'.join(map(str, radii))+'\n',
                            text=True, capture_output=True, check=True).stdout.splitlines()
    assert actual == expected
    print(f'PASS: {len(radii)} circle constructor radii match retail, including signed overflow')

    fixtures, expected = [], []
    for _ in range(2000):
        a,b = sorted(rng.sample(range(-100,101),2))
        c,d = sorted(rng.sample(range(-100,101),2))
        x,z = rng.randrange(-110,111),rng.randrange(-110,111)
        fx,fz = rng.randrange(1,9),rng.randrange(1,9)
        p.uc.mem_write(HEAP+8,struct.pack('<iiii',a,b,c,d))
        p.uc.mem_write(HEAP+4,struct.pack('<I',mission))
        p.uc.mem_write(unit+0x74,struct.pack('<hhhh',x,z,fx,fz))
        _,error=p.call(0x4e38e0,(HEAP+0x800,),ecx=HEAP)
        assert error is None,error
        px,_,pz=struct.unpack('<iii',p.uc.mem_read(HEAP+0x800,12))
        fixtures.append(' '.join(map(str,(a,b,c,d,x,z,fx,fz))))
        expected.append(f'{px} {pz}')
    actual=subprocess.run([args.runner,'--rect-point'],input='\n'.join(fixtures)+'\n',
                          text=True,capture_output=True,check=True).stdout.splitlines()
    assert actual==expected,next(((i,a,b,fixtures[i]) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),None)
    print(f'PASS: {len(fixtures)} rectangle navigation points match retail')

    fixtures,expected=[],[]
    for case in range(4096):
        cx,cz=rng.randrange(-100,101),rng.randrange(-100,101)
        fx,fz=rng.randrange(1,9),rng.randrange(1,9)
        x,z=(cx*16+fx*8)*65536,(cz*16+fz*8)*65536
        if case%8:x+=rng.randrange(-100000000,100000000);z+=rng.randrange(-100000000,100000000)
        inner=rng.randrange(1000);outer=inner+rng.randrange(1000)
        p.uc.mem_write(HEAP+8,struct.pack('<hhiii',cx,cz,inner,outer,0))
        p.uc.mem_write(unit+0x68,struct.pack('<iii',x,0,z))
        p.uc.mem_write(unit+0x78,struct.pack('<hh',fx,fz))
        _,error=p.call(0x4e33b0,(HEAP+0x800,),ecx=HEAP)
        assert error is None,error
        px,_,pz=struct.unpack('<iii',p.uc.mem_read(HEAP+0x800,12))
        fixtures.append(' '.join(map(str,(cx,cz,inner,outer,x,z,fx,fz))))
        expected.append(f'{px} {pz}')
    actual=subprocess.run([args.runner,'--ring-point'],input='\n'.join(fixtures)+'\n',
                          text=True,capture_output=True,check=True).stdout.splitlines()
    assert actual==expected,next(((i,a,b,fixtures[i]) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),None)
    print(f'PASS: {len(fixtures)} ring navigation points match retail')


if __name__ == '__main__':
    main()
