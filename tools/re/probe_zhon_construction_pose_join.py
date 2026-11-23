#!/usr/bin/env python3
"""Join native Zonhunt construction movement/callbacks to its root screen anchor.

This headless diagnostic reuses the bounded retail 41ef00/4dc800 trace and runs
Zonhunt's shipped COB controller in KINGDOMS' native COB VM with the same
callback schedule. It deliberately reports only the unit/root anchor: this
fixture records COB piece setter targets but does not implement timed piece
interpolation, so child-piece setter values are not displayed poses. The
projection comparison evaluates the two known formulas from the same fixture
coordinates; it is not an independent framebuffer measurement.
"""
import argparse
import struct
from pathlib import Path

from emu import HEAP, Icd
from check_zhon_construction_flight_trace import native_persistent_trace


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_COB = ROOT / "assets/extracted/all/scripts/zonhunt.cob"
SAMPLES = (1, 25, 63, 82, 101)


def u32(uc, address):
    return struct.unpack("<I", uc.mem_read(address, 4))[0]


def s32(uc, address):
    return struct.unpack("<i", uc.mem_read(address, 4))[0]


def put(uc, address, value):
    uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))


def names_at(data, offset, count):
    result = []
    for index in range(count):
        string = struct.unpack_from("<I", data, offset + 4 * index)[0]
        result.append(data[string:data.index(b"\0", string)].decode("ascii"))
    return result


def native_display_root_state(cob_path, movement_rows, callback_rows, sample_ticks,
                              capture_context=False):
    data = cob_path.read_bytes()
    header = struct.unpack_from("<10I", data)
    _, script_count, piece_count, code_words, static_count, _, entry_off, \
        script_names_off, piece_names_off, code_off = header
    script_names = names_at(data, script_names_off, script_count)
    piece_names = names_at(data, piece_names_off, piece_count)
    script_index = {name.lower(): index for index, name in enumerate(script_names)}
    piece_index = {name.lower(): index for index, name in enumerate(piece_names)}
    required = ("Create", "BeginFlight", "StartBuilding", "setSFXoccupy")
    absent = [name for name in required if name.lower() not in script_index]
    if absent:
        raise ValueError(f"Zonhunt COB is missing {absent}")

    p = Icd()
    uc = p.uc
    vm, desc, statics, pieces, vtable, scratch = (
        HEAP + offset for offset in (0x80000, 0x90000, 0xA0000, 0xB0000, 0xC0000, 0xD0000)
    )
    code, entries = HEAP + 0x100000, HEAP + 0x200000
    name_table, name_strings = HEAP + 0x300000, HEAP + 0x301000
    sounds = HEAP + 0x380000
    game, unit, unit_type, view = (HEAP + offset for offset in
                                   (0x40000, 0x50000, 0x60000, 0x70000))

    # The VM callbacks in this harness expose COB writes directly. That is
    # enough to verify whether the root piece is controlled, but not to
    # interpolate the timed TURN targets written to child pieces.
    pose = [[0] * 6 for _ in range(piece_count)]
    set_events = []

    def write_pose(base):
        def callback(_uc, sp):
            piece, axis, value = struct.unpack("<3I", uc.mem_read(sp, 12))
            if piece >= piece_count or axis >= 3:
                raise AssertionError(("native Zonhunt COB piece write", piece, axis))
            pose[piece][base + axis] = value
            return 3, 0
        return callback

    def read_pose(base):
        def callback(_uc, sp):
            piece, axis = struct.unpack("<2I", uc.mem_read(sp, 8))
            if piece >= piece_count or axis >= 3:
                raise AssertionError(("native Zonhunt COB piece read", piece, axis))
            return 2, pose[piece][base + axis]
        return callback

    def set_value(_uc, sp):
        value_id, value = struct.unpack("<2i", uc.mem_read(sp, 8))
        set_events.append((value_id, value))
        return 2, 0

    def get_value(_uc, sp):
        query = u32(uc, sp)
        # The test monarch is already complete and alive. In particular, GET 17
        # (BUILD_PERCENT_LEFT) must be zero so Create initializes its controllers.
        return 5, {4: 100, 17: 0, 18: 1}.get(query, 0)

    callbacks = {
        0: write_pose(0), 4: write_pose(3), 8: lambda _uc, _sp: (2, 0),
        12: lambda _uc, _sp: (2, 0), 16: lambda _uc, _sp: (2, 0),
        20: lambda _uc, _sp: (2, 0), 24: read_pose(0), 28: read_pose(3),
        44: lambda _uc, _sp: (2, 0), 48: lambda _uc, _sp: (2, 0),
        52: lambda _uc, _sp: (2, 0),
        56: lambda _uc, sp: (2, u32(uc, sp + 4)),
        80: set_value, 84: get_value,
    }
    put(uc, vm, vtable)
    for offset, callback in callbacks.items():
        address = 0x56A000 + offset * 4
        put(uc, vtable + offset, address)
        p.hooks[address] = callback
    p.hooks.update({
        0x5359A0: lambda _uc, _sp: (0, 0),
        0x5359C0: lambda _uc, _sp: (1, 0),
        0x535A30: lambda _uc, _sp: (2, 0),
        0x5BA3D0: lambda _uc, _sp: (0, scratch),
        0x5BA5D0: lambda _uc, _sp: (0, 0),
    })
    p.freeze_hooks()

    put(uc, vm + 4, 30)
    put(uc, vm + 0x0C, desc)
    put(uc, vm + 0x10, struct.unpack_from("<I", data)[0])
    put(uc, vm + 0x14, statics)
    put(uc, vm + 0x18, pieces)
    put(uc, vm + 0xA64, view)
    put(uc, view + 0x0C, unit)
    put(uc, unit + 0xBC, vm)
    put(uc, unit + 0xB4, unit_type)
    put(uc, unit + 0xB8, HEAP + 0x71000)
    put(uc, unit + 0x130, 0x01000000)
    put(uc, 0x62D55C, game)

    put(uc, desc + 4, script_count)
    put(uc, desc + 8, piece_count)
    put(uc, desc + 0x10, static_count)
    put(uc, desc + 0x18, entries)
    put(uc, desc + 0x24, code)
    put(uc, desc + 0x2C, sounds)
    uc.mem_write(code, data[code_off:code_off + code_words * 4])
    uc.mem_write(entries, data[entry_off:entry_off + script_count * 4])
    string_ptrs, cursor = [], 0
    for name in script_names:
        address = name_strings + cursor
        raw = name.encode("ascii") + b"\0"
        uc.mem_write(address, raw)
        string_ptrs.append(address)
        cursor += len(raw)
    uc.mem_write(name_table, b"".join(struct.pack("<I", value) for value in string_ptrs))
    put(uc, desc + 0x1C, name_table)

    result, error = p.call(0x56DC00, (sounds,), ecx=vm)
    if error or result != 0:
        raise RuntimeError(("native Zonhunt COB init", result, error))

    # GameView starts Create at registration. It sends these flight/build
    # transitions before advancing the display VM on the first 30 Hz fixture tick.
    _, error = p.call(0x56C5F0, (script_index["create"], 0, 1), ecx=vm)
    if error:
        raise RuntimeError(("Zonhunt Create", error))

    callback_by_tick = dict(callback_rows)
    movement_by_tick = {row[0]: row for row in movement_rows}
    sampled = {}
    for tick in range(1, max(sample_ticks) + 1):
        row = movement_by_tick[tick]
        put(uc, game + 0x19F44, tick)
        put(uc, 0x64186C, tick)
        uc.mem_write(unit + 0x68, struct.pack("<3i", *row[1:4]))
        uc.mem_write(unit + 0x7E, struct.pack("<H", row[4]))

        if tick == 1:
            _, error = p.call(0x56C680,
                (script_index["beginflight"], 0, 1, 0, 0, 0, 0, 0), ecx=vm)
            if error:
                raise RuntimeError(("Zonhunt BeginFlight", error))

        dispatches = []
        for name, values in callback_by_tick.get(tick, ()):
            index = script_index.get(name.lower())
            if index is None:
                continue  # retail's native callback-by-name lookup ignores absent methods
            _, error = p.call(0x56C680,
                (index, 0, 1, len(values), *(list(values) + [0] * (4 - len(values)))), ecx=vm)
            if error:
                raise RuntimeError(("native Zonhunt callback", tick, name, values, error))
            dispatches.append((name, values))

        if tick == 1:
            _, error = p.call(0x56C680,
                (script_index["startbuilding"], 0, 1, 0, 0, 0, 0, 0), ecx=vm)
            if error:
                raise RuntimeError(("Zonhunt StartBuilding", error))

        _, error = p.call(0x56C870, (1,), ecx=vm)
        if error:
            raise RuntimeError(("native Zonhunt COB tick", tick, error))
        if tick in sample_ticks:
            active_threads = sum(
                bool(u32(uc, vm + 0x20 + index * 0xA4)) for index in range(16)
            )
            root_piece = piece_index.get("zon_gpoly")
            if root_piece is None:
                raise ValueError("Zonhunt COB is missing root piece Zon_gpoly")
            sampled[tick] = {
                "position": row[1:4], "heading": row[4],
                "native_callbacks": callback_by_tick.get(tick, ()),
                "cob_dispatches": dispatches,
                "set_events": list(set_events),
                "root_piece_setter_values": pose[root_piece][:],
                "active_threads": active_threads,
                "statics": [s32(uc, statics + index * 4) for index in range(static_count)],
            }
            if capture_context:
                sampled[tick]["piece_pose"] = [row[:] for row in pose]
    if capture_context:
        return sampled, p, piece_index, unit
    return sampled


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cob", type=Path, default=DEFAULT_COB)
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=1)
    parser.add_argument("--terrain", type=int, default=100)
    args = parser.parse_args()

    metadata, movement, _phases, callback_rows = native_persistent_trace(103, args.seed)
    if [movement[tick - 1][0] for tick in SAMPLES] != list(SAMPLES):
        raise AssertionError("native placed-build trace did not cover all requested sample ticks")
    if metadata["terrain"] != args.terrain:
        raise AssertionError(("terrain fixture mismatch", metadata["terrain"], args.terrain))
    sampled = native_display_root_state(args.cob, movement, callback_rows, SAMPLES)

    site_x, site_height, site_z = metadata["site"]
    reference = args.terrain
    site_screen_y = site_z - (site_height >> 1) + reference * 0.5
    print("HEADLESS ZONHUNT ROOT ANCHOR (zoom 1; camera translation cancels in site-relative values)")
    print("tick native callbacks(name(args))                           COB dispatches(name(args))   unit(x,y,z)      root-site(dx,dy)  Δy formula  root setter / threads / gates(6,9,10)")
    max_delta = 0.0
    for tick in SAMPLES:
        sample = sampled[tick]
        x, y, z = (value / 65536.0 for value in sample["position"])
        altitude = y - reference
        native_label = ",".join(
            f"{name}({','.join(map(str, values))})" for name, values in sample["native_callbacks"]
        ) or "—"
        cob_label = ",".join(
            f"{name}({','.join(map(str, values))})" for name, values in sample["cob_dispatches"]
        ) or "—"
        # GameView's flat-map root draw anchor splits flyer altitude between
        # its ground position and vertical lift. Retail projects absolute
        # height at half scale. The equations reduce algebraically to the same
        # height-relative projection; this is not an independent pixel sample.
        gameview_dy = z - altitude * 0.5 - site_screen_y
        retail_dy = (z - ((int(sample["position"][1]) >> 16) >> 1)
                     + reference * 0.5 - site_screen_y)
        dy_delta = retail_dy - gameview_dy
        dx = x - site_x
        max_delta = max(max_delta, abs(dy_delta))
        gates = tuple(sample["statics"][index] for index in (6, 9, 10))
        root_setter = tuple(sample["root_piece_setter_values"])
        print(f"{tick:4d} {native_label:<58.58} {cob_label:<31.31} "
              f"{x:7.2f},{y:6.2f},{z:7.2f}  {dx:+8.2f},{gameview_dy:+8.2f}  "
              f"{dy_delta:+.3f}  {root_setter!s:<20.20} "
              f"{sample['active_threads']:2d} / {gates}")
        if root_setter != (0, 0, 0, 0, 0, 0):
            raise AssertionError(("Zonhunt root piece setter changed", tick, root_setter))
        if sample["active_threads"] == 0 or gates != (1, 1, 1):
            raise AssertionError(("Zonhunt display controller inactive", tick, gates,
                                  sample["active_threads"]))
    print(f"PASS: native 41ef00/4dc800 movement and callbacks joined at ticks {list(SAMPLES)}; "
          f"native Zonhunt display COB ticks with active controller, while root piece setters "
          f"remain zero. Formula-evaluation delta is at most {max_delta:.3f} px.")
    print("LIMITS: flat 100-height fixture, controlled 30 Hz display-VM ticks, no child-piece "
          "TURN interpolation, no framebuffer/live camera capture. Root anchor is unaffected by "
          "the controlled piece-callback seam; the northward offset follows native altitude "
          "and orbital ground position. The formula delta is a comparison of algebraically "
          "equivalent equations from the same coordinates, not an independent projection measurement.")


if __name__ == "__main__":
    main()
