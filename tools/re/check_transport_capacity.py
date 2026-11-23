#!/usr/bin/env python3
"""Compare transport capacity decisions with the user-owned retail executable."""
import random, struct, subprocess, sys
from emu import Icd, HEAP
p=Icd()
carrier,passenger,ct,pt,owner,game=[HEAP+i*0x1000 for i in range(6)]
def put(a,n):p.uc.mem_write(a,struct.pack('<I',n&0xffffffff))
def word(a,n):p.uc.mem_write(a,struct.pack('<H',n&0xffff))
for u,t in [(carrier,ct),(passenger,pt)]:
    put(u+0x130,0x1000000);put(u+0xb4,t);put(u+0xb8,owner);put(u+8,HEAP+0x7000)
put(owner,1);p.uc.mem_write(owner+0xea,b'\x01')
put(ct+0x264,0x200);word(pt+0x194,0xffff);put(passenger+0x6c,100<<16)
put(0x62d55c,game);p.uc.mem_write(game+0x19ef8,b'\x14')
counts=[0,0]
p.hooks[0x519ef0]=lambda uc,sp:(0,counts[0])
p.hooks[0x519f10]=lambda uc,sp:(0,counts[1])
p.freeze_hooks()
rng=random.Random(0x519f50);rows=[];expected=[]
for _ in range(4096):
    size,maximum,count,limit,used,budget=[rng.randrange(n) for n in (65,65,65,65,257,257)]
    word(pt+0x246,size);word(ct+0x240,maximum);word(ct+0x242,limit);word(ct+0x244,budget)
    counts[:]=[count,used]
    value,error=p.call(0x519f50,(passenger,),ecx=carrier)
    if error:raise RuntimeError(error)
    rows.append(f'{size} {maximum} {count} {limit} {used} {budget}');expected.append(value)
r=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/transport_test','--capacity'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=list(map(int,r.stdout.split()))
assert actual==expected, next(((i,rows[i],a,b) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),'length mismatch')
print('PASS: 4096 capacity decisions match retail 0x519f50 (other eligibility conditions held valid)')
