#!/usr/bin/env python3
"""Check land and boat corner pruning against the original scan.

Navigator callbacks supply three points and record advancement. Original geometry,
angle arithmetic, distance gates and recursive scans execute without substitutions.
"""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-dbg/retail_motion_test')
    args = ap.parse_args()
    p = Icd()
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    game, unit, mover, kind, nav, table, world = [HEAP+i*0x30000 for i in range(7)]
    put = lambda a,v: p.uc.mem_write(a,struct.pack('<I',v & 0xffffffff))
    get = lambda a: struct.unpack('<I',p.uc.mem_read(a,4))[0]
    put(0x62d55c,game); put(0x62d558,world); put(world,world+32)
    put(unit+8,mover); put(unit+0xb4,kind); put(mover,nav); put(nav,table)
    put(table+0xc,HEAP+0x200000); put(table+0x2c,HEAP+0x200010)
    advances = []
    points = []
    def supply(uc,a):
        assert get(a+4) == 3
        route = [points[-1]]*3 if advances else points
        uc.mem_write(get(a),b''.join(struct.pack('<3i',x,0,z) for x,z in route))
        return 2,0
    def advance(uc,a):
        assert get(a) == 1
        advances.append(True)
        return 1,0
    p.hooks[HEAP+0x200000] = supply
    p.hooks[HEAP+0x200010] = advance
    owner,plane=HEAP+0x210000,HEAP+0x220000
    put(unit+0xb8,owner);put(owner+0x8c,128);put(owner+0x90,128)
    put(game+0x19ef4,plane);p.uc.mem_write(plane,b'\xff\xff'*(128*128))
    p.uc.mem_write(unit+0x78,struct.pack('<h',1))
    probe=0;boat=False;speed_mode=0
    def grade(uc,a):
        nonlocal probe
        value=(4 if speed_mode==1 else 3) if boat and speed_mode and probe==0 else 6
        probe+=1
        return 4,value
    p.hooks[0x4db640] = grade
    p.freeze_hooks()
    rng = random.Random(0x4dba80)
    rows, expected = [], []
    for i in range(8192):
        x,z = [rng.randrange(1048*65536,3048*65536) for _ in range(2)]
        boat=i%2!=0;speed_mode=i//2%3
        put(kind+0x260,0x80000 if boat else 0)
        p.uc.mem_write(kind+0x194,struct.pack('<h',13 if boat else 0))
        points = [(x+rng.randrange(-200*65536,200*65536),
                   z+rng.randrange(-200*65536,200*65536)) for _ in range(3)]
        if i%8 == 0: points[2] = points[1]
        if i%8 == 1: points[1] = (x+rng.choice([31,32,143,144])*65536,z)
        if i%8 == 2: points[0] = (points[1][0],points[0][1])
        if i%8 == 3: points[0] = (points[0][0],points[1][1])
        if i%8 == 4: points[0] = points[1]
        heading = rng.randrange(65536)
        rate = rng.choice([0,1,499,500,999,1000,2500,65535])
        put(unit+0x68,x); put(unit+0x70,z)
        p.uc.mem_write(unit+0x7e,struct.pack('<H',heading))
        p.uc.mem_write(kind+0x18e,struct.pack('<H',rate))
        p.uc.mem_write(mover+0x36,b'\x01\x00')
        advances.clear();probe=0
        _,error = p.call(0x4dba80,(unit,),ecx=mover)
        if error: raise RuntimeError(error)
        row=('Q ' if boat else 'q ')+' '.join(map(str,[x,z,*sum((list(q) for q in points),[]),heading,rate]))
        rows.append(row+(f' {speed_mode}' if boat else ''))
        expected.append(int(bool(advances)))
    proc = subprocess.run([args.binary,'--oracle'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual = list(map(int,proc.stdout.split()))
    if actual != expected:
        for row,want,got in zip(rows,expected,actual):
            if want != got: raise AssertionError((row,want,got))
        raise AssertionError('row count')
    print(f'PASS: {len(rows)} land/boat corner decisions, speed modes, distance gates and turn-rate boundaries')


if __name__ == '__main__': main()
