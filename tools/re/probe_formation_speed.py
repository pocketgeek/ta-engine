#!/usr/bin/env python3
"""Observe the group (formation) speed cap inside retail's ground speed update.

Runs original 4d95f0 with a navigator whose formation bit is reported set and
an owner whose group table supplies the group's ground (+0x58) or boat (+0x84)
speed. A code hook reads the speed maximum the routine has chosen at 4d9a2d,
just before the pitch limit, and compares it with the rule documented in
docs/pathfinding-port.md ("Retail group pacing"). The model below is that
written rule, not a table taken from the binary. Observation only: nothing
from the binary is copied into the engine.

    python3 -B tools/re/probe_formation_speed.py
"""
import random
import struct
from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_FPCW, UC_X86_REG_EBP


def scaled(value, flags, road, water):
    if flags & 0x800: return (road * value) >> 16
    if flags & 0x1000: return (water * value) >> 16
    return value


def mode_scaled(value, flags):
    mode = flags & 0x700
    if mode == 0x100: return (value * 0xaac0) >> 16
    if mode == 0x200: return (value * 0x553f) >> 16
    return value


def model(unit_max, type_max, flags, road, water, kind, active, group, boat, ground):
    own = mode_scaled(scaled(unit_max, flags, road, water), flags)
    if kind not in (1, 2) or not active: return own
    speed = boat if group else ground
    if speed <= 0: return own
    base = scaled(type_max, flags, road, water)
    floor = int(base * 0.25)
    if speed < floor: speed = floor
    if speed >= mode_scaled(base, flags): return own
    if base <= 0: return own
    ratio = int((scaled(unit_max, flags, road, water) / 65536) / (base / 65536) * 65536.0)
    return (ratio * speed) >> 16


def main():
    p = Icd(); p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    unit, mover, kind, nav, vt, owner, table = (HEAP + k * 0x1000 for k in range(7))
    big = HEAP + 0x10000
    def put(fmt, address, *values): p.uc.mem_write(address, struct.pack('<' + fmt, *values))
    put('I', unit + 8, mover); put('I', unit + 0xb4, kind); put('I', mover, nav); put('I', nav, vt)
    put('I', unit + 0xb8, owner); put('I', owner, 1); put('I', owner + 0x84, big)
    formation = HEAP + 0x7000
    put('I', vt + 0x34, formation); p.hooks[formation] = lambda uc, a: (0, 1)
    put('I', vt + 0x3c, 0x4e60d0)  # the ground navigator's own speed query (always zero)
    chosen = []
    def observe(uc, address, size, _):
        ebp = uc.reg_read(UC_X86_REG_EBP)
        chosen.append(struct.unpack('<i', uc.mem_read(ebp + 8, 4))[0])
    p.uc.hook_add(UC_HOOK_CODE, observe, begin=0x4d9a2d, end=0x4d9a2d)
    p.freeze_hooks()
    rng = random.Random(0x51b890); cases = capped = 0
    for i in range(20000):
        type_max = rng.randrange(1, 8 * 65536)
        unit_max = max(1, type_max + rng.randrange(-type_max // 2, type_max // 2 + 1))
        road, water = rng.choice([32768, 65536, 78643, 131072]), rng.choice([0, 32768, 65536, 98304])
        flags = rng.choice([0, 0x800, 0x1000]) | (rng.randrange(3) << 8)
        owner_kind = rng.choice([1, 2, 3]); active = rng.random() < 0.9
        group = rng.randrange(1, 99); boat_class = rng.random() < 0.25
        ground = rng.choice([0, rng.randrange(1, 8 * 65536), rng.randrange(1, type_max + 1)])
        boat = rng.choice([0, rng.randrange(1, 8 * 65536), rng.randrange(1, type_max + 1)])
        put('i', unit + 0x12b, unit_max); put('i', kind + 0x162, type_max)
        put('i', kind + 0x172, road); put('i', kind + 0x16e, water)
        put('I', kind + 0x260, 0x80000 if boat_class else 0); put('h', kind + 0x194, 5 if boat_class else 0)
        p.uc.mem_write(owner + 0xea, bytes([owner_kind]))
        put('I', mover + 0x20, 0); p.uc.mem_write(mover + 0x36, struct.pack('<H', flags))
        p.uc.mem_write(unit + 0x80, b'\0\0'); p.uc.mem_write(unit + 0x7e, b'\0\0')
        put('I', unit + 0xc8, group)
        record = big + group * 0xc4
        put('i', record + 0x2c, int(active)); put('i', record + 0x58, ground); put('i', record + 0x84, boat)
        chosen.clear()
        _, error = p.call(0x4d95f0, (unit, 0), ecx=mover)
        if error or len(chosen) != 1: raise AssertionError((i, error, chosen))
        want = model(unit_max, type_max, flags, road, water, owner_kind, active, boat_class, boat, ground)
        if chosen[0] != want:
            raise AssertionError((i, dict(unit_max=unit_max, type_max=type_max, flags=hex(flags), road=road,
                                          water=water, kind=owner_kind, active=active, boat=boat_class,
                                          ground=ground, boat_speed=boat), chosen[0], want))
        cases += 1; capped += want != mode_scaled(scaled(unit_max, flags, road, water), flags)
    print(f'PASS: {cases} native group speed caps match the documented rule ({capped} capped)')


if __name__ == '__main__': main()
