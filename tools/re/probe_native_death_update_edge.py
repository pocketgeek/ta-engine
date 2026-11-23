#!/usr/bin/env python3
"""Drive the retail active-unit removal edge and attached-SFX teardown.

Runs headlessly from KINGDOMS.icd. The fixture creates one active unit whose
native removal-request bit is already set, then executes the real 0x5130d0
unit-table update, 0x512ae0 removal path, 0x4ee560 owner destructor, and
0x497380 attached-SFX-list destructor. Only unrelated world/render callbacks
and allocator boundaries are stubbed. It measures teardown relative to the
native removal bit, not the SET_UNIT_VALUE host callback that sets that bit.
"""
import struct

from emu import HEAP, Icd
from unicorn import UC_HOOK_CODE


def put32(uc, address, value):
    uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))


def get32(uc, address):
    return struct.unpack("<I", uc.mem_read(address, 4))[0]


def main():
    p = Icd()
    game = HEAP + 0x10000
    unit = HEAP + 0x20000
    unit_type = HEAP + 0x30000
    owner = HEAP + 0x40000
    sfx_list = HEAP + 0x50000
    sentinel = HEAP + 0x51000
    node = HEAP + 0x52000
    game_manager = HEAP + 0x63000
    game_vtable = HEAP + 0x63100
    removal_trace = []

    put32(p.uc, 0x62D55C, game)
    put32(p.uc, game + 0x14E84, unit)
    put32(p.uc, game + 0x14E88, unit)
    put32(p.uc, game + 0x175C4, HEAP + 0x60000)
    put32(p.uc, game + 0x175DC, HEAP + 0x61000)
    put32(p.uc, game + 0x19F54, HEAP + 0x62000)
    put32(p.uc, 0x62DA3C, game_manager)
    put32(p.uc, game_manager, game_vtable)
    # A no-op engine manager callback. It is outside the ICD update/destructor
    # path under test and uses one stack argument.
    put32(p.uc, game_vtable + 0x18, 0x5E9000)
    p.hooks[0x5E9000] = lambda uc, sp: (1, 0)

    put32(p.uc, unit + 0xB8, unit_type)
    put32(p.uc, unit + 0xB4, HEAP + 0x64000)
    put32(p.uc, unit + 0xC0, owner)
    put32(p.uc, unit + 0x130, 0x11000000)  # active + removal requested
    p.uc.mem_write(unit + 0x116, b"\0")  # skip unrelated death-state callbacks
    type_bytes = bytearray(0x300)
    struct.pack_into("<I", type_bytes, 0, 1)
    struct.pack_into("<H", type_bytes, 0xE8, 1)
    p.uc.mem_write(unit_type, bytes(type_bytes))

    # The real owner-model destructor calls this SFX-list manager's vtable.
    put32(p.uc, owner + 0x17C, sfx_list)
    put32(p.uc, sfx_list, 0x5F03A4)  # retail vtable entry -> 0x497380
    put32(p.uc, sfx_list + 4, 0)
    put32(p.uc, sfx_list + 8, sentinel)
    put32(p.uc, sfx_list + 12, 1)
    put32(p.uc, sfx_list + 16, 40)
    put32(p.uc, sentinel, node)
    put32(p.uc, sentinel + 4, node)
    put32(p.uc, node, sentinel)
    put32(p.uc, node + 4, sentinel)
    put32(p.uc, 0x640210, 0)

    # These are unrelated external side effects or caller-owned allocation
    # boundaries. Correct stack-cleanup counts follow the retail call sites.
    for address, argc in (
        (0x52AE30, 2), (0x4C6750, 2), (0x4C6800, 2), (0x523510, 1),
        (0x50AA20, 1), (0x5066A0, 1), (0x4C7190, 0), (0x4DC7F0, 0),
        (0x4EBA00, 0), (0x5BA5D0, 0), (0x4EBAA0, 0),
    ):
        p.hooks[address] = (lambda count: lambda uc, sp: (count, 0))(argc)
    p.freeze_hooks()

    watched = {0x5130D0, 0x512AE0, 0x4EE560, 0x497380}

    def trace(uc, address, size, _):
        if address in watched:
            removal_trace.append(address)

    p.uc.hook_add(UC_HOOK_CODE, trace)
    result, error = p.call(0x5130D0)
    assert not error, error
    assert removal_trace == [0x5130D0, 0x512AE0, 0x4EE560, 0x497380], \
        [hex(address) for address in removal_trace]
    assert get32(p.uc, unit + 0xC0) == 0
    assert get32(p.uc, unit + 0x130) & 0x11000000 == 0
    assert get32(p.uc, sfx_list + 8) == 0
    assert get32(p.uc, sfx_list + 12) == 0
    assert get32(p.uc, 0x640210) == sentinel

    print("PASS: native 0x5130d0 -> 0x512ae0 -> 0x4ee560 -> 0x497380 teardown order")
    print("PASS: owner pointer cleared, active/removal flags cleared, and attached SFX node drained")
    print("LIMIT: SET_UNIT_VALUE 26/31 host mapping and the 31 one-second timer are outside this ICD fixture; "
          "teardown is exact relative to the removal bit, not to the script callback")


if __name__ == "__main__":
    main()
