#!/usr/bin/env python3
"""Compare repeated inactive mover/height updates with independently loaded World.

The original 4dc800 mover and 51b2a0 height dispatcher execute in captured memory.
Navigator tick/active/formation queries are controlled external inputs. Optional
facing targets are supplied identically to both implementations; missions, combat, scripts and other units' AI do not advance.
The captured CRT thread is resolved without changing its RNG, and the bobbing
clock is supplied explicitly. Periodic speed inputs restart coasting without
resetting position, occupancy,
height, pitch or refusal state. No native result is supplied to World.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from balance_inputs import set_balance_inputs
from emureload import CapturedProcess, initial_fixture
from check_captured_tick import find_crt_thread


def main(travel=False,navigation=False,description=None):
    ap=argparse.ArgumentParser(description=description or __doc__)
    ap.add_argument('capture',type=Path)
    ap.add_argument('--fixture',type=Path,required=True)
    ap.add_argument('--save',type=Path,required=True)
    ap.add_argument('--balance',choices=['standard','crusades'],required=True)
    ap.add_argument('--retail-root',type=Path,default=Path('assets/game'))
    ap.add_argument('--runner',type=Path,default=Path('build-dbg/retail_replay_probe'))
    ap.add_argument('--face',action='store_true',help='supply a fixed facing target to each inactive mover')
    ap.add_argument('--rounds',type=int,default=160)
    ap.add_argument('--owner-view',action='store_true',help='supply each mover owner as the native exploration query context')
    ap.add_argument('--exploration-pattern',choices=['captured','hidden','player-stripes'],default='captured',
                    help='controlled initial exploration input for navigation comparisons')
    args=ap.parse_args()
    if (travel or navigation) and args.face: ap.error('travel and inactive facing are separate inputs')
    if args.owner_view and not navigation: ap.error('owner-view applies to navigation comparisons')
    if args.exploration_pattern!='captured' and not navigation: ap.error('exploration patterns apply to navigation comparisons')
    if args.rounds<1: ap.error('positive rounds required')
    capture=json.loads(args.capture.read_text());p=CapturedProcess(capture)
    selected=set_balance_inputs(p,capture,args.save,args.retail_root,args.balance=='crusades',surface=True,motion=True)
    addresses={u['id']:u['address'] for u in p.runtime['units']}
    def read(fmt,address): return struct.unpack('<'+fmt,p.uc.mem_read(address,struct.calcsize('<'+fmt)))
    def write(fmt,address,*values): p.uc.mem_write(address,struct.pack('<'+fmt,*values))
    subjects=[u for u in p.frame['units'] if u['id'] in selected and
              not p.u32(addresses[u['id']]+0xa8) and
              p.u32(addresses[u['id']]+0x130)&3==1]
    if not subjects: raise ValueError('no unattached surface subjects')
    fixture=initial_fixture(args.fixture.read_text().splitlines(),p.frame['tick']);header=fixture[0].split()
    header[1]='37' if navigation else '36';header[5]='0';header[-1]=str(int(args.balance=='crusades'))
    fixture[0]=' '.join(header);fixture.append(str(len(p.frame['units'])))
    for u in p.frame['units']:
        pointer=addresses[u['id']];mover=u.get('mover_address',0)
        phase=read('H',pointer+0x82)[0];stamp=p.u32(mover+0x2c) if mover else 0
        pitch=read('H',pointer+0x80)[0];roll=read('H',pointer+0x7c)[0]
        fixture.append(f'{u["id"]} {u["position_raw"][1]} {stamp} {phase} {pitch} {roll}')
    authored={}
    request_count=0
    if navigation:
        width,height=read('2I',p.game+0x19e98);width//=2;height//=2
        fixture.append(f'{width} {height} {-1 if args.owner_view else read("B",p.game+0x306f)[0]}')
        masks=read('H'*(width*height),p.u32(p.game+0x19ef4))
        if args.exploration_pattern!='captured':
            masks=[0 if args.exploration_pattern=='hidden' else 1<<((x//8+z//8)%10)
                   for z in range(height) for x in range(width)]
            write('H'*len(masks),p.u32(p.game+0x19ef4),*masks)
        fixture.append(' '.join(map(str,masks)));fixture.append(str(len(p.frame['units'])))
        for u in p.frame['units']:
            cache=read('hhihBB',addresses[u['id']]+0x98)
            fixture.append(' '.join(map(str,[u['id'],*cache])))
        for u in subjects:
            x,_,z=u['position_raw'];x=x//65536;z=z//65536
            authored[u['id']]=[(x,z),(x+48,z),(x+48,z-48),(x+96,z-48)]
        # Controller acceptance and formation are controlled external inputs.
        controller=p.brk;p.brk+=4096;table=controller+64;accept=controller+128
        p.put(controller,struct.pack('<I',table));p.put(table+0x10,struct.pack('<I',accept));p.ensure(accept,1)
        p.icd.hooks[accept]=lambda uc,a:(1,0)
        def submit(uc,a):
            nonlocal request_count
            request_count+=1
            from unicorn.x86_const import UC_X86_REG_ECX
            nav=uc.reg_read(UC_X86_REG_ECX)
            write('B',nav+0x114,read('B',nav+0x114)[0]|2)
            return 1,0
        p.icd.hooks[0x4e4f50]=submit
    points=[]
    def navigator_points(uc,args):
        output,count=read('2I',args)
        if count not in (2,3): raise ValueError(('unexpected navigator point count',count))
        write('i'*3*count,output,*(value for x,z in points[:count] for value in (x,0,z)))
        return 2,output
    for u in subjects:
        pointer=addresses[u['id']];mover=u['mover_address']
        # Both sides start with clean height state and an inactive controller.
        write('I',pointer+0x130,p.u32(pointer+0x130)&~0x4000)
        write('H',mover+0x36,read('H',mover+0x36)[0]&~0x10)
        vtable=p.u32(p.u32(mover))
        for offset in ((0x34,) if navigation else (8,0x14,0x34)):
            p.icd.hooks[p.u32(vtable+offset)]=lambda uc,a:(0,0)
        if navigation:
            nav=p.u32(mover);route=authored[u['id']]
            write('I',nav+4,controller);write('I',nav+8,pointer)
            write('h'*len(route)*2,nav+0xc,*(v for point in route for v in point))
            write('I',nav+0x10c,len(route));write('I',nav+0x110,p.frame['tick']);write('B',nav+0x114,1)
            write('B',pointer+0x134,0);write('I',mover+0x30,0)
            write('H',mover+0x36,read('H',mover+0x36)[0]&~0xe0)
        if travel:
            p.icd.hooks[p.u32(vtable+0x14)]=lambda uc,a:(0,1)
            p.icd.hooks[p.u32(vtable+0xc)]=navigator_points
            # Hold scan scheduling as a controlled input; segment delivery is
            # external here. Steering, speed, placement and height stay native.
            write('I',mover+0x30,0xffffffff)
    thread=find_crt_thread(p,capture['rng_calls'][0]['registers']['ebp'])
    p.icd.hooks[0x5dc403]=lambda uc,a:(0,thread)
    clock=0
    p.icd.hooks[0x53ff20]=lambda uc,a:(0,clock)
    p.icd.freeze_hooks()
    facing_targets={u['id']:(read('H',addresses[u['id']]+0x7e)[0]+16384)&65535 for u in subjects}
    rows,expected=[],[];counts=Counter();moving_by_class=Counter();refusals_by_class=Counter();moving=refusals=height_changes=0
    advances=exhausted=0;speed_modes=Counter()
    if navigation:
        for u in subjects:
            route=authored[u['id']]
            rows.append(f'{u["id"]} 0 {p.frame["tick"]} {len(route)} '+
                        ' '.join(str(v*65536) for point in route for v in point))
    for round_number in range(args.rounds):
        tick=p.frame['tick']+round_number+1;clock=tick
        write('I',p.game+0x19f44,tick)
        for u in subjects:
            pointer=addresses[u['id']];mover=u['mover_address']
            if args.owner_view: write('B',p.game+0x306f,read('B',pointer+0xfd)[0])
            # An explicit new speed every 16 rounds; retain all resulting state.
            speed=read('i',pointer+0x12b)[0] if not (travel or navigation) and round_number%16==0 else -1
            if speed>=0: write('i',mover+0x20,speed)
            facing=facing_targets[u['id']] if args.face else -1
            if facing>=0:
                write('H',mover+0x36,read('H',mover+0x36)[0]|0x10)
                write('H',mover+0x34,facing)
            if travel:
                # Authored, time-switched segments depend only on initial input,
                # never on a native future position or query result.
                x,_,z=u['position_raw'];x=x//65536*65536;z=z//65536*65536
                size=(64+32*(u['id']%3))*65536
                route=[(x,z),(x+size,z),(x+size,z-size),(x,z-size)]
                leg=round_number//40%4
                points=[route[(leg+i)%4] for i in range(3)]
                mode=round_number//64%3
                write('H',mover+0x36,(read('H',mover+0x36)[0]&~0xe0)|(mode<<5))
            before=read('3i',pointer+0x68)
            before_count=p.u32(p.u32(mover)+0x10c) if navigation else 0
            for routine in (0x4dc800,0x51b2a0):
                _,error=p.icd.call(routine,(pointer,),ecx=mover)
                if error or p.missing: raise RuntimeError((round_number,u['id'],hex(routine),error,p.missing))
            xyz=list(read('3i',pointer+0x68));flags=read('H',mover+0x36)[0]
            streak=2 if flags&4 else 1 if flags&8 else 0
            heading=read('H',pointer+0x7e)[0];pitch=read('H',pointer+0x80)[0];roll=read('H',pointer+0x7c)[0]
            expected.append(xyz+[read('i',mover+0x20)[0],heading,pitch,roll,flags&0x1800,streak,p.u32(mover+0x2c)])
            if navigation:
                point_count=p.u32(p.u32(mover)+0x10c)
                advances+=before_count-point_count;exhausted+=point_count<2
                speed_modes[(flags>>8)&7]+=1
                expected[-1]+=[point_count,(flags>>5)&7,p.u32(mover+0x30),p.u32(0x64186c),(flags>>8)&7]
                rows.append(f'{u["id"]} {tick} {clock} 0')
            else:
                rows.append(f'{u["id"]} {tick} {clock} {mode} '+ ' '.join(str(value) for point in points for value in point)
                            if travel else f'{u["id"]} {tick} {speed} {clock} {facing}')
            name=bytes(p.uc.mem_read(p.u32(selected[u['id']]),64)).split(b'\0')[0].decode('ascii')
            counts[name]+=1;changed=before[0]!=xyz[0] or before[2]!=xyz[2]
            moving+=changed;moving_by_class[name]+=changed;refusals_by_class[name]+=bool(streak)
            height_changes+=before[1]!=xyz[1];refusals+=bool(streak)
    with tempfile.TemporaryDirectory(prefix='tak-ground-stop-') as temp:
        root=Path(temp);(root/'input').write_text('\n'.join(fixture)+'\n');(root/'steps').write_text('\n'.join(rows)+'\n')
        subprocess.run([str(args.runner),str(args.retail_root),str(root/'input'),str(root/'output'),
                        '--ground-navigation-steps' if navigation else '--ground-travel-steps' if travel else '--ground-brake-steps',str(root/'steps')],check=True)
        actual=[e['result'] for line in (root/'output').read_text().splitlines()
                if (e:=json.loads(line))['kind']==('ground_navigation_step' if navigation else 'ground_travel_step' if travel else 'ground_brake_step')]
    if len(actual)!=len(expected): raise AssertionError(('count',len(actual),len(expected)))
    result_rows=rows[len(subjects):] if navigation else rows
    for i,(row,want,got) in enumerate(zip(result_rows,expected,actual)):
        if want!=got: raise AssertionError((i,row,'retail',want,'World',got))
    print(f'PASS: {len(expected)} sequential {"navigation" if navigation else "active" if travel else "inactive"} mover/height updates, {len(subjects)} units, '
          f'balance={args.balance}, facing={args.face}, moving={moving}, refusal-state={refusals}, height-changes={height_changes}; {dict(counts)}; '
          f'moving-by-class={dict(moving_by_class)}; refusal-state-by-class={dict(refusals_by_class)}')
    if navigation: print(f'Navigator coverage: {advances} consumed points, {exhausted} exhausted-route observations, {request_count} native submission calls; '
                         f'owner-view={args.owner_view}, exploration={args.exploration_pattern}, speed-modes={dict(speed_modes)}')


if __name__=='__main__': main()
