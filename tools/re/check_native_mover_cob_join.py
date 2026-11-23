#!/usr/bin/env python3
"""Join retail mover callbacks to native and local shipped COB state.

The native 0x4dc800 mover runs for two ticks per representative. Its
0x56c640 callback requests are captured, filtered by the shipped COB's actual
method table, then scheduled into retail's 56c680/56c870 script VM. The same
events drive retail_script_test's World-side COB VM, and complete script,
thread, and piece checkpoints are compared. A separate helper comparison
checks movement callback edge ordering. This is headless; it does not render
models or launch the retail GUI.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from emu import HEAP, Icd
from check_movement_callback_order import (
    CALLBACKS, SCRIPT_ROOT, UNIT_ROOT, cob_methods, fbi_info, native_trace,
)


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_UNITS = ("tarblack", "tarlich", "tarcship", "aradrag")
PROFILE_NAMES = {0: "idle", 1: "ground movement", 2: "water movement", 3: "damaged veteran"}


def u32(uc, address):
    return struct.unpack("<I", uc.mem_read(address, 4))[0]


def put(uc, address, value):
    uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))


def names_at(data, offset, count):
    result = []
    for index in range(count):
        string = struct.unpack_from("<I", data, offset + 4 * index)[0]
        result.append(data[string:data.index(b"\0", string)].decode("ascii"))
    return result


def profile_for(unit):
    info = fbi_info(unit)
    methods = cob_methods(unit)
    declared = methods.intersection(CALLBACKS)
    if not declared:
        raise ValueError(f"{unit} does not declare a mover callback")
    can_move = bool(float(info.get("canmove", "0")))
    if not can_move:
        raise ValueError(f"{unit} is not mobile")
    can_fly = bool(float(info.get("canfly", "0")))
    movement_class = info.get("movementclass", "?").upper()
    water = movement_class.startswith("WATER")
    hover = movement_class.startswith("HOVER")
    kind = "flying" if can_fly else "water" if water else "hover" if hover else "ground"
    waterline = int(float(info.get("waterline", "0")))
    mover_profile = dict(
        unit=unit,
        mode=2 if can_fly else 1,
        y=100 if can_fly else 100 - waterline if water else 120,
        sea=100 if water else 0,
        waterline=waterline,
        model_top=0,
        movement_class=kind,
        expected_methods=declared,
    )
    # The script oracle's controlled GET profile mirrors the class-specific
    # state used by the existing animation-roster checks.
    cob_profile = 0 if can_fly else 2 if water else 1
    return mover_profile, cob_profile, kind, declared


def native_rows(unit, mover_profile):
    traces, methods = native_trace(mover_profile)
    if len(traces) != 2:
        raise AssertionError((unit, "native mover did not return two ticks", traces))
    declared = methods.intersection(CALLBACKS)
    script_names = [
        name.lower() for name in names_at(
            (SCRIPT_ROOT / f"{unit}.cob").read_bytes(),
            struct.unpack_from("<10I", (SCRIPT_ROOT / f"{unit}.cob").read_bytes())[7],
            struct.unpack_from("<10I", (SCRIPT_ROOT / f"{unit}.cob").read_bytes())[1],
        )
    ]
    events = []
    for tick, requests in enumerate(traces, 1):
        row = []
        for name, values in requests:
            # 0x4dc800 requests all three standard call-ins. The native COB
            # lookup starts only methods present in this unit's COB table.
            if name.lower() not in script_names:
                continue
            row.append((name, tuple(values)))
            script_index = script_names.index(name.lower())
            args = list(values)
            events.append((tick - 1, script_index, len(args),
                           args + [0] * (4 - len(args))))
        events_for_tick = tuple(row)
        yield tick, tuple(requests), events_for_tick, events
        events = []


def helper_parity(unit, requests, declared, helper_binary):
    first = requests[0]
    values = {name: args[0] for name, args in first}
    turn = values.get("TurnDirection", 0) if "TurnDirection" in declared else 0
    rate = values.get("MoveRate", 0)
    occupancy = values.get("setSFXoccupy", 0)
    rows = f"{turn} {rate} {occupancy}\n{turn} {rate} {occupancy}\n"
    result = subprocess.run([helper_binary, "--trace"], input=rows,
                            check=True, capture_output=True, text=True)
    expected_first = " ".join(
        f"{name}({args[0]})" for name, args in first
        if name != "TurnDirection" or "TurnDirection" in declared
    ) or "-"
    expected = [expected_first, "-"]
    actual = result.stdout.splitlines()
    if actual != expected:
        raise AssertionError({"unit": unit, "native request row": first,
                              "World helper": actual, "expected": expected})


def native_cob_trace(cob_path, profile, events, ticks=2):
    data = cob_path.read_bytes()
    header = struct.unpack_from("<10I", data)
    _, script_count, piece_count, code_words, static_count, _, index_off, \
        script_names_off, _, code_off = header
    script_names = names_at(data, script_names_off, script_count)
    script_index = {name.lower(): i for i, name in enumerate(script_names)}
    if "create" not in script_index:
        raise ValueError(f"{cob_path.name} has no Create")
    saved = bytes(0xA48 + static_count * 4 + piece_count * 0x6C)

    p = Icd()
    uc = p.uc
    vm, desc, code, entries, statics, pieces, vtable, scratch = (
        HEAP + offset for offset in (0, 0x10000, 0x20000, 0x30000,
                                    0x40000, 0x50000, 0x60000, 0x70000)
    )
    # Place bytecode and entry data past the restored-state and host regions.
    code, entries = HEAP + 0x100000, HEAP + 0x200000
    if code_words * 4 > 0x100000:
        raise ValueError("script exceeds native COB oracle code allocation")
    pose = [[0] * 6 for _ in range(piece_count)]
    writes = []
    cursor = 0

    def get(address):
        return u32(uc, address)

    def read(_uc, sp):
        nonlocal cursor
        destination, size = get(sp), get(sp + 4)
        part = saved[cursor:cursor + size]
        cursor += len(part)
        if part:
            uc.mem_write(destination, part)
        return 2, len(part)

    def seek(_uc, sp):
        nonlocal cursor
        cursor = get(sp)
        return 1, 0

    def write_pose(base):
        def call(_uc, sp):
            piece, axis, value = struct.unpack("<3I", uc.mem_read(sp, 12))
            if piece >= piece_count or axis >= 3:
                raise AssertionError(("native COB pose write", piece, axis))
            pose[piece][base + axis] = value
            return 3, 0
        return call

    def read_pose(base):
        def call(_uc, sp):
            piece, axis = struct.unpack("<2I", uc.mem_read(sp, 8))
            if piece >= piece_count or axis >= 3:
                raise AssertionError(("native COB pose read", piece, axis))
            return 2, pose[piece][base + axis]
        return call

    def set_value(_uc, sp):
        writes.extend(struct.unpack("<2i", uc.mem_read(sp, 8)))
        return 2, 0

    def query(_uc, sp):
        key = get(sp)
        mode = profile
        values = {4: 25 if mode == 3 else 100, 18: 1,
                  29: 100 if mode in (1, 2) else 0, 28: int(mode == 2),
                  34: int(mode == 1), 32: 10 if mode == 3 else 0,
                  33: 100 if mode == 1 else 0, 46: int(mode == 3)}
        return 5, values.get(key, 0)

    callbacks = {
        0: write_pose(0), 4: write_pose(3),
        8: lambda _uc, _sp: (2, 0), 12: lambda _uc, _sp: (2, 0),
        16: lambda _uc, _sp: (2, 0), 20: lambda _uc, _sp: (2, 0),
        24: read_pose(0), 28: read_pose(3),
        44: lambda _uc, _sp: (2, 0), 48: lambda _uc, _sp: (2, 0),
        52: lambda _uc, _sp: (2, 0), 56: lambda _uc, sp: (2, get(sp + 4)),
        80: set_value, 84: query,
    }
    for offset, callback in callbacks.items():
        address = 0x56A000 + offset * 4
        put(uc, vtable + offset, address)
        p.hooks[address] = callback
    p.hooks[0x5359A0] = lambda _uc, _sp: (0, len(saved))
    p.hooks[0x5359C0] = seek
    p.hooks[0x535A30] = read
    p.hooks[0x5BA3D0] = lambda _uc, _sp: (0, scratch)
    p.hooks[0x5BA5D0] = lambda _uc, _sp: (0, 0)
    p.freeze_hooks()

    put(uc, vm, vtable)
    put(uc, vm + 4, 30)
    put(uc, vm + 0x0C, desc)
    put(uc, vm + 0x10, struct.unpack_from("<I", saved)[0])
    put(uc, vm + 0x14, statics)
    put(uc, vm + 0x18, pieces)
    put(uc, desc + 0x2C, HEAP + 0x80000)
    put(uc, desc + 4, script_count)
    put(uc, desc + 8, piece_count)
    put(uc, desc + 0x10, static_count)
    put(uc, desc + 0x18, entries)
    put(uc, desc + 0x24, code)
    uc.mem_write(code, data[code_off:code_off + code_words * 4])
    uc.mem_write(entries, data[index_off:index_off + script_count * 4])

    result, error = p.call(0x56DC00, (HEAP + 0x90000,), ecx=vm)
    if error or result != 1:
        raise RuntimeError(("native COB initialization", result, error))
    put(uc, 0x64186C, 1)
    _, error = p.call(0x56C5F0, (script_index["create"], 0, 1), ecx=vm)
    if error:
        raise RuntimeError(("native Create", error))

    timeline = {}
    for tick, script, count, args in events:
        timeline.setdefault(tick, []).append((script, count, args))
    expected = []
    for tick in range(-1, ticks):
        writes.clear()
        if tick < 0:
            # Create was already started immediately above. Capture its
            # initialized state before the first movement callback update.
            pass
        else:
            for script, count, args in timeline.get(tick, ()):
                _, error = p.call(0x56C680,
                    (script, 0, 1, count, *args), ecx=vm)
                if error:
                    raise RuntimeError(("native COB callback", tick + 1,
                                        script_names[script], error))
            _, error = p.call(0x56C870, (1,), ecx=vm)
            if error:
                raise RuntimeError(("native COB tick", tick + 1, error))
        row = [get(vm + 0xA60), get(0x64186C),
               *[get(statics + index * 4) for index in range(static_count)],
               *struct.unpack("<656I", uc.mem_read(vm + 0x20, 16 * 0xA4))]
        for piece in range(piece_count):
            row += [*struct.unpack("<19I", uc.mem_read(pieces + piece * 76, 76)),
                    *pose[piece]]
        expected.append(row + [len(writes), *writes])
    return expected


def local_cob_trace(binary, cob_path, profile, events, ticks):
    data = cob_path.read_bytes()
    header = struct.unpack_from("<10I", data)
    script_count, piece_count, static_count = header[1], header[2], header[4]
    script_names = names_at(data, header[7], script_count)
    create_index = next(i for i, name in enumerate(script_names)
                        if name.lower() == "create")
    state = bytes(0xA48 + static_count * 4 + piece_count * 0x6C)
    with tempfile.TemporaryDirectory(prefix="tak-native-mover-cob-") as temporary:
        directory = Path(temporary)
        state_path = directory / f"{cob_path.stem}.state"
        event_path = directory / f"{cob_path.stem}.events"
        state_path.write_bytes(state)
        event_path.write_text("".join(
            " ".join(map(str, (tick, script, count, *args))) + "\n"
            for tick, script, count, args in events
        ))
        command = [binary, "--state-start", str(cob_path), str(state_path),
                   str(ticks), str(create_index), str(event_path)]
        env = os.environ.copy()
        env["TAK_SCRIPT_ORACLE_PROFILE"] = str(profile)
        result = subprocess.run(command, cwd=ROOT, env=env, check=True,
                                capture_output=True, text=True)
    return [list(map(int, line.split())) for line in result.stdout.splitlines()]


def run_unit(unit, binary, helper_binary):
    cob_path = SCRIPT_ROOT / f"{unit}.cob"
    if not cob_path.exists():
        raise FileNotFoundError(cob_path)
    mover_profile, cob_profile, kind, declared = profile_for(unit)
    collected = list(native_rows(unit, mover_profile))
    requests = [row[1] for row in collected]
    events = [event for row in collected for event in row[3]]
    helper_parity(unit, requests, declared, helper_binary)
    expected = native_cob_trace(cob_path, cob_profile, events)
    actual = local_cob_trace(binary, cob_path, cob_profile, events, ticks=2)
    if expected != actual:
        if len(expected) != len(actual):
            raise AssertionError((unit, "native/local COB row count", len(expected), len(actual)))
        for tick, (native, world) in enumerate(zip(expected, actual)):
            if native != world:
                field = next(i for i, pair in enumerate(zip(native, world))
                             if pair[0] != pair[1])
                raise AssertionError((unit, "native/local COB state", tick, field,
                                      native[field], world[field]))
    dispatches = [row[2] for row in collected]
    return kind, declared, requests, dispatches, len(expected)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build-o2/retail_script_test")
    parser.add_argument("--helper-binary", default="build-o2/retail_movement_animation_test")
    parser.add_argument("--units", nargs="+", default=DEFAULT_UNITS)
    args = parser.parse_args()
    for unit in args.units:
        kind, declared, requests, dispatches, rows = run_unit(
            unit.lower(), args.binary, args.helper_binary)
        family = ",".join(name for name in CALLBACKS if name in declared)
        print(f"PASS: {unit.upper()} {kind} [{family}]; native requests={requests}; "
              f"script-dispatched={dispatches}; native/World COB rows={rows}")
    print(f"PASS: {len(args.units)} native mover-to-COB representatives")


if __name__ == "__main__":
    main()
