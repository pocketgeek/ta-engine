#!/usr/bin/env python3
"""Run mover requests into retail's live native COB name dispatcher.

The fixture attaches a real retail COB VM at the unit's native script link
before calling 0x4dc800. That lets retail's 0x56c640 resolve names and start
the declared COB methods in the same emulation process. It exercises movement,
reversal, stop/resume, and air/hover occupancy changes without launching the
retail GUI. A matching World VM replay is written only after the native trace
has produced its callback schedule.
"""
import argparse
import os
import struct
import subprocess
import tempfile
from pathlib import Path

from emu import HEAP
from emuphase import ARENA, GS, Phase
from check_movement_callback_order import (
    CALLBACKS, SCRIPT_ROOT, cob_methods, fbi_info,
    profile_type_fields,
)
from check_native_mover_cob_join import names_at, u32
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_ESP


ROOT = Path(__file__).resolve().parents[2]


def put(uc, address, value):
    uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))


def s32(uc, address):
    return struct.unpack("<i", uc.mem_read(address, 4))[0]


def native_vm(p, cob_path, profile, saved=None):
    """Initialize retail's COB VM on the same Unicorn machine as the mover."""
    data = cob_path.read_bytes()
    header = struct.unpack_from("<10I", data)
    _, script_count, piece_count, code_words, static_count, _, entry_off, \
        script_names_off, _, code_off = header
    script_names = names_at(data, script_names_off, script_count)
    script_index = {name.lower(): i for i, name in enumerate(script_names)}
    saved = saved or bytes(0xA48 + static_count * 4 + piece_count * 0x6C)

    uc = p.uc
    vm, desc, _unused_code, _unused_entries, statics, pieces, vtable, scratch = (
        HEAP + offset for offset in (0, 0x10000, 0x20000, 0x30000,
                                     0x40000, 0x50000, 0x60000, 0x70000)
    )
    code, entries = HEAP + 0x100000, HEAP + 0x200000
    names_ptrs, names_text = HEAP + 0x90000, HEAP + 0x91000
    sounds = HEAP + 0x80000
    if code_words * 4 > 0x100000:
        raise ValueError(f"{cob_path.name} COB bytecode exceeds oracle allocation")

    cursor = 0
    pose = [[0] * 6 for _ in range(piece_count)]
    writes = []

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
                raise AssertionError(("COB pose setter", piece, axis))
            pose[piece][base + axis] = value
            return 3, 0
        return call

    def read_pose(base):
        def call(_uc, sp):
            piece, axis = struct.unpack("<2I", uc.mem_read(sp, 8))
            if piece >= piece_count or axis >= 3:
                raise AssertionError(("COB pose getter", piece, axis))
            return 2, pose[piece][base + axis]
        return call

    def set_value(_uc, sp):
        writes.extend(struct.unpack("<2i", uc.mem_read(sp, 8)))
        return 2, 0

    # These GET values match the controlled retail host used by the existing
    # native-vs-World COB trace. The scripts themselves remain the shipped COBs.
    def query(_uc, sp):
        key = get(sp)
        return 5, {4: 100, 18: 1, 29: 100 if profile in (1, 2) else 0,
                   28: int(profile == 2), 34: int(profile == 1),
                   32: 0, 33: 100 if profile == 1 else 0,
                   46: 0}.get(key, 0)

    callbacks = {
        0: write_pose(0), 4: write_pose(3),
        8: lambda _uc, _sp: (2, 0), 12: lambda _uc, _sp: (2, 0),
        16: lambda _uc, _sp: (2, 0), 20: lambda _uc, _sp: (2, 0),
        24: read_pose(0), 28: read_pose(3),
        44: lambda _uc, _sp: (2, 0), 48: lambda _uc, _sp: (2, 0),
        52: lambda _uc, _sp: (2, 0),
        56: lambda _uc, sp: (2, get(sp + 4)), 80: set_value, 84: query,
    }
    put(uc, vm, vtable)
    put(uc, vm + 4, 30)
    put(uc, vm + 0x0C, desc)
    put(uc, vm + 0x10, struct.unpack_from("<I", saved)[0])
    put(uc, vm + 0x14, statics)
    put(uc, vm + 0x18, pieces)
    put(uc, desc + 4, script_count)
    put(uc, desc + 8, piece_count)
    put(uc, desc + 0x10, static_count)
    put(uc, desc + 0x18, entries)
    put(uc, desc + 0x24, code)
    put(uc, desc + 0x2C, sounds)
    uc.mem_write(code, data[code_off:code_off + code_words * 4])
    uc.mem_write(entries, data[entry_off:entry_off + script_count * 4])

    raw_names, cursor_names = [], 0
    for name in script_names:
        address = names_text + cursor_names
        raw = name.encode("ascii") + b"\0"
        uc.mem_write(address, raw)
        raw_names.append(address)
        cursor_names += len(raw)
    uc.mem_write(names_ptrs,
                 b"".join(struct.pack("<I", ptr) for ptr in raw_names))

    for offset, callback in callbacks.items():
        address = 0x56A000 + offset * 4
        put(uc, vtable + offset, address)
        p.hooks[address] = callback
    p.hooks.update({
        0x5359A0: lambda _uc, _sp: (0, len(saved)),
        0x5359C0: seek,
        0x535A30: read,
        0x5BA3D0: lambda _uc, _sp: (0, scratch),
        0x5BA5D0: lambda _uc, _sp: (0, 0),
    })

    # Phase normally fixes retail rand(n) to zero for path-search experiments.
    # COB Create uses the actual shared random state, so leave that retail
    # routine live in this joined animation fixture.
    p.hooks.pop(0x535CC0, None)
    p.freeze_hooks()
    result, error = p.call(0x56DC00, (sounds,), ecx=vm)
    if error or result != 1:
        raise RuntimeError(("native COB initialization", result, error))
    put(uc, 0x64186C, 1)
    if "create" in script_index:
        _, error = p.call(0x56C5F0, (script_index["create"], 0, 1), ecx=vm)
        if error:
            raise RuntimeError(("native COB Create", cob_path.name, error))
    # The native mover resolves method names later through this table. Keep it
    # absent during the same integer-script startup boundary used by the
    # standalone native oracle, then install it before the first mover tick.
    put(uc, desc + 0x1C, names_ptrs)
    return {
        "vm": vm, "desc": desc, "statics": statics, "pieces": pieces,
        "script_index": script_index, "script_names": script_names,
        "static_count": static_count, "piece_count": piece_count,
        "pose": pose, "writes": writes,
    }


def native_transition_trace(unit_name, sequence):
    unit_name = unit_name.lower()
    cob_path = SCRIPT_ROOT / f"{unit_name}.cob"
    info = fbi_info(unit_name)
    methods = cob_methods(unit_name)
    declared = methods.intersection(CALLBACKS)
    if not declared:
        raise ValueError(f"{unit_name} declares no movement callbacks")
    can_fly = bool(float(info.get("canfly", "0")))
    water = info.get("movementclass", "").upper().startswith("WATER")
    hover = info.get("movementclass", "").upper().startswith("HOVER")
    kind = "flying" if can_fly else "water" if water else "hover" if hover else "ground"
    waterline = int(float(info.get("waterline", 0)))
    profile = dict(unit=unit_name, mode=2 if can_fly else 1,
                   y=100 if can_fly else 100 - waterline if water else 120,
                   sea=100 if water else 0, waterline=waterline,
                   model_top=0, movement_class=kind, expected_methods=declared)

    p = Phase(64, 64)
    uc = p.uc
    unit = p.unit(10, 10)
    mover, navigator = unit + 0x300, ARENA + 0x0D10000
    settings, options = ARENA + 0x0D21000, ARENA + 0x0D20000
    put(uc, 0x62D55C, GS)
    put(uc, 0x62D558, settings)
    put(uc, settings + 8, options)
    uc.mem_write(options, bytes(0x100))
    uc.mem_write(GS + 0x19EF8, bytes((profile["sea"],)))
    sector_stride = 8
    sectors = p._alloc(sector_stride * sector_stride * 10)
    put(uc, GS + 0x19F18, sectors)
    put(uc, GS + 0x19F1C, sector_stride)
    put(uc, GS + 0x19F30, 1)
    for index in range(sector_stride * sector_stride):
        uc.mem_write(sectors + index * 10 + 1, b"\x64")
    current_sector = sectors + ((10 * 16 >> 7) * sector_stride +
                                (10 * 16 >> 7)) * 10

    start = (160, profile["y"], 160)
    put(uc, unit + 0xA4, current_sector)
    put(uc, current_sector + 6, unit)
    uc.mem_write(unit + 0x68, struct.pack("<3i", *(value << 16 for value in start)))
    uc.mem_write(unit + 0x7E, struct.pack("<H", 0))
    max_velocity = int(float(info.get("maxvelocity", 1.25)) * 65536)
    put(uc, unit + 0x12B, max_velocity)
    put(uc, unit + 0x130, 0x01000000)
    put(uc, mover, navigator)
    put(uc, mover + 0x20, max_velocity)
    put(uc, mover + 0x30, 0x7FFFFFFF)
    uc.mem_write(mover + 0x36, struct.pack("<H", profile["mode"]))
    put(uc, navigator, 0x5F34D4)
    put(uc, navigator + 8, unit)
    uc.mem_write(navigator + 0x0C,
                 struct.pack("<3i", *(value << 16 for value in start)))
    uc.mem_write(navigator + 0x24, struct.pack("<H", 0))
    profile_type_fields(uc, info, profile)

    controller, mission, point = (p._alloc(size) for size in (0x100, 0x100, 12))
    put(uc, mission + 0x0E, unit)
    uc.mem_write(point, struct.pack(
        "<3i", *(value << 16 for value in sequence[0]["target"])))
    _, error = p.icd.call(0x4E40E0, (mission, point), ecx=controller)
    if error:
        raise RuntimeError(("native point controller", error))
    _, error = p.icd.call(0x4E4540, (16,), ecx=controller)
    if error:
        raise RuntimeError(("native controller radius", error))
    put(uc, navigator + 4, controller)

    # Match the standalone native COB oracle's host boundary. Its game pointer
    # is null while Create is restored/started; the mover gets the populated
    # GameState as soon as its own update begins.
    put(uc, 0x62D55C, 0)
    cob_profile = 0 if can_fly else 2 if water else 1
    cob = native_vm(p.icd, cob_path, cob_profile)
    put(uc, 0x62D55C, GS)
    put(uc, unit + 0xBC, cob["vm"])
    calls = []

    def observe(uc_, address, size, _):
        if address != 0x56C640:
            return
        esp = uc_.reg_read(UC_X86_REG_ESP)
        args = struct.unpack("<8I", uc_.mem_read(esp + 4, 32))
        name = bytes(uc_.mem_read(args[0], 64)).split(b"\0", 1)[0].decode("ascii")
        count = args[3]
        values = tuple(struct.unpack("<i", struct.pack("<I", value))[0]
                       for value in args[4:4 + count])
        calls.append((name, values, uc_.reg_read(UC_X86_REG_ECX)))

    from unicorn import UC_HOOK_CODE
    uc.hook_add(UC_HOOK_CODE, observe, begin=0x56C640, end=0x56C640)
    script_events = []
    def snapshot():
        row = [u32(uc, cob["vm"] + 0xA60), u32(uc, 0x64186C),
               *[u32(uc, cob["statics"] + n * 4)
                 for n in range(cob["static_count"])],
               *struct.unpack("<656I", uc.mem_read(cob["vm"] + 0x20, 16 * 0xA4))]
        for piece in range(cob["piece_count"]):
            row += [*struct.unpack("<19I", uc.mem_read(cob["pieces"] + piece * 76, 76)),
                    *cob["pose"][piece]]
        row += [len(cob["writes"]), *cob["writes"]]
        return row

    snapshots = [snapshot()]
    body_angles = [struct.unpack("<3H", uc.mem_read(unit + 0x7C, 6))]
    for tick, step in enumerate(sequence, 1):
        target = step["target"]
        # The point controller copied the original point into +0x26; this is
        # retail's stored target, so edits here model an accepted retarget.
        uc.mem_write(controller + 0x26,
                     struct.pack("<3i", *(value << 16 for value in target)))
        if "mode" in step:
            uc.mem_write(mover + 0x36, struct.pack("<H", step["mode"]))
        if "heading" in step:
            uc.mem_write(unit + 0x7E, struct.pack("<H", step["heading"] & 0xFFFF))
        if "speed" in step:
            put(uc, unit + 0x12B, step["speed"])
            put(uc, mover + 0x20, step["speed"])
        if "state_mode" in step:
            flags = u32(uc, unit + 0x130)
            put(uc, unit + 0x130, (flags & ~3) | (step["state_mode"] & 3))
        if "water_y" in step:
            uc.mem_write(unit + 0x68, struct.pack("<i", step["water_y"] << 16))
        put(uc, GS + 0x19F44, tick)
        calls.clear()
        _, error = p.icd.call(0x4DC800, (unit,), ecx=mover)
        if error:
            raise RuntimeError((unit_name, tick, "native mover", error))
        events = [(name, values) for name, values, this in calls]
        if any(this != cob["vm"] for _, _, this in calls):
            raise AssertionError((unit_name, tick, "mover did not dispatch through unit COB VM", calls))
        script_events.extend((tick - 1, name, values) for name, values in events)
        _, error = p.icd.call(0x56C870, (1,), ecx=cob["vm"])
        if error:
            raise RuntimeError((unit_name, tick, "COB tick", error))

        snapshots.append(snapshot())
        body_angles.append(struct.unpack("<3H", uc.mem_read(unit + 0x7C, 6)))

    return {
        "unit": unit_name, "kind": kind, "declared": declared,
        "events": script_events, "snapshots": snapshots,
        "body_angles": body_angles,
        "accepted_events": [event for event in script_events
                            if event[1].lower() in cob["script_index"]],
    }


def local_trace(binary, unit, cob, profile, events, ticks):
    data = cob.read_bytes()
    header = struct.unpack_from("<10I", data)
    script_count, piece_count, static_count = header[1], header[2], header[4]
    names = names_at(data, header[7], script_count)
    script_index = {name.lower(): i for i, name in enumerate(names)}
    state = bytes(0xA48 + static_count * 4 + piece_count * 0x6C)
    with tempfile.TemporaryDirectory(prefix="tak-native-cob-transitions-") as temporary:
        root = Path(temporary)
        state_path = root / f"{unit}.state"
        event_path = root / f"{unit}.events"
        state_path.write_bytes(state)
        event_path.write_text("".join(
            f"{tick} {script_index[name.lower()]} {len(values)} " +
            " ".join(map(str, (*values, *([0] * (4 - len(values)))))) + "\n"
            for tick, name, values in events if name.lower() in script_index
        ))
        command = [binary, "--state-start", str(cob), str(state_path),
                   str(ticks), str(script_index.get("create", 0)), str(event_path)]
        env = os.environ.copy()
        env["TAK_SCRIPT_ORACLE_PROFILE"] = str(profile)
        result = subprocess.run(command, cwd=ROOT, env=env, check=True,
                                capture_output=True, text=True)
    return [list(map(int, line.split())) for line in result.stdout.splitlines()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--script-binary", default="build-o2/retail_script_test")
    parser.add_argument("--units", nargs="+", default=["tarblack", "tarlich", "tarcship", "aradrag"])
    args = parser.parse_args()

    # Each trace includes an unchanged update, a forced reversal, a stationary
    # target with zero speed, and a restart. Flight additionally changes the
    # native mover state once so its occupancy/flight edge is observable.
    for name in args.units:
        info = fbi_info(name.lower())
        can_fly = bool(float(info.get("canfly", "0")))
        water = info.get("movementclass", "").upper().startswith("WATER")
        hover = info.get("movementclass", "").upper().startswith("HOVER")
        kind = "flying" if can_fly else "water" if water else "hover" if hover else "ground"
        max_speed = int(float(info.get("maxvelocity", 1.25)) * 65536)
        y = 100 if kind == "flying" else 100 - int(float(info.get("waterline", 0))) if kind == "water" else 120
        sequence = [
            {"target": (800, y, 800)},
            {"target": (800, y, 800)},
            {"target": (-800, y, 800), "heading": 32768},
            {"target": (-800, y, 800), "heading": 32768},
            {"target": (160, y, 160), "speed": 0},
            {"target": (160, y, 160), "speed": 0},
            {"target": (800, y, -800), "speed": max_speed, "heading": 0},
        ]
        if kind == "flying":
            sequence.extend([
                {"target": (800, y, -800), "mode": 1, "state_mode": 1},
                {"target": (800, y, -800), "mode": 2, "state_mode": 2},
            ])
        elif kind == "hover":
            sequence.extend([
                {"target": (800, y, -800), "state_mode": 1},
                {"target": (800, y, -800), "state_mode": 2},
            ])
        result = native_transition_trace(name, sequence)
        native = result["snapshots"]
        # The native direct dispatch records only movement call-ins whose COB
        # method exists. Also compare complete VM rows to the local port with
        # the exact same accepted call-in schedule.
        cob_path = SCRIPT_ROOT / f"{name.lower()}.cob"
        profile = 2 if kind == "water" else 0 if kind == "flying" else 1
        actual = local_trace(args.script_binary, name.lower(), cob_path,
                             profile, result["events"], len(sequence))
        if actual != native:
            if len(actual) != len(native):
                raise AssertionError((name, "native/local COB row count", len(native), len(actual)))
            for tick, (want, got) in enumerate(zip(native, actual), 1):
                if want != got:
                    field = next(i for i, pair in enumerate(zip(want, got))
                                 if pair[0] != pair[1])
                    raise AssertionError((name, tick, field, want[field], got[field]))
        requests = ", ".join(
            f"t{tick + 1} {method}({','.join(map(str, values))})"
            for tick, method, values in result["events"]
        )
        accepted = ", ".join(dict.fromkeys(
            method for _, method, _ in result["accepted_events"])) or "none"
        print(f"PASS: {name.upper()} {kind}: native requests [{requests}]; "
              f"COB methods started [{accepted}]; {len(native)} complete VM boundaries match World")


if __name__ == "__main__":
    main()
