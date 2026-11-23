#!/usr/bin/env python3
"""Diagnose the first render-quality CRT prefix after executable tick replay.

Uses the captured camera and quality settings, original culling/row-bin code,
and original quality arithmetic. Does not run camera input, drawing, outer
frame timing or GPU work, and must not be used to inject a draw count into World.
"""
import argparse
import json
import struct
import check_captured_tick as tick
from emureload import CapturedProcess
from emu import STACK,STACK_SZ
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EBP,UC_X86_REG_EBX,UC_X86_REG_ESI,UC_X86_REG_ESP,UC_X86_REG_EIP


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture');ap.add_argument('--ticks',type=int,default=5)
    args=ap.parse_args()
    capture=json.load(open(args.capture))
    if not 1<=args.ticks<len(capture['frames']):ap.error('invalid tick count')
    instances=[]
    class Observed(CapturedProcess):
        def __init__(self,data):
            super().__init__(data);instances.append(self)
    tick.CapturedProcess=Observed
    replay=tick.replay(capture,args.ticks)
    p=instances[0];game=p.game;bins=game+0x19e64;frame=STACK+STACK_SZ-0x2000
    signed=lambda a:struct.unpack('<i',p.uc.mem_read(a,4))[0]
    word=lambda a:struct.unpack('<H',p.uc.mem_read(a,2))[0]
    actual_tick=p.u32(game+0x19f44)
    if actual_tick!=capture['frames'][args.ticks]['tick']:raise AssertionError(replay)
    # 51f970 builds the visible list using the current camera and visibility.
    _,error=p.icd.call(0x51f970)
    if error:raise RuntimeError(error)
    # Execute row initialization, viewport bounds and list binning, stopping
    # before any visual callbacks or native renderer calls.
    for register,value in [(UC_X86_REG_EBP,frame),(UC_X86_REG_ESP,frame-0x100),
                           (UC_X86_REG_EBX,bins),(UC_X86_REG_ESI,game)]:
        p.uc.reg_write(register,value)
    p.uc.emu_start(0x4fc648,0x4fc7db,timeout=5000000)
    if p.uc.reg_read(UC_X86_REG_EIP)!=0x4fc7db:raise RuntimeError('row binning did not finish')
    start=signed(frame-0x24);count=signed(frame-8)
    rows=p.u32(bins+0x58);width=p.u32(bins+0x54)
    if not 0<=start<=rows<=4096 or not 0<=count<=rows-start or not 0<width<=65536:
        raise ValueError('invalid render row bounds')
    selected=[]
    for row in range(start,start+count):
        entries=word(p.u32(bins+8)+row*2)
        if entries>width:raise ValueError('render row exceeds capacity')
        for i in range(entries):
            unit=p.u32(p.u32(bins)+4*(row*width+i))
            flags=p.u32(unit+0x130)
            if flags&0x1000003==0x1000001 and p.u32(unit+0xbc) and not p.u32(unit+0xa8):
                selected.append(word(unit+2))
    thread=tick.find_crt_thread(p,capture['rng_calls'][0]['registers']['ebp'])
    crt=[];owner=[None]
    def observe(uc,address,size,extra):
        crt.append({'tick':actual_tick,'return_address':p.u32(uc.reg_read(UC_X86_REG_ESP)),
                    'seed_before':p.u32(thread+0x14),'unit':owner[0]})
    p.uc.hook_add(UC_HOOK_CODE,observe,begin=0x5d4444,end=0x5d4444)
    initial_quality=signed(0x616438)
    for identity in selected:
        owner[0]=identity
        p.uc.reg_write(UC_X86_REG_EBP,frame);p.uc.reg_write(UC_X86_REG_ESP,frame-0x100)
        p.uc.emu_start(0x4ec7cb,0x4ec879,timeout=1000000)
        if p.uc.reg_read(UC_X86_REG_EIP)!=0x4ec879:raise RuntimeError('quality decision did not finish')
    expected=[e for e in capture['crt_calls'] if e['tick']==actual_tick and e['return_address'] in (0x4ec858,0x4ec7eb)]
    actual=[{k:e[k] for k in ('tick','return_address','seed_before')} for e in crt]
    if actual!=expected:raise AssertionError({'retail':expected,'extracted':actual})
    print(json.dumps({'diagnostic_only':True,'tick':actual_tick,'selected_units':selected,
        'initial_quality_counter':initial_quality,'final_quality_counter':signed(0x616438),
        'matching_crt_calls':crt,'limitations':['captured camera retained; camera input not executed',
        'one explicit render decision pass; outer-frame cadence not validated',
        'drawing and its other side effects excluded; not full-scene parity']},indent=2))


if __name__=='__main__':main()
