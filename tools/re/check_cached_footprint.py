#!/usr/bin/env python3
"""Verify cached footprint borders and ordinary-body aging against retail.

The border test substitutes raw rectangle grades. The body test executes the
actual raw rectangle routine on a flat, feature-free one-cell footprint.
"""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP


def compare(runner, mode, fixtures, expected):
    actual=subprocess.run([runner,mode],input='\n'.join(fixtures)+'\n',
                          text=True,capture_output=True,check=True).stdout.splitlines()
    assert actual==expected,next(((i,a,b,fixtures[i]) for i,(a,b) in
        enumerate(zip(actual,expected)) if a!=b),(len(actual),len(expected)))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runner'); args=parser.parse_args()
    p=Icd(); rng=random.Random(0x508cd0)
    grid=HEAP
    fixtures,expected=[],[]
    for _ in range(3000):
        x,z=rng.randrange(-20,100),rng.randrange(-20,100)
        fx,fz=rng.randrange(1,12),rng.randrange(1,12)
        grades=[rng.randrange(8) for _ in range(5)]
        p.uc.mem_write(grid+4,struct.pack('<hh',fx,fz))
        queries=[]
        def rate(uc,a):
            owner,qx,qz,w,h=struct.unpack('<Iiiii',uc.mem_read(a,20))
            assert owner==grid
            value=grades[len(queries)//4]
            queries.extend((qx,qz,w,h))
            return 5,value
        p.hooks[0x5088f0]=rate
        value,error=p.call(0x508cd0,(grid,x,z))
        assert error is None,error
        fixtures.append(' '.join(map(str,(x,z,fx,fz,*grades))))
        expected.append(' '.join(map(str,(value,*queries))))
    compare(args.runner,'--cached-footprint',fixtures,expected)
    print(f'PASS: {len(fixtures)} cached footprint grades and ordered border queries match retail')
    del p.hooks[0x5088f0]
    game,cells,unit,kind,mover=HEAP+0x1000,HEAP+0x30000,HEAP+0x40000,HEAP+0x41000,HEAP+0x42000
    def put(a,v): p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    put(0x62d55c,game)
    for offset,value in ((0x19e98,8),(0x19e9c,8),(0x19f04,cells),
                         (0x14e84,unit-0x138),(0x14e88,unit)):
        put(game+offset,value)
    p.uc.mem_write(game+0x19ef8,b'\x00')
    p.uc.mem_write(grid+8,struct.pack('<hh',100,-100))
    p.uc.mem_write(grid+0x10,b'\xff\xff\xff\xff')
    cell=cells+(2*8+2)*14
    record=bytearray(14)
    struct.pack_into('<H',record,0,1)
    struct.pack_into('<H',record,8,0xffff)
    record[13]=0x80 # road, so no terrain downgrade below 7
    p.uc.mem_write(cell,bytes(record))
    put(unit+0x130,0x1000000); put(unit+0xb4,kind)
    fixtures,expected=[],[]
    for i in range(3000):
        has_mover,requester=i%2,(i//2)%2
        recent,stale=sorted((rng.randrange(2**32),rng.randrange(2**32)),reverse=True)
        stamp=rng.choice((recent,stale,(recent-1)&0xffffffff,(stale-1)&0xffffffff,rng.randrange(2**32)))
        put(unit+8,mover if has_mover else 0);put(mover+0x28,stamp)
        put(grid+0x33c,unit if requester else 0)
        put(grid+0x34c,recent);put(grid+0x350,stale)
        value,error=p.call(0x5088f0,(grid,2,2,1,1))
        assert error is None,error
        fixtures.append(' '.join(map(str,(has_mover,requester,stamp,recent,stale))))
        expected.append(str(value))
    compare(args.runner,'--cached-body',fixtures,expected)
    print(f'PASS: {len(fixtures)} cached ordinary-body grades match retail')


if __name__=='__main__': main()
