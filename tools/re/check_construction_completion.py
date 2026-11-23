#!/usr/bin/env python3
"""Compare completion flags and requested callbacks with original 429990.

Attachment, activation, alliance and notification callbacks are observed hosts;
this does not verify their internal behavior or completion mission scheduling.
"""
import argparse
import random
import struct
import subprocess
from emu import Icd,HEAP


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-dbg/retail_construction_test')
    args=ap.parse_args();p=Icd()
    builder,site,owner,bkind,skind=[HEAP+0x1000*i for i in range(5)]
    put=lambda a,v:p.uc.mem_write(a,struct.pack('<I',v))
    word=lambda a:struct.unpack('<I',p.uc.mem_read(a,4))[0]
    calls=[];allied=[False]
    p.hooks[0x519850]=lambda uc,argv:(2,int(allied[0]))
    p.hooks[0x51b4f0]=lambda uc,argv:(calls.append('detach') or (5,0))
    p.hooks[0x51e4d0]=lambda uc,argv:(calls.append('activate') or (2,0))
    p.hooks[0x4ea3b0]=lambda uc,argv:(calls.append('notify') or (2,0))
    p.freeze_hooks();rng=random.Random(0x429990);rows=[];expected=[]
    for case in range(8192):
        remaining=struct.unpack('<f',struct.pack('<f',rng.random()))[0]
        flags=rng.getrandbits(32);present,forced,same,relation=[rng.randrange(2) for _ in range(4)]
        same=bool(same and present)
        builder_flags=rng.getrandbits(32);builder_type=rng.getrandbits(32)
        secondary,site_type=rng.getrandbits(32),rng.getrandbits(32)
        if same:builder_flags=flags;builder_type=site_type
        owner_present,attached=rng.randrange(2),rng.randrange(2)
        role,death=rng.randrange(4),rng.randrange(256)
        put(site+0x130,flags);put(site+0xb4,skind);put(site+0xb8,owner)
        put(site+0xa8,HEAP+0x6000 if attached else 0)
        p.uc.mem_write(site+0x108,struct.pack('<f',remaining));p.uc.mem_write(site+0x10f,bytes([death]))
        put(owner,owner_present);p.uc.mem_write(owner+0xea,bytes([role]))
        put(skind+0x260,site_type);put(skind+0x264,secondary)
        put(builder+0x130,builder_flags);put(builder+0xb4,bkind)
        put(bkind+0x260,builder_type);put(bkind+0x264,secondary)
        allied[0]=bool(relation);calls.clear()
        _,error=p.call(0x429990,(site if same else builder if present else 0,site,forced))
        if error:raise RuntimeError(error)
        expected.append([word(site+0x108),word(site+0x130),p.uc.mem_read(site+0x10f,1)[0],
                         calls.count('detach'),calls.count('activate'),calls.count('notify')])
        if calls!=[x for x in ('detach','activate','notify') if x in calls]:raise AssertionError('callback order')
        rows.append(' '.join(map(str,[remaining,flags,present,forced,int(same),relation,builder_flags,
            builder_type,secondary,site_type,owner_present,attached,role,death])))
    proc=subprocess.run([args.binary,'--complete'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual=[list(map(int,row.split())) for row in proc.stdout.splitlines()]
    if actual!=expected:
        for row,want,got in zip(rows,expected,actual):
            if want!=got:raise AssertionError((row,want,got))
        raise AssertionError('row count')
    print('PASS: 8192 original construction completion gates, flags and notification requests')


if __name__=='__main__':main()
