#!/usr/bin/env python3
"""Drive native VTOL pickup through BeCarried and carrier-order completion.

This bounded headless probe combines retail order creation/queue insertion,
the real VTOL_PICKUP mission dispatcher and passenger-target pursuit
controller, native flight mover, map-derived height sectors, native cargo
attachment, the sorted mission descriptor registry, BeCarried dispatch, and
carrier-order completion. It uses the shipped Lake Lokken TNT height and
feature-ID plane and accepts a shipped flying carrier profile (ZONROC by
default, or for example CREAERI with --carrier). Feature definition bodies,
passenger eligibility, effect/UI, heap allocation, and visibility/mover-side
map services are controlled fixture boundaries. The passenger is stationary,
so this does not test passenger ground-route movement, moving passengers,
unloading, carrier COB animation logic, or renderer output. No retail GUI is
launched.

Run from the repository root:
    PYTHONPATH=tools/re python3 tools/re/probe_air_pickup_native_fullmap.py
    PYTHONPATH=tools/re python3 tools/re/probe_air_pickup_native_fullmap.py --carrier creaeri
"""
import argparse
import struct
from pathlib import Path

from emuphase import GS, Phase
from check_movement_callback_order import fbi_info
from check_surface_unload_map_release import cat, parse_tnt
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_ESP


MAP = "Lake Lokken"
DEFAULT_CARRIER = "zonroc"
DEFAULT_PASSENGER = "araarch"
START = (240, 120)
TARGET = (240, 350)
SCALE = 16
BECARRIED_NAME = 0x615984


def fixed(value):
    return int(float(value) * 65536)


def native_half_cell_ticks(info):
    """Mirror retail FBI parsing for UnitType+0x249."""
    multiplier = max(fixed(info.get("watermultiplier", "1")),
                     fixed(info.get("roadmultiplier", "1.2")), 65536)
    best_speed = (multiplier * fixed(info.get("maxvelocity", "0"))) >> 16
    return 255 if best_speed <= 0 else max(1, min(255, (8 << 16) // best_speed))


def u32(uc, address):
    return struct.unpack("<I", uc.mem_read(address, 4))[0]


def i32(uc, address):
    return struct.unpack("<i", uc.mem_read(address, 4))[0]


def put(uc, address, *values):
    uc.mem_write(address, struct.pack("<" + "I" * len(values),
                                      *(value & 0xFFFFFFFF for value in values)))


def byte(uc, address, value):
    uc.mem_write(address, bytes((value & 0xFF,)))


def make_map(phase, root, hpitool):
    """Install TNT-derived cell data and let retail construct height sectors."""
    uc, icd = phase.uc, phase.icd
    map_data = parse_tnt(cat(hpitool, root, "maps.hpi", f"Maps/{MAP}.tnt"))
    width, height, sea, heights, feature_ids, feature_count = map_data
    assert (width, height) == (480, 480), (width, height)
    records = bytearray(width * height * 14)
    for z in range(height):
        for x in range(width):
            index = z * width + x
            offset = index * 14
            corners = (
                heights[index],
                heights[z * width + min(x + 1, width - 1)],
                heights[min(z + 1, height - 1) * width + x],
                heights[min(z + 1, height - 1) * width + min(x + 1, width - 1)],
            )
            records[offset + 4] = heights[index]
            records[offset + 5] = max(corners)
            records[offset + 6] = min(corners)
            struct.pack_into("<H", records, offset + 8, feature_ids[index])
    uc.mem_write(phase.cells_addr, bytes(records))
    put(uc, GS + 0x19E88, width * SCALE)
    put(uc, GS + 0x19E8C, height * SCALE)
    put(uc, GS + 0x19E98, width)
    put(uc, GS + 0x19E9C, height)
    byte(uc, GS + 0x19EF8, sea)
    put(uc, GS + 0x19EC0, feature_count)
    feature_table = phase._alloc(max(320, feature_count * 320))
    put(uc, GS + 0x19EDC, feature_table)
    uc.mem_write(feature_table, bytes(max(320, feature_count * 320)))

    result, error = icd.call(0x50E740)
    if error:
        raise RuntimeError(("retail map height-sector initializer", error))
    sectors = u32(uc, GS + 0x19F18)
    stride = u32(uc, GS + 0x19F1C)
    expected_stride = (width + 7) // 8
    assert stride == expected_stride, (stride, expected_stride)

    # Independently recompute the native sector initializer's raw maxima and
    # 3x3 dilation for this actual shipped TNT plane.
    raw = []
    for sz in range(stride):
        for sx in range(stride):
            raw.append(max(
                sea,
                *(max(heights[z * width + x],
                      heights[z * width + min(x + 1, width - 1)],
                      heights[min(z + 1, height - 1) * width + x],
                      heights[min(z + 1, height - 1) * width +
                              min(x + 1, width - 1)])
                  for z in range(sz * 8, min(height, sz * 8 + 8))
                  for x in range(sx * 8, min(width, sx * 8 + 8)))
            ))
    for sz in range(stride):
        for sx in range(stride):
            expected = max(raw[nz * stride + nx]
                           for nz in range(max(0, sz - 1), min(stride, sz + 2))
                           for nx in range(max(0, sx - 1), min(stride, sx + 2)))
            address = sectors + (sz * stride + sx) * 10
            actual = struct.unpack("<BB", uc.mem_read(address, 2))
            if actual != (raw[sz * stride + sx], expected):
                raise AssertionError(("map height sector", sx, sz, actual,
                                      (raw[sz * stride + sx], expected),
                                      max(heights[z * width + x]
                                          for z in range(sz * 8,
                                                         min(height, sz * 8 + 8))
                                          for x in range(sx * 8,
                                                         min(width, sx * 8 + 8)))))
    return map_data, sectors, stride


def init_orders_and_units(phase, map_data, sectors, stride, carrier_name,
                          passenger_name):
    """Create native orders, reciprocal refs, entity-pool units and movers."""
    uc, icd = phase.uc, phase.icd
    def alloc(_uc, stack):
        size = u32(_uc, stack)
        return 0, phase._alloc(max(size, 16))

    def free(_uc, _stack):
        return 0, 0

    # ``malloc/free`` remain bounded arena sinks. Order descriptors and names
    # are registered and looked up from retail; passenger eligibility remains
    # a controlled service boundary.
    carrier_kind = phase._alloc(0x400)
    passenger_kind = phase._alloc(0x400)
    # Retail 0x507400's map/footprint scan indexes a per-type byte mask at
    # +0x12a for every cell in the footprint. Mobile FBI records do not expose
    # a yardmap, so the bounded fixture supplies an empty mask plane rather
    # than leaving the native pointer null.
    for kind in (carrier_kind, passenger_kind):
        occupancy_mask = phase._alloc(0x1000)
        put(uc, kind + 0x12A, occupancy_mask)
    owner = phase._alloc(0x400)
    player_entities = phase._alloc(3 * 0x138)
    carrier, passenger = player_entities + 0x138, player_entities + 2 * 0x138
    carrier_mover, passenger_mover = (phase._alloc(0x400) for _ in range(2))
    carrier_nav = phase._alloc(0x180)
    passenger_nav = phase._alloc(0x180)
    empty_vm_records = []
    carrier_order, passenger_order = (phase._alloc(0x100) for _ in range(2))
    game = GS

    def eligible(_uc, stack):
        return 1, int(u32(_uc, stack) == passenger)

    def feedback(_uc, _stack):
        return 3, 0

    def transfer_effect(_uc, _stack):
        counters[0] += 1
        return 2, 0

    counters = [0]
    icd.hooks.update({
        0x4EB9E0: alloc,
        0x4EBA00: free,
        0x519F50: eligible,
        0x535CC0: lambda _uc, _stack: (1, 0),
        0x50A9C0: feedback,
        0x421E10: transfer_effect,
        # Native model script requests are absent in this bounded no-renderer
        # unit fixture; transport logic/mover stay retail.
        0x56C640: lambda _uc, _stack: (8, 0),
    })

    put(uc, 0x62D55C, game)
    # Register the same three retail descriptor groups that populate the
    # sorted mission registry before constructing any orders. This gives the
    # fixture the retail IDs/handlers and lets BECARRIED resolve natively.
    byte(uc, 0x62DB80, 0)
    put(uc, 0x62DB84, 0, 0, 0)
    for registration in (0x402740, 0x4092E0, 0x421850):
        _, error = icd.call(registration)
        if error:
            raise RuntimeError(("native mission descriptor registration",
                                hex(registration), error))
    descriptors = u32(uc, 0x62DB84)
    descriptor_end = u32(uc, 0x62DB88)
    descriptor_count = (descriptor_end - descriptors) // 25
    if descriptor_count != 76:
        raise AssertionError(("native mission descriptor count",
                              descriptor_count))

    def native_mission_code(name_address):
        output = phase._alloc(4)
        _, error = icd.call(0x4D4BF0, (name_address,), ecx=output)
        if error:
            raise RuntimeError(("native mission-name lookup",
                                hex(name_address), error))
        code = uc.mem_read(output, 1)[0]
        if code == 0:
            raise AssertionError(("retail mission name was not registered",
                                  hex(name_address)))
        row = descriptors + code * 25
        row_name = u32(uc, row + 0x15)
        actual_name = bytes(uc.mem_read(row_name, 80)).split(b"\0")[0]
        requested_name = bytes(uc.mem_read(name_address, 80)).split(b"\0")[0]
        if actual_name.lower() != requested_name.lower():
            raise AssertionError(("native mission code/name mismatch", code,
                                  actual_name, requested_name))
        return code, u32(uc, row + 4)

    vtol_pickup_code, vtol_pickup_handler = native_mission_code(0x604DE4)
    move_pickup_code, move_pickup_handler = native_mission_code(0x604CF8)
    if (vtol_pickup_code, vtol_pickup_handler) != (62, 0x41A680):
        raise AssertionError(("retail VTOL_Pickup descriptor",
                              vtol_pickup_code, hex(vtol_pickup_handler)))
    if (move_pickup_code, move_pickup_handler) != (30, 0x403430):
        raise AssertionError(("retail Move_Seek_Pickup descriptor",
                              move_pickup_code, hex(move_pickup_handler)))
    # Check the sorted row that runtime attachment will look up, without
    # manufacturing or assigning a BECARRIED code in the fixture.
    be_carried_rows = []
    for row in range(descriptors, descriptor_end, 25):
        name = u32(uc, row + 0x15)
        label = bytes(uc.mem_read(name, 80)).split(b"\0")[0]
        if label.lower() == b"becarried":
            be_carried_rows.append(((row - descriptors) // 25,
                                    u32(uc, row + 4)))
    if be_carried_rows != [(11, 0x4024A0)]:
        raise AssertionError(("retail BeCarried descriptor", be_carried_rows))

    native_events = {"lookup_sites": [], "lookup_returns": [],
                     "handlers": [], "removals": []}

    def observe_native_order_code(machine, address, _size, _user):
        tick = u32(machine, GS + 0x19F44)
        esp = machine.reg_read(UC_X86_REG_ESP)
        if address == 0x4D4A5B:
            # The retail caller pushes the BECARRIED name immediately before
            # calling its sorted descriptor lookup at 0x4d4bf0.
            if u32(machine, esp) == BECARRIED_NAME:
                native_events["lookup_sites"].append(tick)
        elif address == 0x4D4A60:
            # Return site immediately after the unhooked native name lookup.
            if native_events["lookup_sites"] and \
                    native_events["lookup_sites"][-1] == tick:
                native_events["lookup_returns"].append(tick)
        elif address == 0x4024A0:
            unit, order = struct.unpack("<II", machine.mem_read(esp + 4, 8))
            native_events["handlers"].append((tick, unit, order))
        elif address == 0x4D6AD0:
            unit, order = struct.unpack("<II", machine.mem_read(esp + 4, 8))
            native_events["removals"].append((tick, unit, order))

    for address in (0x4D4A5B, 0x4D4A60, 0x4024A0, 0x4D6AD0):
        uc.hook_add(UC_HOOK_CODE, observe_native_order_code,
                    begin=address, end=address)

    put(uc, game + 0x19F30, 1)
    put(uc, game + 0x174C8, 101)
    put(uc, game + 0x174CC, 102)

    # Native entity resolver used by 0x51b4f0 -> 0x51b5a0 (IDs 1 and 2).
    put(uc, game + 0x14E84, player_entities)
    put(uc, game + 0x14E88, passenger)
    put(uc, owner, 1)
    byte(uc, owner + 0xEA, 1)
    put(uc, owner + 0x74, carrier)
    put(uc, owner + 0x78, passenger + 0x138)

    carrier_info = fbi_info(carrier_name)
    passenger_info = fbi_info(passenger_name)
    assert carrier_info.get("canfly") == "1"
    assert carrier_info.get("cantransport") == "1"
    assert passenger_info.get("movementclass", "").upper().startswith("GROUND")
    for unit, kind, unit_id, info in (
            (carrier, carrier_kind, 1, carrier_info),
            (passenger, passenger_kind, 2, passenger_info)):
        put(uc, unit + 2, unit_id)
        put(uc, unit + 0xB4, kind)
        put(uc, unit + 0xB8, owner)
        put(uc, unit + 0x130, 0x01000000)
        put(uc, unit + 0x78,
            int(float(info.get("footprintx", "1"))) |
            (int(float(info.get("footprintz", "1"))) << 16))
        byte(uc, kind + 0x24B, 0)
        # The ordinary dispatcher asks the unit's COB object whether it
        # declares certain callbacks. A native VM with zero methods makes the
        # real 0x56c4a0 lookup return its retail "not found" sentinel while
        # avoiding a fake unit+0xbc pointer or substituting the method lookup.
        empty_vm, empty_descriptor = phase._alloc(0x40), phase._alloc(0x40)
        put(uc, unit + 0xBC, empty_vm)
        put(uc, empty_vm + 0x0C, empty_descriptor)
        put(uc, empty_descriptor + 4, 0)
        empty_vm_records.append((empty_vm, empty_descriptor))

    # Use authored selected-carrier movement/flight/transport fields. The
    # tests only control native host calls; all flight calculations are retail
    # code.
    put(uc, carrier + 8, carrier_mover)
    put(uc, carrier + 0x12B, fixed(carrier_info["maxvelocity"]))
    put(uc, carrier_kind + 0x162, fixed(carrier_info["maxvelocity"]))
    put(uc, carrier_kind + 0x166, fixed(carrier_info["brakerate"]))
    put(uc, carrier_kind + 0x16A, fixed(carrier_info["acceleration"]))
    put(uc, carrier_kind + 0x16E, fixed(carrier_info.get("watermultiplier", "1")))
    put(uc, carrier_kind + 0x172, fixed(carrier_info.get("roadmultiplier", "1.2")))
    put(uc, carrier_kind + 0x182, fixed(carrier_info.get("moverate1", "1")))
    put(uc, carrier_kind + 0x186, fixed(carrier_info.get("moverate2", "9")))
    put(uc, carrier_kind + 0x23E, int(float(carrier_info["transportdistance"])))
    put(uc, carrier_kind + 0x260, 0x800)
    put(uc, carrier_kind + 0x264, 0x200)
    uc.mem_write(carrier_kind + 0x23A,
                 struct.pack("<h", int(float(carrier_info["cruisealt"]))))
    put(uc, carrier_kind + 0x126,
        int(float(carrier_info.get("footprintx", "1"))) |
        (int(float(carrier_info.get("footprintz", "1"))) << 16))
    uc.mem_write(carrier_kind + 0x18E,
                 struct.pack("<H", int(float(carrier_info["turnrate"]))))
    half_cell_ticks = native_half_cell_ticks(carrier_info)
    byte(uc, carrier_kind + 0x249, half_cell_ticks)
    put(uc, carrier_kind + 0x182, fixed(carrier_info.get("moverate1", "1")))
    put(uc, carrier_kind + 0x186, fixed(carrier_info.get("moverate2", "9")))

    width, height, sea, heights, _features, _feature_count = map_data
    for unit, mover, position, info in (
            (carrier, carrier_mover, START, carrier_info),
            (passenger, passenger_mover, TARGET, passenger_info)):
        x, z = position
        ground_y = heights[z * width + x]
        y = ground_y + (int(float(info["cruisealt"])) if unit == carrier else 0)
        put(uc, unit + 0x68, x * SCALE * 65536, y * 65536,
            z * SCALE * 65536)
        uc.mem_write(unit + 0x74, struct.pack("<hh", x, z))
        put(uc, unit + 8, mover)
    # The map positions are within the same shipped height sector grid. Both
    # movers begin stopped; the flight controller is native pursuit.
    put(uc, passenger_mover, passenger_nav)
    put(uc, passenger_mover + 0x20, 0)
    put(uc, passenger_nav, 0x5F2A24)
    put(uc, passenger_nav + 8, passenger)
    put(uc, carrier_mover, carrier_nav)
    put(uc, carrier_mover + 0x20, 0)
    put(uc, carrier_mover + 0x30, 0)
    uc.mem_write(carrier_mover + 0x36, struct.pack("<H", 2))
    put(uc, carrier_nav, 0x5F34D4)
    put(uc, carrier_nav + 8, carrier)
    x, z = START
    ground_y = heights[z * width + x]
    put(uc, carrier_nav + 0x0C,
        x * SCALE * 65536, (ground_y + int(float(carrier_info["cruisealt"]))) * 65536,
        z * SCALE * 65536)
    uc.mem_write(carrier_nav + 0x24, b"\0\0")

    # Current sector membership is the same native 128px sector/list layout
    # used by 0x4dc800 and the attachment routine.
    for unit, position in ((carrier, START), (passenger, TARGET)):
        sx, sz = position[0] // 8, position[1] // 8
        sector = sectors + (sz * stride + sx) * 10
        _, error = icd.call(0x506650, (unit, sector))
        if error:
            raise RuntimeError(("native sector insertion", unit, error))
        assert u32(uc, unit + 0xA4) == sector

    # Actual native Move_Seek_Pickup and VTOL_Pickup orders. The passenger
    # order's carrier pointer becomes a reciprocal
    # reference during insertion, and the carrier order references the target.
    p_order = phase._alloc(0x100)
    c_order = phase._alloc(0x100)
    for code, target, order, unit in (
            (move_pickup_code, carrier, p_order, passenger),
            (vtol_pickup_code, passenger, c_order, carrier)):
        result, error = icd.call(
            0x4D6C40, (code, target, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
            ecx=order)
        if error or result != order:
            raise RuntimeError(("native transport order constructor", code,
                                result, error))
        _, error = icd.call(0x4D7750, (unit, order))
        if error:
            raise RuntimeError(("native transport order queue insertion", code,
                                error))
    assert u32(uc, passenger + 0x60) == p_order
    assert u32(uc, carrier + 0x60) == c_order
    assert uc.mem_read(p_order + 4, 1)[0] == move_pickup_code
    assert uc.mem_read(c_order + 4, 1)[0] == vtol_pickup_code
    assert u32(uc, carrier + 0xC4) == p_order + 0x12
    assert u32(uc, passenger + 0xC4) == c_order + 0x12
    return {
        "icd": icd, "uc": uc, "carrier": carrier, "passenger": passenger,
        "carrier_kind": carrier_kind, "passenger_kind": passenger_kind,
        "carrier_mover": carrier_mover, "passenger_mover": passenger_mover,
        "carrier_nav": carrier_nav, "carrier_order": c_order,
        "passenger_order": p_order, "owner": owner,
        "native_events": native_events, "map_effects": counters,
        "carrier_name": carrier_name, "passenger_name": passenger_name,
        "half_cell_ticks": half_cell_ticks,
        "mission_codes": {"VTOL_Pickup": vtol_pickup_code,
                          "Move_Seek_Pickup": move_pickup_code},
    }


def run(args):
    root = Path(args.retail_root).resolve()
    phase = Phase(480, 480)
    map_data, sectors, stride = make_map(phase, root, Path(args.hpitool).resolve())
    carrier_name, passenger_name = args.carrier.lower(), args.passenger.lower()
    live = init_orders_and_units(phase, map_data, sectors, stride,
                                 carrier_name, passenger_name)
    icd, uc = live["icd"], live["uc"]
    carrier, passenger = live["carrier"], live["passenger"]
    carrier_mover, carrier_nav = live["carrier_mover"], live["carrier_nav"]
    carrier_order, passenger_order = live["carrier_order"], live["passenger_order"]
    effects = live["map_effects"]
    initial_position = struct.unpack("<3i", uc.mem_read(carrier + 0x68, 12))

    settings = phase._alloc(0x1000)
    options = settings + 0x100
    put(uc, 0x62D558, settings)
    put(uc, settings, options)
    put(uc, settings + 8, options)
    uc.mem_write(options, bytes(0x100))

    # Start at a real Lake Lokken cell and follow the stationary Araarch via
    # retail's pursuit controller. Each tick dispatches the carrier and, after
    # boarding, the attached passenger's BeCarried order before the real mover.
    tick = 0
    rows = []
    controller_seen = False
    attachment_tick = None
    carrier_retired_tick = None
    selected = False
    for _step in range(args.max_ticks):
        tick += 1
        put(uc, GS + 0x19F44, tick)
        _, error = icd.call(0x4D8450, (carrier,))
        if error:
            raise RuntimeError(("native VTOL pickup dispatcher", tick, error))
        controller = u32(uc, carrier_nav + 4)
        if controller:
            controller_seen = True
            selected = True
        _, error = icd.call(0x4DC800, (carrier,), ecx=carrier_mover)
        if error:
            raise RuntimeError(("native flight mover", tick, error))
        # The ordinary unit-update cadence follows 0x4dc800 with 0x51b2a0.
        # For this flying class it should preserve airborne height unless the
        # native dirty/hover conditions request a ground-supported update.
        _, error = icd.call(0x51B2A0, (carrier,), ecx=carrier_mover)
        if error:
            raise RuntimeError(("native flight height commit", tick, error))
        position = struct.unpack("<3i", uc.mem_read(carrier + 0x68, 12))
        stage = uc.mem_read(carrier_order + 5, 1)[0] \
            if u32(uc, carrier + 0x60) == carrier_order else -1
        events = u32(uc, carrier_order + 0x6A) \
            if u32(uc, carrier + 0x60) == carrier_order else 0
        controller = u32(uc, carrier_nav + 4)
        if u32(uc, carrier + 0xAC) == passenger and \
                u32(uc, passenger + 0xA8) == carrier:
            if attachment_tick is None:
                attachment_tick = tick
            if carrier_retired_tick is None and u32(uc, carrier + 0x60) == 0:
                carrier_retired_tick = tick
            _, error = icd.call(0x4D8450, (passenger,))
            if error:
                raise RuntimeError(("native attached-passenger dispatcher",
                                    tick, error))
        rows.append((tick, *position, stage, events, controller,
                     u32(uc, carrier + 0xAC), u32(uc, passenger + 0xA8)))
        if attachment_tick is not None and carrier_retired_tick is not None:
            break
    if not selected or not controller_seen:
        raise AssertionError("native VTOL pickup never installed pursuit controller")
    if attachment_tick is None:
        if args.allow_incomplete:
            final_position = struct.unpack("<3i", uc.mem_read(carrier + 0x68, 12))
            if final_position == initial_position:
                raise AssertionError(("bounded native mover made no progress",
                                      final_position))
            if u32(uc, carrier + 0x60) != carrier_order or \
                    u32(uc, passenger + 0x60) != passenger_order:
                raise AssertionError("orders changed before the early-stop control")
            if u32(uc, carrier + 0xAC) or u32(uc, passenger + 0xA8):
                raise AssertionError("cargo attached before the early-stop control")
            print(f"PASS: {args.max_ticks} bounded native pursuit ticks moved "
                  f"ZONROC toward stationary Araarch; controller remains "
                  f"installed and both orders remain queued. Boarding is "
                  "correctly outside this short control window.")
            return
        raise AssertionError((f"native {carrier_name} air pickup failed before attachment",
                              rows[-8:]))
    if u32(uc, carrier + 0xAC) != passenger or u32(uc, passenger + 0xA8) != carrier:
        raise AssertionError("native cargo links are not reciprocal")
    carrier_unlinks = [(remove_tick, unit, order)
                       for remove_tick, unit, order
                       in live["native_events"]["removals"]
                       if unit == carrier and order == carrier_order]
    if carrier_retired_tick != attachment_tick:
        raise AssertionError(("native VTOL_Pickup completion boundary",
                              attachment_tick, carrier_retired_tick,
                              carrier_unlinks,
                              hex(u32(uc, carrier + 0x60))))
    if [remove_tick for remove_tick, _unit, _order in carrier_unlinks] != [attachment_tick]:
        raise AssertionError(("native carrier-order unlink tick",
                              attachment_tick, carrier_unlinks))
    if u32(uc, carrier + 0x60) != 0:
        raise AssertionError(("native carrier VTOL_Pickup order remains queued",
                              hex(u32(uc, carrier + 0x60))))
    if (carrier, carrier_order) not in [
            (unit, order) for _remove_tick, unit, order
            in live["native_events"]["removals"]]:
        raise AssertionError(("native carrier order unlink was not observed",
                              live["native_events"]["removals"][-8:]))
    passenger_head = u32(uc, passenger + 0x60)
    if not passenger_head or uc.mem_read(passenger_head + 4, 1)[0] != 11:
        raise AssertionError(("native BeCarried order was not installed on passenger",
                              hex(passenger_head)))
    carried_handlers = live["native_events"]["handlers"]
    if live["native_events"]["lookup_sites"] != [attachment_tick] or \
            live["native_events"]["lookup_returns"] != [attachment_tick]:
        raise AssertionError(("native BECARRIED name lookup did not return",
                              attachment_tick,
                              live["native_events"]["lookup_sites"],
                              live["native_events"]["lookup_returns"]))
    if not any(handler_tick >= attachment_tick and handler_unit == passenger and
               handler_order == passenger_head
               for handler_tick, handler_unit, handler_order in carried_handlers):
        raise AssertionError(("native BeCarried handler was not dispatched",
                              attachment_tick, passenger_head,
                              carried_handlers[-8:]))
    def order_list(unit, offset):
        result = []
        order = u32(uc, unit + offset)
        while order and len(result) < 32:
            code = uc.mem_read(order + 4, 1)[0]
            row = u32(uc, 0x62DB84) + code * 25
            name_ptr = u32(uc, row + 0x15)
            name = bytes(uc.mem_read(name_ptr, 80)).split(b"\0")[0]
            result.append((hex(order), code,
                           name.decode("ascii", "replace")))
            order = u32(uc, order + 0x66)
        return result

    passenger_orders = order_list(passenger, 0x60) + order_list(passenger, 0x64)
    if any(int(pointer, 16) == passenger_order
           for pointer, _code, _name in passenger_orders):
        raise AssertionError(("passenger Move_Seek_Pickup order remains queued",
                              hex(passenger_order), passenger_orders))
    print("Native order lists after boarding:", {
        "carrier_current": order_list(carrier, 0x60),
        "carrier_queued": order_list(carrier, 0x64),
        "passenger_current": order_list(passenger, 0x60),
        "passenger_queued": order_list(passenger, 0x64),
    })
    assert effects[0] >= 2, effects
    print(
        f"PASS: Lake Lokken {carrier_name} native VTOL pickup used retail descriptor IDs "
        f"VTOL_Pickup={live['mission_codes']['VTOL_Pickup']} and "
        f"Move_Seek_Pickup={live['mission_codes']['Move_Seek_Pickup']}; "
        f"{attachment_tick - 1} carrier flight updates over the map-built "
        f"sectors reached pickup range. It attached {passenger_name}, dispatched native "
        f"BeCarried code 11/handler 0x4024a0, removed the passenger pickup "
        f"order, and retired carrier VTOL_Pickup on tick {carrier_retired_tick}. "
        f"boarding tick {attachment_tick}; map sectors {stride}x{stride}, "
        f"native half-cell scale {live['half_cell_ticks']}, "
        f"transfer effects {effects[0]}."
    )
    print("  Native BECARRIED lookup/return and BeCarried handler ticks:",
          live["native_events"]["lookup_returns"],
          [handler_tick for handler_tick, handler_unit, handler_order
           in carried_handlers if handler_unit == passenger and
           handler_order == passenger_head])
    print("  Carrier final XYZ:", tuple(value // 65536 for value in
                                           struct.unpack("<3i", uc.mem_read(carrier + 0x68, 12))))
    print("  Controlled boundaries: feature-definition bodies; passenger "
          "eligibility; allocator/free; audio/effect/UI. COB method lookup "
          "uses an empty-VM sink, so carrier-specific COB behavior is not tested. "
          "Retail's mission descriptor registration, name lookup, unit order "
          "constructors, carrier/passenger handlers, queue insertion/removal, "
          "flight controller/mover, map-sector construction, cargo attachment, "
          "and BeCarried dispatch execute from KINGDOMS.icd. The Araarch stays "
          "stationary; its ground approach/movement is outside this test.")


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--retail-root", default="/home/pocket_geek/tak_data")
    parser.add_argument("--hpitool", default=str(repo / "build-o2/hpitool"))
    parser.add_argument("--carrier", default=DEFAULT_CARRIER,
                        help="shipped flying transport profile (default: zonroc)")
    parser.add_argument("--passenger", default=DEFAULT_PASSENGER,
                        help="shipped ground passenger profile (default: araarch)")
    parser.add_argument("--max-ticks", type=int, default=5000)
    parser.add_argument("--allow-incomplete", action="store_true",
                        help="accept a moving native pursuit that has not yet boarded at the tick limit")
    args = parser.parse_args()
    if args.max_ticks < 1 or args.max_ticks > 10000:
        parser.error("--max-ticks must be in 1..10000")
    run(args)


if __name__ == "__main__":
    main()
