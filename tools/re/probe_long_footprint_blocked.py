#!/usr/bin/env python3
"""Where a shot at a long building stops: its footprint cells, not a circle.

The engine's blocked-shot rule (World::noteShotImpact) counts an impact on
terrain short of the target as an obstruction unless it lies at the target.
This probe checks what "at the target" is in retail: the launch/aim routines
(52bdf0, 52be80), the ballistic updater (52bf90), the projectile collision
(52a4d0), the map-cell lookup (50e660) and the unit selection-quad test
(51f340) run from retail, as in probe_ballistic_blocked_target.py; only
QueryWeapon's muzzle, the unit class eligibility virtual and the impact
dispatcher (529c10) are controlled.

The target is a long body, 4 cells across the line of fire and 12 along it
(the Taros Castle is 8x20), centred 128 px from the muzzle, so its near face
is 32 px from its centre and its far corners are 102 px from it. Three runs:
- open ground: the shot hits the unit (51f340 on the cells it owns) at its
  near face, never earlier;
- a ridge 2 cells (32 px) in front of the near face: the shot stops on the
  terrain with no unit (code 2), 48..80 px short of the centre -- inside a
  circle of the body's long half-extent (96 px), outside its footprint;
- a ridge in the cell just before the near face: still stopped on terrain
  with no unit, 8 px short of the face;
- the same ridge under the footprint's own near column: the cell's occupant
  is tested first and the shot hits the unit.
So in retail a shot reaches a building when it reaches the cells the building
owns, whatever its extent the other way.
"""
import struct

from emu import HEAP, Icd
from unicorn.x86_const import UC_X86_REG_FPCW


def put(uc, address, value):
    uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))


MUZZLE = (400, 20, 400)
CENTRE = (528, 20, 400)                 # 128 px down the line of fire
FOOT_X = range(31, 35)                  # 496..560: 4 cells across the line
FOOT_Z = range(19, 31)                  # 304..496: 12 cells along it


def run(ridge_cells):
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
        uc.mem_write(args[1], struct.pack("<3i", *(v * 65536 for v in MUZZLE)))
        return 4, 0

    def unit_eligibility(_uc, _sp):
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

    put(p.uc, 0x62D55C, game)
    put(p.uc, game + 0x19E98, 64)
    put(p.uc, game + 0x19E9C, 64)
    put(p.uc, game + 0x19F04, cells)
    put(p.uc, game + 0x19ECC, 8155)
    put(p.uc, game + 0x19F44, 1)
    put(p.uc, game + 0x19EC0, 1)
    put(p.uc, game + 0x19EDC, features)
    put(p.uc, game + 0x175DC, game + 0x4000)
    put(p.uc, game + 0x175DC + 0xD3D, 0)
    p.uc.mem_write(game + 0x19EF8, b"\x00")
    for z in range(64):
        for x in range(64):
            cell = cells + (z * 64 + x) * 14
            # (max, min) terrain height: flat 10, a 60-high ridge column.
            h = 60 if x in ridge_cells else 10
            p.uc.mem_write(cell + 5, bytes((h, h)))
            p.uc.mem_write(cell + 8, struct.pack("<H", 0xFFFF))

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
    # The selection quad spans the footprint: 64 across, 192 along.
    quad = [(-32, -96), (32, -96), (32, 96), (-32, 96)]
    p.uc.mem_write(vertices, b"".join(struct.pack("<3i", x * 65536, 0, z * 65536)
                                       for x, z in quad))
    put(p.uc, target_kind + 0x14A, 80 * 65536)
    put(p.uc, target + 0x68, CENTRE[0] * 65536)
    put(p.uc, target + 0x6C, CENTRE[1] * 65536)
    put(p.uc, target + 0x70, CENTRE[2] * 65536)
    put(p.uc, target + 0x130, 0x01000000)
    p.uc.mem_write(target + 0xFD, b"\x01")
    for z in FOOT_Z:
        for x in FOOT_X:
            p.uc.mem_write(cells + (z * 64 + x) * 14, struct.pack("<H", 1))

    # Arabow WEAPON1: 530 px/s, the 16-unit substep ceiling (531ccb).
    raw_speed = int(530 * 65536 / 30)
    substeps = max(1, (raw_speed + 0xFFFFF) >> 20)
    put(p.uc, weapon + 0xCC, raw_speed // substeps)
    put(p.uc, weapon + 0xD0, substeps)
    put(p.uc, weapon + 0xC8, 0)
    put(p.uc, weapon + 0x40, weapon_iface)
    put(p.uc, weapon_iface, weapon_vtable)
    put(p.uc, weapon_vtable + 0x14, unit_gate)
    put(p.uc, wrapper, weapon)
    put(p.uc, wrapper + 0x1A, 0)

    put(p.uc, aim_context + 4, 0)  # low arc
    p.uc.mem_write(aim_context + 8, struct.pack("<f", 1.0))
    p.uc.mem_write(source + 0x7E, struct.pack("<H", 0))
    p.uc.mem_write(delta, struct.pack("<3i", (CENTRE[0] - MUZZLE[0]) * 65536, 0, 0))
    _, error = p.call(0x52BDF0, (source, wrapper, delta), ecx=aim_context)
    assert error is None, error
    put(p.uc, source + 0xB4, source_aux)
    p.uc.mem_write(source_aux + 0x8A, b"\x00\x00")
    _, error = p.call(0x52BE80, (shot, source, wrapper))
    assert error is None, error
    put(p.uc, shot, weapon)
    p.uc.mem_write(shot + 0x92, b"\x00")
    put(p.uc, shot + 0x84, 0)
    p.uc.mem_write(update + 8, struct.pack("<f", 1.0))

    for _ in range(30):
        _, error = p.call(0x52BF90, (shot,), ecx=update)
        assert error is None, error
        if impacts:
            break
    assert len(impacts) == 1, impacts
    args, impact = impacts[0]
    return args[1] == target, args[1] == 0, tuple(v / 65536.0 for v in impact)


def inside_footprint(x, z):
    return FOOT_X[0] * 16 <= x < (FOOT_X[-1] + 1) * 16 and FOOT_Z[0] * 16 <= z < (FOOT_Z[-1] + 1) * 16


hit, _, open_point = run(())
assert hit and inside_footprint(open_point[0], open_point[2]), open_point
assert open_point[0] - CENTRE[0] >= -32, open_point          # never short of the near face

hit, none, ridge_point = run((28,))                           # x 448..464
assert not hit and none, ridge_point
short = CENTRE[0] - ridge_point[0]
assert 32 < short <= 96 and not inside_footprint(ridge_point[0], ridge_point[2]), ridge_point

hit, none, face_point = run((30,))                            # x 480..496
assert not hit and none and 0 < 496 - face_point[0] <= 16, face_point

hit, _, under_point = run((31,))                              # the near footprint column
assert hit and under_point == open_point, under_point

print(f"PASS: open ground hits the body at {open_point} (near face x=496); a ridge in front "
      f"stops it on terrain with no unit at {ridge_point}, {short:.1f} px short of the centre "
      f"(inside the 96 px long half-extent, outside the footprint); one cell before the face "
      f"at {face_point}; a ridge under the near column still lets it hit the body at {under_point}")
