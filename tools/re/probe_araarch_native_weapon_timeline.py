#!/usr/bin/env python3
"""Run KINGDOMS' common weapon dispatcher with Araarch's shipped COB.

The actual 52ae90/52fe30/52fff0/530140/530220/52be80/56c640/56c870 path
executes. Araarch's COB bytecode is loaded into the native script VM, including
its real AimWeapon/FireWeapon/attack1 callbacks. The fixture supplies the live
target reference, muzzle/aim geometry, unit class details, projectile
allocation, display transport, map visibility, and SET_UNIT_VALUE host bridge.
It therefore tests callback and launch behavior for a live assigned target, not
full target acquisition, mover turning, final rendered pixels, or collision.
"""
import argparse
import re
import struct
import subprocess
from pathlib import Path

from emu import HEAP, Icd
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_ESP, UC_X86_REG_FPCW


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ROOT = ROOT / "assets/game"
SCRIPT = ROOT / "assets/extracted/all/scripts/araarch.cob"


def get_u32(uc, address):
    return struct.unpack("<I", uc.mem_read(address, 4))[0]


def get_u16(uc, address):
    return struct.unpack("<H", uc.mem_read(address, 2))[0]


def put_u32(uc, address, value):
    uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))


def put_u16(uc, address, value):
    uc.mem_write(address, struct.pack("<H", value & 0xFFFF))


def fixed(uc, address, value):
    uc.mem_write(address, struct.pack("<i", round(value * 65536)))


def unit_range(retail_root, hpitool):
    text = subprocess.run(
        [str(hpitool), "cat", str(retail_root / "data.hpi"), "units/araarch.fbi"],
        check=True, capture_output=True, text=True,
    ).stdout
    section = re.search(r"\[WEAPON1\]\s*\{(.*?)\n\s*\}", text, re.S | re.I)
    if not section:
        raise ValueError("could not find Araarch WEAPON1 in its shipped FBI")
    match = re.search(r"\brange\s*=\s*(\d+)\s*;", section.group(1), re.I)
    if not match:
        raise ValueError("could not parse Araarch WEAPON1 range")
    return int(match.group(1))


def parse_script(data):
    header = struct.unpack_from("<10I", data)
    _, script_count, piece_count, code_words, static_count, _, entry_off, _, _, code_off = header
    names = []
    for i in range(script_count):
        name_off = struct.unpack_from("<I", data, header[7] + 4 * i)[0]
        names.append(data[name_off:].split(b"\0", 1)[0])
    return header, names, piece_count, static_count, code_words, entry_off, code_off


def run(retail_root, hpitool, trace_steps, static):
    weapon_range = unit_range(retail_root, hpitool)
    data = SCRIPT.read_bytes()
    header, script_names, piece_count, static_count, code_words, entry_off, code_off = parse_script(data)
    if b"AimWeapon" not in script_names or b"FireWeapon" not in script_names:
        raise ValueError("Araarch COB does not contain the expected combat callbacks")

    p = Icd()
    game, units, kind, weapon_type, weapon_object, visibility = [
        HEAP + offset for offset in (0x10000, 0x20000, 0x30000, 0x40000, 0x50000, 0x60000)
    ]
    shooter, target = units + 312, units + 624
    record = shooter + 12
    projectile = HEAP + 0x70000
    weapon_vtable = HEAP + 0x71000
    arrow_model = HEAP + 0x72000
    unit_art = HEAP + 0x73000

    # The native COB VM and its unit-script descriptor live away from the
    # per-unit structures above and the native emulation stack.
    vm, desc, statics, pieces, vtable, scratch = [
        HEAP + offset for offset in (0x80000, 0x90000, 0xA0000, 0xB0000, 0xC0000, 0xD0000)
    ]
    code, entries = HEAP + 0x100000, HEAP + 0x200000
    name_table, name_strings, sound_table = HEAP + 0x300000, HEAP + 0x301000, HEAP + 0x380000

    uc = p.uc
    put = lambda address, value: put_u32(uc, address, value)
    short = lambda address, value: put_u16(uc, address, value)
    word = lambda address: get_u16(uc, address)

    pose = [[0] * 6 for _ in range(piece_count)]
    value_events = []
    callback_starts = []
    projectiles = []
    current_scenario = {"visible": True, "blocked": False, "distance": 300}
    if static:
        current_scenario["visible"] = False

    def write_pose(base):
        def callback(_uc, sp):
            piece = get_u32(uc, sp)
            axis = get_u32(uc, sp + 4)
            value = get_u32(uc, sp + 8)
            if piece < len(pose) and axis < 3:
                pose[piece][base + axis] = value
            return 3, 0
        return callback

    def read_pose(base):
        def callback(_uc, sp):
            piece = get_u32(uc, sp)
            axis = get_u32(uc, sp + 4)
            if piece >= len(pose) or axis >= 3:
                return 2, 0
            return 2, pose[piece][base + axis]
        return callback

    def set_unit_value(_uc, sp):
        unit_value, value = struct.unpack("<2i", uc.mem_read(sp, 8))
        value_events.append((unit_value, value))
        # Production's host bridge maps SET 22/23 onto the selected weapon
        # record's ready/release bits. Value is the weapon slot (Araarch's 0).
        if value == 0 and unit_value == 21:
            short(record + 0x1A, word(record + 0x1A) & 0xFF07)
        elif value == 0 and unit_value == 22:
            short(record + 0x1A, word(record + 0x1A) | 0x08)
        elif value == 0 and unit_value == 23:
            short(record + 0x1A, word(record + 0x1A) | 0x10)
        return 2, 0

    def query_unit_value(_uc, sp):
        query = get_u32(uc, sp)
        # Araarch combat callbacks do not depend on movement queries; these are
        # the normal idle host values for incidental Create threads.
        values = {4: 100, 18: 1, 28: 0, 29: 0, 32: 0, 33: 0, 34: 0, 46: 0}
        return 5, values.get(query, 0)

    def script_geometry(_uc, _sp):
        # The dispatcher asks the unit script for aim geometry before deciding
        # whether to invoke its weapon callbacks. Keep that retail native gate
        # and callbacks, while making the controlled fixture's aligned target
        # require no authored piece/model lookup.
        short(record + 0x16, 0x8000)
        short(record + 0x18, 0xFC00)
        return 2, 0

    def target_point(_uc, sp):
        source, output, slot = struct.unpack("<3I", uc.mem_read(sp, 12))
        if source != shooter or slot != 0:
            raise AssertionError(("native Araarch target-point args", source, slot))
        uc.mem_write(output, bytes(12))
        return 3, 1

    def allocate_projectile(_uc, _sp):
        return 0, projectile

    def source_muzzle(_uc, sp):
        args = struct.unpack("<4I", uc.mem_read(sp, 16))
        if args[0] != shooter or args[1] != projectile + 4 or args[2] & 3 != 0 or args[3] != 0xFFFFFFFF:
            raise AssertionError(("Araarch QueryWeapon muzzle arguments", args))
        # Match World's shipped Araarch QueryWeapon model point for the aligned
        # fixture (x=225, y=141.25, z=200).
        uc.mem_write(args[1], struct.pack("<3i", 225 * 65536, 141 * 65536 + 16384,
                                          200 * 65536))
        return 4, 0

    callbacks = {
        0: write_pose(0), 4: write_pose(3), 8: lambda _uc, _sp: (2, 0),
        12: lambda _uc, _sp: (2, 0), 16: lambda _uc, _sp: (2, 0),
        20: lambda _uc, _sp: (2, 0), 24: read_pose(0), 28: read_pose(3),
        44: lambda _uc, _sp: (2, 0), 48: lambda _uc, _sp: (2, 0),
        52: lambda _uc, _sp: (2, 0), 56: lambda _uc, sp: (2, get_u32(uc, sp + 4)),
        80: set_unit_value, 84: query_unit_value,
    }
    put(vm, vtable)
    for offset, callback in callbacks.items():
        address = 0x56A000 + offset * 4
        put(vtable + offset, address)
        p.hooks[address] = callback
    p.hooks.update({
        0x52FD80: script_geometry,
        0x51AA50: target_point,
        0x529A90: allocate_projectile,
        0x4DD420: source_muzzle,
        0x56A108: lambda _uc, _sp: (0, 0),
        0x4EA4F0: lambda _uc, _sp: (4, 0),
        0x4EA560: lambda _uc, _sp: (2, 0),
        0x5D4444: lambda _uc, _sp: (0, 0),
        0x5359A0: lambda _uc, _sp: (0, 0),
        0x5359C0: lambda _uc, _sp: (1, 0),
        0x535A30: lambda _uc, _sp: (2, 0),
        0x5BA3D0: lambda _uc, _sp: (0, scratch),
        0x5BA5D0: lambda _uc, _sp: (0, 0),
        0x56D850: lambda _uc, _sp: (1, 0),
    })
    p.freeze_hooks()
    # Parse/link the actual unit COB into the retail VM's descriptor shape.
    put(vm + 4, 30)
    put(vm + 0x0C, desc)
    put(vm + 0x14, statics)
    put(vm + 0x18, pieces)
    put(desc + 4, header[1])
    put(desc + 8, piece_count)
    put(desc + 0x10, static_count)
    put(desc + 0x18, entries)
    put(desc + 0x24, code)
    put(desc + 0x2C, sound_table)
    uc.mem_write(code, data[code_off:code_off + code_words * 4])
    uc.mem_write(entries, data[entry_off:entry_off + header[1] * 4])
    name_pointers = []
    cursor = 0
    for name in script_names:
        address = name_strings + cursor
        name_pointers.append(address)
        uc.mem_write(address, name + b"\0")
        cursor += len(name) + 1
    uc.mem_write(name_table, b"".join(struct.pack("<I", item) for item in name_pointers))
    put(desc + 0x1C, name_table)
    uc.reg_write(UC_X86_REG_FPCW, 0x027F)
    result, error = p.call(0x56DC00, (sound_table,), ecx=vm)
    if error:
        raise RuntimeError(f"native Araarch COB VM init: {error}")
    # Install one live native unit-table target and one real-range Araarch
    # ballistic weapon. Position and target assignment mirror the World's
    # explicit attack fixture; only LOS/visibility are varied below.
    put(0x62D55C, game)
    put(game + 0x14E84, units)
    put(game + 0x14E88, units + 4 * 312)
    put(game + 0x19EF4, visibility)
    uc.mem_write(visibility, struct.pack("<H", 0xFFFF if current_scenario["visible"] else 0) * 2048)
    put(shooter + 0xB4, kind)
    put(shooter + 0xBC, vm)
    uc.mem_write(shooter + 0xF4, struct.pack("<f", 1.0))
    uc.mem_write(shooter + 0xD8, struct.pack("<f", 10.0))  # enough unit mana for Araarch's FBI cost
    put(kind + 0x260, 0x10000)
    put(shooter + 0x130, 0)
    put(record, weapon_type)
    put(weapon_type + 0x40, weapon_object)
    put(weapon_object, weapon_vtable)
    put(weapon_vtable + 0x0C, 0x530580)
    put(weapon_vtable + 0x14, 0x56A108)
    put(weapon_vtable + 0x2C, 0x52BE80)
    put(weapon_type + 0x90, weapon_range)
    put(weapon_type + 0x94, 0)
    put(weapon_type + 0xC8, 0)
    raw_speed = int(750 * 65536 / 30)
    substeps = max(1, (raw_speed + 0xFFFFF) >> 20)
    put(weapon_type + 0xCC, raw_speed // substeps)
    put(weapon_type + 0xD0, substeps)
    put(weapon_type + 0x48, arrow_model)
    put(kind + 0x8A, unit_art)
    put(weapon_type + 0x9C, 90)  # FBI reloadtime=3s at 30Hz
    uc.mem_write(weapon_type + 0xD4, struct.pack("<f", 1.0))
    put(target + 0x130, 0x1000000)
    uc.mem_write(record + 4, struct.pack("<2H", 2, 0x8000))
    fixed(uc, shooter + 0x68, 200)
    fixed(uc, shooter + 0x70, 200)
    fixed(uc, target + 0x68, 200 + current_scenario["distance"])
    fixed(uc, target + 0x70, 200)
    put_u16(uc, shooter + 0x7E, 0xC000)  # native heading for World port heading 90 degrees
    put_u16(uc, target + 0x02, 2)
    uc.mem_write(target + 0xFD, b"\x01")

    # Trace actual native name-based dispatch arguments from the call boundary.
    name_by_pointer = dict(zip(name_pointers, script_names))
    last_addresses = []
    def trace_dispatch(uc, address, _size, _user):
        last_addresses.append(address)
        if len(last_addresses) > 128:
            del last_addresses[:64]
        if address == 0x52BF81:
            shot = (struct.unpack("<3i", uc.mem_read(projectile + 4, 12)),
                    struct.unpack("<3i", uc.mem_read(projectile + 0x1C, 12)),
                    struct.unpack("<3H", uc.mem_read(projectile + 0x34, 6)),
                    get_u32(uc, projectile + 0x93))
            projectiles.append((int(current_scenario["distance"]), word(record + 0x14),
                                word(record + 0x1A), bool(current_scenario["visible"]),
                                bool(current_scenario["blocked"]), shot))
        if address != 0x56C640:
            return
        sp = uc.reg_read(UC_X86_REG_ESP)
        args = struct.unpack("<8I", uc.mem_read(sp + 4, 32))
        name = name_by_pointer.get(args[0], bytes(uc.mem_read(args[0], 48)).split(b"\0", 1)[0])
        callback_starts.append((get_u32(uc, game + 0x19F44), name.decode(errors="replace"),
                                args[3], args[4:4 + min(args[3], 4)],
                                uc.reg_read(UC_X86_REG_ECX)))
    p.uc.hook_add(UC_HOOK_CODE, trace_dispatch)

    # Start Araarch's own Create script; subsequent unit updates queue native
    # AimWeapon/FireWeapon threads and the ordinary VM pass runs them.
    create_index = script_names.index(b"Create")
    _, error = p.call(0x56C5F0, (create_index, 0, 1), ecx=vm)
    if error:
        raise RuntimeError(f"native Araarch Create: {error}")

    phases = {} if static else {0: (300, True, False), 25: (451, True, False),
                                50: (450, False, True), 100: (450, True, False)}
    for tick in range(1, trace_steps + 1):
        if tick - 1 in phases:
            distance, visible, blocked = phases[tick - 1]
            current_scenario.update(distance=distance, visible=visible, blocked=blocked)
            fixed(uc, target + 0x68, 200 + distance)
            # Native 16-bit visibility cells: visible masks become zero while
            # the assigned unit reference remains alive and intact.
            uc.mem_write(visibility, struct.pack("<H", 0xFFFF if visible else 0) * 2048)
        put(game + 0x19F44, tick)
        put(0x64186C, tick)
        value_events.clear()
        last_addresses.clear()
        _, error = p.call(0x56C870, (1,), ecx=vm)
        if error:
            raise RuntimeError(f"native Araarch VM tick {tick}: {error}; "
                               f"path={[hex(a) for a in last_addresses[-30:]]}")
        last_addresses.clear()
        _, error = p.call(0x52AE90, (shooter,))
        if error:
            ecx = uc.reg_read(UC_X86_REG_ECX)
            descriptor = get_u32(uc, vm + 0x0C)
            raise RuntimeError(f"native Araarch weapon update {tick}: {error}; "
                               f"ecx={ecx:#x} vm={vm:#x} vm.desc={descriptor:#x} "
                               f"dispatch={callback_starts[-3:]} path={[hex(a) for a in last_addresses[-30:]]}")
        # Include callback output and authored piece state around each change.
        if tick <= 5 or tick in (24, 25, 26, 49, 50, 51, 99, 100, 101) or value_events:
            changed = {i: row[:] for i, row in enumerate(pose) if any(row)}
            print("TRACE", tick, "dist", current_scenario["distance"],
                  "visible", int(current_scenario["visible"]), "blocker", int(current_scenario["blocked"]),
                  "callbacks", callback_starts[-2:], "sets", value_events,
                  "reload", word(record + 0x14), "aimflags", hex(word(record + 0x1A)),
                  "shots", len(projectiles),
                  "lastshot", projectiles[-1][-1] if projectiles else None,
                  "posepieces", len(changed))
    print(f"NATIVE: araarch FBI range={weapon_range}; {len(callback_starts)} native combat callbacks; "
          f"{len(projectiles)} projectile releases; {len(value_events)} SET_UNIT_VALUE writes on final step")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--retail-root", type=Path, default=DEFAULT_ROOT)
    parser.add_argument("--hpitool", type=Path, default=ROOT / "build/hpitool")
    parser.add_argument("--steps", type=int, default=150)
    parser.add_argument("--static", action="store_true", help="keep the live target at the initial in-range, hidden-by-fog position")
    args = parser.parse_args()
    run(args.retail_root.resolve(), args.hpitool.resolve(), args.steps, args.static)


if __name__ == "__main__":
    main()
