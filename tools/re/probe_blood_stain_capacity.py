#!/usr/bin/env python3
"""Execute retail stain insertion/oldest eviction with only allocation replaced."""
import struct
from emu import Icd, HEAP
p=Icd();system,head,origin,nodes=[HEAP+i*0x40000 for i in range(4)]
def put(a,*values):
    p.uc.mem_write(a,struct.pack('<'+'I'*len(values),*(v&0xffffffff for v in values)))
def get(a,n=1):return struct.unpack('<'+'I'*n,p.uc.mem_read(a,n*4))
allocated=0
def allocate(uc,sp):
    global allocated
    assert get(sp)==(36,)
    address=nodes+allocated*40;allocated+=1
    p.uc.mem_write(address,bytes(36));return 1,address
p.hooks[0x4f3ae0]=allocate;p.freeze_hooks()
checks=0
for capacity in (1,2,3,17,100,2048):
    p.uc.mem_write(system,bytes(44));put(head,head,head)
    put(system+8,head,0,capacity);put(0x6401f0,0)
    allocated=0
    expected=[]
    for event in range(max(250,capacity+7)):
        position=(event*65536+17,30*65536+event,event*-32768)
        color=0xff000000|event
        put(origin,*position)
        _,error=p.call(0x4f2500,(1,origin,color-0x100000000),ecx=system)
        assert not error,error
        expected.append((tuple(v&0xffffffff for v in position),color))
        expected=expected[-capacity:]
        assert get(system+0xc)==(len(expected),)
        node=get(head)[0];previous=head
        for wanted_position,wanted_color in expected:
            assert node!=head and get(node+4)==(previous,)
            assert get(node+8+0x10,3)==wanted_position
            assert get(node+8+0xc)==(wanted_color,)
            previous,node=node,get(node)[0]
        assert node==head and get(head+4)==(previous,)
        checks+=1
    before=bytes(p.uc.mem_read(system,44)),bytes(p.uc.mem_read(head,8)),bytes(p.uc.mem_read(nodes,allocated*40))
    for tick in range(1000):
        result,error=p.call(0x4f34f0,ecx=system)
        assert not error and result&255==1
    after=bytes(p.uc.mem_read(system,44)),bytes(p.uc.mem_read(head,8)),bytes(p.uc.mem_read(nodes,allocated*40))
    assert before==after
print(f'PASS: {checks} native blood-stain insertions preserve position/color and evict oldest first, including capacity 2048; 6000 updates leave retained marks unchanged')
