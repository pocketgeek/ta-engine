#!/usr/bin/env python3
"""Compare the initial emitter geometry/quality decoder with retail arithmetic.

Executes 4ee377..4ee46f, before allocator and visual-constructor side effects.
Structure classification and quality inputs are explicit host inputs.
"""
import random
import struct
from emu import Icd, HEAP, STACK, STACK_SZ
from decode_player_cache import construction_emitter_profile
from unicorn.x86_const import UC_X86_REG_EBP, UC_X86_REG_EBX, UC_X86_REG_EDI, UC_X86_REG_ESI, UC_X86_REG_ESP, UC_X86_REG_FPCW


def main():
    p=Icd()
    unit,kind,visual=HEAP,HEAP+0x1000,HEAP+0x2000
    frame=STACK+STACK_SZ-0x2000
    put=lambda a,v:p.uc.mem_write(a,struct.pack('<I',v))
    signed=lambda a:struct.unpack('<i',p.uc.mem_read(a,4))[0]
    put(unit+0xb4,kind);put(visual+12,unit)
    p.uc.reg_write(UC_X86_REG_FPCW,0x027f)
    rng=random.Random(0x4ee377)
    for index in range(4096):
        origin=[rng.randint(-100000000,100000000) for _ in range(3)]
        size=[rng.choice([0,1,65535,65536,8*65536,rng.randrange(0x8000000)]) for _ in range(3)]
        bounds=origin+[a+b for a,b in zip(origin,size)]
        structure,movement,quality=rng.randrange(2),rng.randrange(4),rng.randrange(2)
        current,target=rng.randrange(120),rng.randrange(120)
        p.uc.mem_write(kind+0x13a,struct.pack('<6i',*bounds))
        p.uc.mem_write(kind+0x24a,bytes([movement]))
        put(unit+0x130,0x2000000 if structure else 0)
        p.uc.mem_write(0x61a01c,bytes([quality]));put(0x61a024,current);put(0x61a02c,target)
        for register,value in [(UC_X86_REG_EBP,frame),(UC_X86_REG_ESP,frame-0x100),
                               (UC_X86_REG_EBX,unit),(UC_X86_REG_EDI,visual)]:
            p.uc.reg_write(register,value)
        p.uc.emu_start(0x4ee377,0x4ee46f,timeout=1000000)
        expected=(signed(frame-4),p.uc.reg_read(UC_X86_REG_ESI),signed(frame+16))
        actual=construction_emitter_profile(bounds,structure,movement,quality,current,target)
        if actual!=expected:raise AssertionError((index,bounds,structure,movement,quality,current,target,expected,actual))
    print('PASS: 4096 original emitter geometry and adaptive-quality cases')


if __name__=='__main__':main()
