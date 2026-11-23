#!/usr/bin/env python3
"""Compare sequences of production World movement commitments with retail.

Displacements are controlled inputs. Native 4dad30 executes placement, refusal,
clamping and occupancy writes. Steering, route selection and height updates are
outside this check; no resulting native state is injected into World.
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
from emureload import CapturedProcess


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
    capture=json.loads(args.capture.read_text());p=CapturedProcess(capture)
    selected=set_balance_inputs(p,capture,args.save,args.retail_root,args.balance=='crusades') if args.balance else {}
    grids={g['address'] for g in p.runtime['grids']}|set(selected.values())
    addresses={u['id']:u['address'] for u in p.runtime['units']}
    subjects=[u for u in p.frame['units'] if u.get('mover_address') and p.u32(u['mover_address']+4) in grids
              and not p.u32(addresses[u['id']]+0xa8)]
    if not subjects: raise ValueError('no unattached surface movers')
    fixture=args.fixture.read_text().splitlines();header=fixture[0].split()
    if int(header[2])!=p.frame['tick']: raise ValueError('fixture tick mismatch')
    header[5]='0'
    if args.balance: header[-1]=str(int(args.balance=='crusades'))
    fixture[0]=' '.join(header)
    p.icd.freeze_hooks()
    rng=random.Random(0x4dad30)
    rows,expected=[],[];counts=Counter();refusals=0;crossings=0
    for i in range(args.cases):
        u=subjects[i%len(subjects)];pointer=addresses[u['id']];mover=u['mover_address']
        if i//len(subjects)%4==0:
            dx,dz=struct.unpack('<i',p.uc.mem_read(mover+8,4))[0],struct.unpack('<i',p.uc.mem_read(mover+16,4))[0]
        else:
            dx,dz=rng.randrange(-24*65536,24*65536),rng.randrange(-24*65536,24*65536)
            if i%3==0: dz=0
            if i%3==1: dx=0
        before=bytes(p.uc.mem_read(pointer+0x74,4))
        p.uc.mem_write(mover+8,struct.pack('<3i',dx,0,dz))
        _,error=p.icd.call(0x4dad30,(pointer,),ecx=mover)
        if error or p.missing: raise RuntimeError((i,u['id'],error,p.missing))
        x,_,z=struct.unpack('<3i',p.uc.mem_read(pointer+0x68,12))
        speed=struct.unpack('<i',p.uc.mem_read(mover+0x20,4))[0]
        flags=struct.unpack('<H',p.uc.mem_read(mover+0x36,2))[0]
        streak=2 if flags&4 else 1 if flags&8 else 0
        expected.append([x,z,speed,streak]);rows.append(f'{u["id"]} {dx} {dz}')
        refusals+=bool(streak);crossings+=before!=bytes(p.uc.mem_read(pointer+0x74,4))
        grid=p.u32(mover+4)
        name=bytes(p.uc.mem_read(p.u32(grid),64)).split(b'\0')[0].decode('ascii')
        counts[name]+=1
    with tempfile.TemporaryDirectory(prefix='tak-ground-commit-') as temp:
        root=Path(temp);(root/'input').write_text('\n'.join(fixture)+'\n');(root/'steps').write_text('\n'.join(rows)+'\n')
        subprocess.run([str(args.runner),str(args.retail_root),str(root/'input'),str(root/'output'),
                        '--ground-steps',str(root/'steps')],check=True)
        actual=[e['result'] for line in (root/'output').read_text().splitlines() if (e:=json.loads(line))['kind']=='ground_step']
    if len(actual)!=len(expected): raise AssertionError(('count',len(actual),len(expected)))
    for i,(row,want,got) in enumerate(zip(rows,expected,actual)):
        if want!=got: raise AssertionError((i,row,'retail',want,'World',got))
    print(f'PASS: {len(rows)} sequential World ground commitments, {len(subjects)} units, '
          f'balance={args.balance or "captured"}, refusal-state={refusals}, cell-crossings={crossings}; {dict(counts)}')


if __name__=='__main__': main()
