#!/usr/bin/env python3
"""Compare independently evolving retail/World gate traversal and asynchronous search.

The captured VERPULT mover supplies native type/vtable inputs, relocated into an
otherwise empty authored flat corridor. Both sides start with the same motion
state, source gate assets and fresh search worker. Gate policy runs before script,
movement/height and search, matching World's chosen AI cadence. Formation is
disabled; other AI, combat and render cadence are not advanced. With --missions,
both mission dispatchers advance through point arrival and ground standby;
otherwise the initial move mission remains attached throughout the comparison.
--return-trip supplies a second initial point mission. --replace-at clears the
native queue and constructs a replacement at the specified tick, paired with
World.order; retail input/network command decoding is outside this comparison.
--pair shares a worker/cache between a gate crossing and a separate approach-side
move. --pair-close instead checks a crowded convoy's native stopped-follower outcome.
--park dispatches a complete ground PARK mission using the gate as its target.
Optional target-loss and queued-point inputs must precede PARK completion.
These runs check ring arrival, retirement and standby, rather than gate crossing.
--rectangle runs original MobileBuild to its placement handoff. The mover’s
MaybeBuilding notification is a script host boundary; its script is disabled in
World too. Placement/allocation is not advanced. --rectangle-blocked adds a
sealed wall and requires an out-of-reach abort; --rectangle-reachable supplies
build reach 1024 to require a placement handoff after the same failed route.
Gate command construction/dispatch are observation boundaries: checked commands
are applied through the original active-bit setter before the native script tick.
"""
import argparse
import json
import os
import struct
import subprocess
import tempfile
from pathlib import Path

from emu import HEAP
from emureload import CapturedProcess
from native_gate import NativeGate
from balance_inputs import set_balance_inputs
from check_captured_tick import find_crt_thread
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_ESP, UC_X86_REG_EIP


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture',type=Path)
    parser.add_argument('--save',type=Path,required=True)
    parser.add_argument('--root',type=Path,default=Path('assets/game'))
    parser.add_argument('--gate',choices=('arangate','tarngate','verngate','cregate'),default='arangate')
    parser.add_argument('--balance',choices=('standard','crusades'),required=True)
    parser.add_argument('--rounds',type=int,default=1200)
    parser.add_argument('--binary',default='build-dbg/retail_replay_probe')
    parser.add_argument('--diagnostic-dir',type=Path)
    parser.add_argument('--missions',action='store_true',help='advance the original and World mission dispatchers')
    parser.add_argument('--return-trip',action='store_true',help='queue a second point move back through the gate; requires --missions')
    parser.add_argument('--replace-at',type=int,help='replace the move at this tick; requires --missions')
    parser.add_argument('--replace-goal',type=int,nargs=2,metavar=('X','Z'),help='replacement destination; defaults to the original point')
    parser.add_argument('--pair',action='store_true',help='two independently moving requesters; requires --missions')
    parser.add_argument('--rectangle-reachable',action='store_true',help='give the blocked builder enough reach to proceed to placement')
    parser.add_argument('--rectangle-blocked',action='store_true',help='seal a wall before the build perimeter; requires --rectangle')
    parser.add_argument('--rectangle',action='store_true',help='run MobileBuild through its handoff to placement')
    parser.add_argument('--park',action='store_true',help='dispatch a PARK ring around the gate instead of a point move')
    parser.add_argument('--park-target-loss-at',type=int,default=0,help='supply target-loss event 8 at this PARK tick')
    parser.add_argument('--park-follow-at',type=int,default=0,help='append an ordinary move at this PARK tick')
    parser.add_argument('--ramp',action='store_true',help='authored relief beyond the gate: a bad-slope then good-slope ramp lane between impassable cliffs up to the destination plateau')
    parser.add_argument('--pair-close',action='store_true',help='closely spaced convoy; expects native expanded-radius arrival short of the requested point')
    args=parser.parse_args()
    for value in (args.park_target_loss_at,args.park_follow_at):
        if value and (not args.park or not 1<=value<=args.rounds): parser.error('PARK event ticks require --park and must lie within the run')
    if args.rectangle_reachable and not args.rectangle_blocked: parser.error('--rectangle-reachable requires --rectangle-blocked')
    if args.rectangle_blocked and not args.rectangle: parser.error('--rectangle-blocked requires --rectangle')
    if args.rectangle and (not args.missions or args.park or args.pair or args.return_trip or args.replace_at is not None):
        parser.error('--rectangle requires --missions and cannot combine with other order variants')
    if args.park and (not args.missions or args.pair or args.return_trip or args.replace_at is not None):
        parser.error('--park requires --missions and cannot combine with other order variants')
    if args.pair_close and not args.pair: parser.error('--pair-close requires --pair')
    if args.pair and (not args.missions or args.return_trip or args.replace_at is not None):
        parser.error('--pair requires --missions and cannot combine with queued/replacement scenarios')
    if args.replace_goal is not None and (args.replace_at is None or any(not 32<=v<=976 for v in args.replace_goal)):
        parser.error('--replace-goal requires --replace-at and coordinates within 32..976')
    replacement_goal=args.replace_goal or (512,696)
    if args.return_trip and not args.missions: parser.error('--return-trip requires --missions')
    if args.replace_at is not None and (not args.missions or args.return_trip or not 1<=args.replace_at<=args.rounds):
        parser.error('--replace-at requires --missions, a tick within the run, and no --return-trip')
    if not 1<=args.rounds<=5000: parser.error('rounds must be 1..5000')
    capture=json.loads(args.capture.read_text())
    p=CapturedProcess(capture)
    selected=set_balance_inputs(p,capture,args.save,args.root,args.balance=='crusades',surface=True,motion=True)
    source=next(u['address'] for u in p.runtime['units'] if u['id']==625)
    fields=next(r for r in p.runtime['world_buffers'] if r['name']=='game_fields')
    g=NativeGate(args.root,args.gate,icd=p.icd,game_fields=bytes(p.uc.mem_read(p.game,fields['size'])),freeze=False)
    p.game=g.game
    if args.park:
        # The gate VM fixture formerly needed only footprint coordinates.
        # A PARK target also reads its world position and type footprint.
        p.put(g.gate+0x68,struct.pack('<3i',512*65536,0,512*65536))
        p.put(g.kind+0x126,struct.pack('<2h',g.fx,g.fz))
    read=lambda fmt,a:struct.unpack('<'+fmt,p.uc.mem_read(a,struct.calcsize('<'+fmt)))
    def write(fmt,a,*v):p.put(a,struct.pack('<'+fmt,*v))
    unit=g.pool+624
    p.put(unit,bytes(p.uc.mem_read(source,312)))
    mover=p.u32(unit+8);nav=p.u32(mover);grid=selected[625]
    if args.rectangle_reachable: write('H',p.u32(unit+0xb4)+0x230,1024)
    write('I',unit+0xa4,0);write('I',unit+0xb0,0)
    for oldunit in p.runtime['units']: write('I',oldunit['address']+0x130,p.u32(oldunit['address']+0x130)&~0x1000000)
    write('H',unit+2,2);write('B',unit+0xfd,0);write('I',unit+0x60,0);write('I',unit+0xa8,0)
    write('3i',unit+0x68,512*65536,0,344*65536)
    write('2h',unit+0x74,30,20);write('2h',unit+0x126,30,20)
    write('4H',unit+0x7c,0,0,0,0)
    write('I',unit+0x130,p.u32(unit+0x130)&~0x4000)
    write('B',unit+0x134,0)
    if args.missions: write('I',unit+0xd0,0)
    write('I',nav+8,unit);write('I',nav+4,0);write('I',nav+0x10c,0);write('I',nav+0x110,0);write('B',nav+0x114,0)
    write('5I',mover+0x20,0,0,0,0,0);write('H',mover+0x36,1)
    owner=g.game+0x2404
    p.put(owner,bytes(p.uc.mem_read(p.u32(source+0xb8),0x110)))
    write('I',owner,1);write('B',owner+0xea,2);write('B',owner+0xeb,0)
    write('B',owner+0xe3,1)  # 4f6379: every non-campaign player is in the 5x budget class
    write('2I',owner+0x74,g.gate,unit);write('I',unit+0xb8,owner)
    for n in range(1,10):write('I',g.game+0x2404+n*0x110,0)
    write('I',g.game+0x14e88,unit);write('I',p.u32(p.u32(0x62d558)+8)+12,2)
    write('B',g.game+0x19ef8,0);write('B',g.game+0x306f,0)
    plane,visibility,obj,point,mission,ai=[HEAP+n for n in (0x110000,0x111000,0x112000,0x113000,0x114000,0x115000)]
    write('I',ai,owner);write('B',ai+0x1a5,1);write('I',owner+0x80,ai)
    old=p.u32(grid)
    for address in range(0x62dbf0,0x634670,0x354):write('I',address,0)
    write('I',grid,old);write('2I',grid+0x340,64,64);write('3I',grid+0x348,plane,0,0);write('I',grid+0x33c,0)
    p.put(visibility,struct.pack('<H',0xffff)*1024);write('I',g.game+0x19ef4,visibility)
    for x in range(64):
        if not g.x<=x<g.x+g.fx:write('H',g.cells+(g.z*64+x)*14+8,0xfffc)
    if args.rectangle_blocked:
        for x in range(64): write('H',g.cells+(40*64+x)*14+8,0xfffc)
    heights=bytearray(64*64)
    if args.ramp:
        # Rows 35..39 climb 10 per cell (GROUND4/5 bad slope, grade 4), rows
        # 40..42 climb 5 (grade 6) inside the x=24..39 lane; elsewhere rows
        # 35..37 climb 22 per cell (above the hard slope limit). Both sides
        # receive these heights; retail derives grades/height from the cells.
        for z in range(64):
            for x in range(64):
                if 24<=x<40: h=10*min(max(z-34,0),5)+5*min(max(z-39,0),3)
                else: h=22*min(max(z-34,0),3)
                heights[z*64+x]=h
        for z in range(64):
            for x in range(64):
                corners=[heights[min(z+dz,63)*64+min(x+dx,63)] for dz in (0,1) for dx in (0,1)]
                p.uc.mem_write(g.cells+(z*64+x)*14+4,bytes((heights[z*64+x],max(corners),min(corners))))
    # World installs retail's projected map-edge blockers with feature inputs.
    scenario=HEAP+0x11b000
    write('2I',g.game+0x19e88,1024,1024);write('I',g.game+0x175dc,scenario)
    write('I',scenario+0xd39,0);g.call(0x50eef0)
    # Stamp the authored moving body into its initial footprint.
    for z in range(20,24):
        for x in range(30,34):write('H',g.cells+(z*64+x)*14,2)
    if args.pair:
        follower=g.pool+3*312
        follower_mover,follower_nav,follower_mission=[HEAP+n for n in (0x118000,0x119000,0x11a000)]
        p.put(follower,bytes(p.uc.mem_read(unit,312)))
        p.put(follower_mover,bytes(p.uc.mem_read(mover,56)))
        p.put(follower_nav,bytes(p.uc.mem_read(nav,277)))
        write('H',follower+2,3);write('I',follower+8,follower_mover)
        write('I',follower_mover,follower_nav);write('I',follower_nav+8,follower)
        follower_z=232 if args.pair_close else 120
        follower_x=512 if args.pair_close else 192
        follower_cell_x=(follower_x-24)//16
        follower_cell=(follower_z-24)//16
        write('3i',follower+0x68,follower_x*65536,0,follower_z*65536)
        write('2h',follower+0x74,follower_cell_x,follower_cell);write('2h',follower+0x126,follower_cell_x,follower_cell)
        write('I',owner+0x78,follower);write('I',g.game+0x14e88,follower)
        write('I',p.u32(p.u32(0x62d558)+8)+12,3)
        for z in range(follower_cell,follower_cell+4):
            for x in range(follower_cell_x,follower_cell_x+4):write('H',g.cells+(z*64+x)*14,3)
    g.call(0x4e01d0,(0,64|(64<<16)),grid)
    write('10I',0x634674,*([0]*10));g.call(0x415f80,(),obj)
    write('I',g.game+0x19e70,obj);write('I',obj+0x225,503);write('I',obj+0x115,g.gate)
    build_kind=0
    if args.rectangle:
        table=p.u32(g.game+0x175c4)
        build_kind=next(i for i in range(1,p.u32(g.game+0x175b8)) if bytes(p.uc.mem_read(table+i*676+32,32)).split(b'\0')[0].decode().lower()==args.gate)
    write('3i',point,512*65536,0,(800 if args.rectangle else 792 if args.pair else 696)*65536)
    g.call(0x4d6c40,(27 if args.rectangle else 33 if args.park else 28,g.gate if args.park else 0,0 if args.park else point,build_kind,0,0,0,0,0,0,0,0,0),mission)
    if args.park: write('2I',mission+0x4e,64,1)
    write('I',mission+0xe,unit);write('I',unit+0x60,mission)
    actors=[(unit,mover,nav,mission)]
    if args.pair:
        write('3i',point,(512 if args.pair_close else 768)*65536,0,(696 if args.pair_close else 344)*65536)
        g.call(0x4d6c40,(28,0,point,0,0,0,0,0,0,0,0,0),follower_mission)
        write('I',follower_mission+0xe,follower);write('I',follower+0x60,follower_mission)
        actors.append((follower,follower_mover,follower_nav,follower_mission))
    if args.return_trip:
        second=HEAP+0x117000
        write('3i',point,512*65536,0,344*65536)
        g.call(0x4d6c40,(28,0,point,0,0,0,0,0,0,0,0,0),second)
        write('I',second+0xe,unit);write('I',mission+0x66,second)
    p.icd.hooks[p.u32(p.u32(nav)+0x34)]=lambda uc,a:(0,0)
    thread=find_crt_thread(p,capture['rng_calls'][0]['registers']['ebp'])
    p.icd.hooks[0x5dc403]=lambda uc,a:(0,thread)
    clock=0;p.icd.hooks[0x53ff20]=lambda uc,a:(0,clock)
    commands=[]
    token=HEAP+0x116000
    write('B',token,23)
    def order(uc,a):
        values=read('10I',a)
        assert values[0] in (0x6049dc,0x604a04), values
        assert values[1:]==(1,1,g.gate,0,0,0,0,1,0), values
        commands.append(int(values[0]==0x604a04))
        return 10,token
    def dispatch(uc,a):
        assert p.u32(a)&0xff==23
        return 1,0
    p.icd.hooks[0x4d4bf0]=order
    p.icd.hooks[0x4d7a30]=dispatch
    handoff=0;aborted=0
    if args.rectangle:
        def unreachable_notice(uc,a):
            assert read('2I',a)==(unit,0x604ed4), 'unexpected MobileBuild notice'
            return 2,0
        p.icd.hooks[0x4f5db0]=unreachable_notice
        def placement_boundary(uc,address,size,data):
            nonlocal handoff
            handoff=clock
            uc.reg_write(UC_X86_REG_EIP,0x6ffff000)
        p.uc.hook_add(UC_HOOK_CODE,placement_boundary,begin=0x405824,end=0x405824)
        builder_vm=p.u32(unit+0xbc)
        def builder_script(uc,address,size,data):
            if uc.reg_read(UC_X86_REG_ECX)!=builder_vm: return
            sp=uc.reg_read(UC_X86_REG_ESP)
            assert p.u32(sp)==0x405623, 'unexpected builder script boundary'
            uc.reg_write(UC_X86_REG_EAX,0)
            uc.reg_write(UC_X86_REG_ESP,sp+16)
            uc.reg_write(UC_X86_REG_EIP,p.u32(sp))
        p.uc.hook_add(UC_HOOK_CODE,builder_script,begin=0x56c5c0,end=0x56c5c0)
    grades=[];query=[]
    deliveries=[]
    def delivered(uc,address,size,data):
        current=uc.reg_read(UC_X86_REG_ECX)
        for identity,(_,_,actor_nav,_) in enumerate(actors,2):
            if current==actor_nav: deliveries.append((clock,identity))
    p.uc.hook_add(UC_HOOK_CODE,delivered,begin=0x4e4ea0,end=0x4e4ea0)
    if args.diagnostic_dir:
        def grade_entry(uc,address,size,data):
            query[:]=read('2i',uc.reg_read(UC_X86_REG_ESP)+4)
        def grade_return(uc,address,size,data):
            value=uc.reg_read(UC_X86_REG_EAX)
            if value>=2**31: value-=2**32
            grades.append([clock,*query,value])
        p.uc.hook_add(UC_HOOK_CODE,grade_entry,begin=0x4139d0,end=0x4139d0)
        for address in (0x4139fe,0x413a51,0x413a9a,0x413ae1,0x413bb9,0x413bd7,0x413bf7,0x413c36,0x413c77):
            p.uc.hook_add(UC_HOOK_CODE,grade_return,begin=address,end=address)
    p.icd.freeze_hooks();write('I',0x64186c,1);write('I',g.game+0x19f44,0)
    if not args.park and not args.rectangle:
        for _,_,_,initial_mission in actors:g.call(0x4d4da0,(initial_mission+0x22,4),initial_mission)
    assert read('2h',unit+0x78)==(4,4), 'fixture requires the captured VERPULT footprint'
    expected=[];gate_states=[];ring_states=[]
    for clock in range(1,args.rounds+1):
        write('I',g.game+0x19f44,clock)
        if clock==args.replace_at:
            g.call(0x4d6a50,(unit,))
            replacement=HEAP+0x117000
            write('3i',point,replacement_goal[0]*65536,0,replacement_goal[1]*65536)
            g.call(0x4d6c40,(28,0,point,0,0,0,0,0,0,0,0,0),replacement)
            write('I',replacement+0xe,unit);write('I',unit+0x60,replacement)
        if clock==args.park_target_loss_at:
            assert read('B',p.u32(unit+0x60)+4)[0]==33, 'target-loss tick must precede PARK completion'
            write('I',unit+0xd0,p.u32(unit+0xd0)|8)
        if clock==args.park_follow_at:
            assert read('B',p.u32(unit+0x60)+4)[0]==33, 'following-move tick must precede PARK completion'
            following=HEAP+0x117000
            write('3i',point,512*65536,0,696*65536)
            g.call(0x4d6c40,(28,0,point,0,0,0,0,0,0,0,0,0),following)
            write('I',following+0xe,unit);write('I',p.u32(unit+0x60)+0x66,following)
        commands.clear();g.call(0x40a020,(g.gate,))
        for active in commands:g.call(0x51e4d0,(1,active),g.gate)
        g.call(0x56c870,(1,),g.vm)
        try:
            for unit,mover,nav,mission in actors:
                if args.missions: g.call(0x4d8450,(unit,))
                if handoff: break
                if args.park:
                    controller=p.u32(nav+4)
                    ring_states.append([clock,read('hhiii',controller+8) if controller and p.u32(controller)==0x5f290c else None,read('4h',nav+12)])
                g.call(0x4dc800,(unit,),mover);g.call(0x51b2a0,(unit,),mover)
            if not handoff: g.call(0x416430,(1,),obj)
        except Exception:
            print('MISSING',p.missing,flush=True);raise
        for unit,mover,nav,mission in actors:
            flags=read('H',mover+0x36)[0];nf=read('B',nav+0x114)[0];count=p.u32(nav+0x10c)
            expected.append([*read('3i',unit+0x68),read('i',mover+0x20)[0],read('H',unit+0x7e)[0],read('H',unit+0x80)[0],read('H',unit+0x7c)[0],flags&0x1800,2 if flags&4 else 1 if flags&8 else 0,p.u32(mover+0x2c),(flags>>5)&7,p.u32(mover+0x30),p.u32(0x64186c),(flags>>8)&7,int(bool(nf&2)),p.u32(mission+0x6a),nf&1,count,*read('h'*(count*2),nav+12),read('B',unit+0x134)[0]&15])
            if args.missions:
                head=p.u32(unit+0x60)
                kind=read('B',head+4)[0] if head else 0
                expected[-1]=expected[-1][:15]+([kind,read('B',head+5)[0],p.u32(head+6),p.u32(head+10),p.u32(head+0x6a),p.u32(head+0x5a),p.u32(head+0x4e) if kind==28 else 0] if head else [0]*7)+[p.u32(unit+0xd0),int(bool(p.u32(nav+4)))]
        gate_states.append(g.state())
        if args.rectangle and not handoff and expected[-1][15]!=27: aborted=clock
        if handoff or aborted: break

    mode='--gate-composed-rectangle-reachable' if args.rectangle_reachable else '--gate-composed-rectangle-blocked' if args.rectangle_blocked else '--gate-composed-rectangle' if args.rectangle else '--gate-composed-park' if args.park else '--gate-composed-pair-close' if args.pair_close else '--gate-composed-pair' if args.pair else '--gate-composed-replace' if args.replace_at is not None else '--gate-composed-queue' if args.return_trip else '--gate-composed-missions' if args.missions else '--gate-composed'
    command=[args.binary,str(args.root),mode,args.gate,str(int(args.balance=='crusades')),str(args.rounds),str(read('i',unit+0x12b)[0])]
    if args.park: command.extend(map(str,(args.park_target_loss_at,args.park_follow_at)))
    if args.replace_at is not None: command.extend(map(str,(args.replace_at,*replacement_goal)))
    environment=dict(os.environ,**({'TAK_SEARCH_DIAG':'1'} if args.diagnostic_dir else {}))
    with tempfile.NamedTemporaryFile(prefix='tak-gate-heights-') as relief:
        if args.ramp:
            relief.write(bytes(heights));relief.flush()
            environment['TAK_GATE_HEIGHTS']=relief.name
        result=subprocess.run(command,text=True,capture_output=True,check=True,env=environment)
    rows=[json.loads(line) for line in result.stdout.splitlines()]
    actual=[row['result'] for row in rows if row['kind']==('mission_movement' if args.missions else 'search_movement')]
    gates=[row['result'] for row in rows if row['kind']=='gate']
    if args.diagnostic_dir:
        args.diagnostic_dir.mkdir(parents=True,exist_ok=True)
        (args.diagnostic_dir/'native-rings.json').write_text(json.dumps(ring_states))
        (args.diagnostic_dir/'native-grades.json').write_text(json.dumps(grades))
        (args.diagnostic_dir/'native-rng.json').write_text(json.dumps(p.events))
        (args.diagnostic_dir/'native-deliveries.json').write_text(json.dumps(deliveries))
        (args.diagnostic_dir/'world-grades.log').write_text(result.stderr)
        for name,values in [('native-movement',expected),('world-movement',actual),('native-gate',gate_states),('world-gate',gates)]:
            (args.diagnostic_dir/(name+'.json')).write_text(json.dumps(values))
        world_grades=[list(map(int,line.split()[1:])) for line in result.stderr.splitlines() if line.startswith('GRADE ')]
        if grades!=world_grades:
            index=next((i for i,(a,b) in enumerate(zip(grades,world_grades)) if a!=b),min(len(grades),len(world_grades)))
            raise AssertionError(('grade query',index,grades[index:index+1],world_grades[index:index+1]))
    compared_rounds=handoff or aborted or args.rounds
    assert len(expected)==len(actual)==compared_rounds*len(actors)
    assert len(gate_states)==len(gates)==compared_rounds
    for n,(want,got) in enumerate(zip(expected,actual)):
        if want!=got:raise AssertionError(('movement',n//len(actors)+1,actors[n%len(actors)][0],want,got))
    for n,(wgate,ggate) in enumerate(zip(gate_states,gates),1):
        if wgate!=ggate:
            k=next(k for k,(a,b) in enumerate(zip(wgate,ggate)) if a!=b)
            raise AssertionError(('gate',n,k,wgate[k:k+6],ggate[k:k+6]))
    if args.rectangle:
        terminal='build_handoff' if handoff else 'build_abort'
        assert handoff or aborted, 'build approach did not finish'
        assert [row['tick'] for row in rows if row['kind']==terminal]==[compared_rounds], 'build handoff/abort tick differs'
        if args.rectangle_blocked:
            assert any(row[19]&0x200 for row in expected[:-1]), 'blocked approach never failed'
            assert (handoff if args.rectangle_reachable else aborted), 'failed approach did not respect build reach'
        else:
            assert handoff and any(row[19]&0x100 for row in expected[:-1]), 'rectangle arrival missing'
            assert any(2 in state[-g.fx*g.fz:] for state in gate_states), 'builder never crossed gate'
    elif not args.park:
        assert any(state[1] for state in gate_states), 'gate never opened'
        assert any(2 in state[-g.fx*g.fz:] for state in gate_states), 'mover never occupied the gate footprint'
        assert not gate_states[-1][1], 'gate did not close after traversal'
        final_x,final_z=(512,352) if args.return_trip else (512,704)
        if args.pair and not args.pair_close: final_x,final_z=768,352
        if args.replace_at is not None:
            final_x,final_z=(((value-24)//16)*16+32 for value in replacement_goal)
        if args.pair_close:
            assert any(row[15]==28 and row[19]&0x100 and row[21]>0 for row in expected[1::2]), 'follower did not accept an expanded goal radius'
            assert abs(expected[-1][2]-704*65536)>=16*65536, 'close convoy did not reproduce arrival short of the requested point'
        else:
            assert abs(expected[-1][0]-final_x*65536)<16*65536 and abs(expected[-1][2]-final_z*65536)<16*65536, 'mover did not arrive'
        if args.pair:
            assert {identity for _,identity in deliveries}=={2,3}, 'both requesters must receive native route deliveries'
            assert any(expected[n][14] and expected[n+1][14] for n in range(0,len(expected),2)), 'requests never overlap'
            assert expected[-2][15]==44 and abs(expected[-2][0]-512*65536)<16*65536 and abs(expected[-2][2]-800*65536)<16*65536, 'leader did not arrive and enter standby'
        if args.return_trip: assert any(state[2]>600*65536 for state in expected), 'mover skipped the outbound leg'
        if args.missions: assert expected[-1][15]==44, 'mover did not enter standby'
        if args.ramp:
            heights_seen={row[1]>>16 for row in expected[::2 if args.pair else 1]}
            assert max(heights_seen)>=60 and len(heights_seen)>20, ('ramp lane was not climbed',sorted(heights_seen))
            print(f'  ramp: {len(heights_seen)} distinct surface heights up to {max(heights_seen)}, '
                  f'{len({row[5] for row in expected})} distinct pitches')
    else:
        assert any(row[23] for row in expected), 'ring controller never installed'
        if not args.park_follow_at:
            assert any(row[15]==33 and row[19]&0x100 for row in expected), 'ring never reached'
        else:
            assert any(row[15]==28 for row in expected), 'following move never activated'
            assert abs(expected[-1][0]-512*65536)<16*65536 and abs(expected[-1][2]-704*65536)<16*65536, 'following move did not arrive'
        assert expected[-1][15]==44, 'PARK did not complete into standby'
        assert any(row[:3]!=expected[0][:3] for row in expected), 'PARK never moved'
    print(f'PASS: {compared_rounds} composed gate/search/movement updates, {args.gate}, {args.balance}; includes goal completion; mission dispatch={args.missions}, return trip={args.return_trip}, replacement tick={args.replace_at}, pair={args.pair}, close convoy={args.pair_close}, park={args.park}, target loss={args.park_target_loss_at}, following move={args.park_follow_at}, rectangle={args.rectangle}, blocked={args.rectangle_blocked}, reachable={args.rectangle_reachable}')


if __name__=='__main__':
    main()
