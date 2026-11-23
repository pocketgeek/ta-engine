#!/usr/bin/env python3
"""Compare free-slot selection and actual CRT rand execution with retail."""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP, STACK, STACK_SZ
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EIP, UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EBX, UC_X86_REG_EDI, UC_X86_REG_ESI


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runner'); args=parser.parse_args()
    p=Icd(); uc=p.uc
    gs=HEAP; pool=HEAP+0x40000; thread=HEAP+0x30000; ebp=STACK+STACK_SZ-1024
    def write(address,value): uc.mem_write(address,struct.pack('<I',value&0xffffffff))
    write(0x62d55c,gs)
    p.hooks[0x5dc403]=lambda uc,args:(0,thread)
    for address in (0x512255,0x5121cd):
        uc.hook_add(UC_HOOK_CODE,lambda uc,a,s,d:uc.emu_stop(),begin=address,end=address)
    # Deliberate early stop: opt into emu.Icd.call returning without reaching the return address.
    p.allow_early_stop=True
    rng=random.Random(0x5121d8); fixtures=[]; expected=[]
    for capacity in (1,4,31,500):
        for pattern in range(8):
            for seed in (0,1,0xffffffff,0x12345678,0xb209a60f):
                flags=[0x1000000 if (pattern==0 or (pattern>1 and rng.randrange(4))) else 0 for _ in range(capacity)]
                # Irrelevant lower flag bits must not mark an allocated slot.
                flags=[f|rng.randrange(65536) for f in flags]
                allocated=sum(bool(f&0x1000000) for f in flags)
                uc.mem_write(gs+0x14e7c,struct.pack('<H',capacity))
                uc.mem_write(gs+0x24ec,struct.pack('<H',allocated))
                write(gs+0x2478,pool); write(gs+0x247c,pool+(capacity-1)*0x138)
                write(thread+0x14,seed); write(ebp+0x24,0)
                for i,flag in enumerate(flags): write(pool+i*0x138+0x130,flag)
                for reg,value in ((UC_X86_REG_EBP,ebp),(UC_X86_REG_ESP,ebp-128),
                                  (UC_X86_REG_EBX,gs),(UC_X86_REG_EDI,gs)):
                    uc.reg_write(reg,value)
                uc.emu_start(0x512162,0,timeout=5_000_000)
                eip=uc.reg_read(UC_X86_REG_EIP)
                assert eip in (0x512255,0x5121cd),hex(eip)
                slot=(uc.reg_read(UC_X86_REG_ESI)-pool)//0x138 if eip==0x512255 else -1
                end_seed=struct.unpack('<I',uc.mem_read(thread+0x14,4))[0]
                expected.append(f'{slot} {end_seed}')
                fixtures.append(' '.join(map(str,(seed,capacity,allocated,*flags))))
    actual=subprocess.run([args.runner,'--slot'],input='\n'.join(fixtures)+'\n',text=True,capture_output=True,check=True).stdout.splitlines()
    assert actual==expected,next(((i,a,b) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),(len(actual),len(expected)))
    print(f'PASS: {len(expected)} entity allocations; selected free slot and CRT stream match retail')
    # 511570 gives every pool maxunits records, as 499590 (thiscall, stores
    # +0xc) clamps the requested unit cap. World starts a pool at that size
    # for every positive cap: retail's own 20..500 and the engine's larger
    # lobby caps alike. (Cap 0 is the engine's "uncapped", no retail value.)
    options=HEAP+0x60000; caps=list(range(1,2101))+[4000,65535,0x7fffffff]; natives=[]
    for cap in caps:
        write(options+0xc,0xdeadbeef)
        _,error=p.call(0x499590,(cap,),ecx=options); assert error is None,error
        natives.append(struct.unpack('<I',uc.mem_read(options+0xc,4))[0])
    world=subprocess.run([args.runner,'--pool-capacity'],input='\n'.join(map(str,caps))+'\n',text=True,capture_output=True,check=True).stdout.split()
    world=[int(v) for v in world]
    assert world==natives,next((c,a,b) for c,a,b in zip(caps,world,natives) if a!=b)
    print(f'PASS: {len(caps)} unit caps; World pool capacity matches 499590 maxunits '
          f'(250 -> {natives[249]}, 500 -> {natives[499]}, 2000 -> {natives[1999]})')

if __name__=='__main__': main()
