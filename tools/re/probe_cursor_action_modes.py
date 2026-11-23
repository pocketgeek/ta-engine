#!/usr/bin/env python3
"""Probe retail cursor selector action modes 2 (Revive), 5 (Unload), and 6 (Load).

This executes the installed KINGDOMS.icd 0x4dd780 selector in Unicorn. The
outer eligibility gate and the two native cell-target predicates are hooked to
true so the test isolates the selector's capability/slot branch; all selector
instructions and their native return values execute from the ICD. The point
cell and visibility plane are real synthetic structures in the ICD layout.
"""
import struct

from emu import HEAP, Icd


SELECTOR = 0x4DD780
NORMAL = 19
REVIVE = 10
UNLOAD = 13
LOAD = 12


def put_u32(uc, address, value):
    uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))


def put_u16(uc, address, value):
    uc.mem_write(address, struct.pack("<H", value & 0xFFFF))


def main():
    icd = Icd()
    uc = icd.uc
    game, unit, unit_type, map_obj, visible, point, resolved_cell = [
        HEAP + i * 0x10000 for i in range(1, 8)
    ]
    target, target_type = HEAP + 0x80000, HEAP + 0x90000

    # The selector's mode-2 point route first checks that the position is on a
    # visible cell, then asks native 0x497100/0x496fd0 whether the cell contains
    # the relevant revive/animate target. Return a valid cell and allow each
    # target predicate so this fixture exercises the mode's type-bit gates.
    put_u32(uc, 0x62D55C, game)
    put_u32(uc, game + 0x19EF4, visible)
    uc.mem_write(game + 0x306F, b"\x00")  # visibility bit index
    for i in range(16 * 16):
        put_u16(uc, visible + i * 2, 1)
    put_u32(uc, map_obj + 0x8C, 16)
    put_u32(uc, map_obj + 0x90, 16)
    put_u32(uc, unit + 0x130, 0x01000000)  # live and selector-eligible
    put_u32(uc, unit + 0xB4, unit_type)
    put_u32(uc, unit + 0xB8, map_obj)
    put_u32(uc, target + 0x130, 0x01000000)
    put_u32(uc, target + 0xB4, target_type)
    put_u32(uc, target + 0xB8, map_obj)
    # Selector point coordinates are signed 16-bit world coordinates; the
    # selector shifts by five to get its cell index.
    put_u16(uc, point + 2, 320)
    put_u16(uc, point + 6, 0)
    put_u16(uc, point + 10, 320)

    # These are the native cell/target validity services called by the selector.
    # They are the only branch predicates replaced in this focused fixture.
    icd.hooks[0x4DD6A0] = lambda _uc, _args: (1, 1)
    icd.hooks[0x50E660] = lambda _uc, _args: (1, resolved_cell)
    icd.hooks[0x497100] = lambda _uc, _args: (1, 1)
    icd.hooks[0x496FD0] = lambda _uc, _args: (1, 1)
    boarding_allowed = [1]
    icd.hooks[0x519F50] = lambda _uc, _args: (1, boarding_allowed[0])
    # Keep 520b60 real: it counts selected transports in the local roster.
    put_u32(uc, game + 0x2478, unit)
    put_u32(uc, game + 0x247C, unit)
    put_u32(uc, unit + 0x130, 0x01000010)
    icd.freeze_hooks()

    cases = (
        # UnitDef +0x264: canmove=0x100, canresurrect=0x1000,
        # cananimate=0x20000000, cantransport=0x200.
        (2, 0x00000100, 14, "mode 2 needs a mobile capable caster"),
        (2, 0x00001100, REVIVE, "mode 2 canresurrect -> Revive"),
        (2, 0x20000100, REVIVE, "mode 2 cananimate -> Revive"),
        (2, 0x00001000, NORMAL, "mode 2 caster flag without canmove -> Normal"),
        (2, 0x00000200, NORMAL, "mode 2 transport flag without canmove -> Normal"),
        (5, 0x00000200, UNLOAD, "mode 5 cantransport -> Unload"),
        (5, 0x00000100, NORMAL, "mode 5 without cantransport -> Normal"),
        (6, 0x00000200, LOAD, "mode 6 valid passenger target -> Load"),
        (5, 0x20000100, NORMAL, "mode 5 caster flags do not imply Teleport"),
    )
    for mode, flags, expected, label in cases:
        put_u32(uc, unit_type + 0x264, flags)
        target_unit = target if mode == 6 else 0
        target_point = 0 if mode == 6 else point
        got, error = icd.call(SELECTOR, args=(mode, unit, target_unit, target_point))
        assert error is None, (label, error)
        assert got == expected, (label, hex(flags), got, expected)
        print(f"mode {mode}, UnitDef+264={flags:#010x}: cursor slot {got} ({label})")

    put_u32(uc, unit_type + 0x264, 0x200)
    for carriers in (0, 1, 2):
        put_u32(uc, unit + 0x130, 0x01000010 if carriers else 0x01000000)
        second = unit + 0x138
        put_u32(uc, second + 0x130, 0x01000010)
        put_u32(uc, second + 0xB4, unit_type)
        put_u32(uc, game + 0x247C, second if carriers == 2 else unit)
        for has_target in (False, True):
            for allowed in (0, 1):
                boarding_allowed[0] = allowed
                got, error = icd.call(SELECTOR, args=(
                    6, unit, target if has_target else 0, 0 if has_target else point))
                expected = LOAD if carriers == 1 and has_target and allowed else NORMAL
                assert error is None, error
                assert got == expected, (carriers, has_target, allowed, got, expected)
                print(f"Load: carriers={carriers}, target={has_target}, "
                      f"boarding={allowed} -> cursor {got}")

    print("Native UI command tags map LOAD to action mode 6 and UNLOAD to mode 5.")
    print("Native cursor IDs: mode 6 -> slot 12 (Cursorload); mode 5 -> slot 13 (CursorUnload).")
    print("Parser flag sites: canmove=+0x264/0x100, cantransport=+0x200,")
    print("canresurrect=+0x1000, cananimate=+0x20000000 (KINGDOMS.icd 0x4c06xx-0x4c07xx).")
    print("Target helper predicates were enabled synthetically; this does not identify")
    print("the exact live corpse/cell state those predicates accept.")


if __name__ == "__main__":
    main()
