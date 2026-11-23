#!/usr/bin/env python3
"""Compare visual refresh decisions and CRT consumption with original code."""
import argparse
import random
import struct
import subprocess
from emu import Icd,HEAP,STACK,STACK_SZ
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EBP,UC_X86_REG_ESP,UC_X86_REG_FPCW


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-dbg/retail_visual_test')
    args=ap.parse_args()
    p=Icd();app,settings,thread=HEAP,HEAP+0x1000,HEAP+0x2000
    frame=STACK+STACK_SZ-0x2000
    put=lambda a,v:p.uc.mem_write(a,struct.pack('<I',v))
    real=lambda a,v:p.uc.mem_write(a,struct.pack('<f',v))
    word=lambda a:struct.unpack('<I',p.uc.mem_read(a,4))[0]
    f32=lambda v:struct.unpack('<f',struct.pack('<f',v))[0]
    put(0x62d558,app);put(app+0x18,settings)
    p.hooks[0x5dc403]=lambda uc,args:(0,thread)
    p.freeze_hooks();p.uc.reg_write(UC_X86_REG_FPCW,0x027f)
    calls=[]
    p.uc.hook_add(UC_HOOK_CODE,lambda uc,a,size,data:calls.append(word(uc.reg_read(UC_X86_REG_ESP))),begin=0x5d4444,end=0x5d4444)
    rng=random.Random(0x4ec7cb);rows=[];expected=[]
    for case in range(8192):
        setting=rng.choice([0,1,50,99,100,101,255]);smooth=rng.randrange(241)
        fps=rng.choice([0,0.8241902474800211,40,60,240,rng.random()*240])
        threshold=f32(rng.choice([1,40,60,240,rng.uniform(1,240)]))
        bias=f32(rng.choice([0,1,5,rng.uniform(0,10)]));probability=f32(rng.random()*2)
        seed=rng.getrandbits(32)
        put(settings+0x1d,setting);put(0x616438,smooth)
        p.uc.mem_write(0x641840,struct.pack('<d',fps))
        real(0x61a004,threshold);real(0x61a008,bias);real(0x641834,probability)
        put(thread+0x14,seed);calls.clear()
        p.uc.reg_write(UC_X86_REG_EBP,frame);p.uc.reg_write(UC_X86_REG_ESP,frame-0x100)
        p.uc.emu_start(0x4ec7cb,0x4ec879,timeout=1000000)
        expected.append([word(0x616438),word(0x641834),word(thread+0x14),len(calls),calls[0] if calls else 0,p.uc.mem_read(frame-1,1)[0]])
        rows.append(' '.join(map(str,[setting,smooth,fps,threshold,bias,probability,seed])))
    proc=subprocess.run([args.binary,'--quality'],input='\n'.join(rows)+'\n',capture_output=True,text=True,check=True)
    actual=[list(map(int,line.split())) for line in proc.stdout.splitlines()]
    if actual!=expected:
        for row,want,got in zip(rows,expected,actual):
            if want!=got:raise AssertionError((row,want,got))
        raise AssertionError('row count')
    print(f'PASS: {len(rows)} original visual-quality decisions, state updates and CRT calls')


if __name__=='__main__':main()
