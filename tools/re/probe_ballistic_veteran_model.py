#!/usr/bin/env python3
"""Verify native BallisticWeapon selects a veteran bolt mesh at launch.

Calls retail 0x52be80 with controlled model pointers and a stub for only the
unit-rank query. This covers the shipped Ballista WEAPON1 `model`,
`veteranmodel`, and `veteranlevel` dispatch without launching the game GUI.
"""
import struct
from emu import Icd, HEAP

p = Icd()
game, shot, weapon, wrapper, source, source_aux = [HEAP + i * 0x30000 for i in range(1, 7)]
base_model = HEAP + 0x220000
veteran_model = HEAP + 0x230000
origin = (0x00123456, -0x00034567, 0x002468ac)
active_rank = 0


def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def muzzle_lookup(uc, sp):
    args = struct.unpack('<4I', uc.mem_read(sp, 16))
    assert args == (source, shot + 4, struct.unpack('<I', uc.mem_read(wrapper + 0x1a, 4))[0] & 3,
                    0xffffffff), args
    uc.mem_write(args[1], struct.pack('<3i', *origin))
    return 4, 0


def veteran_rank(_uc, sp):
    assert struct.unpack('<I', _uc.mem_read(sp, 4))[0] == source
    return 1, active_rank


p.hooks.update({0x4dd420: muzzle_lookup, 0x519310: veteran_rank})
p.freeze_hooks()

put(0x62d55c, game)
put(game + 0x19f44, 17)
put(game + 0x19ecc, 8155)
put(source + 0xb4, source_aux)
put(source_aux + 0x8a, 0)
put(weapon + 0x48, base_model)
put(weapon + 0x4c, veteran_model)
put(weapon + 0x50, 10)  # the shipped Ballista's veteranlevel
put(weapon + 0x54, 0)
put(weapon + 0x58, 0)
put(weapon + 0xcc, 760 * 2184)
put(weapon + 0xd0, 2)
put(wrapper, weapon)
put(shot, weapon)
p.uc.mem_write(wrapper + 0x16, struct.pack('<H', 0x1234))
p.uc.mem_write(wrapper + 0x18, struct.pack('<H', 0x4567))
p.uc.mem_write(wrapper + 0x1a, struct.pack('<H', 2))

for active_rank, expected in ((0, base_model), (9, base_model),
                              (10, veteran_model), (11, veteran_model)):
    _, error = p.call(0x52be80, (shot, source, wrapper))
    assert error is None, (active_rank, error)
    got = struct.unpack('<I', p.uc.mem_read(shot + 0x93, 4))[0]
    assert got == expected, (active_rank, hex(got), hex(expected))

print('PASS: retail 0x52be80 stores Ballista model below veteranlevel 10 and veteranmodel at ranks 10+ in shot+0x93')
