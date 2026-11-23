#!/usr/bin/env python3
"""Headlessly exercise retail main-menu door methods from KINGDOMS.icd.

Runs the native hit-test, hover-enter, update, and click methods in Unicorn. The
button objects, focus manager, Bink frame records, and BinkGoto/audio/callback
services are synthetic; this verifies native method behavior without launching
the retail GUI or decoding/rendering video.

    python3 tools/re/probe_menu_door_native_state.py
"""
import struct
from emu import HEAP, Icd


DOORS = (
    ("PlayComputer", (71, 219, 101, 158), (40, 192, 149, 188)),
    ("PlayStory", (289, 217, 62, 168), (242, 202, 148, 192)),
    ("PlayPlayer", (487, 216, 63, 157), (419, 136, 161, 243)),
    ("Credits", (124, 42, 71, 130), (67, 20, 143, 156)),
)

HIT_TEST = 0x4AD170
CLICK = 0x4AD200
UPDATE = 0x4AD270
HOVER_ENTER = 0x4AD390
FOCUS_GLOBAL = 0x65DDB4
BINK_GOTO_IAT = 0x5EB474


def put32(uc, address, value):
    uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))


def get32(uc, address):
    return struct.unpack("<I", uc.mem_read(address, 4))[0]


def call(icd, address, args=(), this=None):
    result, error = icd.call(address, args=args, ecx=this)
    assert error is None, (hex(address), error)
    return result


def main():
    icd = Icd()
    uc = icd.uc
    focus = HEAP + 0x2000
    sound = HEAP + 0x2100
    binks = [HEAP + 0x3000 + index * 0x100 for index in range(8)]
    gotos = []
    goto_hook = HEAP + 0x100

    def bink_goto(uc, args):
        values = [get32(uc, args + index * 4) for index in range(3)]
        gotos.append(values)
        return 3, 0

    icd.hooks[goto_hook] = bink_goto
    put32(uc, BINK_GOTO_IAT, goto_hook)
    icd.freeze_hooks()

    for index, (name, hotspot, widget) in enumerate(DOORS):
        gotos.clear()
        obj = HEAP + 0x10000 + index * 0x300
        x, y, width, height = hotspot
        wx, wy, ww, wh = widget

        # Exercise the actual native hit-test against the door-specific custom
        # rectangle and verify it ignores the larger GUI widget-only area.
        for offset, value in ((4, wx), (8, wy), (0x0C, ww), (0x10, wh),
                              (0x13E, x), (0x142, y), (0x146, width),
                              (0x14A, height)):
            put32(uc, obj + offset, value)
        points = ((x, y), (x + width, y + height),
                  (x + width + 1, y), (wx + 1, wy + 1))
        hits = [call(icd, HIT_TEST, point, obj) for point in points]
        assert hits == [1, 1, 0, 0], (name, hits)

        put32(uc, obj + 0x40, 8)          # native state values are 0..7
        put32(uc, obj + 0x3C, 2)          # idle state displays clip 4
        put32(uc, obj + 0x13A, 2)         # last selected state
        uc.mem_write(obj + 0x139, b"\0")
        put32(uc, obj + 0x68, sound)
        put32(uc, sound + 4, 0xA100)
        put32(uc, sound + 8, 0xA200)
        put32(uc, sound + 0x14, 0xA300)
        put32(uc, obj + 0x28, 0)          # omit menu callback side effect
        for state in (4, 5, 6, 7):
            put32(uc, obj + 0x79 + state * 24, binks[state])
            put32(uc, binks[state] + 0x10, 10)  # frame count
            put32(uc, binks[state] + 0x14, 0)   # current frame

        put32(uc, FOCUS_GLOBAL, focus)
        put32(uc, focus + 0x18, obj)
        call(icd, HOVER_ENTER, this=obj)
        assert get32(uc, obj + 0x3C) == 5

        # Pointer exits while clip 5 is still playing. Retail keeps state 5 until
        # its last frame, enters state 6, then the next update selects state 7.
        put32(uc, focus + 0x18, 0)
        call(icd, UPDATE, this=obj)
        assert get32(uc, obj + 0x3C) == 5
        put32(uc, binks[5] + 0x14, 10)
        call(icd, UPDATE, this=obj)
        assert get32(uc, obj + 0x3C) == 6
        call(icd, UPDATE, this=obj)
        assert get32(uc, obj + 0x3C) == 7
        assert uc.mem_read(obj + 0x139, 1)[0] == 0

        # Re-entry during clip 7 only latches hover; it does not interrupt the
        # exit clip. Once it ends, another native hover-enter event can start 5.
        put32(uc, focus + 0x18, obj)
        call(icd, HOVER_ENTER, this=obj)
        assert get32(uc, obj + 0x3C) == 7
        assert uc.mem_read(obj + 0x139, 1)[0] == 1
        put32(uc, binks[7] + 0x14, 10)
        call(icd, UPDATE, this=obj)
        assert get32(uc, obj + 0x3C) == 2
        call(icd, HOVER_ENTER, this=obj)
        assert get32(uc, obj + 0x3C) == 5

        # The native click handler switches through state 3 to state 4 and calls
        # the menu callback. This synthetic object leaves that callback unset.
        put32(uc, obj + 0x3C, 6)
        put32(uc, obj + 0x13A, 6)
        uc.mem_write(obj + 0x139, b"\1")
        call(icd, CLICK, (0,), obj)
        assert get32(uc, obj + 0x3C) == 4

        print(f"{name:12} hotspot={hits}; leave-in=5->6->7->2; re-enter-in-7 holds 7; click ends 4; seeks={len(gotos)}")

    print("PASS: native door methods executed headlessly; synthetic Bink/audio/menu services; no retail GUI")


if __name__ == "__main__":
    main()
