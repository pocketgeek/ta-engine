#!/usr/bin/env python3
"""Compare the normal C++ ground-order core against captured retail dispatch.

Runs real retail handlers on independently restored initial snapshots. Includes
explicit counterfactual event/deadline inputs, not later-frame state injection.
Requires response mode zero and no diversion/auxiliary-order branch. This does
not certify World integration, queue restoration, or complete-state parity.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess

from emureload import CapturedProcess


def check(capture, binary, identity, pending, advance, stage_override=None, flag_08000000=False):
    p=CapturedProcess(capture)
    p.frame=dict(p.frame)
    u=next(u for u in p.runtime['units'] if u['id']==identity)
    a=u['primary']
    def put(address,value): p.put(address,struct.pack('<I',value & 0xffffffff))
    if pending is not None:
        put(a+0x6a,pending); put(u['address']+0xd0,0)
    if advance:
        p.frame['tick']=p.u32(a+10)  # dispatch one tick after the original deadline
    if stage_override is not None:
        p.put(a+5,bytes((stage_override,))); put(a+6,0)
    if flag_08000000:
        put(a+0x5a,p.u32(a+0x5a)|0x8000000)
    tick=(p.frame['tick']+1)&0xffffffff
    stage=p.uc.mem_read(a+5,1)[0]
    if p.u32(a+0x52) or stage>2:
        raise ValueError('unsupported ground response mode/stage')
    foot=struct.unpack('<h',p.uc.mem_read(u['address']+0x78,2))[0]
    inputs=[tick,p.u32(0x64186c),p.u32(u['address']+0xd0),stage,
            p.u32(a+6),p.u32(a+10),p.u32(a+0x6a),p.u32(a+0x5a),p.u32(a+0x4e),
            foot,u['address'],p.u32(p.obj+0x58)]
    result=p.run_mission(identity)
    if result['error'] or result['missing']:
        raise AssertionError({'id':identity,'error':result['error'],'missing':result['missing']})
    expected=[p.uc.mem_read(a+5,1)[0],p.u32(a+6),p.u32(a+10),p.u32(a+0x6a),p.u32(a+0x5a),
              p.u32(a+0x4e),p.u32(u['address']+0xd0),p.u32(0x64186c),p.u32(p.obj+0x58),
              sum(d['controller_before']!=d['controller_after'] for d in result['dispatches'])]
    expected += [r['bound'] for r in result['rng_calls']]
    actual=subprocess.run([binary,'--ground'],input=' '.join(map(str,inputs))+'\n',
                          text=True,capture_output=True,check=True)
    observed=list(map(int,actual.stdout.split()))
    if observed!=expected:
        raise AssertionError({'id':identity,'pending':pending,'advance':advance,
                              'stage_override':stage_override,'flag_08000000':flag_08000000,
                              'input':inputs,'retail':expected,'port':observed,
                              'dispatches':result['dispatches']})
    return len(result['dispatches'])


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture',type=Path)
    parser.add_argument('--binary',default='./build-dbg/retail_mission_test')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    capture=json.loads(args.capture.read_text())
    runtime=capture['frames'][0]['runtime_state']
    missions={m['address']:m for m in runtime['missions']}
    ids=[]; unsupported=[]
    for u in runtime['units']:
        m=missions.get(u['primary'])
        if not m or m['handler']!=0x402b00: continue
        fields=bytes.fromhex(m['fields_hex'])
        mode=struct.unpack_from('<i',fields,0x52)[0]
        if mode or fields[5]>2:
            unsupported.append({'id':u['id'],'response_mode':mode,'stage':fields[5]})
        else: ids.append(u['id'])
    calls=0; cases=0
    for identity in ids:
        for pending,advance,stage,flag in ((None,False,None,False),(0,False,None,False),
                (0x2000,False,None,False),(0,True,None,False),(None,False,0,False),
                (None,False,1,False),(0x2000,False,None,True)):
            calls+=check(capture,args.binary,identity,pending,advance,stage,flag); cases+=1
    if not cases: raise ValueError('capture has no ground move missions')
    result={'complete_state_match':False,'world_integration':False,
            'units':ids,'unsupported':unsupported,'cases':cases,'handler_calls':calls,'all_compared_fields_match':True,
            'scope':'normal ground dispatcher/core; captured and counterfactual initial events/deadlines'}
    with args.output.open('x') as out: json.dump(result,out,indent=2); out.write('\n')
    print(json.dumps(result))


if __name__=='__main__': main()
