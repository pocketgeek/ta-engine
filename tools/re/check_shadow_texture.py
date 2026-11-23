#!/usr/bin/env python3
"""Observe retail shadow texture dispatch and transparent-pixel coverage.

Uses synthetic geometry, texels and palette lookup values. No retail code or
asset data is copied into the engine. This is a bounded raster-path probe,
not a full-game screenshot comparison.
"""
import struct
from emu import Icd,HEAP
p=Icd();put=lambda a,v:p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
surface=HEAP;instance=HEAP+0x1000;obj=HEAP+0x2000;vertices=HEAP+0x3000;prim=HEAP+0x4000;indices=HEAP+0x5000;texture=HEAP+0x6000;game=HEAP+0x10000
put(0x62d55c,game);put(instance,1);put(instance+0x1cc,obj);put(instance+0x1f0,vertices);p.uc.mem_write(instance+0x1f6,b'\x0b\x00');p.uc.mem_write(instance+0xe4,b'\x01');put(obj+4,4);put(obj+8,1);put(obj+0xc,-1);put(obj+0x28,prim)
for i,(x,y,z) in enumerate([(0,0,0),(16,0,0),(16,0,-16),(0,0,-16)]):p.uc.mem_write(vertices+i*12,struct.pack('<iii',x*65536,y*65536,z*65536))
p.uc.mem_write(indices,struct.pack('<4H',0,1,2,3));put(prim+4,4);put(prim+0xc,indices);put(prim+0x10,texture)
calls=[]
def capture(uc,args):
    a=struct.unpack('<9I',bytes(uc.mem_read(args,36)));calls.append(a);return 9,1
p.hooks[0x5415a0]=capture;p.hooks[0x537d30]=lambda uc,args:(1,0)
p.freeze_hooks()
result,error=p.call(0x4eda80,[surface,instance]);assert not error,error
assert len(calls)==1,calls
assert calls[0][1]==texture,calls
assert calls[0][4]==4 and calls[0][7:]==(1,15),calls
print('PASS original shadow walker forwards texture and quad to textured rasterizer, mode 15:',calls)

# Run the original span rasterizer too. Supply a synthetic palette lookup so
# this checks coverage, not a copied retail palette or an assumed shadow shade.
q=Icd()
put=lambda a,v:q.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
span=HEAP; tex=HEAP+0x100; pixels=HEAP+0x200; dst=HEAP+0x300
palette=HEAP+0x400; table=HEAP+0x1000
put(span,0);put(span+4,2);put(span+8,0);put(span+0xc,0)
put(span+0x10,2*65536);put(span+0x14,0)
put(tex+0x10,pixels);put(palette+0x28,table)
q.uc.mem_write(pixels,bytes([0,7]));q.uc.mem_write(table,bytes([222])*4096)
q.hooks[0x53fe40]=lambda uc,args:(0,palette)
q.freeze_hooks()
for transparent,expected in [(0,bytes([99,222])),(7,bytes([222,99]))]:
    q.uc.mem_write(dst,bytes([99,99]))
    result,error=q.call(0x541c10,[span,tex,dst,0,0,0,1,1,transparent,15])
    assert not error,error
    actual=bytes(q.uc.mem_read(dst,2))
    assert actual==expected,(transparent,actual,expected)
print('PASS original mode-15 shadow spans preserve transparent pixels and write covered pixels')
