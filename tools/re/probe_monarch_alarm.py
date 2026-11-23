#!/usr/bin/env python3
"""Observe the retail monarch alarm cooldown/payload without launching retail."""
import struct
from emu import Icd, STACK, STACK_SZ
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESP

p = Icd()
calls = []
def play(uc, args):
    name, priority, positional, volume = struct.unpack('<4I', uc.mem_read(args, 16))
    name = bytes(uc.mem_read(name, 32)).split(b'\0')[0].decode()
    calls.append((name, priority, positional, volume))
    return 4, 0
p.hooks[0x50a720] = play
p.uc.hook_add(UC_HOOK_CODE, lambda u,a,n,d: u.emu_stop(), begin=0x50ac28, end=0x50ac28)
# Deliberate early stop: opt into emu.Icd.call returning without reaching the return address.
p.allow_early_stop = True
# Enter after the native wall-clock conversion to integer milliseconds.
for now, deadline, expected in [(0,0,True),(14999,15000,False),(15000,15000,True),
                                 (15001,15000,True),(100000,115000,False)]:
    calls.clear()
    p.uc.mem_write(0x640a64, struct.pack('<I', deadline))
    p.uc.reg_write(UC_X86_REG_EAX, now)
    p.uc.reg_write(UC_X86_REG_ESP, STACK + STACK_SZ - 0x1000)
    p.uc.emu_start(0x50ac01, 0, count=1000)
    assert calls == ([('AlarmMon',7,0,127)] if expected else []), calls
    actual = struct.unpack('<I',p.uc.mem_read(0x640a64,4))[0]
    assert actual == (now + 15000 if expected else deadline), (now,actual)
print('PASS: native AlarmMon name, priority 7, global routing, and 15000 ms cooldown boundaries')
