#!/usr/bin/env python3
"""Compare spawn/stance animation inputs with the local retail setter and GET 46."""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP

p = Icd()
unit, script, host = [HEAP + i * 0x10000 for i in range(3)]
def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))
def call(address, args, ecx=0):
    result, error = p.call(address, args, ecx=ecx)
    if error:
        raise RuntimeError(error)
    return result
put(script + 0xa64, host)
put(host + 0xc, unit)
rng = random.Random(0x5198a0)
rows, expected = [], []
for i in range(2048):
    enabled = i & 1
    standing, move, fire = (rng.randrange(4) for _ in range(3))
    request = (i // 2) % 4  # 3 retains the initial/default standing order
    flags = (rng.getrandbits(32) & ~0x13f0000) | (enabled << 24)
    flags |= (standing << 20) | (fire << 18) | (move << 16)
    put(unit + 0x130, flags)
    if request < 3:
        call(0x5198a0, (unit, request))
    expected.append(tuple(call(0x50ceb0, (query, 0, 0, 0, 0), script)
                          for query in (46, 2, 3)))
    rows.append(f'{enabled} {standing} {move} {fire} {request}')
result = subprocess.run([sys.argv[1] if len(sys.argv) > 1 else
                         'build-o2/animation_roster_test', '--standing-order'],
                        input='\n'.join(rows) + '\n', text=True,
                        capture_output=True, check=True)
actual = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]
assert actual == expected, next(((rows[i], a, b) for i, (a, b) in
                                enumerate(zip(actual, expected)) if a != b),
                               'length mismatch')
print('PASS: 2048 standing-order/default/setter cases match retail GET 46, 2 and 3')

# The [UNITINFO] parser (4c005d..4c00fb) for every combination of the three
# keys, absent or 0..3, through the native getter's defaults: the composite it
# stores (UnitDef +0x260 bits 4-5, copied to +0x130 bits 20-21 at 511c5d and
# read by GET 46) and the move/fire fields, against World's rule (sim.cpp,
# "Standing orders"). An absent or 3 standingunitorder is stored as 2.
import itertools
from unicorn.x86_const import UC_X86_REG_EBX, UC_X86_REG_ESI, UC_X86_REG_ESP
typ, obj = HEAP + 0x40000, HEAP + 0x41000
keys = {}
def getter(uc, sp):
    name_ptr, default = struct.unpack('<Ii', uc.mem_read(sp, 8))
    name = bytes(uc.mem_read(name_ptr, 32)).split(b'\0')[0].decode()
    return 2, keys.get(name, default)
p.hooks[0x543190] = getter
put(obj + 4, HEAP + 0x42000)
def ours(suo, smo, sfo):
    suo = 3 if suo is None else suo & 3
    if suo == 2: return 2, 1, 2
    if suo == 1: return 1, 0, 2
    if suo == 0: return 0, 0, 0
    return 2, (2 if smo is None else smo & 3), (2 if sfo is None else sfo & 3)
cases = 0
for suo, smo, sfo in itertools.product((None, 0, 1, 2, 3), repeat=3):
    keys.clear()
    for name, value in (('standingunitorder', suo), ('standingmoveorder', smo), ('standingfireorder', sfo)):
        if value is not None: keys[name] = value
    p.uc.mem_write(typ, bytes(0x300))
    p.uc.reg_write(UC_X86_REG_EBX, typ); p.uc.reg_write(UC_X86_REG_ESI, obj)
    p.uc.reg_write(UC_X86_REG_ESP, 0x70000000 + 0x100000 - 0x1000)
    p.uc.emu_start(0x4c005d, 0x4c00fb)
    flags = struct.unpack('<I', p.uc.mem_read(typ + 0x260, 4))[0]
    native = ((flags >> 4) & 3, flags & 3, (flags >> 2) & 3)
    assert native == ours(suo, smo, sfo), (suo, smo, sfo, native, ours(suo, smo, sfo))
    cases += 1
print(f'PASS: {cases} standing-order key combinations parse as retail (absent or 3 -> composite 2)')
