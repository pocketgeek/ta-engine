#!/usr/bin/env python3
"""Compare World surface height/pitch/roll with independently loaded map/FBI/model assets.

Only initial unit state and query coordinates enter World, never native heights
or support points. Optional balance selection supplies source FBI inputs to
both implementations. This checks the adapter, not complete movement routes.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import random
import struct
import subprocess
import tempfile

from balance_inputs import set_balance_inputs
from emureload import CapturedProcess, initial_fixture


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture',type=Path)
    ap.add_argument('--fixture',type=Path,required=True)
    ap.add_argument('--retail-root',type=Path,default=Path('assets/game'))
    ap.add_argument('--runner',type=Path,default=Path('build-dbg/retail_replay_probe'))
    ap.add_argument('--balance',choices=['standard','crusades'])
    ap.add_argument('--save',type=Path)
    ap.add_argument('--cases',type=int,default=12000)
    args=ap.parse_args()
    if args.cases<1 or (args.balance and not args.save): ap.error('positive cases and save for balance required')
    capture=json.loads(args.capture.read_text())
    p=CapturedProcess(capture)
    selected=set_balance_inputs(p,capture,args.save,args.retail_root,args.balance=='crusades',surface=True) if args.balance else {}
    grids={g['address'] for g in p.runtime['grids']}|set(selected.values())
    subjects=[u for u in p.frame['units'] if u.get('mover_address') and p.u32(u['mover_address']+4) in grids]
    if not subjects: raise ValueError('no surface subjects')
    addresses={u['id']:u['address'] for u in p.runtime['units']}
    fixture=initial_fixture(args.fixture.read_text().splitlines(),p.frame['tick'])
    header=fixture[0].split()
    header[1]='36';header[5]='0'
    if args.balance: header[-1]=str(int(args.balance=='crusades'))
    fixture[0]=' '.join(header)
    fixture.append(str(len(p.frame['units'])))
    for u in p.frame['units']:
        mover=u.get('mover_address',0)
        phase=struct.unpack('<H',p.uc.mem_read(addresses[u['id']]+0x82,2))[0]
        stamp=p.u32(mover+0x2c) if mover else 0
        pitch=struct.unpack('<H',p.uc.mem_read(addresses[u['id']]+0x80,2))[0]
        roll=struct.unpack('<H',p.uc.mem_read(addresses[u['id']]+0x7c,2))[0]
        fixture.append(f'{u["id"]} {u["position_raw"][1]} {stamp} {phase} {pitch} {roll}')
    clock=0
    p.icd.hooks[0x53ff20]=lambda uc,a:(0,clock)
    p.icd.freeze_hooks()
    width,height=p.frame['map_cells']
    rng=random.Random(0x51b2a0)
    rows,expected=[],[]
    counts=Counter()
    for i in range(args.cases):
        u=subjects[i%len(subjects)]
        pointer=addresses[u['id']]
        x,old_y,z=u['position_raw']
        heading=struct.unpack('<H',p.uc.mem_read(pointer+0x7e,2))[0]
        if i>=len(subjects):
            x=rng.randrange(-16*65536,width*16*65536)
            z=rng.randrange(-16*65536,height*16*65536)
            old_y=rng.randrange(-256*65536,512*65536)
            heading=rng.randrange(65536)
        clock=rng.getrandbits(32)
        old_pitch,old_roll=rng.randrange(65536),rng.randrange(65536)
        p.uc.mem_write(pointer+0x80,struct.pack('<H',old_pitch))
        p.uc.mem_write(pointer+0x7c,struct.pack('<H',old_roll))
        p.uc.mem_write(pointer+0x68,struct.pack('<3i',x,old_y,z))
        p.uc.mem_write(pointer+0x7e,struct.pack('<H',heading))
        p.put(pointer+0x130,struct.pack('<I',(p.u32(pointer+0x130)&~3)|0x4001))
        _,error=p.icd.call(0x51b2a0,(pointer,))
        if error: raise RuntimeError((u['id'],error))
        expected.append([struct.unpack('<i',p.uc.mem_read(pointer+0x6c,4))[0],
                         struct.unpack('<H',p.uc.mem_read(pointer+0x80,2))[0],
                         struct.unpack('<H',p.uc.mem_read(pointer+0x7c,2))[0]])
        rows.append(f'{u["id"]} {x} {z} {old_y} {heading} {clock} {old_pitch} {old_roll}')
        grid=p.u32(u['mover_address']+4)
        name=bytes(p.uc.mem_read(p.u32(grid),64)).split(b'\0')[0].decode('ascii')
        counts[name]+=1
    with tempfile.TemporaryDirectory(prefix='tak-world-height-') as temp:
        root=Path(temp)
        (root/'input').write_text('\n'.join(fixture)+'\n')
        (root/'queries').write_text('\n'.join(rows)+'\n')
        subprocess.run([str(args.runner),str(args.retail_root),str(root/'input'),str(root/'output'),
                        '--ground-height-queries',str(root/'queries')],check=True)
        actual=[[e['result'],e['pitch'],e['roll']] for line in (root/'output').read_text().splitlines()
                if (e:=json.loads(line))['kind']=='ground_height']
    if len(actual)!=len(expected): raise AssertionError(('count',len(actual),len(expected)))
    for row,want,got in zip(rows,expected,actual):
        if want!=got: raise AssertionError((row,'retail',want,'World',got))
    print(f'PASS: {len(rows)} World surface height/pitch/roll queries, {len(subjects)} units, balance={args.balance or "captured"}; {dict(counts)}')


if __name__=='__main__': main()
