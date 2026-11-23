#!/usr/bin/env python3
"""A water grave's feature under the feature update 4959c0, natively (gap hunt 8).

512ee0 places a body's corpse in water with velocity (0, -0x2ccc, 0) at the
body's height and no decomposition (+0x3c stays 0). This runs the real
feature update 4959c0 each tick on such a record (one record in the update
list; 50e600 is answered from a cell table, and the removal 496380, the
rest 494a80 and the other feature calls are observation boundaries) for
random sea levels, cell corner heights (511260 reads (+5 + +6) / 2), body
heights above and below the sea and cells off the map, and requires
retail_script_test --water-grave (World's retailFeatureSinkStep, the step
tools/re/check_corpse_sink.py also checks) to give the same outcome: the
update that removes it (a fade, only on a bed of 0) or its coming to rest
for good. Observation only: nothing from the binary is copied into the
engine.

    python3 tools/re/check_water_grave.py --binary build/retail_script_test
"""
import sys, struct, random, subprocess, argparse
from emu import Icd, HEAP
ap=argparse.ArgumentParser();ap.add_argument('--binary',required=True);ap.add_argument('--cases',type=int,default=1500)
args=ap.parse_args()
p=Icd()
GAME=HEAP; CELLS=HEAP+0x30000; RECS=HEAP+0x60000; TYPES=HEAP+0x70000
log=[]
def put(a,v,f='<I'): p.uc.mem_write(a,struct.pack(f,v))
def get(a,f='<I'): return struct.unpack(f,p.uc.mem_read(a,struct.calcsize(f)))[0]
W=H=8
offmap=[False]
def cell(uc,sp):
    x,z=struct.unpack('<ii',uc.mem_read(sp,8))
    if offmap[0] or x<0 or z<0 or x>=W or z>=H: return 2,0
    return 2,CELLS+(z*W+x)*14
p.hooks[0x50e600]=cell
p.hooks[0x5373d0]=lambda uc,sp:(1,0)
p.hooks[0x495f30]=lambda uc,sp:(0,0)
def ev(name,n):
    def f(uc,sp):
        log.append(name); return n,0
    return f
for a,name,n in ((0x496380,'remove',2),(0x494f00,'replace',3),(0x494a80,'rest',2),(0x495300,'other',1),(0x495110,'other',2)):
    p.hooks[a]=ev(name,n)
p.freeze_hooks()
put(0x62d55c,GAME)
put(GAME+0x19e98,W);put(GAME+0x19e9c,H);put(GAME+0x19f04,CELLS);put(GAME+0x19edc,TYPES)
put(GAME+0x19e74,RECS);put(GAME+0x19ec0,0);put(GAME+0x19ecc,0x1fdb)
def native(y,hi,lo,sea,off):
    offmap[0]=off
    p.uc.mem_write(GAME+0x19ef8,bytes([sea]))
    for i in range(W*H):
        c=bytearray(14); c[5]=hi; c[6]=lo; p.uc.mem_write(CELLS+i*14,bytes(c))
    p.uc.mem_write(RECS,bytes(0x60)); p.uc.mem_write(TYPES,bytes(0x140))
    put(RECS,0xffff,'<H')
    put(RECS+8,(3*16+5)<<16);put(RECS+0xc,y&0xffffffff);put(RECS+0x10,(3*16+5)<<16)
    put(RECS+0x18,0xffffd334);put(RECS+0x28,3,'<h');put(RECS+0x2a,3,'<h')
    put(GAME+0x19e7c,0)
    log.clear()
    for t in range(1,20000):
        put(GAME+0x19f44,t)
        r,err=p.call(0x4959c0,())
        assert not err,err
        if log:
            assert log==['remove'] or log==['rest'],log
            return t-1 if log[0]=='remove' else -1
    raise SystemExit('no outcome')
rng=random.Random(8)
cases=[]
for i in range(args.cases):
    sea=rng.choice((rng.randrange(1,256),20,40,60))
    kind=rng.randrange(6)
    if kind==0: hi,lo=0,0
    elif kind==1: hi,lo=rng.choice(((1,0),(0,1),(2,0),(1,1)))
    else:
        lo=rng.randrange(0,max(1,sea)); hi=min(255,lo+rng.randrange(0,12))
    off=rng.randrange(20)==0
    yk=rng.randrange(5)
    if yk==0: y=sea<<16
    elif yk==1: y=rng.randrange(0,max(1,sea))<<16 | rng.randrange(65536)
    elif yk==2: y=(sea+rng.randrange(1,120))<<16 | rng.randrange(65536)
    elif yk==3: y=((hi+lo)>>1)<<16
    else: y=rng.randrange(-3<<16,(sea+4)<<16)
    cases.append((y,hi,lo,sea,off))
inp=''.join('%d %d %d\n'%(y,-1 if off else (hi+lo)>>1,sea) for y,hi,lo,sea,off in cases)
ours=subprocess.run([args.binary,'--water-grave'],input=inp,capture_output=True,text=True,check=True).stdout.split()
stay=fade=0
for (y,hi,lo,sea,off),o in zip(cases,ours):
    n=native(y,hi,lo,sea,off)
    if n!=int(o):
        print('MISMATCH y=%#x hi=%d lo=%d sea=%d off=%s native=%d ours=%s'%(y,hi,lo,sea,off,n,o)); sys.exit(1)
    stay+=n<0; fade+=n>=0
assert len(ours)==len(cases)
print('ok',len(cases),'cases:',fade,'faded out,',stay,'came to rest for good')
