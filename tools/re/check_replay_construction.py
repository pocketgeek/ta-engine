#!/usr/bin/env python3
"""Compare restored construction pools/progress with executable tick replay.

Stops before the first missing outer-render CRT call. Completion notifications,
render scheduling and full scene equality are outside this comparison.
"""
import argparse
import json
import struct
import check_captured_tick as tick
from livesample import header, STRIDE


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture');ap.add_argument('port')
    ap.add_argument('--ticks',type=int,default=5)
    args=ap.parse_args()
    if not 1 <= args.ticks <= 10000:ap.error('--ticks must be between 1 and 10000')
    capture=json.load(open(args.capture))
    port={e['tick']:e for line in open(args.port) if (e:=json.loads(line)).get('kind')=='frame'}
    original=tick.snapshot
    checked=[];checked_missions=[]
    def snapshot(p):
        observed=original(p)
        frame=port[observed['tick']]
        # The replay snapshot has slot addresses from the original executable.
        _,base,_,_,_=header(p)
        f32=lambda a:struct.unpack('<f',p.uc.mem_read(a,4))[0]
        word=lambda a:struct.unpack('<H',p.uc.mem_read(a,2))[0]
        for u in frame['units']:
            if 'flying_construction' in u:
                mission=p.u32(base+u['id']*STRIDE+0x60)
                expected=[p.uc.mem_read(mission+5,1)[0],*(p.u32(mission+o) for o in (6,10,0x6a,0x5a))]
                if u['flying_construction']!=expected:
                    raise AssertionError((observed['tick'],u['id'],'flying construction mission',u['flying_construction'],expected))
            if 'construction' not in u:continue
            address=base+u['id']*STRIDE
            if 'get_built' in u:
                mission=p.u32(address+0x60)
                if not mission:raise AssertionError('original GetBuilt mission absent')
                builder=p.u32(mission+0x16)
                expected_mission=[p.uc.mem_read(mission+5,1)[0],p.u32(mission+6),
                    p.u32(mission+10),p.u32(mission+0x6a),word(builder+2) if builder else 0]
                checked_missions.append((observed['tick'],u['id']))
                if u['get_built']!=expected_mission:
                    raise AssertionError((observed['tick'],u['id'],'GetBuilt',u['get_built'],expected_mission))
            actual=(struct.pack('<f',u['construction'][0]),u['construction'][1])
            expected=(bytes(p.uc.mem_read(address+0x108,4)),word(address+0x10c))
            if actual!=expected:raise AssertionError((observed['tick'],u['id'],actual,expected))
        for index,resource in enumerate(frame['resources']):
            pool=p.u32(p.game+0x2404+index*0x110+0x10c)
            if resource['mana']!=f32(pool):
                raise AssertionError((observed['tick'],index,'mana',resource['mana'],f32(pool)))
        checked.append(observed['tick'])
        return observed
    tick.snapshot=snapshot
    result=tick.replay(capture,args.ticks)
    if len(checked)!=args.ticks:raise AssertionError(result)
    mismatch=result['first_mismatch']
    if mismatch:
        differences=mismatch.get('differences',[])
        if mismatch.get('tick')!=checked[-1] or not differences or any(
                d.get('field')!='crt_rng' or d.get('emulated') or
                any(e.get('return_address')!=0x4ec858 for e in d.get('retail',[]))
                for d in differences):
            raise AssertionError(mismatch)
        # A missing renderer call after the final simulated tick is reported,
        # never supplied to the port or replaced with a captured seed.
        print('Remaining executable-replay difference:',json.dumps(mismatch))
    print(f'PASS: construction remaining work, HP and resource pools at {len(checked)} original tick boundaries; {len(checked_missions)} GetBuilt mission comparisons')


if __name__=='__main__':main()
