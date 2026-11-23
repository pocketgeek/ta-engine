#!/usr/bin/env python3
"""Execute native transient-effect creation/update with allocator-only sinks."""
import random
import struct
from emu import Icd, HEAP
p=Icd()
head,node,point,art=[HEAP+i*0x1000 for i in range(4)]
def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
def read(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
freed=[]
def allocate(uc,sp):
    assert read(sp)==32
    return 0,node

def free(uc,sp):
    assert read(sp)==node
    freed.append(node)
    return 0,0
p.hooks.update({0x4eb9e0:allocate,0x4eba00:free})
p.freeze_hooks()
rng=random.Random(0x421ca0)
for case in range(256):
    durations=[rng.randrange(0,16) for _ in range(rng.randrange(1,17))]
    data=bytearray(0x30+len(durations)*8)
    struct.pack_into('<HB',data,0,len(durations),0)
    for i,d in enumerate(durations):struct.pack_into('<H',data,0x2c+i*8,d)
    p.uc.mem_write(art,bytes(data));put(head,head);put(head+4,head)
    put(0x62a38c,head);put(0x62a390,0);freed.clear()
    position=[rng.randrange(-0x80000000,0x80000000) for _ in range(3)]
    p.uc.mem_write(point,struct.pack('<3i',*position))
    result,error=p.call(0x421e10,(point,art));assert not error,error
    assert read(0x62a390)==1 and read(head)==node
    assert bytes(p.uc.mem_read(node+8,12))==struct.pack('<3i',*position)
    p.uc.mem_write(point,bytes(12))  # Source movement cannot relocate the effect.
    remaining=[max(1,d) for d in durations]
    total=sum(remaining)
    for age in range(total+1):
        if age==total:
            assert freed==[node] and read(0x62a390)==0 and read(head)==head
            break
        elapsed=age;frame=0
        assert bytes(p.uc.mem_read(node+8,12))==struct.pack('<3i',*position)
        while elapsed>=remaining[frame]:elapsed-=remaining[frame];frame+=1
        assert struct.unpack('<H',p.uc.mem_read(node+0x14,2))[0]==frame
        result,error=p.call(0x421ca0,());assert not error,error
print('PASS: 256 native transport-effect timelines use authored single-tick durations and expire after one playthrough')
