#!/usr/bin/env python3
"""Compare World request -> PathService -> navigator delivery with retail.

The grade/exploration planes, requester positions, and exact-cell goal are controlled.
Original scheduler, search, reconstruction and navigator delivery all run.
Native admission and cached-grade/exploration queries run by default. With
--terrain, original cache construction, preparation, cleanup and live placement
also run from height/occupancy inputs. The mission event sink is substituted.
"""
import argparse
import random
import struct
import subprocess
from emuphase import Phase, OBJ, GS, TYPE, CELLS
from check_cost_search import digest
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EIP, UC_X86_REG_ESP, UC_X86_REG_EAX


def case(fx,fz,boat,budget,kind,exploration=-1,admission=True,moving=False,terrain=False,coverage=None):
    start,goal=(6,6),(26,20)
    if kind==0: goal=(26,6)
    grades=[6]*1024
    if kind in (1,2):
        for z in range(28 if kind==1 else 32): grades[z*32+16]=0
    if kind==3:
        rng=random.Random(0x414450)
        grades=[rng.choice((0,4,5,6,6,7)) for _ in grades]
    if kind==4: grades=[0]*1024
    grades[start[1]*32+start[0]]=grades[goal[1]*32+goal[0]]=6
    if terrain:
        grades=[0]*1024
        for z in range(32):
            for x in range(32):
                if kind in (1,2) and x==16 and z<(24 if kind==1 else 32): grades[z*32+x]=64
                if kind==3: grades[z*32+x]=max(0,min(120,(x-12)*20))
                if kind==4 and 12<=x<=22 and 12<=z<=22: grades[z*32+x]=((x+z)%4)*20
    p=Phase(32,32);unit=p.unit(*start)
    assert p.construct() is None
    p.plant_request(unit,start,goal)
    def put(address,value): p.uc.mem_write(address,struct.pack('<I',value&0xffffffff))
    def get(address): return struct.unpack('<I',p.uc.mem_read(address,4))[0]
    config=GS+0x600000
    put(config+8,config+0x100);put(config+0x10c,4)
    p.uc.mem_write(GS+0x3068,b'\x01\x00')
    player=GS+0x2404;put(player,1);p.uc.mem_write(player+0xea,b'\x01\x00')
    p.uc.mem_write(player+0xe3,b'\x01')  # 4f6379: every skirmish player is in the 5x budget class
    first=unit-0x138;put(player+0x74,first);put(player+0x78,first+3*0x138)
    put(OBJ+0x115,first);put(OBJ+0x58,0);put(OBJ+0x225,budget)
    p.uc.mem_write(unit+0x78,struct.pack('<hh',fx,fz))
    p.uc.mem_write(p.GRID+4,struct.pack('<hh',fx,fz))
    put(TYPE+0x172,98304 if moving else 65536);put(TYPE+0x16e,49152 if moving else 65536)
    p.uc.mem_write(TYPE+0x18e,struct.pack('<H',700 if moving else 30))
    put(TYPE+0x260,0x80000 if boat else 0)
    p.uc.mem_write(TYPE+0x192,struct.pack('<hh',10000 if boat else 20,13 if boat else -10000))
    world=lambda cell:(cell[0]*16+fx*8,cell[1]*16+fz*8)
    p.uc.mem_write(p.NAV+0xc,struct.pack('<hhhh',*world(start),*world(goal)))
    put(p.NAV+0x10c,2);p.uc.mem_write(p.NAV+0x114,b'\x03')
    pending=[True];events=[0];queries=[]
    vt=get(p.NAV)
    if not admission: p.icd.hooks[get(vt+0x18)]=lambda uc,a:(0,p.NAV if pending[0] else 0)
    if not terrain: p.icd.hooks[0x4e1ee0]=lambda uc,a:(2,0)
    def notify(uc,a): events[0]|=get(a);return 1,0
    def finish(uc,a): pending[0]=False;return 1,0
    def grade(uc,a):
        x,z=struct.unpack('<ii',uc.mem_read(a,8));queries.extend((x,z))
        return 3,grades[z*32+x] if 0<=x<32 and 0<=z<32 else 0
    p.icd.hooks[0x4e2470]=notify
    if not terrain: p.icd.hooks[0x4e2060]=finish
    if exploration<0:
        p.icd.hooks[0x4139d0]=grade
    else:
        if not terrain:
            packed=[0]*128
            for z in range(32):
                for x in range(32): packed[(z//8)*32+x]|=grades[z*32+x]<<(4*(z%8))
            p.uc.mem_write(p.GMAP,struct.pack('<128I',*packed))
        masks=[65535 if exploration==0 else 0 if exploration==1 else 1<<((x//4+z//4)%2)
               for z in range(16) for x in range(16)]
        visibility=p._alloc(512);put(GS+0x19ef4,visibility)
        p.uc.mem_write(visibility,struct.pack('<256H',*masks))
        def observe_grade(uc,address,size,data):
            queries.extend(struct.unpack('<ii',uc.mem_read(uc.reg_read(UC_X86_REG_ESP)+4,8)))
        p.uc.hook_add(UC_HOOK_CODE,observe_grade,begin=0x4139d0,end=0x4139d0)
        if coverage is not None and coverage.get('trace'):
            coverage['grades']=[]
            def returned_grade(uc,address,size,data):
                result=uc.reg_read(UC_X86_REG_EAX)
                if result>=0x80000000: result-=0x100000000
                coverage['grades'].append((get(GS+0x19f44),*queries[-2:],result))
            for address in (0x4139fe,0x413a51,0x413a9a,0x413ae1,0x413bb9,0x413bd7,0x413bf7,0x413c36,0x413c77):
                p.uc.hook_add(UC_HOOK_CODE,returned_grade,begin=address,end=address)
    if terrain:
        blocker=unit+0x138;blocker_mover=p._alloc(0x100);blocker_nav=p._alloc(0x120)
        put(GS+0x14e84,first);put(GS+0x14e88,blocker);put(unit+0x130,0x1000001)
        p.uc.mem_write(blocker,b'\x00'*0x138)
        p.uc.mem_write(blocker+2,struct.pack('<H',2));put(blocker+0x130,0x1000001)
        put(blocker+8,blocker_mover);put(blocker+0xb4,TYPE);put(blocker+0xb8,get(unit+0xb8))
        p.uc.mem_write(blocker+0x74,struct.pack('<hhhh',20,18,2,2))
        put(blocker_mover,blocker_nav);put(blocker_mover+4,p.GRID)
        put(blocker_nav,vt);put(blocker_nav+8,blocker)
        p.uc.mem_write(GS+0x19ef8,bytes([32 if boat else 0]))
        limits=(10000,13,10000,13,255,127,255,127) if boat else (20,-10000,20,-10000,30,15,30,15)
        p.uc.mem_write(p.GRID+8,struct.pack('<4h4B',*limits))
        p.uc.mem_write(TYPE+0x126,struct.pack('<hh',fx,fz));put(TYPE+0x18a,p.GRID)
        p.uc.mem_write(TYPE+0x23c,bytes([255,255] if boat else [30,30]))
        p.uc.mem_write(TYPE+0x24a,b'\x01');put(unit+0x12b,65536);put(blocker+0x12b,65536)
        records=bytearray(32*32*14)
        for z in range(32):
            for x in range(32):
                i=(z*32+x)*14
                quad=[grades[min(z+dz,31)*32+min(x+dx,31)] for dz in (0,1) for dx in (0,1)]
                records[i+4]=grades[z*32+x];records[i+5]=max(quad);records[i+6]=min(quad)
                struct.pack_into('<H',records,i+8,65535)
        def occupy(position,value):
            for z in range(position[1],position[1]+fz):
                for x in range(position[0],position[0]+fx):
                    p.uc.mem_write(CELLS+(z*32+x)*14,struct.pack('<H',value))
        p.uc.mem_write(CELLS,bytes(records));occupy(start,1)
        for z in range(18,20):
            for x in range(20,22): p.uc.mem_write(CELLS+(z*32+x)*14,struct.pack('<H',2))
        live_queries=[0]
        def observe_live(uc,address,size,data): live_queries[0]+=1
        p.uc.hook_add(UC_HOOK_CODE,observe_live,begin=0x4db640,end=0x4db640)
    p.icd.freeze_hooks()
    if terrain:
        # Warm both caches at tick zero through their real refresh/preparation
        # paths. No native result is supplied to World.
        for address,arguments in ((0x4e01d0,(0,32|(32<<16))),
                                  (0x4e1ee0,(unit,0)),(0x4e2060,(unit,))):
            _,error=p.icd.call(address,arguments,ecx=p.GRID)
            assert error is None,error
    expected=[]
    previous=start
    for tick in range(1,5001):
        if moving:
            position=(6 if tick<5 else 7 if tick<12 else 8 if tick<18 else 7,6 if tick<10 else 7)
            if terrain: occupy(previous,0);occupy(position,1);previous=position
            p.uc.mem_write(unit+0x74,struct.pack('<hh',*position))
            x,z=world(position)
            p.uc.mem_write(unit+0x68,struct.pack('<iii',x*65536,(32 if boat else 0)*65536,z*65536))
            p.uc.mem_write(unit+0x7e,struct.pack('<H',0 if tick<12 else 16384 if tick<18 else 32768))
            p.uc.mem_write(get(unit+8)+0x36,struct.pack('<H',0 if tick<10 else 0x800 if tick<18 else 0x1000))
        put(GS+0x19f44,tick);put(0x634674,int(pending[0]));queries.clear()
        _,error=p.icd.call(0x416430,(1,),ecx=OBJ)
        assert error is None and p.uc.reg_read(UC_X86_REG_EIP)==0x6ffff000,error
        if terrain: pending[0]=bool(p.uc.mem_read(p.NAV+0x114,1)[0]&2)
        count=get(p.NAV+0x10c);active=p.uc.mem_read(p.NAV+0x114,1)[0]&1
        points=struct.unpack('<'+'h'*(count*2),p.uc.mem_read(p.NAV+0xc,count*4))
        values=(tick,int(pending[0]),get(p.NAV+0x110),digest(queries),events[0],active,get(unit+0x134)&15,count,*points)
        if terrain:
            packed=struct.unpack('<128I',p.uc.mem_read(p.GMAP,512))
            values+=digest((packed[(z//8)*32+x]>>(4*(z%8)))&15 for z in range(32) for x in range(32)),
        expected.append(' '.join(map(str,values)))
        if not pending[0]: break
    else: raise AssertionError('native search did not finish')
    data=' '.join(map(str,(fx,fz,int(boat),budget,*goal,exploration,int(admission),int(moving),int(terrain),*grades)))+'\n'
    if coverage is not None: coverage['live_queries']=live_queries[0] if terrain else 0
    return data,expected+['END']


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('runner')
    parser.add_argument('--exploration',choices=('raw','full','hidden','stripes','all'),default='all')
    parser.add_argument('--admission',choices=('native','bypass'),default='native')
    parser.add_argument('--motion',choices=('stationary','moving','both'),default='both')
    parser.add_argument('--terrain',action='store_true',help='build cached grades from heights and occupancy using original routines')
    args=parser.parse_args()
    if args.terrain and args.exploration=='raw': parser.error('--terrain requires cached-grade exploration')
    total=ticks=live_total=0
    modes={'raw':-1,'full':0,'hidden':1,'stripes':2}
    for name,mode in modes.items():
        if args.terrain and mode<0: continue
        if args.exploration not in ('all',name): continue
        mode_ticks=0
        mode_count=mode_live=0
        for motion in ('stationary','moving'):
            if args.motion not in ('both',motion): continue
            for fx,fz in ((1,1),(2,2),(2,3),(3,2),(2,6),(6,2),(7,4),(4,7)):
                for boat in (False,True):
                    for budget in (7,503,12000):
                        for kind in range(5):
                            coverage={}
                            data,expected=case(fx,fz,boat,budget,kind,mode,args.admission=='native',motion=='moving',args.terrain,coverage)
                            actual=subprocess.run([args.runner,'--world-search'],input=data,text=True,capture_output=True,check=True).stdout.splitlines()
                            assert actual==expected,(name,motion,fx,fz,boat,budget,kind,next(((i,a,b) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),(len(actual),len(expected))))
                            total+=1;mode_count+=1;ticks+=len(expected)-1;mode_ticks+=len(expected)-1
                            mode_live+=coverage['live_queries'];live_total+=coverage['live_queries']
        print(f'PASS: {name} exploration, {mode_count} searches, {mode_ticks} ticks, {mode_live} native live placements',flush=True)
    print(f'PASS: {total} World searches, {ticks} ticks; queries, notifications, flags and delivered navigator points match retail; {live_total} native live placements')


if __name__=='__main__': main()
