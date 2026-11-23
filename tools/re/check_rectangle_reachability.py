#!/usr/bin/env python3
"""Compare rectangle connectivity against the local retail executable.

Only allocator/free host calls are substituted; the complete connectivity
routine executes on synthetic packed grade planes, including map boundaries.
"""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-dbg/retail_cost_test')
    ap.add_argument('--cases', type=int, default=20000)
    args = ap.parse_args()
    p = Icd()
    obj, grid, arena = HEAP, HEAP+0x10000, HEAP+0x20000
    brk = arena
    p.hooks[0x4eba00] = lambda uc,a: (0,0)
    def alloc(uc,a):
        nonlocal brk
        n = struct.unpack('<I',uc.mem_read(a,4))[0]
        address = brk
        brk += (n+15)&~15
        if brk > arena+0x100000: raise RuntimeError('allocator arena exhausted')
        return 0,address
    p.hooks[0x4eb9e0] = alloc
    p.freeze_hooks()
    rng = random.Random(315)
    def pair(x,z): return struct.unpack('<i',struct.pack('<HH',x&65535,z&65535))[0]
    rows, expected = [], []
    for case in range(args.cases):
        brk = arena
        w,h = (16,16) if case < args.cases//2 else (rng.randrange(5,33),rng.randrange(5,33))
        cells = [rng.randrange(16) for _ in range(w*h)]
        sw,sh,tw,th = [rng.randrange(1,5) for _ in range(4)]
        mode = rng.randrange(2)
        sx,sz,tx,tz = [rng.randrange(1,15) for _ in range(4)]
        limited = 0
        if case >= args.cases//2:
            sx,tx = [rng.randrange(-3,w+3) for _ in range(2)]
            sz,tz = [rng.randrange(-3,h+3) for _ in range(2)]
            limited = rng.randrange(2)
            if case%3 == 0:
                cells = [rng.choice([6,6,6,8]) for _ in cells]
        packed = [0]*(w*((h+7)//8))
        for z in range(h):
            for x in range(w): packed[(z//8)*w+x] |= cells[z*w+x]<<(4*(z%8))
        p.uc.mem_write(obj+0x340,struct.pack('<III',w,h,grid))
        p.uc.mem_write(grid,struct.pack('<'+'I'*len(packed),*packed))
        result,error = p.call(0x4e1570,(pair(sx,sz),pair(sw,sh),pair(tx,tz),pair(tw,th),mode,limited),ecx=obj)
        if error: raise RuntimeError((case,error))
        rows.append(' '.join(map(str,[w,h,sx,sz,sw,sh,tx,tz,tw,th,mode,limited,*cells])))
        expected.append(result&255)
    proc = subprocess.run([args.binary,'--reachability'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual = list(map(int,proc.stdout.split()))
    if actual != expected:
        for i,(want,got) in enumerate(zip(expected,actual)):
            if want != got: raise AssertionError((i,rows[i],want,got))
        raise AssertionError(('row count',len(expected),len(actual)))
    print(f'PASS: {len(rows)} rectangle connectivity cases, static/dynamic grades, bounded traversal and map edges')


if __name__ == '__main__': main()
