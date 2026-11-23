#!/usr/bin/env python3
"""Native death-score accumulation slice512bd5..512c6b.

Runs retail arithmetic/gates with synthetic player and unit records. Stops before
resource accounting; no game launch, damage, gameplay or scheduler is substituted.
"""
import struct,itertools
from emu import Icd,HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESI
p=Icd();game,unit,kind=HEAP,HEAP+0x40000,HEAP+0x50000
p.uc.mem_write(0x62d55c,struct.pack('<I',game))
p.uc.mem_write(unit+0xb4,struct.pack('<I',kind))
for stop in (0x512c6b,0x512c9c):p.uc.hook_add(UC_HOOK_CODE,lambda u,a,n,d:u.emu_stop(),begin=stop,end=stop)
# Deliberate early stop: opt into emu.Icd.call returning without reaching the return address.
p.allow_early_stop=True
cases=0
for attacker,owner,remaining,disabled,xp,prior in itertools.product((0,1,10),(0,1),(0.,.25),(0,4),(0,666,5000),(0,123,0x7ffffff0)):
 p.uc.mem_write(unit+0xfc,bytes([attacker,owner]));p.uc.mem_write(unit+0x108,struct.pack('<f',remaining))
 p.uc.mem_write(game+0x3070,bytes([disabled]));p.uc.mem_write(kind+0x1c2,struct.pack('<I',xp))
 score=game+0x24a8+attacker*0x110
 p.uc.mem_write(score,struct.pack('<I',prior));p.uc.reg_write(UC_X86_REG_ESI,unit)
 p.uc.emu_start(0x512bd5,0,count=1000)
 got=struct.unpack('<I',p.uc.mem_read(score,4))[0]
 want=(prior+xp)&0xffffffff if attacker!=10 and attacker!=owner and remaining==0 and not disabled else prior
 assert got==want,(attacker,owner,remaining,disabled,xp,prior,got,want)
 cases+=1
print(f'PASS: {cases} native death-score gates and experiencepoints accumulation including 32-bit wrap')

# The disable bit is not campaign mode: SET_UNIT_VALUE40 explicitly switches to
# script-owned player0 score. GET40 reads a requested raw native player slot.
for prior_flags in (0,1,2,8,0xf0):
 for value in (0,17,2500,0xffffffff):
  p.uc.mem_write(game+0x3070,bytes([prior_flags]))
  _,error=p.call(0x4d3f60,(40,value if value<0x80000000 else value-0x100000000));assert not error,error
  got=struct.unpack('<I',p.uc.mem_read(game+0x24a8,4))[0]
  assert got==value
  assert p.uc.mem_read(game+0x3070,1)[0]==prior_flags|4
  for player in range(8):
   expected=value if player==0 else player*123
   p.uc.mem_write(game+0x24a8+player*0x110,struct.pack('<I',expected))
   got,error=p.call(0x4d4060,(40,player,0,0,0));assert not error,error
   assert got==expected,(player,expected,got)
print('PASS: native SET40 writes player0 score and disables accrual; GET40 reads requested player score')
