#!/usr/bin/env python3
"""Compare dense same-destination Hunter movement with native retail on Ulasem Arena.

Initial Hunters are cloned from a captured Hunter's native type/mover layout.
Terrain/features remain captured map inputs; original bodies are removed. Both
sides own their evolving occupancy, missions, shared worker and RNG. Formation,
combat, AI strategy and animation scripts are outside this movement comparison.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess

from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EBP, UC_X86_REG_EBX, UC_X86_REG_EDI, UC_X86_REG_EIP, UC_X86_REG_ESI, UC_X86_REG_ESP

from balance_inputs import set_balance_inputs
from check_captured_tick import find_crt_thread
from emureload import CapturedProcess


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture',type=Path)
    ap.add_argument('--save',type=Path,required=True)
    ap.add_argument('--root',type=Path,default=Path('assets/game'))
    ap.add_argument('--binary',default='build-dbg/retail_replay_probe')
    ap.add_argument('--balance',choices=('standard','crusades'),default='crusades')
    ap.add_argument('--count',type=int,default=450)
    ap.add_argument('--rounds',type=int,default=1800)
    ap.add_argument('--budget',type=int,default=12000)
    ap.add_argument('--spacing',type=int,default=3,help='initial grid spacing in map cells; minimum 2')
    ap.add_argument('--goal',type=int,nargs=2,default=(2000,1600))
    ap.add_argument('--start',type=int,nargs=2,default=(2000,2400))
    ap.add_argument('--blocker',type=int,nargs=2,action='append',default=[],metavar=('X','Z'),
                    help='extra Hunter ordered to its own position (a parked obstacle); repeatable')
    ap.add_argument('--mover',type=int,nargs=4,action='append',default=[],metavar=('X','Z','GX','GZ'),
                    help='extra Hunter at X,Z ordered to GX,GZ (e.g. opposing traffic); repeatable')
    ap.add_argument('--then',type=int,nargs=2,metavar=('GX','GZ'),
                    help='queue a second Move_Ground for the main group after the first goal')
    ap.add_argument('--dump-map',type=int,nargs=5,metavar=('X0','Z0','X1','Z1','STEP'),
                    help='print native 2x2 footprint passability over a region and exit')
    ap.add_argument('--world-tick',action='store_true',
                    help='drive World through World::order and full World::tick (Retail mode) instead of the composed probe calls')
    ap.add_argument('--unit-cap',type=int,default=2000,
                    help='--world-tick: the match unit cap (default 2000, the lobby default); the native pool is maxunits '
                         'for it as 499590 sets it (clamped to 20..500; 0 = uncapped, the 250 default)')
    ap.add_argument('--output',type=Path,required=True)
    args=ap.parse_args()
    extras=[(x,z,x,z) for x,z in args.blocker]+[tuple(m) for m in args.mover]
    total=args.count+len(extras)
    if not 1<=args.count<=1000 or not 1<=args.rounds<=5000 or args.budget<1 or args.spacing<2:ap.error('invalid bounds')
    # The composed probe keeps its fixture pool (one slot per Hunter, in
    # order). --world-tick runs World's production pools: the native pool has
    # the match capacity and 5121d8 places each Hunter, holes included.
    if args.unit_cap<0:ap.error('invalid unit cap')
    args.output.mkdir(parents=True,exist_ok=True)
    capture=json.loads(args.capture.read_text());p=CapturedProcess(capture)
    selected=set_balance_inputs(p,capture,args.save,args.root,args.balance=='crusades',surface=True,motion=True)
    read=lambda fmt,a:struct.unpack('<'+fmt,p.uc.mem_read(a,struct.calcsize('<'+fmt)))
    def write(fmt,a,*v):p.put(a,struct.pack('<'+fmt,*v))
    def alloc(size):
        result=p.brk;p.brk=(result+size+15)&~15;p.put(result,bytes(size));return result
    def call(address,values=(),this=None):
        result,error=p.icd.call(address,values,ecx=this)
        if error or p.missing:raise RuntimeError((hex(address),error,p.missing))
        return result
    capacity=total
    if args.world_tick:
        # 499590 sets maxunits (thiscall on the options object, +0xc), which
        # 511570 then gives every pool. Uncapped is no retail setting: World
        # uses the options default 250 (4c7c7a).
        options=p.u32(p.u32(0x62d558)+8)
        if args.unit_cap:call(0x499590,(args.unit_cap,),options);capacity=read('I',options+12)[0]
        else:capacity=250
        if not total<=capacity:ap.error(f'the entity pool ({capacity}) must hold every Hunter')
    source=next(u['address'] for u in p.runtime['units']
                if bytes(p.uc.mem_read(p.u32(u['address']+0xb4)+32,32)).split(b'\0')[0].lower()==b'zonter')
    source_id=read('H',source+2)[0];grid=selected[source_id]
    unit_template=bytes(p.uc.mem_read(source,312))
    mover_template=bytes(p.uc.mem_read(p.u32(source+8),56))
    nav_template=bytes(p.uc.mem_read(p.u32(p.u32(source+8)),277))
    assert read('2h',source+0x78)==(2,2)
    kind=p.u32(source+0xb4);base=read('i',kind+0x162)[0]
    width,height=read('2I',p.game+0x19e98);cells=p.u32(p.game+0x19f04)
    for u in p.runtime['units']:write('I',u['address']+0x130,0)
    data=bytearray(p.uc.mem_read(cells,width*height*14))
    for i in range(width*height):struct.pack_into('<H',data,i*14,0)
    p.put(cells,bytes(data))
    pool=alloc((capacity+1)*312)
    write('I',p.game+0x14e84,pool);write('I',p.game+0x14e88,pool+capacity*312)
    owner=p.game+0x2404
    write('I',owner,1);write('B',owner+0xea,2);write('B',owner+0xeb,0);write('B',owner+0xe3,1)
    write('2I',owner+0x74,pool+312,pool+capacity*312)
    ai=alloc(0x200);write('I',ai,owner);write('B',ai+0x1a5,1);write('I',owner+0x80,ai)
    for i in range(1,10):write('I',p.game+0x2404+i*0x110,0)
    write('I',p.u32(p.u32(0x62d558)+8)+12,capacity);write('H',p.game+0x14e7c,capacity);write('H',owner+0xe8,0)
    write('B',p.game+0x306f,0)
    visibility=alloc((width//2)*(height//2)*2)
    p.put(visibility,struct.pack('<H',0xffff)*((width//2)*(height//2)))
    write('I',p.game+0x19ef4,visibility)
    old=p.u32(grid)
    for address in range(0x62dbf0,0x634670,0x354):write('I',address,0)
    write('I',grid,old);write('I',grid+0x33c,0);write('2I',grid+0x34c,0,0)
    plane=alloc(width*((height+7)//8)*4);write('I',grid+0x348,plane)
    write('10I',0x634674,*([0]*10));obj=alloc(0x400);call(0x415f80,this=obj)
    p.obj=obj;write('I',p.game+0x19e70,obj);write('I',obj+0x225,args.budget);write('I',obj+0x115,pool+312)
    clock=0;p.icd.hooks[0x53ff20]=lambda uc,a:(0,clock)
    thread=find_crt_thread(p,capture['rng_calls'][0]['registers']['ebp'])
    p.icd.hooks[0x5dc403]=lambda uc,a:(0,thread)
    crt_seed=p.u32(thread+0x14)
    frame=alloc(0x2000)
    def allocate_entity():
        # 512162..512255 with player 0's record: the CRT-ranked free slot.
        uc=p.uc;stops=[uc.hook_add(UC_HOOK_CODE,lambda uc,a,s,d:uc.emu_stop(),begin=a,end=a) for a in (0x512255,0x5121cd)]
        try:
            write('I',frame+0x1800+0x24,0)
            for reg,value in ((UC_X86_REG_EBP,frame+0x1800),(UC_X86_REG_ESP,frame+0x1000),(UC_X86_REG_EBX,p.game),(UC_X86_REG_EDI,p.game)):uc.reg_write(reg,value)
            uc.emu_start(0x512162,0,timeout=5_000_000)
        finally:
            for h in stops:uc.hook_del(h)
        if uc.reg_read(UC_X86_REG_EIP)!=0x512255:raise RuntimeError('native entity pool exhausted')
        entity=uc.reg_read(UC_X86_REG_ESI)
        write('I',entity+0x130,p.u32(entity+0x130)|0x1000000)
        write('H',p.game+0x24ec,read('H',p.game+0x24ec)[0]+1)
        return (entity-pool)//312-1
    nav_vtable=struct.unpack_from('<I',nav_template)[0]
    p.icd.hooks[p.u32(nav_vtable+0x34)]=lambda uc,a:(0,0)
    p.icd.freeze_hooks()
    if args.dump_map:
        x0,z0,x1,z1,step=args.dump_map
        print(f'map {width}x{height} cells; grades of a 2x2 footprint, {step}px step, origin ({x0},{z0}); "#" impassable (<6)')
        for z in range(z0,z1,step):
            print(f'{z:5d} '+''.join('.' if call(0x5088f0,(grid,x//16-1,z//16-1,2,2))>=6 else '#' for x in range(x0,x1,step)),flush=True)
        return
    # Grid-aligned, nonoverlapping footprints, nearest first to a supplied center.
    candidates=[(x*16+16,z*16+16) for z in range(max(2,args.start[1]//16-64),min(height-4,args.start[1]//16+64),args.spacing)
                for x in range(max(2,args.start[0]//16-64),min(width-4,args.start[0]//16+64),args.spacing)]
    candidates.sort(key=lambda q:((q[0]-args.start[0])**2+(q[1]-args.start[1])**2,q[1],q[0]))
    positions=[]
    for x,z in candidates:
        if any(abs(x-bx)<48 and abs(z-bz)<48 for bx,bz,_,_ in extras):continue
        if call(0x5088f0,(grid,x//16-1,z//16-1,2,2))>=6:
            positions.append((x,z))
            if len(positions)==args.count:break
    if len(positions)!=args.count:raise ValueError('not enough clear starting footprints')
    gx,gz=args.goal
    goals=[(x*16+16,z*16+16) for z in range(max(2,gz//16-32),min(height-4,gz//16+32),2)
           for x in range(max(2,gx//16-32),min(width-4,gx//16+32),2)]
    goals.sort(key=lambda q:((q[0]-gx)**2+(q[1]-gz)**2,q[1],q[0]))
    gx,gz=next((x,z) for x,z in goals if call(0x5088f0,(grid,x//16-1,z//16-1,2,2))>=6)
    for bx,bz,_,_ in extras:
        if bx%16 or bz%16 or call(0x5088f0,(grid,bx//16-1,bz//16-1,2,2))<6:raise ValueError(('extra footprint not clear/aligned',bx,bz))
    goal_of=[(gx,gz)]*len(positions)+[(a,b) for _,_,a,b in extras]
    positions=positions+[(x,z) for x,z,_,_ in extras]
    actors=[]
    slots=[allocate_entity() for _ in positions] if args.world_tick else list(range(len(positions)))
    for index,(x,z) in enumerate(positions):
        identity=slots[index]+1
        point=alloc(12);write('3i',point,goal_of[index][0]*65536,0,goal_of[index][1]*65536)
        unit=pool+identity*312;mover=alloc(56);nav=alloc(277);mission=alloc(0x72)
        p.put(unit,unit_template);p.put(mover,mover_template);p.put(nav,nav_template)
        write('H',unit+2,identity);write('I',unit+8,mover);write('I',mover,nav);write('I',mover+4,grid)
        write('I',unit+0xb8,owner);write('B',unit+0xfd,0)
        write('I',unit+0xa4,0);write('I',unit+0xb0,0);write('I',unit+0xa8,0);write('I',unit+0xd0,0)
        write('I',unit+0x130,p.u32(unit+0x130)&~0x4000);write('B',unit+0x134,0)
        write('3i',unit+0x68,x*65536,0,z*65536);write('2h',unit+0x74,x//16-1,z//16-1)
        write('2h',unit+0x126,x//16-1,z//16-1);write('4H',unit+0x7c,0,0,0,0)
        write('i',unit+0x12b,base)
        write('5I',mover+0x20,0,0,0,0,0);write('H',mover+0x36,1)
        write('I',nav+8,unit);write('I',nav+4,0);write('I',nav+0x10c,0);write('I',nav+0x110,0);write('B',nav+0x114,0)
        call(0x4d6c40,(28,0,point,0,0,0,0,0,0,0,0,0),mission)
        write('I',mission+0xe,unit);write('I',unit+0x60,mission)
        if args.then and index<args.count:
            second=alloc(0x72);write('3i',point,args.then[0]*65536,0,args.then[1]*65536)
            call(0x4d6c40,(28,0,point,0,0,0,0,0,0,0,0,0),second)
            write('I',second+0xe,unit);write('I',mission+0x66,second)
        for cz in range(z//16-1,z//16+1):
            for cx in range(x//16-1,x//16+1):write('H',cells+(cz*width+cx)*14,identity)
        actors.append((unit,mover,nav,mission))
    call(0x4e01d0,(0,width|(height<<16)),grid)
    write('I',0x64186c,1);write('I',p.game+0x19f44,0)
    for unit,mover,nav,mission in actors:call(0x4d4da0,(mission+0x22,4),mission)
    command=args.output/'input.txt';world_output=args.output/'world.jsonl'
    if args.world_tick:
        command.write_text(f'{total} {args.rounds} {args.budget} {base} {crt_seed} {args.unit_cap}\n'+''.join(f'{x} {z} {2 if args.then and i<args.count else 1} {a} {b}'+(f' {args.then[0]} {args.then[1]}' if args.then and i<args.count else '')+'\n'
                for i,((x,z),(a,b)) in enumerate(zip(positions,goal_of))))
    else:
        if extras or args.then:ap.error('--blocker/--mover/--then require --world-tick')
        command.write_text(f'{args.count} {args.rounds} {args.budget} {gx} {gz} {base}\n'+''.join(f'{x} {z}\n' for x,z in positions))
    print(f'World: {args.count} Hunters + {len(args.blocker)} blockers + {len(args.mover)} movers, goal={(gx,gz)}, budget={args.budget}, rounds={args.rounds}, world_tick={args.world_tick}',flush=True)
    run=subprocess.run([args.binary,str(args.root),'--crowd-world' if args.world_tick else '--crowd-movement','ulasem arena',str(int(args.balance=='crusades')),str(command),str(world_output)],check=True,stdout=subprocess.PIPE,text=True)
    if args.world_tick:
        reported=next(line.split()[1:] for line in run.stdout.splitlines() if line.startswith('SLOTS '))
        world_slots=[int(v) for v in reported]
        if world_slots!=[capacity]+slots:raise AssertionError({'native_capacity':capacity,'native_slots':slots,'world':world_slots})
        print(f'Entity pool: unit cap {args.unit_cap}, capacity {capacity}, CRT seed {crt_seed:#x}; World and 5121d8 place the Hunters in slots {slots}',flush=True)
    stats=[{'blocked_ticks':0,'longest_blocked_run':0,'run':0,'arrival_tick':None,'previous':(x*65536,z*65536)} for x,z in positions]
    with world_output.open() as port:
        for clock in range(1,args.rounds+1):
            write('I',p.game+0x19f44,clock)
            for unit,mover,nav,mission in actors:
                # 51e1e5..51e21b: active dispatcher, queued dispatcher, mover, height.
                call(0x4d8450,(unit,));call(0x4d85e0,(unit,));call(0x4dc800,(unit,),mover);call(0x51b2a0,(unit,),mover)
            call(0x416430,(1,),obj)
            for identity,(unit,mover,nav,mission) in enumerate(actors,1):
                flags=read('H',mover+0x36)[0];nf=read('B',nav+0x114)[0];head=p.u32(unit+0x60)
                kind=read('B',head+4)[0] if head else 0
                expected=[*read('3i',unit+0x68),read('i',mover+0x20)[0],read('H',unit+0x7e)[0],read('H',unit+0x80)[0],read('H',unit+0x7c)[0],flags&0x1800,2 if flags&4 else 1 if flags&8 else 0,p.u32(mover+0x2c),(flags>>5)&7,p.u32(mover+0x30),p.u32(0x64186c),(flags>>8)&7,int(bool(nf&2))]
                expected+=([kind,read('B',head+5)[0],p.u32(head+6),p.u32(head+10),p.u32(head+0x6a),p.u32(head+0x5a),p.u32(head+0x4e) if kind==28 else 0] if head else [0]*7)+[p.u32(unit+0xd0),int(bool(p.u32(nav+4)))]
                actual=json.loads(next(port))['result']
                if actual!=expected:
                    failure={'tick':clock,'unit':identity,'retail':expected,'world':actual}
                    (args.output/'mismatch.json').write_text(json.dumps(failure,indent=2))
                    raise AssertionError(failure)
                state=stats[identity-1];position=(expected[0],expected[2])
                blocked=position==state['previous'] and expected[3]>6553
                state['blocked_ticks']+=int(blocked);state['run']=state['run']+1 if blocked else 0
                state['longest_blocked_run']=max(state['longest_blocked_run'],state['run']);state['previous']=position
                if kind==44 and state['arrival_tick'] is None:state['arrival_tick']=clock
            if clock%100==0:print(f'PASS through tick {clock}; settled={sum(s["arrival_tick"] is not None for s in stats)}',flush=True)
        if port.read():raise AssertionError('extra World output')
    summary={'units':args.count,'then':args.then,'blockers':args.blocker,'movers':args.mover,'world_tick':args.world_tick,'start':list(args.start),'ticks':args.rounds,'spacing_cells':args.spacing,'goal':[gx,gz],'balance':args.balance,'all_fields_match':True,'units_stationary_with_retained_speed':sum(s['blocked_ticks']>0 for s in stats),'maximum_blocked_ticks':max(s['longest_blocked_run'] for s in stats),'settled':sum(s['arrival_tick'] is not None for s in stats),'units_detail':stats}
    (args.output/'summary.json').write_text(json.dumps(summary,indent=2))
    print('PASS',json.dumps({k:v for k,v in summary.items() if k!='units_detail'}),flush=True)


if __name__=='__main__':main()
