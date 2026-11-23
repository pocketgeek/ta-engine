#!/usr/bin/env python3
"""Probe Airstrike selection through the active weapon slot in the retail ICD.

The selector's mode-3 Airstrike gate reads WeaponType+0xc8 from the weapon
chosen by native 0x519ac0. This fixture executes both routines in Unicorn and
checks that a dropped, non-active weapon does not change the cursor.
"""
from emu import HEAP, Icd


WEAPON_SELECTOR = 0x519AC0
CURSOR_SELECTOR = 0x4DD780
AIRSTRIKE_MODE = 3
ATTACK_SLOT = 1
AIRSTRIKE_SLOT = 2
NORMAL_SLOT = 19
LIVE = 0x01000000
WEAPON_SWITCHING = 0x00010000
DROPPED_WEAPON_FLAG = 0x20
UNIT_AIRSTRIKE_FLAG = 0x20


def put_u32(uc, address, value):
    uc.mem_write(address, (value & 0xFFFFFFFF).to_bytes(4, "little"))


def get_u32(uc, address):
    return int.from_bytes(uc.mem_read(address, 4), "little")


def main():
    icd = Icd()
    uc = icd.uc
    unit = HEAP + 0x10000
    unit_type = HEAP + 0x11000
    weapons = [HEAP + 0x12000 + i * 0x100 for i in range(3)]

    put_u32(uc, unit + 0x130, LIVE)
    put_u32(uc, unit + 0xB4, unit_type)
    put_u32(uc, unit_type + 0x260, WEAPON_SWITCHING)
    put_u32(uc, unit_type + 0x264, UNIT_AIRSTRIKE_FLAG)
    # Native UnitDef slots start at unit+0x0c and advance by 0x1c bytes.
    for slot, weapon in enumerate(weapons):
        put_u32(uc, unit + 0x0C + slot * 0x1C, weapon)
        flags = 0x00040000
        if slot == 1:
            flags |= DROPPED_WEAPON_FLAG
        put_u32(uc, weapon + 0xC8, flags)

    # This native service supplies the selector's unrelated command eligibility
    # value. The weapon selector and the cursor gate remain native instructions.
    icd.hooks[0x520A80] = lambda _uc, _sp: (0, 1)
    icd.freeze_hooks()

    for active_slot, expected_cursor in (
        (0, ATTACK_SLOT),
        (1, AIRSTRIKE_SLOT),
        (2, ATTACK_SLOT),
    ):
        status = LIVE | (active_slot << 30)
        put_u32(uc, unit + 0x130, status)
        selected_slot, error = icd.call(WEAPON_SELECTOR, ecx=unit)
        assert error is None, error
        # Native caller consumes AL; upper EAX holds the weapon pointer.
        selected_slot &= 0xFF
        selected_weapon = get_u32(uc, unit + 0x0C + selected_slot * 0x1C)
        dropped = bool(get_u32(uc, selected_weapon + 0xC8) & DROPPED_WEAPON_FLAG)
        cursor, error = icd.call(
            CURSOR_SELECTOR, args=(AIRSTRIKE_MODE, unit, 0, 0)
        )
        assert error is None, error
        assert selected_slot == active_slot, (active_slot, selected_slot)
        assert cursor == expected_cursor, (active_slot, dropped, cursor, expected_cursor)
        print(
            f"active slot {active_slot}, dropped={dropped}: native cursor slot {cursor}"
        )

    # Removing the UnitDef gate suppresses Airstrike even when the selected
    # weapon itself has the authored `dropped` flag.
    put_u32(uc, unit + 0x130, LIVE | (1 << 30))
    put_u32(uc, unit_type + 0x264, 0)
    cursor, error = icd.call(CURSOR_SELECTOR, args=(AIRSTRIKE_MODE, unit, 0, 0))
    assert error is None, error
    assert cursor == NORMAL_SLOT, cursor
    print("cleared UnitDef+0x264 bit 0x20 with active dropped weapon: normal slot 19")

    # Without the native weapon-switching capability, 0x519ac0 uses slot zero;
    # stale selection bits in the unit state do not select the dropped slot.
    put_u32(uc, unit_type + 0x260, 0)
    put_u32(uc, unit_type + 0x264, UNIT_AIRSTRIKE_FLAG)
    put_u32(uc, unit + 0x130, LIVE | (1 << 30))
    selected_slot, error = icd.call(WEAPON_SELECTOR, ecx=unit)
    assert error is None, error
    selected_slot &= 0xFF
    assert selected_slot == 0, selected_slot
    cursor, error = icd.call(CURSOR_SELECTOR, args=(AIRSTRIKE_MODE, unit, 0, 0))
    assert error is None, error
    assert cursor == ATTACK_SLOT, cursor
    print("weapon switching disabled: native cursor follows slot zero (Attack)")


if __name__ == "__main__":
    main()
