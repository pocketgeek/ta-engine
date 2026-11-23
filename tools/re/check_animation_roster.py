#!/usr/bin/env python3
"""Audit every supplied COB's callback timelines and piece state against retail.

Reads user-owned scripts and executable locally; writes only coverage/results.
Controlled host profiles exercise idle, road, water, and damaged veteran states.
This checks VM execution, not renderer pixels or game-driven callback timing.
"""
import argparse
import concurrent.futures
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('scripts',type=Path)
    ap.add_argument('--binary',default='build-o2/retail_script_test')
    ap.add_argument('--ticks',type=int,default=1500)
    ap.add_argument('--output',type=Path,required=True)
    args=ap.parse_args()
    jobs=[];inventory=[]
    with tempfile.TemporaryDirectory(prefix='tak-animation-') as temporary:
        for cob in sorted(args.scripts.glob('*.cob')):
            data=cob.read_bytes();h=struct.unpack_from('<10I',data)
            names=[]
            for i in range(h[1]):
                offset=struct.unpack_from('<I',data,h[7]+i*4)[0]
                names.append(data[offset:].split(b'\0',1)[0].decode('ascii'))
            inventory.append(dict(script=cob.name,entries=names,pieces=h[2]))
            if not any(n.lower()=='create' for n in names):continue
            state=Path(temporary)/(cob.stem+'.state')
            state.write_bytes(bytes(0xa48+h[4]*4+h[2]*0x6c))
            for profile in range(4):jobs.append((cob,state,profile))
        def run(job):
            cob,state,profile=job
            command=[sys.executable,str(Path(__file__).with_name('check_script_state.py')),
                     str(cob),str(state),'--binary',args.binary,'--ticks',str(args.ticks),
                     '--notify','Create','--profile',str(profile),'--timeline']
            try:
                p=subprocess.run(command,capture_output=True,text=True,timeout=120)
                return dict(script=cob.name,profile=profile,passed=p.returncode==0,
                            detail=(p.stdout+p.stderr).strip())
            except subprocess.TimeoutExpired:
                return dict(script=cob.name,profile=profile,passed=False,detail='timeout')
        if not jobs:raise ValueError("no scripts with Create found in the supplied directory")
        results=[]
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
            for result in pool.map(run,jobs):
                results.append(result)
                if not result['passed']:print('FAIL',result,flush=True)
                elif len(results)%40==0:print(f'{len(results)}/{len(jobs)} checked',flush=True)
        args.output.write_text(json.dumps(dict(inventory=inventory,results=results),indent=2)+'\n')
    failures=sum(not r['passed'] for r in results)
    print(f'{len(inventory)} scripts inventoried; {len(results)} runs; {failures} failures')
    return bool(failures)


if __name__=='__main__':sys.exit(main())
