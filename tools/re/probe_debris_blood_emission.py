#!/usr/bin/env python3
"""Execute native blood emission, including trig, construction and list insertion.

Only CRT random draws and list-node allocation are sinks. Synthetic colors and
positions; no game assets are exported. This does not test particle updates.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP

p=Icd()
emitter,head,colors,origin,nodes=[HEAP+i*0x40000 for i in range(5)]
def put(a,*v):
    p.uc.mem_write(a,struct.pack('<'+'I'*len(v),*(x&0xffffffff for x in v)))
def get(a,n=1):
    return struct.unpack('<'+'I'*n,p.uc.mem_read(a,4*n))
rng=random.Random(0x4f2130)
draws=[];allocated=[]
def random_draw(uc,sp):
    value=rng.randrange(32768);draws.append(value);return 0,value
def allocate(uc,sp):
    assert get(sp)==(48,)
    node=nodes+len(allocated)*64;allocated.append(node)
    p.uc.mem_write(node,bytes(48));return 1,node
p.hooks.update({0x5d4444:random_draw,0x4f3a70:allocate})
p.freeze_hooks()
palette=(0xff102030,0xff405060,0xff708090)
put(colors,*palette)
total=0
rows=[];expected=[]
for case in range(256):
    capacity=case%101;existing=case%(capacity+1);requested=case%107
    count=min(requested,capacity-existing)
    position=tuple(rng.randrange(-10000000,10000000) for _ in range(3))
    put(origin,*position)
    p.uc.mem_write(emitter,bytes(44))
    put(emitter+8,head,existing,capacity)
    put(emitter+0x24,colors,3);put(head,head,head)
    draws.clear();allocated.clear()
    _,error=p.call(0x4f2130,(requested,origin),ecx=emitter)
    assert not error,(case,error)
    assert len(allocated)==count and len(draws)==count*4
    assert get(emitter+0xc)==(existing+count,)
    node=get(head)[0]
    for i in range(count):
        assert node==allocated[i]
        particle=node+8
        assert get(particle+8)==(0,)
        assert get(particle+0xc)==(palette[draws[i*4]*3//32768],)
        assert get(particle+0x10,3)==tuple(v&0xffffffff for v in position)
        assert get(particle+0x20)==((draws[i*4+3]*16384//32768)*5+8192,)
        rows.append(' '.join(map(str,(*position,*palette,*draws[i*4:i*4+4]))))
        expected.append((*struct.unpack('<6i',p.uc.mem_read(particle+0x10,24)),get(particle+0xc)[0],4))
        node=get(node)[0]
    assert node==head
    total+=count
print(f'PASS: {total} native blood particles: capacity clamp, four CRT draws each, palette selection, body origin, initial stage and upward velocity; native trig executes')
if len(sys.argv)>1:
    result=subprocess.run([sys.argv[1],'--blood-emit'],input='\n'.join(rows)+'\n',
                          text=True,capture_output=True,check=True)
    actual=[tuple(map(int,line.split())) for line in result.stdout.splitlines()]
    assert actual==expected
    print(f'PASS: {total} compiled blood launches match native positions, all velocity components, colors and random draw counts')
