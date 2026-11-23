#!/usr/bin/env python3
"""Compare native and World VM death-script damage-flame emission timelines.

This is a headless KINGDOMS.icd/Unicorn trace. It starts Killed then Dying on a
fresh/reset VM, observes the native VM's SFX host calls, and compares them with
the same reset-separated renderer VM used by World. It does not drive the outer
retail unit death/update dispatcher or claim its exact model-destructor tick.
"""
import argparse
from pathlib import Path
import struct
import subprocess
import sys

from emu import HEAP, Icd
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP


def u32(uc, address):
    return struct.unpack("<I", uc.mem_read(address, 4))[0]


def set_u32(uc, address, value):
    uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))


def run_native(script, severity, damage_type):
    data = script.read_bytes()
    h = struct.unpack_from("<10I", data)
    _, script_count, piece_count, code_words, static_count, _, index_offset, \
        name_offset, _, code_offset = h

    p = Icd()
    vm, desc, code, entries, statics, pieces, vtable, scratch, view, unit = (
        HEAP + n * 0x10000 for n in range(10)
    )
    code = HEAP + 0x100000
    entries = HEAP + 0x200000
    game = HEAP + 0xD0000
    events = []
    created = []
    current_piece = [-1]
    writes = []
    tick = 0

    def get(address):
        return u32(p.uc, address)

    def hook(slot, count):
        def invoke(uc, sp):
            args = struct.unpack("<" + "I" * count, uc.mem_read(sp, count * 4))
            if slot == 0x50:
                writes.append((tick, *struct.unpack("<2i", uc.mem_read(sp, 8))))
            if slot == 0x54:
                query = args[0]
                return count, (100 if query == 4 else 1 if query == 18 else 0)
            if slot == 0x38:
                return count, args[1]
            return count, 0
        return invoke

    # Match the native VM host slots. Route extended EMIT_SFX through retail's
    # actual 50da20 dispatcher; only visibility/model refresh and the final
    # damage-flame creation boundary are controlled below.
    slot_args = {
        0x00: 3, 0x04: 3, 0x08: 2, 0x0C: 2, 0x10: 2, 0x14: 2,
        0x18: 2, 0x1C: 2, 0x2C: 2, 0x30: 2, 0x34: 2, 0x38: 2,
        0x50: 2, 0x54: 5,
    }
    for slot, count in slot_args.items():
        address = 0x56A000 + slot * 16
        set_u32(p.uc, vtable + slot, address)
        if slot != 0x30:
            p.hooks[address] = hook(slot, count)

    p.hooks[0x4F7210] = lambda uc, sp: (2, 1)
    p.hooks[0x4EE620] = lambda uc, sp: (1, 0)

    def position(uc, sp):
        output, owner, piece = struct.unpack("<3I", uc.mem_read(sp, 12))
        current_piece[0] = piece
        uc.mem_write(output, struct.pack("<3i", 100, 200, 300))
        return 3, output

    def create_damage_flame(uc, sp):
        point, kind, owner = struct.unpack("<3I", uc.mem_read(sp, 12))
        coordinates = struct.unpack("<3i", uc.mem_read(point, 12))
        created.append((tick, current_piece[0], kind, coordinates, owner))
        return 3, 0

    p.hooks[0x4DD250] = position
    p.hooks[0x502DA0] = create_damage_flame

    p.hooks[0x5359A0] = lambda uc, sp: (0, len(data))
    p.hooks[0x5359C0] = lambda uc, sp: (1, 0)
    p.hooks[0x535A30] = lambda uc, sp: (2, 0)
    p.hooks[0x5BA3D0] = lambda uc, sp: (0, scratch)
    p.hooks[0x5BA5D0] = lambda uc, sp: (0, 0)
    p.freeze_hooks()

    set_u32(p.uc, 0x62D55C, game)
    set_u32(p.uc, vtable + 0x30, 0x50DA20)

    def dispatch_entry(uc, address, size, user_data):
        esp = uc.reg_read(UC_X86_REG_ESP)
        piece, sfx = struct.unpack("<2I", uc.mem_read(esp + 4, 8))
        if 260 <= sfx <= 262:
            current_piece[0] = piece
            events.append((tick, piece, sfx))

    p.uc.hook_add(UC_HOOK_CODE, dispatch_entry, begin=0x50DA20, end=0x50DA20)
    set_u32(p.uc, vm, vtable)
    set_u32(p.uc, vm + 4, 30)
    set_u32(p.uc, vm + 0x0C, desc)
    set_u32(p.uc, vm + 0x10, struct.unpack_from("<I", data)[0])
    set_u32(p.uc, vm + 0x14, statics)
    set_u32(p.uc, vm + 0x18, pieces)
    set_u32(p.uc, desc + 0x2C, HEAP + 0xA0000)
    set_u32(p.uc, desc + 4, script_count)
    set_u32(p.uc, desc + 8, piece_count)
    set_u32(p.uc, desc + 0x10, static_count)
    set_u32(p.uc, desc + 0x18, entries)
    set_u32(p.uc, desc + 0x24, code)
    p.uc.mem_write(code, data[code_offset:code_offset + code_words * 4])
    p.uc.mem_write(entries, data[index_offset:index_offset + script_count * 4])
    result, error = p.call(0x56DC00, (HEAP + 0xC0000,), ecx=vm)
    assert not error and result == 0, (script.name, "initialize", result, error)
    set_u32(p.uc, vm + 0xA64, view)
    set_u32(p.uc, view + 0x0C, unit)
    set_u32(p.uc, unit + 0x68, 100)
    set_u32(p.uc, unit + 0x6C, 200)
    set_u32(p.uc, unit + 0x70, 300)
    set_u32(p.uc, 0x64186C, 1)

    names = []
    for i in range(script_count):
        offset = struct.unpack_from("<I", data, name_offset + i * 4)[0]
        names.append(data[offset:].split(b"\0", 1)[0].decode("ascii"))
    lower_names = [name.lower() for name in names]

    def start(name, args):
        try:
            script_index = lower_names.index(name.lower())
        except ValueError:
            return False
        result, error = p.call(
            0x56C680,
            (script_index, 0, 1, len(args), *(args + [0] * (4 - len(args)))),
            ecx=vm,
        )
        assert not error and result == 1, (script.name, name, result, error)
        return True

    assert start("Killed", [severity, 0, damage_type]), script.name
    if not start("Dying", [damage_type]):
        start("death", [])
    for update in range(600):
        tick = update + 1
        result, error = p.call(0x56C870, (1,), ecx=vm)
        assert not error, (script.name, "update", tick, error)
    return events, writes, created


def run_world(binary, script, severity, damage_type):
    result = subprocess.run(
        [str(binary), "--death-sfx-timeline", str(script), str(severity), str(damage_type)],
        text=True, capture_output=True, check=True,
    )
    events = []
    writes = []
    for line in result.stdout.splitlines():
        fields = line.split()
        if fields[0] == "E":
            _, tick, piece, code = fields
            events.append((int(tick), int(piece), int(code)))
        elif fields[0] == "U":
            _, tick, unit_value, value = fields
            writes.append((int(tick), int(unit_value), int(value)))
        else:
            raise AssertionError((script.name, "unexpected World timeline row", line))
    return events, writes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scripts", type=Path, default=Path("assets/extracted/all/scripts"))
    parser.add_argument("--world-binary", type=Path, default=Path("build-o2/animation_roster_test"))
    args = parser.parse_args()

    cases = (("tarmage", 260), ("tarhel", 261), ("crefire", 262))
    compared = 0
    latest = 0
    for name, expected_code in cases:
        script = args.scripts / f"{name}.cob"
        if not script.is_file():
            raise FileNotFoundError(script)
        for severity in (1, 100, 1000):
            for damage_type in (0, 1):
                native, writes, created = run_native(script, severity, damage_type)
                world, world_writes = run_world(args.world_binary, script, severity, damage_type)
                native_rows = native
                if native_rows != world:
                    raise AssertionError((name, severity, damage_type, native_rows, world))
                if not native_rows or expected_code not in {code for _, _, code in native_rows}:
                    raise AssertionError((name, severity, damage_type, "no attached damage-flame emission"))
                # The direct-VM timeline is bounded by the 120-tick corpse
                # animation window. This is not the attached owner's actual
                # SET26/SET31 retirement deadline, which has a separate probe.
                if max(tick for tick, _, _ in native_rows) >= 120:
                    raise AssertionError((name, "death SFX extends beyond World handoff", native_rows))
                native_writes = [(tick, value_id, value) for tick, value_id, value in writes]
                if native_writes != world_writes:
                    raise AssertionError((name, severity, damage_type,
                                          "native/World unit-value timeline differs",
                                          native_writes, world_writes))
                created_rows = [(at, piece, kind + 260) for at, piece, kind, _, _ in created]
                if created_rows != native_rows:
                    raise AssertionError((name, severity, damage_type,
                                          "native dispatcher and attached-flame creation differ",
                                          native_rows, created_rows))
                if any(owner != HEAP + 0x80000 or point != (100, 200, 300)
                       for _, _, _, point, owner in created):
                    raise AssertionError((name, "native controlled creation sink lost owner/position", created))
                latest = max(latest, max(tick for tick, _, _ in native_rows))
                compared += len(native_rows)

    # This existing native destructor probe runs the actual owner-model teardown
    # and proves that its attached SFX list is synchronously drained.
    subprocess.run([sys.executable, str(Path(__file__).with_name("probe_attached_sfx_teardown.py"))],
                   check=True)
    print(f"PASS: {compared} native/World death damage-flame callbacks and unit-value writes "
          f"match across tarmage/tarhel/crefire and 6 severity/type inputs; latest event tick {latest} "
          "falls inside the 120-tick direct-VM corpse window")
    print("LIMIT: this direct-VM callback comparison does not model the live SET26/SET31 "
          "owner-retirement deadline or rendered attached-effect lifetime")


if __name__ == "__main__":
    main()
