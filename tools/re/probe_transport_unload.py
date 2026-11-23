#!/usr/bin/env python3
"""Observe retail air/sea unload transfer/retry stages with controlled placement results.

This runs 41ae20 (air) or 408d50 (sea) with placement, effects, attachment/queue sinks and random draws
controlled by the harness. It compares PARK spacing with the C++ helper, but
does not claim World::tickTransport matches the complete native mission trace.
The observations expose exact-site/retry behavior the current spiral lacks.
"""
import argparse
import json
import math
import random as random_module
import subprocess
import struct
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ECX


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output',required=True)
    ap.add_argument('--binary',default='build-o2/transport_test')
    ap.add_argument('--sea',action='store_true')
    args=ap.parse_args()
    handler=0x408d50 if args.sea else 0x41ae20
    p=Icd()
    carrier,passenger,kind,mission,game=[HEAP+i*0x10000 for i in range(5)]
    def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    def read(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
    def byte(a,v):p.uc.mem_write(a,bytes([v]))
    trace=[];placement=[1,1]
    remaining=0;draws=[]
    def fits(uc,sp):
        typ,unit_id,cell,enabled,relaxed=struct.unpack('<5I',uc.mem_read(sp,20))
        assert typ==kind and (unit_id&65535)==0 and enabled==1 and relaxed in (0,1)
        trace.append(dict(call='placement',x=cell&65535,z=cell>>16,relaxed=relaxed))
        return 5,placement[relaxed]
    def wait(uc,sp):
        delay=read(sp);trace.append(dict(call='sleep',delay=delay));return 1,0
    def sound(uc,sp):trace.append(dict(call='sound'));return 3,0
    def effect(uc,sp):
        point,effect_id=struct.unpack('<2I',uc.mem_read(sp,8))
        trace.append(dict(call='effect',id=effect_id,position=list(struct.unpack('<3i',uc.mem_read(point,12)))))
        return 2,0
    def blocked(uc,sp):trace.append(dict(call='blocked_chatter'));return 2,0
    def position(uc,sp):return 4,0
    def detach(uc,sp):
        put(passenger+0xa8,0);put(carrier+0xac,passenger if remaining else 0);return 5,0
    def random(uc,sp):
        if read(sp)==6:return 1,0
        assert read(sp)==3
        return 1,draws.pop(0)
    def string(uc,sp):return 1,0
    def park(uc,sp):
        values=list(struct.unpack('<11I',uc.mem_read(sp,44)))
        trace.append(dict(call='park',arguments=values[1:]));return 11,0
    controller=HEAP+0x70000
    def flight_goal(uc,sp):
        point=read(sp+4)
        trace.append(dict(call='flight_goal',position=list(struct.unpack('<3i',uc.mem_read(point,12)))))
        return 2,controller
    def flight_radius(uc,sp):
        trace[-1]['radius']=read(sp);return 1,0
    def bind(uc,sp):
        ref=uc.reg_read(UC_X86_REG_ECX);put(ref+4,read(sp));return 1,ref
    def approach(uc,sp):
        point,radius=struct.unpack('<2I',uc.mem_read(sp,8))
        trace.append(dict(call='approach',radius=radius,
                          position=list(struct.unpack('<3i',uc.mem_read(point,12)))))
        return 2,0
    p.hooks.update({0x4eb9e0:lambda uc,sp:(0,controller),0x4e40e0:flight_goal,
                    0x4e4540:flight_radius,0x4d4d40:lambda uc,sp:(1,0),0x5199f0:bind,0x4d4da0:approach,0x51b480:position,0x51b4f0:detach,0x535cc0:random,
                    0x4d4bf0:string,0x4d78a0:park,0x519ef0:lambda uc,sp:(0,remaining),
                    0x507d10:fits,0x4d6a10:wait,0x50a9c0:sound,
                    0x421e10:effect,0x4f5db0:blocked})
    p.freeze_hooks()
    put(0x62d55c,game);put(game+0x19f30,1)
    put(game+0x174c8,101);put(game+0x174cc,102)
    put(carrier+0xb4,kind);put(kind+0x264,0x200);put(carrier+0xac,passenger)
    put(carrier+0x68,80<<16);put(carrier+0x6c,100<<16);put(carrier+0x70,240<<16)
    put(passenger+0x130,0x1000000);put(passenger+0xa8,carrier);put(passenger+0xb4,kind)
    put(mission+0x16,passenger)
    put(mission+0x22,(160<<16)+123);put(mission+0x26,20<<16);put(mission+0x2a,(320<<16)+456)
    observations=[]
    if args.sea:
        p.uc.mem_write(kind+0x23e,struct.pack('<H',150))
        put(carrier+8,HEAP+0x60000)
        stage1_events=(0,0x100,0x200,0x400,0x500,0x700,0x800,0x1000,0x2000,0x2700)
        cases=[(0,True,0),(1,True,0),(1,True,0x200)]
        cases.extend((1,False,event) for event in stage1_events)
        for stage,inside,events in cases:
            byte(mission+5,stage);put(mission+0x4e,5);put(mission+0x52,7)
            # The real primary dispatcher clears these fields before calling
            # the handler. Keep each direct-handler observation independent.
            put(mission+6,0);put(mission+0xa,0xffffffff);put(mission+0x6a,0)
            put(carrier+0x68,(80 if inside else 1000)<<16);trace.clear()
            result,error=p.call(handler,(carrier,mission,events))
            if error:raise RuntimeError(error)
            expected=1 if stage==0 or inside else (8 if events&0x200 else 2)
            assert result==expected
            assert read(mission+0x4e)==(0 if stage==0 else 6 if inside else 5)
            assert read(mission+0x52)==(0 if stage==0 else 7)
            goals=[event for event in trace if event['call']=='approach']
            waits=[event['delay'] for event in trace if event['call']=='sleep']
            if stage==1 and not inside and not (events&0x200):
                assert goals==[dict(call='approach',radius=116,
                    position=[(160<<16)+123,20<<16,(320<<16)+456])]
                assert waits==[15]
            elif stage==1 and inside:
                assert not goals and waits==[1]
            elif stage==1:
                assert not goals and not waits
            observations.append(dict(input_stage=stage,inside=inside,events=events,
                result=result,wait_mask=read(mission+6),output_counter=read(mission+0x52),
                attempts=read(mission+0x4e),trace=list(trace)))
        put(carrier+0x68,80<<16)
    for count in (0,1,14,15):
        for placement in ([1,1],[0,1],[0,0]):
            byte(mission+5,2);put(mission+0x52,count);trace.clear()
            result,error=p.call(handler,(carrier,mission,0))
            if error:raise RuntimeError(error)
            stage=p.uc.mem_read(mission+5,1)[0];counter=read(mission+0x52)
            expected=(4 if count>=15 else 2) if placement[0] else (1 if placement[1] else 8)
            assert result==expected
            assert stage==(4 if placement[0] and count>=15 else 2)
            assert counter==count+int(bool(placement[0]) and count<15)
            checks=[event for event in trace if event['call']=='placement']
            assert all(event['x']==10 and event['z']==20 for event in checks)
            observations.append(dict(input_stage=2,counter=count,placement=placement,
                result=result,output_stage=stage,output_counter=counter,trace=list(trace)))
    # Transfer visuals use the full mission point, not passenger or terrain Y.
    # Keep its X/Z stable for the controlled placement sink; vary height through
    # negative, fractional and positive values independently of carrier height.
    placement=[1,1]
    saved_point=bytes(p.uc.mem_read(mission+0x22,12))
    for landing_y in (-65537,-1,0,1,20*65536+32768,300*65536):
        put(mission+0x26,landing_y)
        byte(mission+5,2);put(mission+0x4e,1);put(mission+0x52,0);trace.clear()
        result,error=p.call(handler,(carrier,mission,0))
        assert not error,error
        effects=[event for event in trace if event['call']=='effect']
        assert effects==[
            dict(call='effect',id=101,position=[(160<<16)+123,landing_y,(320<<16)+456]),
            dict(call='effect',id=102,position=list(struct.unpack('<3i',p.uc.mem_read(carrier+0x68,12))))
        ],effects
        observations.append(dict(input_stage=2,landing_y=landing_y,result=result,trace=list(trace),
            counter=0,attempts=1,placement=[1,1],output_stage=p.uc.mem_read(mission+5,1)[0],
            output_counter=read(mission+0x52)))
    p.uc.mem_write(mission+0x22,saved_point)
    for attempts in (0,5,6,7):
        byte(mission+5,3);put(mission+0x4e,attempts);put(mission+0x52,9);trace.clear()
        result,error=p.call(handler,(carrier,mission,0))
        if error:raise RuntimeError(error)
        assert result==(4 if attempts<6 else 9)
        assert read(mission+0x52)==(0 if attempts<6 else 9)
        observations.append(dict(input_stage=3,counter=9,attempts=attempts,result=result,
            output_stage=p.uc.mem_read(mission+5,1)[0],output_counter=read(mission+0x52),trace=list(trace)))
    # Stage 4 installs PARK on the released passenger. Keep attachment and
    # queue allocation as sinks, observing the real argument/radius calculation.
    put(carrier+0x130,0x1000020)
    for remaining in (0,1,6,7):
        for first,second in ((0,0),(0,2),(2,2)):
            for footx,footz in ((1,1),(2,3),(4,4)):
                byte(mission+5,4);put(passenger+0xa8,carrier)
                put(carrier+0xac,passenger)
                p.uc.mem_write(kind+0x126,struct.pack('<hh',footx,footz))
                draws[:]=[first,second];trace.clear()
                result,error=p.call(handler,(carrier,mission,0))
                if error:raise RuntimeError(error)
                calls=[event['arguments'] for event in trace if event['call']=='park']
                radius=(first+second+1)*16+min(remaining,6)*(math.isqrt((footx*footx+footz*footz)*256)//2)
                assert calls==[[1,passenger,0,0,radius,0,0,0,0,0]],calls
                assert result==0 and not draws
                observations.append(dict(input_stage=4,remaining=remaining,random=[first,second],
                    next_footprint=[footx,footz],result=result,radius=radius,trace=list(trace)))
    rows=[' '.join(map(str,entry['random']+[entry['remaining']]+entry['next_footprint']))
          for entry in observations if entry['input_stage']==4]
    expected=[entry['radius'] for entry in observations if entry['input_stage']==4]
    port=subprocess.run([args.binary,'--unload-park'],input='\n'.join(rows)+'\n',
                        text=True,capture_output=True,check=True)
    assert list(map(int,port.stdout.split()))==expected
    if args.sea:
        rows=[];expected=[]
        for entry in observations:
            if entry['input_stage']!=1:continue
            waits=[event['delay'] for event in entry['trace'] if event['call']=='sleep']
            approach_count=sum(event['call']=='approach' for event in entry['trace'])
            rows.append(' '.join(map(str,[1,5,entry['events'],int(entry['inside']),1])))
            expected.append((entry['result'],1,entry['attempts'],
                entry['wait_mask'] | (1 if waits else 0),waits[-1] if waits else 0,approach_count))
        port=subprocess.run([args.binary,'--unload-approach'],input='\n'.join(rows)+'\n',
                            text=True,capture_output=True,check=True)
        actual=[tuple(map(int,line.split())) for line in port.stdout.splitlines()]
        assert actual==expected,(actual,expected)
    if not args.sea:
        rows=[];expected=[]
        put(carrier+0xac,passenger);put(passenger+0xa8,carrier)
        put(mission+0x22,read(carrier+0x68));put(mission+0x2a,read(carrier+0x70))
        for heading in (0,1,8192,16384,32768,49152,65535):
            for radius in (0,1,34,150,419,1000):
                p.uc.mem_write(carrier+0x7e,struct.pack('<H',heading))
                p.uc.mem_write(kind+0x23e,struct.pack('<H',radius))
                byte(mission+5,1);trace.clear()
                result,error=p.call(handler,(carrier,mission,0))
                if error:raise RuntimeError(error)
                assert result==1
                goals=[event for event in trace if event['call']=='flight_goal']
                assert len(goals)==1 and goals[0]['radius']==16,trace
                position=list(struct.unpack('<3i',p.uc.mem_read(carrier+0x68,12)))
                rows.append(' '.join(map(str,position+[heading,radius])))
                expected.append(tuple(goals[0]['position']+[0x30,16]))
                observations.append(dict(input_stage=1,heading=heading,range=radius,trace=list(trace)))
        port=subprocess.run([args.binary,'--unload-stepout'],input='\n'.join(rows)+'\n',
                            text=True,capture_output=True,check=True)
        assert [tuple(map(int,line.split())) for line in port.stdout.splitlines()]==expected
    rng=random_module.Random(0x408df5);rows=[];expected=[]
    put(carrier+8,HEAP+0x60000);put(carrier+0xac,passenger)
    put(passenger+0xa8,carrier);put(mission+0x22,0);put(mission+0x2a,0)
    for i in range(4096):
        radius=rng.randrange(0,1001)
        dx,dz=[rng.randrange(-1000*65536,1000*65536) for _ in range(2)]
        if i%2==0:dx=radius*65536+rng.randrange(-65536,65536);dz=rng.randrange(-65536,65536)
        p.uc.mem_write(kind+0x23e,struct.pack('<H',radius))
        put(carrier+0x68,dx);put(carrier+0x70,dz);byte(mission+5,1);trace.clear()
        result,error=p.call(handler,(carrier,mission,0))
        if error:raise RuntimeError(error)
        assert result in (1,2),result
        rows.append(f'{dx} {dz} {radius}');expected.append(int(result==1))
    port=subprocess.run([args.binary,'--transport-range'],input='\n'.join(rows)+'\n',
                        text=True,capture_output=True,check=True)
    actual=list(map(int,port.stdout.split()))
    assert actual==expected,next(((rows[i],a,b) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),'length mismatch')
    print('PASS: 4096 native fixed-point transport range comparisons')
    transfers=[entry for entry in observations if entry['input_stage'] in (2,3)]
    rows=[];expected=[]
    for entry in transfers:
        rows.append(' '.join(map(str,[entry['input_stage'],entry['counter'],entry.get('attempts',0),
                                      1]+entry.get('placement',[0,0]))))
        calls=entry['trace'];waits=[event['delay'] for event in calls if event['call']=='sleep']
        expected.append((entry['result'],entry['output_stage'],entry['output_counter'],waits[-1] if waits else 0,
            sum(event['call']=='sound' for event in calls),sum(event['call']=='blocked_chatter' for event in calls),
            sum(event['call']=='placement' for event in calls)))
    port=subprocess.run([args.binary,'--unload-transfer'],input='\n'.join(rows)+'\n',
                        text=True,capture_output=True,check=True)
    actual=[tuple(map(int,line.split())) for line in port.stdout.splitlines()]
    assert actual==expected,(actual,expected)
    with open(args.output,'w') as output:json.dump(observations,output,indent=2);output.write('\n')
    print(f'Observed {len(observations)} retail {'sea' if args.sea else 'air'}-unload cases; exact-site checks verified, transfer/retry state and all 36 PARK radii match the port')


if __name__=='__main__':main()
