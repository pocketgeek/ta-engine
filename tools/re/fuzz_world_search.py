#!/usr/bin/env python3
"""Randomized World-adapter search comparison against retail (fuzz).

Generalizes check_world_search.py from its five fixed 32x32 layouts with a
fixed start (6,6), heading 0 and two cost profiles to generated inputs:
random start/goal cells (goals inside obstacles, start == goal), random
initial heading, turn rate (incl. the 200/1000 cost-table boundaries),
road/water multipliers (incl. the 81920 road-scaling boundary), square and
rectangular footprints 1x1..4x4, boats, mazes, walled rooms with doors,
one-cell and diagonal-only gaps, slope/traffic/road noise, exploration masks,
cached terrain from heights, budgets and moving requesters. The actual World
request -> PathService -> navigator delivery runs against original 416430
(with 415170, 4146e0, 4142c0, 414450, 4e4ea0, 4e54a0, 4139d0, 413c80; terrain
mode adds 4e01d0/508cd0/4e1ee0/4e2060/4db640). Every tick compares ordered
grade queries, pending state, admission stamp, mission events, activation,
outcome flags and the delivered navigator points.

    python3 tools/re/fuzz_world_search.py build/retail_trace_test --cases 200 --seed 1
"""
import argparse
import random
import struct
import subprocess
import sys
import time
from emuphase import Phase, OBJ, GS, TYPE, CELLS
from check_cost_search import digest
from fuzz_search_worker import maze
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EIP, UC_X86_REG_ESP, UC_X86_REG_EAX


def case(fx,fz,boat,budget,start,goal,grades,heading,turn,road,water,exploration=-1,admission=True,moving=False,terrain=False,coverage=None):
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
    put(TYPE+0x172,road);put(TYPE+0x16e,water)
    p.uc.mem_write(TYPE+0x18e,struct.pack('<H',turn))
    p.uc.mem_write(unit+0x7e,struct.pack('<H',heading))
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
            position=(start[0]+(0 if tick<5 else 1 if tick<12 else 2 if tick<18 else 1),start[1]+(0 if tick<10 else 1))
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
    else: return None,None
    data=' '.join(map(str,(fx,fz,int(boat),budget,*goal,exploration,int(admission),int(moving),int(terrain),*start,heading,turn,road,water,*grades)))+'\n'
    if coverage is not None: coverage['live_queries']=live_queries[0] if terrain else 0
    return data,expected+['END']



def layout(rng, kind):
    W = H = 32
    g = [6] * (W * H)
    if kind == 'maze':
        g = maze(rng, W, H)
    elif kind == 'gap':
        x = rng.randrange(8, 24)
        for z in range(H): g[z * W + x] = 0
        if rng.random() < .8: g[rng.randrange(H) * W + x] = rng.choice((6, 4, 5, 7))
    elif kind == 'diag':
        off = rng.randrange(-16, 16)
        for z in range(H):
            if 0 <= z + off < W: g[z * W + z + off] = 0
    elif kind == 'noise':
        d = rng.choice(((0, 6), (0, 0, 6, 6, 6), (0, 4, 5, 6, 6, 7), (4, 5, 6, 7), (0, 5, 5, 6),
                        (0, 1, 2, 3, 4, 5, 6, 7)))
        g = [rng.choice(d) for _ in g]
    elif kind == 'rooms':
        for _ in range(rng.randrange(2, 7)):
            x0, z0 = rng.randrange(28), rng.randrange(28)
            x1, z1 = min(31, x0 + rng.randrange(2, 12)), min(31, z0 + rng.randrange(2, 12))
            for z in range(z0, z1 + 1):
                for x in range(x0, x1 + 1):
                    if x in (x0, x1) or z in (z0, z1): g[z * W + x] = 0
            side = rng.randrange(4)
            door = ((rng.randrange(x0, x1 + 1), z0) if side == 0 else (rng.randrange(x0, x1 + 1), z1) if side == 1
                    else (x0, rng.randrange(z0, z1 + 1)) if side == 2 else (x1, rng.randrange(z0, z1 + 1)))
            g[door[1] * W + door[0]] = 6
    elif kind == 'bands':
        for z in range(H):
            v = rng.choice((4, 5, 6, 7, 6))
            for x in range(W): g[z * W + x] = v
    return g


def terrain_heights(rng):
    kind = rng.randrange(4)
    h = [0] * 1024
    wall = rng.choice((24, 32))
    step = rng.choice((10, 20))
    for z in range(32):
        for x in range(32):
            if kind == 0 and x == 16 and z < wall: h[z * 32 + x] = 64
            if kind == 1: h[z * 32 + x] = max(0, min(120, (x - 12) * step))
            if kind == 2: h[z * 32 + x] = rng.choice((0, 0, 10, 20, 40, 80))
            if kind == 3 and 10 <= x <= 24 and 8 <= z <= 24: h[z * 32 + x] = ((x * 3 + z) % 5) * 15
    return h


def generate(rng, index):
    kinds = ('open', 'maze', 'gap', 'diag', 'noise', 'rooms', 'bands')
    kind = kinds[index % len(kinds)]
    fx, fz = rng.choice(((1, 1), (2, 2), (3, 3), (4, 4), (1, 2), (2, 1), (2, 3), (3, 2), (1, 4), (4, 2)))
    boat = rng.random() < .3
    budget = rng.choice((7, 37, 100, 503, 2000, 12000))
    exploration = rng.choice((-1, -1, 0, 1, 2))
    terrain = exploration >= 0 and rng.random() < .2
    moving = rng.random() < .3
    admission = rng.random() < .85
    # The bypassed lookup hands the navigator to any allocated slot; terrain
    # fixtures allocate a second (blocker) entity, so they need real admission.
    admission = admission or terrain
    heading = rng.randrange(65536)
    turn = rng.choice((0, 30, 199, 200, 201, 450, 700, 999, 1000, 1500, rng.randrange(2000)))
    road = rng.choice((65536, 81919, 81920, 98304, 131072, 49152))
    water = rng.choice((65536, 32768, 49152, 98304, 131072))
    hi = 31 - max(fx, fz) - (3 if moving else 0)
    start = (rng.randrange(0, hi), rng.randrange(0, hi))
    goal = (rng.randrange(0, 32 - fx), rng.randrange(0, 32 - fz))
    if rng.random() < .05: goal = start
    if terrain:
        grades = terrain_heights(rng)
        kind = 'terrain'
        # keep the requester (and its moving path) clear of the 2x2 blocker at (20,18)
        while start[0] <= 22 and start[0] + fx + 3 >= 19 and start[1] <= 20 and start[1] + fz + 2 >= 17:
            start = (rng.randrange(0, hi), rng.randrange(0, hi))
    else:
        grades = layout(rng, kind)
        if exploration >= 0:
            # A cached grade 2 sends 4139d0 to the live placement query
            # 4db640, which reads map records, unit bodies and type limits that
            # only the --terrain fixture models on both sides (it is checked
            # there and in check_world_grade.py). Without terrain the native
            # map is empty and answers -1 everywhere.
            grades = [6 if g == 2 else g for g in grades]
        if rng.random() < .85: grades[start[1] * 32 + start[0]] = 6
        if rng.random() < .7: grades[goal[1] * 32 + goal[0]] = 6
    return dict(fx=fx, fz=fz, boat=boat, budget=budget, start=start, goal=goal, grades=grades,
                heading=heading, turn=turn, road=road, water=water, exploration=exploration,
                admission=admission, moving=moving, terrain=terrain), kind


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runner')
    parser.add_argument('--cases', type=int, default=100)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--only', type=int, default=-1)
    parser.add_argument('--same-cell', action='store_true',
                        help='accepted for compatibility; same-cell goals always run (protocol 180)')
    parser.add_argument('--skip-same-cell', action='store_true', help='skip goals in the start cell')
    args = parser.parse_args()
    rng = random.Random(args.seed)
    failures = passed = ticks = skipped = same = 0
    kinds = {}
    t0 = time.time()
    for index in range(args.cases):
        params, kind = generate(rng, index)
        if args.only >= 0 and index != args.only: continue
        if params['start'] == params['goal'] and args.skip_same_cell:
            # Retail 4e54e0 submits a goal in the requester's own cell and
            # 415170 completes it at once (500 work, 0x1000, empty route).
            # Retail-mode World::requestPath submits it too (protocol 180).
            same += 1; continue
        data, expected = case(**params)
        if data is None:
            skipped += 1; continue
        result = subprocess.run([args.runner, '--world-search2'], input=data, text=True, capture_output=True)
        actual = result.stdout.splitlines()
        if result.returncode != 0 or actual != expected:
            failures += 1
            diff = next(((i, a, b) for i, (a, b) in enumerate(zip(actual, expected)) if a != b),
                        (len(actual), len(expected)))
            shown = {k: v for k, v in params.items() if k != 'grades'}
            print(f'FAIL {index} {kind} {shown} rc={result.returncode} {result.stderr.strip()[:200]} '
                  f'first diff {diff}', flush=True)
            continue
        passed += 1; ticks += len(expected) - 1
        kinds[kind] = kinds.get(kind, 0) + 1
    print(f'{"PASS" if not failures else "FAIL"}: {passed} randomized World searches ({ticks} ticks), '
          f'{failures} mismatches, {skipped} unfinished in 5000 ticks, {same} same-cell skipped, seed {args.seed}; '
          f'{sorted(kinds.items())} [{time.time()-t0:.0f}s]')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
