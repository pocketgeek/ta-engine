#!/usr/bin/env python3
"""Observe what native CRT setup does with a type name the roster lacks.

A Crusades game loads only unitscb/, so a scenario made under the standard
balance can name a type (zonspide, monmum, ...) that does not exist. This runs
KINGDOMS.icd's custom-type resolve loop (4cd235..4cd25a), its name lookup
(515ac0 -> 5159f0) and the custom-stat apply (4cd590) over a small synthetic
type table. No executable bytes or assets are distributed by this probe.

Observed: the lookup skips table slot 0 and answers 0 for an unknown name; the
loop stores that 0 with no test and no message; 4cd590 matches a unit's type id
(always >= 1) against the stored ids, so the record is simply never applied.
The placed-unit loop (4cd28a) uses the same lookup and indexes the type table
with the 0 unchecked; that path is not exercised here.
"""
import struct
from emu import Icd, HEAP, STACK
from unicorn.x86_const import UC_X86_REG_EBX, UC_X86_REG_EDI, UC_X86_REG_ESP

GAME, TABLE, SCRIPT, RECORDS, UNIT = HEAP, HEAP + 0x20000, HEAP + 0x30000, HEAP + 0x31000, HEAP + 0x32000
ENTRY = 0x2a4


def main():
    probe = Icd()
    put = lambda address, fmt, *values: probe.uc.mem_write(address, struct.pack(fmt, *values))
    names = ['', 'ARASWORD', 'MONELEM', 'ZONHAND']   # slot 0 is the sentinel; sorted after it
    for slot, name in enumerate(names):
        base = TABLE + slot * ENTRY
        probe.uc.mem_write(base, bytes(ENTRY))
        probe.uc.mem_write(base + 0x20, name.encode() + b'\0')
        put(base + 0x80, '<H', slot)
    put(0x62d55c, '<I', GAME)
    put(GAME + 0x175b8, '<I', len(names))
    put(GAME + 0x175c4, '<I', TABLE)

    wanted = ['zonhand', 'ZONSPIDE', 'arasword', 'monmum', 'monelem']
    for i, name in enumerate(wanted):
        record = RECORDS + i * 0x32
        probe.uc.mem_write(record, bytes(0x32))
        probe.uc.mem_write(record, name.encode() + b'\0')
        put(record + 0x20, '<H', 0xbeef)
        put(record + 0x22, '<4i', 0, 50 + i, 150 + i, 0)   # health, armor, weapon, veteran
    put(SCRIPT + 0x124, '<II', RECORDS, RECORDS + len(wanted) * 0x32)
    probe.uc.reg_write(UC_X86_REG_EBX, SCRIPT)
    probe.uc.reg_write(UC_X86_REG_EDI, SCRIPT)
    probe.uc.reg_write(UC_X86_REG_ESP, STACK + 0x8000)
    probe.uc.emu_start(0x4cd235, 0x4cd25a, timeout=5_000_000)
    ids = [struct.unpack('<H', probe.uc.mem_read(RECORDS + i * 0x32 + 0x20, 2))[0] for i in range(len(wanted))]
    assert ids == [3, 0, 1, 0, 2], ids   # case-insensitive; unknown names resolve to 0, no stop

    probe.hooks[0x519370] = lambda uc, args: (2, 0)   # veteran setter: a host boundary
    for type_id, record in ((1, 2), (2, 4), (3, 0)):
        put(UNIT, '<H', type_id)
        _, error = probe.call(0x4cd590, (UNIT,), ecx=SCRIPT)
        assert not error, error
        attack, defense = struct.unpack('<ff', probe.uc.mem_read(UNIT + 0xe8, 8))
        assert abs(defense - (50 + record) * .01) < 1e-5 and abs(attack - (150 + record) * .01) < 1e-5
    print('PASS: unknown custom types resolve to id 0 and are skipped silently; known ones still apply')


if __name__ == '__main__':
    main()
