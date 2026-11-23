#!/usr/bin/env python3
"""Compare deterministic integer rotation with retail's rounded x87 rotation.

Optional model/state arguments also check the saved build point against captured
memory. Inputs and derived model data remain outside tracked source files.
"""
import argparse
import json
import random
import struct
import subprocess
from emu import Icd, HEAP


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-dbg/retail_script_test')
    ap.add_argument('--capture'); ap.add_argument('--cob'); ap.add_argument('--model'); ap.add_argument('--state')
    ap.add_argument('--unit',type=int); ap.add_argument('--piece',type=int,default=0)
    args=ap.parse_args()
    p=Icd(); p.freeze_hooks(); rng=random.Random(536143)
    # COB GET 27 uses native heading, including its zero direction. Factory
    # scripts subtract this value to counter-rotate their build pads.
    host,visual,unit=HEAP,HEAP+0x2000,HEAP+0x3000
    p.uc.mem_write(host+0xa64,struct.pack('<I',visual))
    p.uc.mem_write(visual+12,struct.pack('<I',unit))
    for heading in (0,1,8192,16384,32767,32768,49152,65535):
        p.uc.mem_write(unit+0x7e,struct.pack('<H',heading))
        actual,error=p.call(0x50ceb0,(27,0,0,0,0),ecx=host)
        if error or actual!=heading: raise AssertionError((heading,actual,error))
    print('PASS: COB GET 27 reports the native heading at all eight boundary cases')
    rows=[(rng.randrange(-1000000000,1000000000),rng.randrange(-1000000000,1000000000),rng.randrange(65536))
          for _ in range(10000)]
    rows += [(x,y,h) for x,y in [(0,0),(1,0),(0,1),(2147483647,2147483647),(-2147483648,0)]
             for h in [0,1,16384,32768,49152,65535]]
    expected=[]
    for x,y,h in rows:
        p.uc.mem_write(HEAP,struct.pack('<2i',x,y))
        _,error=p.call(0x536143,(h,HEAP))
        if error: raise RuntimeError(error)
        expected.append(struct.unpack('<2i',p.uc.mem_read(HEAP,8)))
    proc=subprocess.run([args.binary,'--rotate'],input=''.join(f'{x} {y} {h}\n' for x,y,h in rows),
                        text=True,capture_output=True,check=True)
    actual=[tuple(map(int,row.split())) for row in proc.stdout.splitlines()]
    if len(actual)!=len(expected): raise AssertionError('rotation count differs')
    for row,want,got in zip(rows,expected,actual):
        if want!=got: raise AssertionError((row,want,got))
    print(f'PASS: {len(rows)} integer rotations match the original, including overflow and rounding')
    if args.capture:
        if not all((args.cob,args.model,args.state,args.unit is not None)):
            ap.error('--capture requires --cob, --model, --state and --unit')
        from emureload import CapturedProcess
        with open(args.capture) as source: captured=CapturedProcess(json.load(source))
        unit=next(u for u in captured.runtime['units'] if u['id']==args.unit)['address']
        heading=struct.unpack('<H',captured.uc.mem_read(unit+0x7e,2))[0]
        _,error=captured.icd.call(0x4dd0f0,(HEAP+0x300000,unit,args.piece))
        if error: raise RuntimeError(error)
        expected=struct.unpack('<3i',captured.uc.mem_read(HEAP+0x300000,12))
        proc=subprocess.run([args.binary,'--origin',args.cob,args.model,args.state,str(heading),str(args.piece)],
                            text=True,capture_output=True,check=True)
        actual=tuple(map(int,proc.stdout.split()))
        if actual!=expected: raise AssertionError((expected,actual))
        print('PASS: complete saved build-point hierarchy matches the original:',actual)


if __name__=='__main__': main()
