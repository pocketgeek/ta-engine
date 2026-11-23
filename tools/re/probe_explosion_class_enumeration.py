#!/usr/bin/env python3
"""Native explosion loader's TDF enumeration contract, with no routine sinks.

Synthetic parsed nodes isolate enumeration from archive and text parsing. This
does not establish how the parser orders children from the original file.
"""
import random
import struct
from emu import Icd, HEAP
p=Icd();p.freeze_hooks()
cursor,root,children,nodes,names=[HEAP+i*0x10000 for i in range(5)]
def put(a,*v):p.uc.mem_write(a,struct.pack('<'+'I'*len(v),*v))
def call(address,args=(),ecx=None):
    value,error=p.call(address,args,ecx=ecx);assert not error,error
    return value
rng=random.Random(0x492daa);checks=0
for case in range(256):
    count=case%65
    order=list(range(count));rng.shuffle(order)
    put(root+8,children,children+count*4)
    for i,key in enumerate(order):
        node=nodes+key*32;name=names+key*32
        p.uc.mem_write(name,f'class-{key:02d}'.encode()+b'\0')
        put(node,name);put(node+8,0,0);put(children+i*4,node)
    put(cursor,root,0)
    for i,key in enumerate(order):
        assert call(0x542970,(i,),cursor)==1
        selected=struct.unpack('<I',p.uc.mem_read(cursor+4,4))[0]
        assert selected==nodes+key*32
        assert call(0x542f50,(),selected)==names+key*32
        assert call(0x542f60,(),selected)==0
        call(0x5429a0,(),cursor)
        checks+=1
    assert call(0x542970,(count,),cursor)==0
print(f'PASS: {checks} native class selections retain parsed child-vector order, including empty classes and out-of-range termination; no name sorting/remapping')
