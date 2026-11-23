#!/usr/bin/env python3
"""Probe retail's Airstrike cursor and its authored weapon flag without a GUI.

The first fixture executes the retail parser instructions that read FBI key
`dropped` and set WeaponType+0xc8 bit 0x20. The second passes that WeaponType
through the selected unit's real active-weapon slot into cursor selector
0x4dd780, mode 3 (the ATTACK order). The outer action eligibility predicate is
stubbed true; the weapon parser and cursor selector branches execute natively.
"""
import struct

from emu import HEAP, STACK, STACK_SZ, Icd
from unicorn.x86_const import (
    UC_X86_REG_EAX,
    UC_X86_REG_EBP,
    UC_X86_REG_EBX,
    UC_X86_REG_EDI,
    UC_X86_REG_ESI,
    UC_X86_REG_ESP,
)


WEAPON_FLAG_PARSER = 0x531280
WEAPON_FLAG_PARSER_END = 0x5312AA
WEAPON_FLAG_READER = 0x543190
DROPPED_KEY = 0x619DC8
CURSOR_SELECTOR = 0x4DD780
AIRSTRIKE_MODE = 3
AIRSTRIKE_SLOT = 2
ATTACK_SLOT = 1
NORMAL_SLOT = 19
AIRSTRIKE_NAME = 0x6145E8


def put_u32(uc, address, value):
    uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))


def get_u32(uc, address):
    return struct.unpack("<I", uc.mem_read(address, 4))[0]


def c_string(uc, address):
    data = bytearray()
    while True:
        byte = uc.mem_read(address + len(data), 1)[0]
        if byte == 0:
            return data.decode("ascii")
        data.append(byte)


def main():
    icd = Icd()
    uc = icd.uc
    weapon_type = HEAP + 0x12000
    config_node = HEAP + 0x13000
    unit = HEAP + 0x10000
    unit_type = HEAP + 0x11000
    dropped_value = [0]
    parsed_keys = []

    def read_weapon_int(uc, sp):
        key, default = struct.unpack("<2I", uc.mem_read(sp, 8))
        parsed_keys.append((key, default))
        assert key == DROPPED_KEY, hex(key)
        return 2, dropped_value[0]

    icd.hooks[WEAPON_FLAG_READER] = read_weapon_int
    icd.hooks[0x520A80] = lambda _uc, _sp: (0, 1)
    icd.freeze_hooks()

    # Execute the native WeaponType parser block that writes +0x98 and builds
    # the +0xc8 flag bit from the same authored FBI key. Other bits must survive.
    for value, expected_flag in ((0, 0x40000), (1, 0x40020)):
        dropped_value[0] = value
        put_u32(uc, weapon_type + 0xC8, 0x40000)
        uc.reg_write(UC_X86_REG_EBX, weapon_type)
        uc.reg_write(UC_X86_REG_ESI, config_node)
        uc.reg_write(UC_X86_REG_EDI, 0)  # authored boolean's default
        uc.reg_write(UC_X86_REG_EAX, 0)
        uc.reg_write(UC_X86_REG_EBP, STACK + 0x10000)
        uc.reg_write(UC_X86_REG_ESP, STACK + STACK_SZ - 0x1000)
        parsed_keys.clear()
        uc.emu_start(WEAPON_FLAG_PARSER, WEAPON_FLAG_PARSER_END, timeout=1_000_000)
        assert get_u32(uc, weapon_type + 0xC8) == expected_flag
        assert parsed_keys == [(DROPPED_KEY, 0)], parsed_keys

    # Native UnitDef weapon slots begin at unit+0x0c with 0x1c-byte spacing.
    # 0x519ac0 selects slot zero when weapon switching is disabled.
    put_u32(uc, unit + 0x130, 0x01000000)  # live; invalid-state bit clear
    put_u32(uc, unit + 0xB4, unit_type)
    put_u32(uc, unit + 0x8, 1)
    put_u32(uc, unit + 0xC, weapon_type)
    put_u32(uc, unit_type + 0x264, 0x20)   # selector gate; authored source unresolved
    assert c_string(uc, AIRSTRIKE_NAME) == "cursorairstrike"

    # Native action selector mode 3 is ATTACK. Airstrike requires both the
    # UnitDef capability bit and the selected WeaponType `dropped` flag.
    put_u32(uc, weapon_type + 0xC8, 0x40020)
    slot, error = icd.call(CURSOR_SELECTOR,
                           args=(AIRSTRIKE_MODE, unit, 0, 0))
    assert error is None, error
    assert slot == AIRSTRIKE_SLOT, f"dropped attack returned slot {slot}, expected 2"

    put_u32(uc, unit_type + 0x264, 0)
    slot, error = icd.call(CURSOR_SELECTOR,
                           args=(AIRSTRIKE_MODE, unit, 0, 0))
    assert error is None, error
    assert slot == NORMAL_SLOT, f"missing attack capability returned slot {slot}, expected 19"

    put_u32(uc, unit_type + 0x264, 0x20)
    put_u32(uc, weapon_type + 0xC8, 0x40000)
    slot, error = icd.call(CURSOR_SELECTOR,
                           args=(AIRSTRIKE_MODE, unit, 0, 0))
    assert error is None, error
    assert slot == ATTACK_SLOT, f"ordinary attack returned slot {slot}, expected 1"

    print("PASS: native FBI `dropped` parser key sets only WeaponType+0xc8 bit 0x20")
    print("PASS: ATTACK mode 3 returns Airstrike only with UnitDef+0x264 and active-weapon flags")


if __name__ == "__main__":
    main()
