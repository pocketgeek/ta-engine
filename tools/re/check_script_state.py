#!/usr/bin/env python3
"""Execute original saved-script restore and updates against the integer port.

Queries and unit-value writes use a controlled host; this checks the complete
thread/animation state, not world placement or all engine query semantics.
"""
import argparse
import os
import tempfile
from pathlib import Path
import struct
import subprocess
from emu import Icd,HEAP
from unicorn.x86_const import UC_X86_REG_ESP


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('cob',type=Path); ap.add_argument('state',type=Path)
    ap.add_argument('--binary',default='build-dbg/retail_script_test')
    ap.add_argument('--ticks',type=int,default=100)
    ap.add_argument('--notify',help='start this script immediately before timed updates')
    ap.add_argument('--timeline',action='store_true',help='exercise production, flight, combat, wind, cloak and death callbacks')
    ap.add_argument('--zhon-flight-timeline',action='store_true',
                    help='join the bounded native Zonhunt construction movement callbacks to COB state')
    ap.add_argument('--profile',type=int,choices=range(4),help='controlled idle, road movement, water movement, or damaged veteran host')
    args=ap.parse_args()
    if args.timeline and args.zhon_flight_timeline:
        ap.error('--timeline and --zhon-flight-timeline are separate callback schedules')
    if args.zhon_flight_timeline:
        if args.cob.stem.lower()!='zonhunt':
            ap.error('--zhon-flight-timeline requires zonhunt.cob')
        args.notify=args.notify or 'Create'
        if args.ticks<1 or args.ticks>10000:
            ap.error('--zhon-flight-timeline requires 1..10000 ticks')
    data=args.cob.read_bytes(); saved=args.state.read_bytes()
    h=struct.unpack_from('<10I',data); _,ns,np,nc,nv,_,index,_,_,off=h
    p=Icd(); vm,desc,code,entries,statics,pieces,vtable,scratch=[HEAP+n*0x10000 for n in range(8)]
    # Large dragon/centaur scripts exceed 64 KiB; keep bytecode away from
    # entry tables and restored piece state in the emulated heap.
    code=HEAP+0x100000
    entries=HEAP+0x200000
    if nc*4>0x100000:raise ValueError("script exceeds oracle code allocation")
    put=lambda a,v:p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    get=lambda a:struct.unpack('<I',p.uc.mem_read(a,4))[0]
    pose=[[0]*6 for _ in range(np)]; cursor=0; writes=[]
    def read(uc,sp):
        nonlocal cursor
        dest,size=get(sp),get(sp+4); part=saved[cursor:cursor+size]; cursor+=len(part)
        if part: uc.mem_write(dest,part)
        return 2,len(part)
    def seek(uc,sp):
        nonlocal cursor
        cursor=get(sp); return 1,0
    def write_pose(base):
        def call(uc,sp):
            pose[get(sp)][base+get(sp+4)]=get(sp+8); return 3,0
        return call
    def read_pose(base):
        return lambda uc,sp:(2,pose[get(sp)][base+get(sp+4)])
    def set_value(uc,sp):
        writes.extend(struct.unpack('<2i',uc.mem_read(sp,8))); return 2,0
    def query(uc,sp):
        key=get(sp)
        if args.profile is None:return 5,1 if key==18 else 100
        mode=args.profile
        return 5,{4:25 if mode==3 else 100,18:1,29:100 if mode in (1,2) else 0,
                  28:int(mode==2),34:int(mode==1),32:10 if mode==3 else 0,
                  33:100 if mode==1 else 0,46:int(mode==3)}.get(key,0)
    callbacks={0:write_pose(0),4:write_pose(3),8:lambda uc,sp:(2,0),
               12:lambda uc,sp:(2,0),16:lambda uc,sp:(2,0),20:lambda uc,sp:(2,0),
               24:read_pose(0),28:read_pose(3),44:lambda uc,sp:(2,0),
               48:lambda uc,sp:(2,0),52:lambda uc,sp:(2,0),56:lambda uc,sp:(2,get(sp+4)),
               80:set_value,84:query}
    for i,(offset,callback) in enumerate(callbacks.items()):
        address=0x56a000+i*16; put(vtable+offset,address); p.hooks[address]=callback
    p.hooks[0x5359a0]=lambda uc,sp:(0,len(saved))
    p.hooks[0x5359c0]=seek; p.hooks[0x535a30]=read
    p.hooks[0x5ba3d0]=lambda uc,sp:(0,scratch)
    p.hooks[0x5ba5d0]=lambda uc,sp:(0,0)
    p.freeze_hooks()
    put(vm,vtable); put(vm+4,30); put(vm+0xc,desc); put(vm+0x10,struct.unpack_from('<I',saved)[0])
    put(vm+0x14,statics); put(vm+0x18,pieces)
    put(desc+0x2c,HEAP+0xa0000) # controlled sound names/results, display effects excluded
    put(desc+4,ns); put(desc+8,np); put(desc+0x10,nv); put(desc+0x18,entries); put(desc+0x24,code)
    p.uc.mem_write(code,data[off:off+nc*4]); p.uc.mem_write(entries,data[index:index+ns*4])
    result,error=p.call(0x56dc00,(HEAP+0x90000,),ecx=vm)
    if error or result!=1: raise RuntimeError(error or result)
    put(0x64186c,1); expected=[]; ready=[]
    notification=None
    if args.notify:
        names=[]
        for i in range(ns):
            name_offset=struct.unpack_from('<I',data,h[7]+4*i)[0]
            names.append(data[name_offset:].split(b'\0',1)[0].decode('ascii').lower())
        notification=names.index(args.notify.lower())
    events=[]
    if args.timeline:
        if notification is None:raise ValueError('--timeline requires --notify Create')
        for tick,name,values in [(10,'SetMaxReloadTime',[1000]),(30,'Activate',[]),
                (30,'BeginFlight',[]),(30,'setSFXoccupy',[5]),(150,'StartBuilding',[]),
                (300,'StopBuilding',[]),(300,'MoveRate',[2]),(300,'TurnDirection',[5]),
                (450,'AimWeapon',[0,0,0,0]),(450,'FireWeapon',[0]),(600,'TargetCleared',[0]),
                (600,'WindChange',[10,8192]),(750,'StartCloaking',[]),(900,'StopCloaking',[]),
                (900,'BeginLanding',[]),(900,'setSFXoccupy',[0]),(1050,'Deactivate',[]),
                (1200,'Killed',[50,0,1]),(1200,'Dying',[1])]:
            if name.lower() in names:events.append((tick,names.index(name.lower()),len(values),values+[0]*(4-len(values))))
    if args.zhon_flight_timeline:
        from check_zhon_construction_flight_trace import native_persistent_trace
        metadata,_movement,_phases,callback_rows=native_persistent_trace(args.ticks,1)
        if metadata['terrain']!=100:
            raise AssertionError(('Zhonhunt flight fixture terrain changed',metadata['terrain']))
        for required in ('BeginFlight','StartBuilding'):
            if required.lower() not in names:
                raise AssertionError(('zonhunt callback is missing',required))
        by_tick=dict(callback_rows)
        for one_based_tick in range(1,args.ticks+1):
            zero_based_tick=one_based_tick-1
            if one_based_tick==1:
                events.append((zero_based_tick,names.index('beginflight'),0,[0,0,0,0]))
            for name,values in by_tick.get(one_based_tick,()):
                script=names.index(name.lower()) if name.lower() in names else None
                if script is not None:
                    args_values=list(values)
                    events.append((zero_based_tick,script,len(args_values),
                                   args_values+[0]*(4-len(args_values))))
            if one_based_tick==1:
                events.append((zero_based_tick,names.index('startbuilding'),0,[0,0,0,0]))
    for tick in range(-1 if notification is not None else 0,args.ticks):
        writes.clear()
        if tick<0: _,error=p.call(0x56c5f0,(notification,0,1),ecx=vm)
        else:
            for when,script,count,values in events:
                if when==tick:
                    _,error=p.call(0x56c680,(script,0,1,count,*values),ecx=vm)
                    if error:raise RuntimeError(error)
            _,error=p.call(0x56c870,(1,),ecx=vm)
        if error: raise RuntimeError(f"{error}; return={get(p.uc.reg_read(UC_X86_REG_ESP)):#x}")
        row=[get(vm+0xa60),get(0x64186c),*[get(statics+n*4) for n in range(nv)],
             *struct.unpack('<656I',p.uc.mem_read(vm+0x20,16*0xa4))]
        for piece in range(np):
            row += [*struct.unpack('<19I',p.uc.mem_read(pieces+piece*76,76)),*pose[piece]]
        expected.append(row+[len(writes),*writes])
        if any(writes[n:n+2]==[5,1] for n in range(0,len(writes),2)): ready.append(tick+1)
    command=[args.binary,'--state-start' if notification is not None else '--state',str(args.cob),str(args.state),str(args.ticks)]
    if notification is not None:command.append(str(notification))
    env=os.environ.copy()
    if args.profile is not None:env["TAK_SCRIPT_ORACLE_PROFILE"]=str(args.profile)
    with tempfile.NamedTemporaryFile(mode='w',prefix='tak-animation-events-') as timeline:
        if args.timeline or args.zhon_flight_timeline:
            for tick,script,count,values in events:
                timeline.write(' '.join(map(str,[tick,script,count,*values]))+'\n')
            timeline.flush();command.append(timeline.name)
        proc=subprocess.run(command,env=env,text=True,capture_output=True,check=True)
    actual=[list(map(int,row.split())) for row in proc.stdout.splitlines()]
    if len(actual)!=len(expected): raise AssertionError('row count mismatch')
    if os.environ.get('TAK_SCRIPT_ORACLE_DUMP'):
        import json
        Path(os.environ['TAK_SCRIPT_ORACLE_DUMP']).write_text(json.dumps(dict(expected=expected,actual=actual)))
    for tick,(want,got) in enumerate(zip(expected,actual)):
        if want!=got:
            index=next(i for i,pair in enumerate(zip(want,got)) if pair[0]!=pair[1])
            raise AssertionError(('tick',tick+1,'word',index,'retail',want[index],'port',got[index]))
    if args.zhon_flight_timeline:
        print(f'PASS: {len(expected)} Zonhunt script-state ticks match retail with native 41ef00/4dc800 '
              f'construction callbacks at their movement ticks; build-ready updates {ready}')
    else:
        print(f'PASS: {len(expected)} restored script thread/animation/RNG boundaries; build-ready updates {ready}')


if __name__=='__main__': main()
