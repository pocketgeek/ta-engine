#!/usr/bin/env python3
"""Trace retail's Capture/Pickup/Teleport cursor route and frame phase.

This is an offline Unicorn probe. It executes the native cursor-table setter
(0x525ca0 -> 0x575e30 -> 0x589ff0), the live frame updater (0x58a080), and
selector 0x4dd780. The cursor manager's sequence records are synthetic, but
the three GAF frame counts/delays are read from extracted retail assets.

It does not launch retail, render a pointer, or claim that synthetic records
recreate the game's complete cursor-manager initialization.
"""
import struct
import sys
from pathlib import Path

from emu import HEAP, ICD, Icd


SET_BY_CURSOR_TABLE = 0x525CA0
SET_DEFAULT_CURSOR = 0x575E30
UPDATE_LIVE_POINTER = 0x58A080
ACTION_SELECTOR = 0x4DD780
CURSOR_MANAGER_VTABLE = 0x5F571C
CAPTURE_PICKUP_TELEPORT = {
    4: "cursorcapture",
    8: "cursorpickup",
    9: "cursorteleport",
}
NORMAL_SLOT = 19


def u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def read_cursor_sequences(gaf_path):
    """Read just GAF entry/frame timing metadata, without decoding pixels."""
    data = gaf_path.read_bytes()
    assert u32(data, 0) == 0x00010100, f"unexpected GAF version: {gaf_path}"
    count = u32(data, 4)
    sequences = {}
    for index in range(count):
        entry = u32(data, 12 + index * 4)
        frame_count = u16(data, entry)
        name = data[entry + 8:entry + 40].split(b"\0", 1)[0].decode("ascii").lower()
        delays = []
        for frame in range(frame_count):
            delays.append(u32(data, entry + 44 + frame * 8))
        sequences[name] = (frame_count, delays)
    return sequences


def put_u32(uc, address, value):
    uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))


def put_u16(uc, address, value):
    uc.mem_write(address, struct.pack("<H", value & 0xFFFF))


def get_u32(uc, address):
    return u32(uc.mem_read(address, 4), 0)


def call(icd, address, *, args=(), this=None):
    result, error = icd.call(address, args=args, ecx=this)
    assert error is None, (hex(address), error)
    return result


def direct_call_sites(target, start=None, end=None):
    """Find rel32 CALL xrefs in the retail ICD's raw .text bytes."""
    data = Path(ICD).read_bytes()
    text_file_offset = 0x1000
    text_vma = 0x401000
    text_size = 0x1E9912
    sites = []
    low = text_file_offset if start is None else text_file_offset + start - text_vma
    high = text_file_offset + text_size - 5 if end is None else text_file_offset + end - text_vma
    for offset in range(low, high):
        if data[offset] != 0xE8:
            continue
        address = text_vma + offset - text_file_offset
        destination = address + 5 + struct.unpack_from("<i", data, offset + 1)[0]
        if destination == target:
            sites.append(address)
    return sites


def verify_native_registration_order():
    """Decode retail's actual ID/name stores from the cursor init routine."""
    data = Path(ICD).read_bytes()
    start, end = 0x4BEEC0, 0x4BF103
    sites = direct_call_sites(0x575D00, start, end)
    assert len(sites) == 21, [hex(site) for site in sites]

    expected = {
        1: "cursorattack",
        2: "cursorairstrike",
        3: "cursortoofar",
        4: "cursorcapture",
        5: "cursordefend",
        6: "cursorrepair",
        7: "cursorpatrol",
        8: "cursorpickup",
        9: "cursorteleport",
        10: "cursorrevive",
        11: "cursorreclamate",
        12: "cursorload",
        13: "cursorunload",
        14: "cursormove",
        15: "cursorselect",
        16: "cursorfindsite",
        17: "cursorred",
        18: "cursorgrn",
        19: "cursornormal",
        20: "cursorhourglass",
        21: "pathicon",
    }

    def text_bytes(address, count):
        offset = 0x1000 + address - 0x401000
        return data[offset:offset + count]

    def rdata_string(address):
        if 0x5EB000 <= address < 0x5EB000 + 0x18862:
            offset = 0x1EB000 + address - 0x5EB000
        else:
            assert 0x604000 <= address < 0x62B000, hex(address)
            offset = 0x204000 + address - 0x604000
        return data[offset:data.index(0, offset)].decode("ascii").lower()

    registered = {}
    for ordinal, call_site in enumerate(sites):
        push_distance = 10 if ordinal == 0 else 16
        name_push = text_bytes(call_site - push_distance, 5)
        gaf_push = text_bytes(call_site - push_distance + 5, 5)
        assert name_push[0] == gaf_push[0] == 0x68, hex(call_site)
        assert struct.unpack_from("<I", gaf_push, 1)[0] == 0x613068
        name = rdata_string(struct.unpack_from("<I", name_push, 1)[0])

        after = call_site + 5
        before = sites[ordinal + 1] if ordinal + 1 < len(sites) else end
        slot_stores = []
        for address in range(after, before - 5):
            opcode = text_bytes(address, 2)
            if opcode not in (b"\x89\x81", b"\x89\x82"):
                continue
            displacement = struct.unpack("<I", text_bytes(address + 2, 4))[0]
            if 0x17544 <= displacement <= 0x17594 and (displacement - 0x17540) % 4 == 0:
                slot_stores.append(displacement)
        assert len(slot_stores) == 1, (name, [hex(value) for value in slot_stores])
        slot = (slot_stores[0] - 0x17540) // 4
        assert expected.get(slot) == name, (slot, name, expected.get(slot))
        registered[slot] = name

    assert registered == expected, registered
    assert rdata_string(0x6131A0) == "load"
    assert rdata_string(0x613198) == "unload"
    assert text_bytes(0x4B1136, 5) == b"\x68" + struct.pack("<I", 0x6131A0)
    assert text_bytes(0x4B1144, 2) == b"\x6A\x06"
    assert text_bytes(0x4B11D3, 5) == b"\x68" + struct.pack("<I", 0x613198)
    assert text_bytes(0x4B11E1, 2) == b"\x6A\x05"
    print("native cursor initialization IDs:", {
        slot: expected[slot] for slot in (4, 8, 9, 12, 13)
    })
    print("native UI command tags: LOAD -> action mode 6; UNLOAD -> action mode 5")


def verify_native_setter_routes():
    """Connect the fixed-slot setter callsites to the native action selector."""
    selector_sites = direct_call_sites(ACTION_SELECTOR)
    assert selector_sites == [0x521DE6], [hex(site) for site in selector_sites]

    table_setter_sites = direct_call_sites(SET_BY_CURSOR_TABLE)
    expected_table_setter_sites = [
        0x4BA0A7, 0x4E7609, 0x4E7824, 0x4E7849, 0x525AF7, 0x525D7F,
        0x525E6E, 0x525E8F, 0x525EAD, 0x525ECF, 0x525EE3, 0x525F02,
        0x5277E5, 0x527F5C, 0x527FC8,
    ]
    assert table_setter_sites == expected_table_setter_sites, [
        hex(site) for site in table_setter_sites
    ]

    data = Path(ICD).read_bytes()
    text_file_offset = 0x1000
    text_vma = 0x401000

    def bytes_at(address, count):
        start = text_file_offset + address - text_vma
        return data[start:start + count]

    fixed_index_pushes = {
        0x4E7601: 20,       # Hourglass
        0x4E7822: 19,       # Normal
        0x4E7847: 19,
        0x525AF5: 19,
        0x525D7D: 19,
        0x5277E3: 19,
        0x527F54: 19,
        0x527FC0: 19,
    }
    for push_site, expected in fixed_index_pushes.items():
        assert bytes_at(push_site, 2) == bytes((0x6A, expected)), (
            hex(push_site), bytes_at(push_site, 2).hex()
        )

    # The remaining table-setter use is the zero/setup slot or selector
    # results. Six selector-driven cases occur inside this one UI action path;
    # each is immediately fed by native 0x521cd0, the sole selector caller.
    dynamic_sites = {
        0x525E6E, 0x525E8F, 0x525EAD, 0x525ECF, 0x525EE3, 0x525F02,
    }
    for setter_site in dynamic_sites:
        assert bytes_at(setter_site - 1, 1) == b"\x50", hex(setter_site)
        selector_call_found = False
        for call_site in range(setter_site - 16, setter_site - 1):
            if bytes_at(call_site, 1) != b"\xE8":
                continue
            displacement = struct.unpack("<i", bytes_at(call_site + 1, 4))[0]
            if call_site + 5 + displacement == 0x521CD0:
                selector_call_found = True
                break
        assert selector_call_found, hex(setter_site)

    # The non-immediate, non-selector remaining call is a setup/reset caller
    # which pushes EBX. Its containing code zeros EBX immediately before the
    # registration loop; the slot is not derived from a unit/action selection.
    assert bytes_at(0x4B9FB1, 2) == b"\x33\xDB"
    assert bytes_at(0x4BA0A6, 1) == b"\x53"

    service_setter_sites = direct_call_sites(SET_DEFAULT_CURSOR)
    assert service_setter_sites == [0x4B3922, 0x5255C1, 0x525C75, 0x525CC0], [
        hex(site) for site in service_setter_sites
    ]
    print("native direct action-selector callers:", [hex(x) for x in selector_sites])
    print("native table-setter fixed slots: Normal=19, Hourglass=20; other gameplay values come from selector")
    print("native table-setter selector-fed sites:", [hex(x) for x in sorted(dynamic_sites)])


def manager_probe(sequences):
    icd = Icd()
    uc = icd.uc
    game = HEAP + 0x10000
    manager = HEAP + 0x20000
    frames = HEAP + 0x38000
    lock_service = HEAP + 0x3A000
    uc.mem_write(manager, bytes(0x800))
    uc.mem_write(game, bytes(0x200))
    put_u32(uc, 0x62D55C, game)
    put_u32(uc, 0x65DDCC, manager)
    put_u32(uc, manager, CURSOR_MANAGER_VTABLE)
    put_u32(uc, manager + 0x404, frames)
    put_u32(uc, manager + 0x409, 0xFFFFFFFF)  # no default selected yet
    put_u32(uc, manager + 0x40D, 0xFFFFFFFF)  # no temporary override
    put_u32(uc, manager + 0x411, 0)            # shared native frame countdown

    delay_by_frame = {}
    sequence_frame_id = {}
    for ordinal, (slot, name) in enumerate(CAPTURE_PICKUP_TELEPORT.items()):
        count, delays = sequences[name]
        assert count == 1, f"{name} unexpectedly has {count} frames"
        # The game registrations map these native selector slots to the named
        # entries; each synthetic native record points at a unique frame ID.
        frame_id = 0xC400 + ordinal
        sequence_frame_id[slot] = frame_id
        delay_by_frame[frame_id] = delays[0]
        record = manager + slot * 0x20
        frame_list = frames + ordinal * 0x100
        put_u32(uc, record + 0x10, 0)
        put_u32(uc, record + 0x18, frame_list)
        put_u32(uc, record + 0x1C, frame_list + 4)
        put_u32(uc, frame_list, frame_id)
        # The table at game+0x17540 is indexed by the native cursor slot.
        put_u32(uc, game + 0x17540 + slot * 4, slot)

    queried_frames = []

    def get_delay(_uc, sp):
        (frame_id,) = struct.unpack("<I", _uc.mem_read(sp, 4))
        queried_frames.append(frame_id)
        assert frame_id in delay_by_frame, hex(frame_id)
        return 1, delay_by_frame[frame_id]

    # The table setter locks the global cursor service. Keep the two lock
    # services inert while retaining all cursor selection/frame instructions.
    icd.hooks[0x56FB20] = lambda _uc, _sp: (0, lock_service)
    icd.hooks[0x56FB80] = lambda _uc, _sp: (0, 0)
    icd.hooks[0x56FC20] = lambda _uc, _sp: (0, 0)
    icd.hooks[0x58D9F0] = get_delay
    icd.freeze_hooks()

    def select(slot):
        call(icd, SET_BY_CURSOR_TABLE, args=(slot,))
        assert get_u32(uc, manager + 0x409) == slot

    def update():
        call(icd, UPDATE_LIVE_POINTER, this=manager)

    for slot, name in CAPTURE_PICKUP_TELEPORT.items():
        select(slot)
        for _ in range(max(8, sequences[name][1][0] * 3)):
            update()
        frame_index = get_u32(uc, manager + slot * 0x20 + 0x10)
        assert frame_index == 0, (name, frame_index)
        assert sequence_frame_id[slot] in queried_frames, (name, queried_frames)
        print(f"native table setter selected slot {slot} ({name}); frame index stays 0")

    # Switching these one-frame cursors consumes the native shared countdown,
    # but there is no different art frame to reveal any sequence-start phase.
    assert get_u32(uc, manager + 0x411) >= 0
    print(f"native shared countdown after slot switches: {get_u32(uc, manager + 0x411)}")


def selector_probe():
    """Exercise every native action mode with a live, no-target baseline unit."""
    icd = Icd()
    uc = icd.uc
    game, unit, unit_type, map_obj, visible, point, resolved_cell = [
        HEAP + i * 0x10000 for i in range(1, 8)
    ]
    put_u32(uc, 0x62D55C, game)
    put_u32(uc, 0x62D558, game)
    put_u32(uc, game + 0x19EF4, visible)
    uc.mem_write(game + 0x306F, b"\x00")
    for index in range(16 * 16):
        put_u16(uc, visible + index * 2, 1)
    put_u32(uc, map_obj + 0x8C, 16)
    put_u32(uc, map_obj + 0x90, 16)
    put_u32(uc, unit + 0x130, 0x01000000)  # live, eligible unit
    put_u32(uc, unit + 0x8, 1)
    put_u32(uc, unit + 0xB4, unit_type)
    put_u32(uc, unit + 0xB8, map_obj)
    put_u32(uc, unit_type + 0x264, 0)
    put_u32(uc, unit_type + 0x260, 0)
    icd.hooks[0x4DD6A0] = lambda _uc, _sp: (1, 1)
    icd.hooks[0x50E660] = lambda _uc, _sp: (1, resolved_cell)
    icd.hooks[0x497100] = lambda _uc, _sp: (1, 1)
    icd.hooks[0x496FD0] = lambda _uc, _sp: (1, 1)
    icd.hooks[0x519F50] = lambda _uc, _sp: (1, 1)
    icd.hooks[0x520B60] = lambda _uc, _sp: (1, 1)
    icd.freeze_hooks()

    observed = {}
    for mode in range(1, 15):
        observed[mode] = call(icd, ACTION_SELECTOR, args=(mode, unit, 0, 0))
        assert observed[mode] not in (4, 8, 9), (mode, observed[mode])
    # Capture is action mode 13 in the native selector's table; with every
    # ability flag enabled it still falls through to Normal (slot 19).
    put_u32(uc, unit_type + 0x264, 0xFFFFFFFF)
    capture_mode = call(icd, ACTION_SELECTOR, args=(13, unit, 0, 0))
    assert capture_mode == NORMAL_SLOT, capture_mode
    print("native selector baseline modes 1–14:", observed)
    print("native Capture/action mode 13 with all UnitDef capability bits: slot 19 (Normal)")

    target = HEAP + 0x80000
    target_type = HEAP + 0x90000
    put_u32(uc, target + 0x130, 0x01000000)
    put_u32(uc, target + 0xB4, target_type)
    put_u32(uc, target + 0xB8, map_obj)
    put_u32(uc, unit_type + 0x264, 0x00000200)  # cantransport
    put_u16(uc, point + 2, 320)
    put_u16(uc, point + 6, 0)
    put_u16(uc, point + 10, 320)
    unload_slot = call(icd, ACTION_SELECTOR, args=(5, unit, 0, point))
    assert unload_slot == 13, unload_slot
    load_slot = call(icd, ACTION_SELECTOR, args=(6, unit, target, 0))
    assert load_slot == 12, load_slot
    print("native selector: LOAD mode 6 -> slot 12 (cursorload)")
    print("native selector: UNLOAD mode 5 + cantransport -> slot 13 (cursorunload)")


def main():
    root = Path(sys.argv[1]) if len(sys.argv) == 2 else Path("assets/extracted/all")
    if len(sys.argv) > 2:
        raise SystemExit("usage: probe_cursor_legacy_start_phase.py [extracted-retail-data-root]")
    sequences = read_cursor_sequences(root / "anims/cursors.gaf")
    normal_count, _ = sequences["cursornormal"]
    assert normal_count == 1, normal_count
    for slot, name in CAPTURE_PICKUP_TELEPORT.items():
        count, delays = sequences[name]
        assert count == 1, (name, count)
        print(f"retail GAF {name}: {count} frame, delay {delays[0]} ticks, native slot {slot}")
    verify_native_registration_order()
    verify_native_setter_routes()
    manager_probe(sequences)
    selector_probe()
    print("PASS: Capture/Pickup/Teleport start phase has no visible frame distinction in shipped art")
    print("Limits: synthetic manager records; no renderer, real pointer input, or retail GUI")


if __name__ == "__main__":
    main()
