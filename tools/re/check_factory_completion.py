#!/usr/bin/env python3
"""Compare a controlled first factory completion against executable ticks.

The initial VERPULT type has zero cost and inverse build time 100 in both
engines. No later-frame values are supplied to either simulation. Native
rendering is excluded from both executions, including with --ticks above five.
--natural-production leaves the initial catalogue unchanged and also checks
mobile-site birth once the builder reaches its perimeter.
"""
import argparse
import json
from pathlib import Path
import shlex
import struct
import subprocess
import tempfile

import check_captured_tick as tick
from livesample import header,STRIDE
from probe_saved_movement import initial_sector_links


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture',type=Path);ap.add_argument('probe_input',type=Path)
    ap.add_argument('--runner',type=Path,required=True);ap.add_argument('--retail-root',type=Path,required=True)
    ap.add_argument('--ticks',type=int,default=5)
    ap.add_argument('--natural-production',action='store_true')
    args=ap.parse_args();capture=json.loads(args.capture.read_text())
    if not 5<=args.ticks<=3000:ap.error('--ticks must be between 5 and 3000')
    lines=args.probe_input.read_text().splitlines();fields=lines[0].split()
    if fields[0]!='TAK_MOVEMENT_PROBE' or int(fields[1]) not in range(25,38):raise ValueError('requires probe format 25 through 37')
    if fields[1]=='29':
        sectors=initial_sector_links(capture['frames'][0]['runtime_state'])
        lines.append(str(len(sectors)));lines.extend(' '.join(map(str,row)) for row in sectors)
        fields[1]='30'
    fields[5]=str(args.ticks);lines[0]=' '.join(fields);changed=0
    for i,line in enumerate(lines):
        if line.startswith('"verpult" '):
            fields=shlex.split(line)
            if len(fields)!=17:raise ValueError('unexpected catalogue format')
            if not args.natural_production:fields[4]='0';fields[11]='100'
            lines[i]='"verpult" '+' '.join(fields[1:]);changed+=1
    if not changed:raise ValueError('VERPULT absent from initial catalogue')
    with tempfile.TemporaryDirectory(prefix='tak-factory-completion-') as directory:
        source=Path(directory)/'input.txt';output=Path(directory)/'port.jsonl'
        source.write_text('\n'.join(lines)+'\n')
        subprocess.run([str(args.runner.resolve()),str(args.retail_root),str(source),str(output)],check=True)
        events=[json.loads(line) for line in output.read_text().splitlines()]
    frames={e['tick']:e for e in events if e['kind']=='frame'}
    original_process=tick.CapturedProcess;original_snapshot=tick.snapshot;observed=[]
    class FastOutput(original_process):
        def __init__(self,data):
            super().__init__(data)
            count,table=self.u32(self.game+0x175b8),self.u32(self.game+0x175c4)
            kinds=[table+i*676 for i in range(1,count)
                   if bytes(self.uc.mem_read(table+i*676+32,32)).split(b'\0')[0].lower()==b'verpult']
            if len(kinds)!=1:raise ValueError('expected original VERPULT type')
            if not args.natural_production:
                self.put(kinds[0]+0x20e,struct.pack('<f',0));self.put(kinds[0]+0x216,struct.pack('<f',100))
    def snapshot(p):
        result=original_snapshot(p);frame=frames[result['tick']]
        actual=[{k:e[k] for k in ('bound','seed_before')} for e in events
                if e['kind']=='rng' and e['tick']<=result['tick']]
        wanted=[{k:e[k] for k in ('bound','seed_before')} for e in p.events]
        if actual!=wanted:
            first=next((i for i,pair in enumerate(zip(actual,wanted)) if pair[0]!=pair[1]),min(len(actual),len(wanted)))
            raise AssertionError((result['tick'],'gameplay RNG',first,actual[first:first+1],wanted[first:first+1],len(actual),len(wanted),p.events[first:first+1]))
        units={u['id']:u for u in result['units']}
        if set(units)!={u['id'] for u in frame['units']}:raise AssertionError((result['tick'],'unit identities', sorted(set(units)-{u['id'] for u in frame['units']}), sorted({u['id'] for u in frame['units']}-set(units))))
        _,base,_,_,_=header(p)
        for u in frame['units']:
            expected=units[u['id']]
            actual=[u[k] for k in ('x_raw','z_raw','heading')]
            wanted=[expected['position_raw'][0],expected['position_raw'][2],expected['heading']]
            if expected['speed_raw'] is not None:actual.append(u['speed_raw']);wanted.append(expected['speed_raw'])
            if actual!=wanted:raise AssertionError((result['tick'],u['id'],'motion',actual,wanted,
                {k:u[k] for k in ('ground_refusal','ground_mode','ground_scan_tick') if k in u},
                {k:expected.get(k) for k in ('movement_flags','refusal_deadline','movement_tick')}))
            if u['id']==821 and u['y_raw']!=expected['position_raw'][1]:
                raise AssertionError((result['tick'],821,'flight altitude',u['y_raw'],expected['position_raw'][1]))
            if u['id']==821 and 'flight_missions' in u:
                mission=p.u32(base+821*STRIDE+0x60);missions=[]
                while mission:
                    raw=bytes(p.uc.mem_read(mission,0x72))
                    missions.append([raw[4],raw[5],p.u32(mission+6),p.u32(mission+10),
                                     p.u32(mission+0x6a),p.u32(mission+0x5a),
                                     struct.unpack_from('<i',raw,0x22)[0],struct.unpack_from('<i',raw,0x2a)[0]])
                    mission=p.u32(mission+0x66)
                if u['flight_missions']!=missions:raise AssertionError((result['tick'],821,'flight missions',u['flight_missions'],missions))
            if u['id']==338 and result['tick']>=10159 and u.get('build_approach'):
                navigator=p.u32(p.u32(base+338*STRIDE+8));count=p.u32(navigator+0x10c)
                points=[list(struct.unpack('<2h',p.uc.mem_read(navigator+12+i*4,4))) for i in range(count)]
                route=u['route']
                actual_route=([[route[0][3]>>16,route[0][4]>>16]] if route else [])+[[o[0]>>16,o[1]>>16] for o in route]
                if count and actual_route!=points:raise AssertionError((result['tick'],338,'build approach route',actual_route,points))
            if ((u['id']==248 and result['tick']>=10277) or
                (u['id']==146 and result['tick']>=10122)) and u.get('ground_missions'):
                navigator=p.u32(p.u32(base+u['id']*STRIDE+8));count=p.u32(navigator+0x10c)
                points=[list(struct.unpack('<2h',p.uc.mem_read(navigator+12+i*4,4))) for i in range(count)]
                route=u['route']
                actual_route=([[route[0][3]>>16,route[0][4]>>16]] if route else [])+[[o[0]>>16,o[1]>>16] for o in route]
                if count and actual_route!=points:raise AssertionError((result['tick'],u['id'],'navigator route',actual_route,points))
            if u['id']==806 and 'ground_refusal' in u:
                flags=expected['movement_flags']
                refusal=2 if flags&4 else 1 if flags&8 else 0
                if u['ground_refusal']!=refusal:
                    raise AssertionError((result['tick'],u['id'],'refusal',u['ground_refusal'],refusal))
            if 'construction' in u:
                address=base+u['id']*STRIDE
                if (struct.pack('<f',u['construction'][0]),u['construction'][1])!=(
                        bytes(p.uc.mem_read(address+0x108,4)),p.u32(address+0x10c)&65535):
                    raise AssertionError((result['tick'],u['id'],'construction'))
            if 'factory_queue' in u:
                address=base+u['id']*STRIDE
                mission=p.u32(address+0x60);original_queue=[];seen=set()
                definitions=p.u32(0x62db84)
                while mission:
                    if mission in seen:raise ValueError('cyclic factory mission queue')
                    seen.add(mission)
                    kind=p.uc.mem_read(mission+4,1)[0]
                    if p.u32(definitions+kind*25+4)==0x401c20:
                        original_queue.append(p.u32(mission+0x4e))
                    mission=p.u32(mission+0x66)
                if u['factory_queue']!=original_queue:
                    raise AssertionError((result['tick'],u['id'],'factory queue',u['factory_queue'],original_queue))
            if u['id'] in (540,821) and u.get('script_threads'):
                script=p.u32(base+u['id']*STRIDE+0xbc)
                original_threads=[list(struct.unpack('<8I',p.uc.mem_read(script+0x20+i*0xa4,32))) for i in range(16)]
                for slot,(actual_thread,original_thread) in enumerate(zip(u['script_threads'],original_threads)):
                    if actual_thread[0] or original_thread[0]:
                        if actual_thread!=original_thread:raise AssertionError((result['tick'],u['id'],'script thread',slot,actual_thread,original_thread))
            if u['id']==927 and u.get('ground_missions'):
                address=base+u['id']*STRIDE;mission=p.u32(address+0x60)
                raw=bytes(p.uc.mem_read(mission,0x72))
                original=[raw[5],p.u32(mission+6),p.u32(mission+10),p.u32(mission+0x6a),
                          p.u32(mission+0x4e),p.u32(mission+0x5a),
                          struct.unpack_from('<i',raw,0x52)[0],*struct.unpack_from('<2h',raw,0x2e),
                          struct.unpack_from('<h',raw,0x24)[0],struct.unpack_from('<h',raw,0x2c)[0]]
                actual=u['ground_missions'][0][:5]+u['ground_missions'][0][6:]
                if actual!=original:raise AssertionError((result['tick'],927,'strike movement mission',actual,original))
                if result['tick']>=10132:
                    navigator=p.u32(p.u32(address+8));count=p.u32(navigator+0x10c)
                    points=[list(struct.unpack('<2h',p.uc.mem_read(navigator+12+i*4,4))) for i in range(count)]
                    route=u['route']
                    actual_route=([[route[0][3]>>16,route[0][4]>>16]] if route else [])+[[o[0]>>16,o[1]>>16] for o in route]
                    if actual_route!=points:raise AssertionError((result['tick'],927,'navigator route',actual_route,points))
        for i,resource in enumerate(frame['resources']):
            pool=p.u32(p.game+0x2404+i*0x110+0x10c)
            if struct.pack('<f',resource['mana'])!=bytes(p.uc.mem_read(pool,4)):
                raise AssertionError((result['tick'],i,'resource pool'))
        for owner,state in enumerate(frame.get('ai',[])):
            if state is None:continue
            manager=p.u32(p.game+0x2404+owner*0x110+0x80)
            countdown=struct.unpack('<i',p.uc.mem_read(manager+5,4))[0]
            if countdown!=state['countdown']:raise AssertionError((result['tick'],owner,'AI countdown'))
            for slot,deadline,active,dirty,parameters,members in state['groups']:
                squad=p.u32(manager+0x11+slot*4);data=p.u32(squad+8)
                begin,end=p.u32(data+0xb8),p.u32(data+0xbc)
                original_members=[(pointer-base)//STRIDE for pointer in
                    struct.unpack(f'<{(end-begin)//4}I',p.uc.mem_read(begin,end-begin))] if begin!=end else []
                original=[p.u32(squad+12),p.u32(data+8),p.u32(data+0xb0),
                          list(struct.unpack('<9i',p.uc.mem_read(data+12,36))),original_members]
                if [deadline,active,dirty,parameters,members]!=original:
                    raise AssertionError((result['tick'],owner,slot,'AI squad',
                        [deadline,active,dirty,parameters,members],original))
        observed.append(result['tick']);return result
    tick.CapturedProcess=FastOutput;tick.snapshot=snapshot;capture.pop('crt_calls',None)
    try:report=tick.replay(capture,args.ticks,stop_on_difference=False,extend_simulation=True)
    finally:tick.CapturedProcess=original_process;tick.snapshot=original_snapshot
    if len(observed)!=args.ticks:raise AssertionError(report)
    crt=[{k:e[k] for k in ('tick','return_address','seed_before')} for e in events if e['kind']=='crt']
    if crt!=report['crt_calls']:
        wanted=report['crt_calls']
        first=next((i for i,(actual,expected) in enumerate(zip(crt,wanted))
                    if actual!=expected),min(len(crt),len(wanted)))
        raise AssertionError(('CRT draws differ','first index',first,
                              'actual',crt[max(0,first-2):first+3],
                              'retail',wanted[max(0,first-2):first+3],
                              'lengths',len(crt),len(wanted)))
    initial_ids={u['id'] for u in capture['frames'][0]['units']}
    newborn=[u for u in frames[observed[4]]['units'] if u['id'] not in initial_ids]
    if args.natural_production:
        if len(newborn)!=1 or newborn[0]['construction'][0]<=0:
            raise AssertionError('expected unfinished factory output')
        if args.ticks>=26 and len([u for u in frames[observed[-1]]['units'] if u['id'] not in initial_ids])<2:
            raise AssertionError('expected mobile builder site birth')
    elif len(newborn)!=1 or newborn[0]['construction'][0]!=0 or newborn[0]['speed_raw']==0:
        raise AssertionError('expected one finished moving factory output')
    label='natural production' if args.natural_production else 'factory completion'
    print(f'PASS: {len(observed)} {label} tick boundaries, all unit motion/construction/resources, factory queues and AI squads, '
          f'{sum(e["kind"]=="rng" for e in events)} gameplay and {len(crt)} CRT draws')


if __name__=='__main__':main()
