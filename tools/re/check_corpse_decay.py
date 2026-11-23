#!/usr/bin/env python3
"""A land corpse's decomposition under the feature update 4959c0, natively (gap hunt 8).

513027..513034 load a land corpse's +0x3c with its feature type's
decomposition (decomposetime x 30); a reclaim wake (406615, VTOL 420ba4)
reloads it. This runs the real feature update 4959c0 tick by tick on one
such record (the update list holds just it; the removal 496380 and the rest
494a80 are observation boundaries, the body's model a stub) and requires
retail_script_test --corpse-decay (retailCorpseDecayLife, and
retailCorpseDecayHold for a reload after a random number of updates) to name
the same removal update, or none. Observation only: nothing from the binary
is copied into the engine.

    python3 tools/re/check_corpse_decay.py --binary build/retail_script_test
"""
import sys, struct, random, subprocess, argparse
from emu import Icd, HEAP
ap=argparse.ArgumentParser();ap.add_argument('--binary',required=True);ap.add_argument('--cases',type=int,default=600)
args=ap.parse_args()
p=Icd()
GAME=HEAP; CELLS=HEAP+0x30000; RECS=HEAP+0x60000; TYPES=HEAP+0x70000; OBJ=HEAP+0x80000; OBJ2=HEAP+0x81000
log=[]
def put(a,v,f='<I'): p.uc.mem_write(a,struct.pack(f,v))
def get(a,f='<I'): return struct.unpack(f,p.uc.mem_read(a,struct.calcsize(f)))[0]
p.hooks[0x50e600]=lambda uc,sp:(2,CELLS)
p.hooks[0x5373d0]=lambda uc,sp:(1,0)
p.hooks[0x495f30]=lambda uc,sp:(0,0)
def ev(name,n):
    def f(uc,sp):
        log.append(name); return n,0
    return f
for a,name,n in ((0x496380,'remove',2),(0x494f00,'replace',3),(0x494a80,'rest',2),(0x495300,'other',1),(0x495110,'other',2)):
    p.hooks[a]=ev(name,n)
p.freeze_hooks()
put(0x62d55c,GAME);put(GAME+0x19e74,RECS);put(GAME+0x19ec0,0);put(GAME+0x19edc,TYPES)
def native(d0,wake,d1):
    p.uc.mem_write(RECS,bytes(0x60)); p.uc.mem_write(TYPES,bytes(0x140))
    put(RECS,0xffff,'<H'); put(RECS+0xc,50<<16); put(RECS+4,OBJ); put(OBJ,0); put(OBJ+0x1c8,OBJ2); put(OBJ+0x174,0)
    put(RECS+0x3c,d0); put(GAME+0x19e7c,0); log.clear()
    for t in range(1,70000):
        if t==wake+1: put(RECS+0x3c,d1)    # the reclaim wake runs before this tick's update
        r,err=p.call(0x4959c0,()); assert not err,err
        if log:
            assert log in (['remove'],['rest']),log
            return t-1 if log[0]=='remove' else -1
    raise SystemExit('no outcome')
rng=random.Random(0x4959c0)
cases=[]
for i in range(args.cases):
    d0=rng.choice((0,1,2,3,58,59,60,61,90,rng.randrange(1,2000)))
    # A reload before the first update of a corpse with no decomposition
    # would catch it before it rests; World's reclaim never comes that early.
    wake=-1 if rng.randrange(3)==0 else rng.randrange(1 if d0==0 else 0,d0+70)
    d1=d0 if rng.randrange(4) else rng.choice((1,2,30,59,60,61,rng.randrange(1,400)))
    cases.append((d0,wake,d1))
inp=''.join('%d %d %d\n'%c for c in cases)
ours=subprocess.run([args.binary,'--corpse-decay'],input=inp,capture_output=True,text=True,check=True).stdout.split()
assert len(ours)==len(cases)
woken=0
for (d0,wake,d1),o in zip(cases,ours):
    n=native(d0,wake,d1)
    if n!=int(o):
        print('MISMATCH decompose=%d wake=%d reload=%d native=%d ours=%s'%(d0,wake,d1,n,o)); sys.exit(1)
    woken+=wake>=0
print('ok',len(cases),'cases,',woken,'with a reclaim reload: the removal update matches')
