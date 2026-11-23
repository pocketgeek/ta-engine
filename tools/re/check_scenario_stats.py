#!/usr/bin/env python3
"""Observe native CRT custom-type defaults without launching retail.

Runs KINGDOMS.icd 4cd590; the veteran setter is observed as a host boundary.
The native routine reads the record's armor/weapon/veteran fields, not health.
No executable bytes or assets are distributed by this probe.
"""
import struct
from emu import Icd, HEAP, STACK
from unicorn.x86_const import UC_X86_REG_EBP, UC_X86_REG_EBX, UC_X86_REG_ESI, UC_X86_REG_ESP


def main():
    probe = Icd()
    script, record, unit = HEAP, HEAP + 0x1000, HEAP + 0x2000
    def put(address, fmt, *values):
        probe.uc.mem_write(address, struct.pack(fmt, *values))
    put(script + 0x124, '<II', record, record + 0x32)
    put(record + 0x20, '<H', 17)
    put(unit, '<H', 17)
    veterans = []
    def veteran(uc, args):
        target, level = struct.unpack('<II', uc.mem_read(args, 8))
        assert target == unit
        veterans.append(level)
        return 2, 0
    probe.hooks[0x519370] = veteran
    for health in (0, 50, 100, 200, 1000):
        for armor, weapon, level in ((0, 0, 0), (50, 150, 3), (200, 50, 7), (1000, 1000, 9)):
            put(record + 0x22, '<4i', health, armor, weapon, level)
            put(unit + 0x10c, '<h', 73)
            _, error = probe.call(0x4cd590, (unit,), ecx=script)
            assert not error, error
            attack, defense = struct.unpack('<ff', probe.uc.mem_read(unit + 0xe8, 8))
            assert abs(attack - weapon * .01) < .00001
            assert abs(defense - armor * .01) < .00001
            assert struct.unpack('<h', probe.uc.mem_read(unit + 0x10c, 2))[0] == 73
            assert veterans[-1] == level
    f32 = lambda value: struct.unpack('<f', struct.pack('<f', value))[0]
    for base_armor, base_weapon, armor, weapon in ((200, 50, 50, 150), (137, 243, 91, 321), (0, 100, 1000, 0)):
        put(unit + 0xe8, '<ff', f32(base_weapon * f32(.01)), f32(base_armor * f32(.01)))
        put(record + 0x104, '<ii', armor, weapon)
        probe.uc.reg_write(UC_X86_REG_EBP, STACK + 0x1000)
        probe.uc.reg_write(UC_X86_REG_EBX, record)
        probe.uc.reg_write(UC_X86_REG_ESI, unit)
        probe.uc.emu_start(0x4cd33a, 0x4cd371)
        attack, defense = struct.unpack('<ff', probe.uc.mem_read(unit + 0xe8, 8))
        assert attack == f32(f32(base_weapon * f32(.01)) * weapon * f32(.01))
        assert defense == f32(f32(base_armor * f32(.01)) * armor * f32(.01))
    kind = HEAP + 0x3000
    for maximum in (100, 333):
        for health in (0, 1, 33, 75, 100):
            put(unit + 0xb4, '<I', kind)
            put(unit + 0x10c, '<h', maximum)
            put(kind + 0x1be, '<I', maximum)
            put(record + 0x100, '<i', health)
            put(STACK + 0x1000 - 0x18, '<I', 0)
            probe.uc.reg_write(UC_X86_REG_EBP, STACK + 0x1000)
            probe.uc.reg_write(UC_X86_REG_ESP, STACK + 0x8000)
            probe.uc.reg_write(UC_X86_REG_EBX, record)
            probe.uc.reg_write(UC_X86_REG_ESI, unit)
            probe.uc.emu_start(0x4cd371, 0x4cd3e9)
            actual = struct.unpack('<h', probe.uc.mem_read(unit + 0x10c, 2))[0]
            assert actual == max(0, min(maximum, int(f32(health * f32(.01) * maximum))))
    print('PASS: 20 native custom-stat, 3 placement-product and 10 placement-health cases')


if __name__ == '__main__':
    main()
