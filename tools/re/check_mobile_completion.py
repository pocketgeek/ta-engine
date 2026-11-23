#!/usr/bin/env python3
"""Compare a controlled near-completion tick against the original executable.

Both engines receive the same change to initial remaining work. No later
capture state is injected. This checks mobile completion, queued movement,
construction pools and RNG; it does not certify full scene equality.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile

import check_captured_tick as tick
from livesample import header, STRIDE


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture',type=Path)
    ap.add_argument('probe_input',type=Path)
    ap.add_argument('--runner',type=Path,required=True)
    ap.add_argument('--retail-root',type=Path,required=True)
    ap.add_argument('--site',type=int,default=389)
    args=ap.parse_args()
    capture=json.loads(args.capture.read_text())
    lines=args.probe_input.read_text().splitlines()
    fields=lines[0].split()
    if fields[0]!='TAK_MOVEMENT_PROBE' or fields[1] not in ('25','26','27'):
        raise ValueError('requires probe format 25/26/27 with inactive navigator stamps')
    fields[5]='1';lines[0]=' '.join(fields)
    candidates=[i for i,line in enumerate(lines)
                if line.startswith(f'{args.site} ') and len(line.split())==8]
    if len(candidates)!=1:raise ValueError('expected one initial construction site')
    index=candidates[0];fields=lines[index].split();fields[1]='0.000001'
    lines[index]=' '.join(fields)
    with tempfile.TemporaryDirectory(prefix='tak-completion-') as directory:
        source=Path(directory)/'input.txt';output=Path(directory)/'port.jsonl'
        source.write_text('\n'.join(lines)+'\n')
        subprocess.run([str(args.runner.resolve()),str(args.retail_root),str(source),str(output)],check=True)
        events=[json.loads(line) for line in output.read_text().splitlines()]
    frame=next(e for e in events if e['kind']=='frame' and e['tick']==capture['frames'][0]['tick']+1)
    original_process=tick.CapturedProcess;original_snapshot=tick.snapshot
    observed=[]
    class NearCompletion(original_process):
        def __init__(self,data):
            super().__init__(data)
            address=next(u['address'] for u in self.runtime['units'] if u['id']==args.site)
            self.put(address+0x108,struct.pack('<f',0.000001))
    def snapshot(p):
        result=original_snapshot(p)
        _,base,_,_,_=header(p)
        original_units={u['id']:u for u in result['units']}
        for unit in frame['units']:
            expected=original_units[unit['id']]
            # Terrain height is not restored for ground units by this probe.
            actual=[unit[k] for k in ('x_raw','z_raw','heading','speed_raw')]
            wanted=[expected['position_raw'][0],expected['position_raw'][2],
                    expected['heading'],expected['speed_raw']]
            if wanted[-1] is None:actual=actual[:-1];wanted=wanted[:-1]
            if actual!=wanted:raise AssertionError((unit['id'],'motion',actual,wanted))
            if 'construction' in unit:
                address=base+unit['id']*STRIDE
                actual=(struct.pack('<f',unit['construction'][0]),unit['construction'][1])
                wanted=(bytes(p.uc.mem_read(address+0x108,4)),p.u32(address+0x10c)&65535)
                if actual!=wanted:raise AssertionError((unit['id'],'construction',actual,wanted))
                mission=p.u32(address+0x60)
                waiting=bool(mission and p.uc.mem_read(mission+4,1)[0]==21)
                if waiting!=('get_built' in unit):raise AssertionError((unit['id'],'waiting mission presence'))
                if waiting:
                    builder=p.u32(mission+0x16)
                    wanted=[p.uc.mem_read(mission+5,1)[0],p.u32(mission+6),p.u32(mission+10),
                            p.u32(mission+0x6a),(p.u32(builder)>>16) if builder else 0]
                    if unit['get_built']!=wanted:raise AssertionError((unit['id'],'waiting mission',wanted))
        for player,resource in enumerate(frame['resources']):
            pool=p.u32(p.game+0x2404+player*0x110+0x10c)
            if struct.pack('<f',resource['mana'])!=bytes(p.uc.mem_read(pool,4)):
                raise AssertionError((player,'resource pool'))
        gameplay=[{k:e[k] for k in ('bound','seed_before')} for e in events if e['kind']=='rng']
        wanted=[{k:e[k] for k in ('bound','seed_before')} for e in p.events]
        if gameplay!=wanted:raise AssertionError(('gameplay RNG',gameplay,wanted))
        observed.append(len(gameplay))
        return result
    tick.CapturedProcess=NearCompletion;tick.snapshot=snapshot
    # Original live observations describe the unmodified fixture. They are not
    # an oracle for this variant; compare the two executions directly instead.
    capture.pop('crt_calls',None)
    try:report=tick.replay(capture,1)
    finally:tick.CapturedProcess=original_process;tick.snapshot=original_snapshot
    if len(observed)!=1:raise AssertionError(report)
    crt=[{k:e[k] for k in ('tick','return_address','seed_before')} for e in events if e['kind']=='crt']
    if crt!=report['crt_calls']:raise AssertionError(('CRT RNG',crt,report['crt_calls']))
    site=next(u for u in frame['units'] if u['id']==args.site)
    if site['construction'][0]!=0:raise AssertionError('site did not finish')
    print(f'PASS: controlled completion of {args.site}; {len(frame["units"])} motion states, '
          f'construction/resources, {observed[0]} gameplay and {len(crt)} CRT draws')


if __name__=='__main__':main()
