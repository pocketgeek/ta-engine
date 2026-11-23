#!/usr/bin/env python3
"""Observe native type footprint bounds initialization, excluding legacy mode.

4c1410..4c1494 executes unchanged; only 48d070's mode query is substituted.
Later type modifications and allocation are outside this probe's scope.
"""
import struct
from emu import Icd,HEAP,STACK,STACK_SZ
from unicorn.x86_const import UC_X86_REG_EBX,UC_X86_REG_ESP,UC_X86_REG_EIP
p=Icd();p.hooks[0x48d070]=lambda uc,sp:(0,0);p.freeze_hooks()
cases=0
for x in (1,2,3,7,12,32):
 for z in (1,2,3,7,12,32):
  p.uc.mem_write(HEAP+0x126,struct.pack('<hh',x,z))
  p.uc.mem_write(HEAP+0x14a,struct.pack('<i',81*65536))
  p.uc.reg_write(UC_X86_REG_EBX,HEAP)
  p.uc.reg_write(UC_X86_REG_ESP,STACK+STACK_SZ-0x1000)
  p.uc.emu_start(0x4c1410,0x4c1494,timeout=1000000)
  assert p.uc.reg_read(UC_X86_REG_EIP)==0x4c1494
  bounds=struct.unpack('<6i',p.uc.mem_read(HEAP+0x13a,24))
  assert bounds==(-x*8*65536,0,-z*8*65536,x*8*65536,81*65536,z*8*65536),bounds
  cases+=1
print(f'PASS: {cases} native type bound initializations; footprint half-extents, zero bottom, preserved model top in non-legacy mode')
