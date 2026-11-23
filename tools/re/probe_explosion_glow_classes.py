#!/usr/bin/env python3
"""Compare built-in retail glow timelines with the production class envelopes.

Leave native class records untouched. Execute manager update and draw; only
viewport admission and final geometry submission are sinks. No sprites/debris.
"""
import struct
import subprocess
import sys
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW

p = Icd()
p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
manager, effect, game, settings, options, sight = [HEAP+i*0x40000 for i in range(6)]

def put(address, *values):
    p.uc.mem_write(address, struct.pack('<'+'I'*len(values),
                                       *(v & 0xffffffff for v in values)))

calls = []
def draw(uc, sp):
    calls.append(struct.unpack('<5i', uc.mem_read(sp, 20)))
    return 5, 0

p.hooks.update({0x48c870: lambda uc, sp: (1, 1), 0x4916d0: draw})
p.freeze_hooks()
put(0x62d55c, game)
put(0x62d558, settings)
put(settings+8, options)
p.uc.mem_write(options+0x15, b'\1')
put(game+0x2404+0x88, sight, 64, 64)
p.uc.mem_write(sight, b'\1'*4096)
rows, native = [], []
for kind in range(3):
    for start in (0, 1973, 0xfffffffa):
        p.uc.mem_write(manager, bytes(0x3000))
        p.uc.mem_write(effect, bytes(64))
        put(manager+0x2b5c, effect)
        put(effect+0xc, 500*65536, 20*65536, 500*65536)
        put(effect+0x18, start, kind)
        p.uc.mem_write(effect+0x20, b'\1')
        for elapsed in range(26):
            put(game+0x19f44, start+elapsed)
            _, error = p.call(0x492320, ecx=manager)
            assert not error, error
            calls.clear()
            _, error = p.call(0x492070, ecx=manager)
            assert not error, error
            assert len(calls) <= 1
            if calls:
                assert calls[0][:2] == (500, 490) and calls[0][4] == 196
            native.append((1, *calls[0][2:4]) if calls else (0, 0, 0))
            rows.append(f'{kind} {elapsed}')

assert len(sys.argv) == 2, 'pass the retail_visual_test executable'
result = subprocess.run([sys.argv[1], '--glow-class'], input='\n'.join(rows)+'\n',
                        text=True, capture_output=True, check=True)
actual = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]
assert len(actual) == len(native)
for row, observed, expected in zip(rows, actual, native):
    assert observed == expected, (row, observed, expected)
print(f'PASS: {len(rows)} built-in glow update/draw samples match production classes, including expiry and timestamp wrap')
