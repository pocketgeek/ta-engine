#!/usr/bin/env python3
"""512ee0's water grave, natively (gap hunt 6).

Runs the corpse placement 512ee0 with the real 511170 over 3000 random
height layouts and positions (the feature lookup 50e600, the placement 495360
and the smoke 502be0 are observation boundaries) and requires: the body is a
water grave (the placed feature's +0x18 = -0x2ccc, no smoke) exactly when the
interpolated ground height under it is at or below sea level. It also counts
how often the old corner-depth test disagreed. World::updateCorpseWindow
ports the predicate; building_death_test checks it.
Observation only: nothing from the binary is copied into the engine.

    python3 tools/re/check_corpse_water.py
"""
import sys, struct, random
from emu import Icd, HEAP
p=Icd()
GAME=HEAP; CELLS=HEAP+0x30000; UNIT=HEAP+0x60000; TYPE=HEAP+0x61000; FEAT=HEAP+0x62000; FTYPES=HEAP+0x70000
log=[]
def put(a,v,f='<I'): p.uc.mem_write(a,struct.pack(f,v))
def get(a,f='<I'): return struct.unpack(f,p.uc.mem_read(a,struct.calcsize(f)))[0]
p.hooks[0x50e600]=lambda uc,sp:(2,CELLS)
def place(uc,sp):
    log.append('place'); return 5,FEAT
p.hooks[0x495360]=place
def smoke(uc,sp):
    log.append('smoke'); return 4,0
p.hooks[0x502be0]=smoke
p.freeze_hooks()
put(0x62d55c,GAME)
W,H=8,8
put(GAME+0x19e98,W);put(GAME+0x19e9c,H);put(GAME+0x19f04,CELLS);put(GAME+0x19edc,FTYPES)
put(UNIT+0xb4,TYPE)
put(TYPE+0x24e,3,'<H');put(TYPE+0x25c,0,'<H');put(TYPE+0x25e,0,'<H');put(TYPE+0x260,0)
rng=random.Random(5)
def interp(h,x,z):
    px,pz=x>>16,z>>16;cx,cz=px>>4,pz>>4
    if cx<0 or cz<0 or cx+1>=W or cz+1>=H: return -1
    fx,fz=px&15,pz&15
    a,b,c,d=h[cz][cx],h[cz][cx+1],h[cz+1][cx],h[cz+1][cx+1]
    def td(v): return int(v/16) if v>=0 else -int(-v/16)
    top=a+td((b-a)*fx); bot=c+td((d-c)*fx)
    return top+td((bot-top)*fz)
n=0;diff=0
for case in range(3000):
    sea=rng.choice((30,40,50))
    h=[[rng.choice((sea-3,sea-1,sea,sea+1,sea+4,rng.randrange(0,90))) for _ in range(W)] for _ in range(H)]
    for z in range(H):
        for x in range(W):
            p.uc.mem_write(CELLS+(z*W+x)*14,bytes(14)); p.uc.mem_write(CELLS+(z*W+x)*14+4,bytes([h[z][x]]))
    p.uc.mem_write(GAME+0x19ef8,bytes([sea]))
    x=rng.randrange(0,W*16)<<16 | rng.randrange(0,65536); z=rng.randrange(0,H*16)<<16 | rng.randrange(0,65536)
    put(UNIT+0x68,x);put(UNIT+0x6c,0);put(UNIT+0x70,z)
    put(FEAT+0x18,0);log.clear()
    r,err=p.call(0x512ee0,(UNIT,1,1,0,0))
    assert not err,err
    water=get(FEAT+0x18)==0xffffd334
    assert ('smoke' in log)==(not water)
    want=interp(h,x,z)<=sea
    if water!=want: print('MISMATCH',x>>16,z>>16,sea,interp(h,x,z)); sys.exit(1)
    ours=(0<=(x>>16)//16<W and 0<=(z>>16)//16<H and h[(z>>16)//16][(x>>16)//16]<sea)
    n+=1; diff+= ours!=water
print('ok',n,'cases: water iff 511170 height <= sea; the corner-depth test differs in',diff)
