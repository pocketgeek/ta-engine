#!/usr/bin/env python3
"""Run an aimed Arabow shot through native blocker and unit collision.

The launch/aim routines (52bdf0, 52be80), ballistic updater (52bf90), map-cell
lookup (50e660), and unit selection-quad collision (51f340) run from retail.
Only QueryWeapon's muzzle, the class-specific unit eligibility virtual, and
the final impact dispatcher are controlled. One run has a feature in the
shot's path; the other removes it while keeping the target and trajectory fixed.
"""
import pathlib
import re
import struct

from emu import HEAP, Icd
from unicorn.x86_const import UC_X86_REG_FPCW


ROOT = pathlib.Path(__file__).resolve().parents[2]


def put(uc, address, value):
    uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))


def run(blocked):
    p = Icd()
    (game, source, wrapper, weapon, aim_context, delta, shot, update,
     source_aux, cells, features, units, target_kind, target_model, primitive,
     vertices, indices, weapon_iface, weapon_vtable, unit_gate) = [
        HEAP + i * 0x20000 for i in range(1, 21)
    ]
    target = units + 0x138  # native Unit table entries are 0x138 bytes
    impacts = []

    def query_weapon(uc, sp):
        args = struct.unpack("<4I", uc.mem_read(sp, 16))
        assert args[0] == source and args[1] == shot + 4
        assert args[2] & 3 == 0 and args[3] == 0xFFFFFFFF, args
        uc.mem_write(args[1], struct.pack("<3i", 400 * 65536, 20 * 65536,
                                          400 * 65536))
        return 4, 0

    def unit_eligibility(_uc, _sp):
        # The normal class virtual admits the authored model-quad test below.
        return 0, 0

    def impact_dispatch(uc, sp):
        args = struct.unpack("<5I", uc.mem_read(sp, 20))
        position = struct.unpack("<3i", uc.mem_read(shot + 4, 12))
        impacts.append((args, position))
        return 5, 0

    p.hooks.update({0x4DD420: query_weapon, unit_gate: unit_eligibility,
                    0x529C10: impact_dispatch})
    p.freeze_hooks()
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027F)

    # Retail's ordinary 64x64, 16-unit map-cell table. The native map lookup
    # and collision routine both consume these cells directly.
    put(p.uc, 0x62D55C, game)
    put(p.uc, game + 0x19E98, 64)
    put(p.uc, game + 0x19E9C, 64)
    put(p.uc, game + 0x19F04, cells)
    put(p.uc, game + 0x19ECC, 8155)  # retail default OTA gravity 112
    put(p.uc, game + 0x19F44, 1)
    put(p.uc, game + 0x19EC0, 1)  # one feature type
    put(p.uc, game + 0x19EDC, features)
    put(p.uc, game + 0x175DC, game + 0x4000)
    put(p.uc, game + 0x175DC + 0xD3D, 0)
    p.uc.mem_write(game + 0x19EF8, b"\x00")
    for z in range(64):
        for x in range(64):
            cell = cells + (z * 64 + x) * 14
            p.uc.mem_write(cell + 5, bytes((10, 10)))
            p.uc.mem_write(cell + 8, struct.pack("<H", 0xFFFF))

    # A 255-unit tree top in cell (28,25), at world (448,400). The shot is
    # aimed through it toward an enemy centered at (520,400).
    if blocked:
        feature_cell = cells + (25 * 64 + 28) * 14
        p.uc.mem_write(feature_cell + 8, struct.pack("<H", 0))
        p.uc.mem_write(features + 0x138, b"\xFF")

    # Native unit table with one active enemy occupying the shot's target cell.
    put(p.uc, game + 0x14E84, units)
    put(p.uc, game + 0x14E88, target)
    p.uc.mem_write(target, bytes(0x138))
    put(p.uc, target + 0xB4, target_kind)
    put(p.uc, target_kind + 0x2A0, target_model)
    put(p.uc, target_model + 0x0C, 0)
    put(p.uc, target_model + 0x24, vertices)
    put(p.uc, target_model + 0x28, primitive)
    put(p.uc, primitive + 0x0C, indices)
    p.uc.mem_write(indices, struct.pack("<4H", 0, 1, 2, 3))
    # A 40x40-unit authored selection quad gives the unobstructed control a
    # direct hit before its low-arc shot drops below the unit's ground plane.
    quad = [(-20, -20), (20, -20), (20, 20), (-20, 20)]
    p.uc.mem_write(vertices, b"".join(struct.pack("<3i", x * 65536, 0, z * 65536)
                                       for x, z in quad))
    put(p.uc, target_kind + 0x14A, 80 * 65536)
    put(p.uc, target + 0x68, 520 * 65536)
    put(p.uc, target + 0x6C, 20 * 65536)
    put(p.uc, target + 0x70, 400 * 65536)
    put(p.uc, target + 0x130, 0x01000000)  # active/collidable native unit
    p.uc.mem_write(target + 0xFD, b"\x01")  # enemy owner
    for z in range(24, 27):
        for x in range(31, 34):
            p.uc.mem_write(cells + (z * 64 + x) * 14, struct.pack("<H", 1))

    # Arabow WEAPON1 native speed parse: 530 px/s * 65536/30, then the
    # 16-unit substep ceiling from 531ccb.
    raw_speed = int(530 * 65536 / 30)
    substeps = max(1, (raw_speed + 0xFFFFF) >> 20)
    per_substep = raw_speed // substeps
    put(p.uc, weapon + 0xCC, per_substep)
    put(p.uc, weapon + 0xD0, substeps)
    put(p.uc, weapon + 0xC8, 0)
    put(p.uc, weapon + 0x40, weapon_iface)
    put(p.uc, weapon_iface, weapon_vtable)
    put(p.uc, weapon_vtable + 0x14, unit_gate)
    put(p.uc, wrapper, weapon)
    put(p.uc, wrapper + 0x1A, 0)  # QueryWeapon piece

    # Retail aims at the target center with its low arc, then creates the real
    # ballistic shot from the queried muzzle.
    put(p.uc, aim_context + 4, 0)  # lobpreferred
    p.uc.mem_write(aim_context + 8, struct.pack("<f", 1.0))
    p.uc.mem_write(source + 0x7E, struct.pack("<H", 0))
    p.uc.mem_write(delta, struct.pack("<3i", 120 * 65536, 0, 0))
    _, error = p.call(0x52BDF0, (source, wrapper, delta), ecx=aim_context)
    assert error is None, error
    yaw_offset, pitch = struct.unpack("<2H", p.uc.mem_read(wrapper + 0x16, 4))
    assert (yaw_offset, pitch) == (0x4000, 249), (yaw_offset, pitch)

    put(p.uc, source + 0xB4, source_aux)
    p.uc.mem_write(source_aux + 0x8A, b"\x00\x00")
    _, error = p.call(0x52BE80, (shot, source, wrapper))
    assert error is None, error
    put(p.uc, shot, weapon)
    p.uc.mem_write(shot + 0x92, b"\x00")  # shooter owner
    put(p.uc, shot + 0x84, 0)  # no projectile-intercept target
    p.uc.mem_write(update + 8, struct.pack("<f", 1.0))
    assert struct.unpack("<3i", p.uc.mem_read(shot + 4, 12)) == (
        400 * 65536, 20 * 65536, 400 * 65536)

    for _ in range(20):
        _, error = p.call(0x52BF90, (shot,), ecx=update)
        assert error is None, error
        if impacts:
            break
    assert len(impacts) == 1, (blocked, impacts)
    args, impact = impacts[0]
    assert args[0] == shot and args[2:] == (1, 1, 0), args
    if blocked:
        # 52a4d0 reports feature collision as code 2 with a null unit. The
        # native dispatcher therefore enters environmental/feature damage.
        assert args[1] == 0, hex(args[1])
        assert 448 <= (impact[0] >> 16) < 464, impact
        assert impact[1] <= 20 * 65536 + 255 * 65536, impact
    else:
        # Removing only the blocker lets the same native trajectory collide
        # with the enemy's 51f340 selection quad.
        assert args[1] == target, (hex(args[1]), hex(target), impact)
        assert (impact[0] >> 16) <= 520 and (impact[0] >> 16) >= 500, impact
    return args[1], tuple(value / 65536.0 for value in impact)


root = ROOT / "assets/extracted/all/units/arabow.fbi"
text = root.read_text(errors="replace")
block = re.search(r"\[WEAPON1\]\s*\{(.*?)\n\}", text, re.S)
assert block, "Arabow WEAPON1 is missing"
for pattern in (r"\btype\s*=\s*Ballistic\s*;",
                r"\bmodel\s*=\s*araarrow\s*;",
                r"\bweaponvelocity\s*=\s*530\s*;",
                r"\bdefault\s*=\s*476\s*;"):
    haystack = block.group(1)
    if "default" in pattern:
        damage = re.search(r"\[DAMAGE\]\s*\{(.*?)\n\s*\}", haystack, re.S)
        assert damage and re.search(pattern, damage.group(1), re.I), pattern
    else:
        assert re.search(pattern, haystack, re.I), pattern

blocked_unit, blocked_point = run(True)
clear_unit, clear_point = run(False)
assert blocked_unit == 0 and clear_unit != 0
print("PASS: native Arabow trajectory hits the intervening feature (null unit) and "
      f"dispatches at {blocked_point}; without that blocker it hits unit {clear_unit} "
      f"at {clear_point}")
