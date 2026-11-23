#!/usr/bin/env python3
"""529c10's water drop, natively (gap hunt 6).

On a map with nosealeveltrigger (rules +0xd3d), an impact that struck no unit,
in a map cell whose highest corner (50e660's cell +5) is below sea level (game
+0x19ef8, unsigned), returns at once: only the shot's retirement (529af0),
no explosion, no sound, no direct hit 52a460 and no area pass 529dc0. Every
other impact (an ordinary map, land, off the map, a struck unit) reaches the
damage. World::applyHit ports it; kill_credit_test checks the death blast.
Observation only: nothing from the binary is copied into the engine.

    python3 tools/re/check_impact_water.py
"""
import sys, struct, itertools
from emu import Icd, HEAP
p=Icd()
GAME=HEAP; RULES=HEAP+0x20000; CELLS=HEAP+0x30000; SHOT=HEAP+0x40000; WEAP=HEAP+0x41000; UNIT=HEAP+0x42000
log=[]
def put(a,v,f='<I'): p.uc.mem_write(a,struct.pack(f,v))
def rec(name,n):
    def h(uc,sp):
        log.append(name); return n,0
    return h
for a,n,name in ((0x529af0,1,'retire'),(0x48c870,1,'q'),(0x48c0b0,2,'explode'),(0x50a7d0,3,'sound'),
                 (0x50b140,4,'soundclass'),(0x502c00,2,'fx'),(0x492c80,3,'explclass'),(0x52a460,3,'direct'),(0x529dc0,2,'area')):
    p.hooks[a]=rec(name,n)
p.freeze_hooks()
put(0x62d55c,GAME)
W,H=8,8
put(GAME+0x19e98,W);put(GAME+0x19e9c,H);put(GAME+0x19f04,CELLS);put(GAME+0x175dc,RULES)
SEA=40
p.uc.mem_write(GAME+0x19ef8,bytes([SEA]))
put(WEAP+0x84,0);put(WEAP+0x70,0xffffffff);put(WEAP+0x6c,0xffffffff);put(WEAP+0xb0,0xffffffff)
rows=[]
for trig,high,struck,aoe,onmap,retire in itertools.product((0,1),(SEA-1,SEA,SEA+1,0),(0,1),(0,64),(1,0),(0,1)):
    log.clear()
    put(RULES+0xd3d,trig)
    for i in range(W*H): p.uc.mem_write(CELLS+i*14,bytes(14))
    cx,cz=3,4
    p.uc.mem_write(CELLS+(cz*W+cx)*14+5,bytes([high]))
    put(SHOT,WEAP); put(WEAP+0x8a,aoe,'<H')
    x=(cx*16+5)<<16 if onmap else -(5<<16); z=(cz*16+9)<<16
    put(SHOT+4,x,'<i');put(SHOT+8,0);put(SHOT+12,z,'<i')
    p.uc.mem_write(SHOT+0x92,bytes([0]))
    put(GAME+0x2404,0)
    r,err=p.call(0x529c10,(SHOT,UNIT if struck else 0,retire,1,0))
    assert not err,err
    dropped=not ('area' in log or 'direct' in log)
    expect=bool(trig and onmap and high<SEA and not struck)
    rows.append((trig,high,struck,aoe,onmap,retire,tuple(log)))
    if dropped!=expect: print('MISMATCH',trig,high,struck,aoe,onmap,retire,log); sys.exit(1)
    if dropped and any(c!='retire' for c in log): print('unexpected calls',log); sys.exit(1)
    if dropped and (('retire' in log)!=bool(retire)): print('retire?',log); sys.exit(1)
print('ok', len(rows), 'cases; dropped', sum(1 for r in rows if not ('area' in r[6] or 'direct' in r[6])))
