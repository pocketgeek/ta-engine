#!/usr/bin/env python3
"""Compare an unmodified port fixture with offline retail tick bodies.

Extends from the initial capture without injecting later state. Outer-frame
rendering is excluded in both executions; this is not full-game parity.
Unsupported behavior in either engine fails the comparison.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile

import check_captured_tick as tick
from livesample import header, STRIDE
from probe_saved_movement import compare_frames


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture',type=Path)
    ap.add_argument('probe_input',type=Path)
    ap.add_argument('--runner',type=Path,required=True)
    ap.add_argument('--retail-root',type=Path,required=True)
    ap.add_argument('--ticks',type=int,required=True)
    args=ap.parse_args()
    if not 1<=args.ticks<=3000:ap.error('ticks must be 1..3000')
    capture=json.loads(args.capture.read_text())
    lines=args.probe_input.read_text().splitlines();fields=lines[0].split()
    if fields[0]!='TAK_MOVEMENT_PROBE' or int(fields[2])!=capture['frames'][0]['tick']:
        raise ValueError('probe must start at the captured tick')
    if int(fields[3])!=capture['frames'][0]['rng_before']:
        raise ValueError('probe starting RNG disagrees with capture')
    fields[5]=str(args.ticks);lines[0]=' '.join(fields)
    with tempfile.TemporaryDirectory(prefix='tak-replay-fixture-') as directory:
        source=Path(directory)/'input.txt';output=Path(directory)/'port.jsonl'
        source.write_text('\n'.join(lines)+'\n')
        subprocess.run([str(args.runner.resolve()),str(args.retail_root),str(source),str(output)],check=True)
        events=[json.loads(line) for line in output.read_text().splitlines()]
    frames={e['tick']:e for e in events if e['kind']=='frame'}
    original=tick.snapshot;checked=[]
    def snapshot(p):
        observed=original(p);frame=frames[observed['tick']]
        difference=compare_frames([observed],[frame])
        if difference:raise AssertionError(difference)
        actual=[(e['bound'],e['seed_before']) for e in events
                if e['kind']=='rng' and e['tick']<=observed['tick']]
        expected=[(e['bound'],e['seed_before']) for e in p.events]
        if actual!=expected:
            first=next((i for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),min(len(actual),len(expected)))
            raise AssertionError((observed['tick'],'gameplay RNG',first,actual[first:first+1],expected[first:first+1]))
        _,base,_,_,_=header(p)
        native={u['id']:u for u in observed['units']}
        for u in frame['units']:
            address=base+u['id']*STRIDE
            if u.get('flight_missions') or 'flying_construction' in u:
                if u['y_raw']!=native[u['id']]['position_raw'][1]:
                    raise AssertionError((observed['tick'],u['id'],'flight altitude'))
            if 'flying_construction' in u:
                mission=p.u32(address+0x60)
                expected=[p.uc.mem_read(mission+5,1)[0],*(p.u32(mission+o) for o in (6,10,0x6a,0x5a))]
                if u['flying_construction']!=expected:
                    raise AssertionError((observed['tick'],u['id'],'flying mission',u['flying_construction'],expected))
            if 'construction' in u:
                if (struct.pack('<f',u['construction'][0])!=bytes(p.uc.mem_read(address+0x108,4)) or
                    u['construction'][1]!=int.from_bytes(p.uc.mem_read(address+0x10c,2),'little')):
                    raise AssertionError((observed['tick'],u['id'],'construction'))
        for owner,resource in enumerate(frame['resources']):
            pool=p.u32(p.game+0x2404+owner*0x110+0x10c)
            if struct.pack('<f',resource['mana'])!=bytes(p.uc.mem_read(pool,4)):
                raise AssertionError((observed['tick'],owner,'resource pool'))
        checked.append(observed['tick']);return observed
    tick.snapshot=snapshot
    # Compare the port with native tick bodies. Captured outer-frame draws
    # remain evidence of a separate whole-frame replay limitation.
    capture.pop('crt_calls',None)
    try:report=tick.replay(capture,args.ticks,stop_on_difference=False,extend_simulation=True)
    finally:tick.snapshot=original
    if len(checked)!=args.ticks:raise AssertionError(report['first_mismatch'])
    keys=('tick','return_address','seed_before')
    actual=[{k:e[k] for k in keys} for e in events if e['kind']=='crt']
    expected=report['crt_calls']
    if actual!=expected:
        first=next((i for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),min(len(actual),len(expected)))
        raise AssertionError(('CRT RNG',first,actual[first:first+1],expected[first:first+1],len(actual),len(expected)))
    print(f'PASS: {len(checked)} tick-body transitions; unit motion, active flying-builder missions, '
          f'construction and resources; {sum(e["kind"]=="rng" for e in events)} gameplay / {len(actual)} CRT draws')


if __name__=='__main__':main()
