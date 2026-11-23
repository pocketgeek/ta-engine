#!/usr/bin/env python3
"""Trace shipped Tarhel SFX 260/261 from native death callbacks through owner removal.

The headless KINGDOMS.icd/Unicorn trace builds the death state through native
0x512610/0x512860, dispatches Tarhel's real Killed/Dying COB handlers and SFX
callbacks, then advances native 0x51d3e0/0x51e380 to the owner teardown. The
final 0x502da0 SFX creation sink is controlled: it inserts test list nodes for
each callback so retail's real 0x497380 destructor must unlink those exact
entries. World death/render timelines are compared for callback and SET31
stop parity; the ordinary direct-VM timeline is also checked for late events.

Fixture limits: game/player/unit/FBI/model/view records, queried XYZ, list-node
allocation/insertion, model allocation, unrelated graphics callbacks, and COB
shutdown are synthetic or controlled. Retail constructs the death event, runs
COB dispatch/VM/SFX routing, advances the unit timer/removal path, and destroys
the owner/list. The test does not execute the SFX object allocator, compare
rendered pixels, or launch the retail GUI.
"""
import argparse
from pathlib import Path
import subprocess
import struct

from emu import HEAP, Icd
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EIP, UC_X86_REG_ESP


def put(p, address, value):
    p.uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))


def u32(p, address):
    return struct.unpack("<I", p.uc.mem_read(address, 4))[0]


def f32(p, address, value=None):
    if value is not None:
        p.uc.mem_write(address, struct.pack("<f", value))
    return struct.unpack("<f", p.uc.mem_read(address, 4))[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scripts", type=Path, default=Path("assets/extracted/all/scripts"))
    parser.add_argument("--world-binary", type=Path,
                        default=Path("build-o2/animation_roster_test"))
    parser.add_argument("--death-type", type=int, default=1,
                        help="native death-state type (default: 1)")
    args = parser.parse_args()

    script = args.scripts / "tarhel.cob"
    data = script.read_bytes()
    header = struct.unpack_from("<10I", data)
    _, script_count, piece_count, code_words, static_count, _, index_offset, \
        name_offset, _, code_offset = header

    p = Icd()
    vm, desc, statics, pieces, vtable, scratch, view, unit, unit_type, owner = (
        HEAP + n * 0x10000 for n in range(10)
    )
    code, entries = HEAP + 0x100000, HEAP + 0x200000
    game, game_manager, game_vtable = HEAP + 0xD0000, HEAP + 0xE0000, HEAP + 0xE1000
    global_manager, global_vtable = HEAP + 0xE2000, HEAP + 0xE3000
    sfx_list, sentinel, node = HEAP + 0xF0000, HEAP + 0xF1000, HEAP + 0xF2000
    sfx_vtable = HEAP + 0xF3000
    events, native_sfx, writes, callback_names, callback_dispatches, tick = [], [], [], [], [], [0]
    attached_nodes = []
    next_effect_node = [HEAP + 0xF4000]

    def host_hook(slot, count):
        def invoke(uc, sp):
            raw = struct.unpack("<" + "I" * count, uc.mem_read(sp, count * 4))
            if slot == 0x54:
                value = (0 if raw[0] == 17 else
                         100 if raw[0] == 4 else 1 if raw[0] == 18 else 0)
                return count, value
            if slot == 0x38:
                return count, raw[1]
            return count, 0
        return invoke

    slot_args = {
        0x00: 3, 0x04: 3, 0x08: 2, 0x0C: 2, 0x10: 2, 0x14: 2,
        0x18: 2, 0x1C: 2, 0x2C: 2, 0x30: 2, 0x34: 2, 0x38: 2,
        0x54: 5,
    }
    for slot, count in slot_args.items():
        address = 0x56A000 + slot * 16
        put(p, vtable + slot, address)
        p.hooks[address] = host_hook(slot, count)
    # Unlike the generic death timeline probe, unit-value calls use retail's
    # real vtable target. Extended EMIT_SFX also remains in retail code.
    put(p, vtable + 0x30, 0x50DA20)
    put(p, vtable + 0x50, 0x50D450)
    # Retirement invokes the VM object's +0x60 shutdown method. The script
    # interpreter state is already dead at that point; this is the sole VM
    # object-lifetime seam, not the SET/callback/update path under test.
    put(p, vtable + 0x60, 0x56A130)

    p.hooks[0x4F7210] = lambda uc, sp: (2, 1)
    p.hooks[0x4EE620] = lambda uc, sp: (1, 0)
    p.hooks[0x56A130] = lambda uc, sp: (1, 0)

    def position(uc, sp):
        output, _, piece = struct.unpack("<3I", uc.mem_read(sp, 12))
        uc.mem_write(output, struct.pack("<3i", 100, 200, 300))
        return 3, output

    def create_flame(uc, sp):
        point, kind, effect_owner = struct.unpack("<3I", uc.mem_read(sp, 12))
        coordinates = struct.unpack("<3i", uc.mem_read(point, 12))
        events.append((tick[0], kind, coordinates, effect_owner))
        # Bridge only the final creation-sink boundary: model the native
        # attached-list insertion for this emitted effect, then let retail's
        # owner/list destructor drain these callback-created list entries.
        view_ptr = effect_owner
        owning_unit = u32(p, view_ptr + 0x0C)
        owning_model = u32(p, owning_unit + 0xC0)
        manager = u32(p, owning_model + 0x17C)
        sentinel_ptr = u32(p, manager + 8)
        new_node = next_effect_node[0]
        next_effect_node[0] += 0x100
        old_tail = u32(p, sentinel_ptr + 4)
        old_head = u32(p, sentinel_ptr)
        put(p, new_node, sentinel_ptr)
        put(p, new_node + 4, old_tail)
        put(p, old_tail, new_node)
        put(p, sentinel_ptr + 4, new_node)
        if old_head == sentinel_ptr:
            put(p, sentinel_ptr, new_node)
        put(p, manager + 12, u32(p, manager + 12) + 1)
        attached_nodes.append(new_node)
        return 3, new_node

    p.hooks[0x4DD250] = position
    p.hooks[0x502DA0] = create_flame
    p.hooks[0x5359A0] = lambda uc, sp: (0, len(data))
    p.hooks[0x5359C0] = lambda uc, sp: (1, 0)
    p.hooks[0x535A30] = lambda uc, sp: (2, 0)
    p.hooks[0x5BA3D0] = lambda uc, sp: (0, scratch)
    p.hooks[0x5BA5D0] = lambda uc, sp: (0, 0)
    # Model animation allocation is outside the death timer/SFX owner path.
    p.hooks[0x544430] = lambda uc, sp: (1, 0)
    # Unrelated external side effects touched by native unit retirement.
    for address, argc in (
        (0x52AE30, 2), (0x4C6750, 2), (0x4C6800, 2), (0x523510, 1),
        (0x50AA20, 1), (0x5066A0, 1), (0x4C7190, 0), (0x4DC7F0, 0),
        (0x4EBA00, 1), (0x4EBAA0, 0),
    ):
        p.hooks[address] = (lambda count: lambda uc, sp: (count, 0))(argc)
    # 0x51de30 and 0x51e380 call the model-owned effect-list update slot.
    # It is a graphics update seam; the list's actual native destructor stays
    # installed at vtable slot 0.
    put(p, sfx_vtable, 0x497380)
    put(p, sfx_vtable + 0x0C, 0x56A100)
    p.hooks[0x56A100] = lambda uc, sp: (0, 0)
    # Game manager update callback unrelated to the death timer.
    put(p, 0x62DA3C, game_manager)
    put(p, game_manager, game_vtable)
    put(p, game_vtable + 0x1C, 0x56A110)
    p.hooks[0x56A110] = lambda uc, sp: (1, 0)
    put(p, game_vtable + 0x18, 0x56A120)
    p.hooks[0x56A120] = lambda uc, sp: (1, 0)
    put(p, 0x62D558, global_manager)
    put(p, global_manager, global_vtable)
    p.freeze_hooks()

    put(p, 0x62D55C, game)
    put(p, 0x64186C, 1)
    put(p, vm, vtable)
    put(p, vm + 4, 30)
    put(p, vm + 0x0C, desc)
    put(p, vm + 0x10, struct.unpack_from("<I", data)[0])
    put(p, vm + 0x14, statics)
    put(p, vm + 0x18, pieces)
    put(p, desc + 0x2C, HEAP + 0xA0000)
    put(p, desc + 4, script_count)
    put(p, desc + 8, piece_count)
    put(p, desc + 0x10, static_count)
    put(p, desc + 0x18, entries)
    put(p, desc + 0x24, code)
    p.uc.mem_write(code, data[code_offset:code_offset + code_words * 4])
    p.uc.mem_write(entries, data[index_offset:index_offset + script_count * 4])
    result, error = p.call(0x56DC00, (HEAP + 0xC0000,), ecx=vm)
    assert not error and result == 0, ("init", result, error)

    names = []
    name_table, name_text = HEAP + 0x210000, HEAP + 0x220000
    name_cursor = 0
    for i in range(script_count):
        offset = struct.unpack_from("<I", data, name_offset + i * 4)[0]
        raw_name = data[offset:].split(b"\0", 1)[0]
        names.append(raw_name.decode("ascii"))
        put(p, name_table + i * 4, name_text + name_cursor)
        p.uc.mem_write(name_text + name_cursor, raw_name + b"\0")
        name_cursor += len(raw_name) + 1
    # 0x56c4a0 resolves the callback names passed by native 0x512610 and
    # 0x512860 through descriptor +0x1c. Direct index starts above do not use it.
    put(p, desc + 0x1C, name_table)
    lower_names = [name.lower() for name in names]

    def start(name, values):
        index = lower_names.index(name.lower())
        return p.call(0x56C680,
                      (index, 0, 1, len(values), *(values + [0] * (4 - len(values)))),
                      ecx=vm)

    put(p, vm + 0xA64, view)
    put(p, view + 0x0C, unit)
    put(p, unit + 0xBC, vm)
    put(p, unit + 0xB8, unit_type)
    put(p, unit + 0xB4, unit_type)
    put(p, unit + 0xC0, owner)
    put(p, unit + 0x130, 0x11001000)  # active + death-update/timer branch
    put(p, unit + 0x68, 100)
    put(p, unit + 0x6C, 200)
    put(p, unit + 0x70, 300)
    f32(p, unit + 0xF4, 1.0)  # Avoid unrelated zero-rate locomotion division.
    put(p, unit_type, 1)
    put(p, unit_type + 0x1BE, 1)  # Native per-unit rate divisor.
    put(p, unit_type + 0x260, 0x800)
    # One synthetic player owns this inclusive one-unit range. The player type
    # and state bytes gate the native per-player unit loops in 0x51d3e0.
    put(p, game + 0x2404, HEAP + 0x350000)
    p.uc.mem_write(game + 0x24EE, b"\x01\x00")
    put(p, game + 0x2478, unit)
    put(p, game + 0x247C, unit)
    put(p, game + 0x2510, HEAP + 0x360000)
    # 0x512610 creates a native death event containing the unit's numeric
    # id; 0x512860 resolves it through this inclusive retail unit table.
    # Put the sole fixture unit at id 1, leaving id 0 null as in retail.
    put(p, unit + 2, 1)
    put(p, game + 0x14E84, unit - 0x138)
    put(p, game + 0x14E88, unit)
    p.uc.mem_write(unit + 0x10C, struct.pack("<h", -1))
    p.uc.mem_write(unit + 0x111, b"\x64")
    put(p, owner + 0x17C, sfx_list)
    put(p, sfx_list, sfx_vtable)
    put(p, sfx_list + 8, sentinel)
    put(p, sfx_list + 12, 0)
    put(p, sfx_list + 16, 40)
    put(p, sentinel, sentinel)
    put(p, sentinel + 4, sentinel)
    put(p, 0x640210, 0)
    f32(p, owner + 0x18, 0.0)
    f32(p, 0x5F03CC, 0.03)

    trace = []
    list_unlinks = []
    recent = []
    watched = {0x50D450, 0x56C870, 0x51E380, 0x51DE30, 0x512610, 0x512860,
               0x512AE0, 0x4EE560, 0x497380, 0x497400}
    def watch(uc, address, size, _):
        if address in watched:
            trace.append((tick[0], address))
        if address == 0x497400:
            esp = uc.reg_read(UC_X86_REG_ESP)
            list_unlinks.append((tick[0], u32(p, esp + 4), u32(p, esp + 8)))
        if address in (0x56C720, 0x56C640):
            esp = uc.reg_read(UC_X86_REG_ESP)
            name_ptr = u32(p, esp + 4)
            callback_dispatches.append((hex(address),
                                        bytes(uc.mem_read(name_ptr, 32)).split(b"\0", 1)[0].decode(),
                                        hex(u32(p, esp))))
        if address == 0x56C4A0:
            esp = uc.reg_read(UC_X86_REG_ESP)
            name_ptr = u32(p, esp + 4)
            callback_names.append(bytes(uc.mem_read(name_ptr, 32)).split(b"\0", 1)[0].decode())
        if address == 0x50DA20:
            esp = uc.reg_read(UC_X86_REG_ESP)
            piece, code = struct.unpack("<2I", uc.mem_read(esp + 4, 8))
            if 260 <= code <= 262:
                native_sfx.append((tick[0], piece, code))
        if address == 0x50D450:
            esp = uc.reg_read(UC_X86_REG_ESP)
            writes.append((tick[0], *struct.unpack("<2i", uc.mem_read(esp + 4, 8))))
    p.uc.hook_add(UC_HOOK_CODE, watch)
    p.uc.hook_add(UC_HOOK_CODE, lambda uc, address, size, _: recent.append(address)
                  if 0x401000 <= address < 0x5EA000 else None)

    result, error = p.call(0x512610, (unit, args.death_type))
    if error:
        raise RuntimeError(("native death state 0x512610", error,
                            hex(p.uc.reg_read(UC_X86_REG_EIP)),
                            [hex(a) for a in recent[-40:]]))

    update_horizon = 35
    snapshots = []
    for frame in range(1, update_horizon + 1):
        tick[0] = frame
        put(p, game + 0x19F44, frame)
        # The callback runs at tick 0; each outer unit update advances the
        # timer, then the next update consumes its removal bit.
        result, error = p.call(0x51D3E0)
        if error:
            raise RuntimeError(("native 0x51d3e0", frame, error,
                                hex(p.uc.reg_read(UC_X86_REG_EIP)),
                                [hex(a) for a in recent[-25:]]))
        snapshot = (frame, u32(p, unit + 0x130), f32(p, owner + 0x18),
                    u32(p, owner + 0x17C), u32(p, sfx_list + 8),
                    u32(p, sfx_list + 12), u32(p, 0x640210),
                    u32(p, sentinel), u32(p, sentinel + 4))
        snapshots.append(snapshot)
        if u32(p, unit + 0xC0) == 0:
            break

    assert writes == [(0, 31, 1)], writes
    assert native_sfx == [
        (0, 21, 261), (0, 0, 261), (0, 15, 261), (0, 13, 260), (0, 9, 260)
    ], native_sfx
    assert events == [
        (0, 1, (100, 200, 300), view),
        (0, 1, (100, 200, 300), view),
        (0, 1, (100, 200, 300), view),
        (0, 0, (100, 200, 300), view),
        (0, 0, (100, 200, 300), view),
    ], events
    assert len(attached_nodes) == 5 and len(snapshots) == 35, (attached_nodes, snapshots)
    assert abs(snapshots[0][2] - 0.97) < 0.00001, snapshots[0]
    assert abs(snapshots[32][2] - 0.01) < 0.00001, snapshots[32]
    assert snapshots[33][1] & 0x20000000 and snapshots[33][2] == 0, snapshots[33]
    assert snapshots[34][4] == 0 and snapshots[34][5] == 0, snapshots[34]
    assert snapshots[34][6] == sentinel and u32(p, unit + 0xC0) == 0, snapshots[34]
    assert [entry[2] for entry in list_unlinks] == attached_nodes, (list_unlinks, attached_nodes)
    assert [(t, address) for t, address in trace if address in (0x512610, 0x512860)] == [
        (0, 0x512610), (0, 0x512860)
    ], trace
    assert [(t, address) for t, address in trace
            if address in (0x512AE0, 0x4EE560, 0x497380)] == [
        (35, 0x512AE0), (35, 0x4EE560), (35, 0x497380)
    ], trace
    assert [t for t, address in trace if address == 0x51E380] == list(range(1, 35)), trace
    assert [(t, address) for t, address in trace if address == 0x56C870] == [
        (0, 0x56C870)
    ], trace
    assert [(address, name) for address, name, _ in callback_dispatches] == [
        ("0x56c720", "Killed"), ("0x56c640", "Dying")
    ], callback_dispatches

    world = subprocess.run(
        [str(args.world_binary), "--death-sfx-timeline", str(script), "100", "1"],
        text=True, capture_output=True, check=True,
    )
    render_host = subprocess.run(
        [str(args.world_binary), "--death-render-timeline", str(script), "100", "1"],
        text=True, capture_output=True, check=True,
    )
    def parse_timeline(output):
        effects, unit_values = [], []
        for line in output.splitlines():
            fields = line.split()
            if fields[0] == "E":
                effects.append(tuple(map(int, fields[1:])))
            elif fields[0] == "U":
                unit_values.append(tuple(map(int, fields[1:])))
            else:
                raise AssertionError(("unexpected timeline row", line))
        return effects, unit_values

    world_sfx, world_writes = parse_timeline(world.stdout)
    render_sfx, render_writes = parse_timeline(render_host.stdout)
    world_initial = [row for row in world_sfx if row[0] == 0]
    world_late = [row for row in world_sfx if row[0] > 0]
    assert native_sfx == world_initial, (native_sfx, world_initial)
    assert world_writes == [(0, 31, 1)], world_writes
    assert render_sfx == world_initial and render_writes == [(0, 31, 1)], (
        render_sfx, render_writes, world_initial
    )
    late_ticks = {frame: sum(row[0] == frame for row in world_late)
                  for frame in (18, 36, 54, 72)}
    assert late_ticks == {18: 5, 36: 5, 54: 5, 72: 5} and len(world_late) == 20, (
        late_ticks, world_late
    )

    retirement = [(t, address) for t, address in trace
                  if address in (0x512AE0, 0x4EE560, 0x497380)]
    print(f"PASS: retail Tarhel death builder dispatches Killed/Dying at tick 0; "
          f"attached SFX callbacks {native_sfx} match World render-host tick 0")
    print(f"PASS: SET31 at tick 0; native timer expires on update 34 and owner/list "
          f"retirement runs at update 35: {[(t, hex(a)) for t, a in retirement]}")
    print(f"PASS: controlled 0x502da0 sink linked {len(attached_nodes)} test nodes and "
          f"native 0x497400 unlinked the same nodes at tick 35")
    print(f"PASS: World render-host suppresses later callbacks; direct VM's 20 later "
          f"events occur at ticks {sorted(late_ticks)}")
    print("LIMIT: synthetic XYZ/model/unit records, list-node insertion and SFX creation "
          "sink; no native SFX allocator, live pixels, or retail GUI")

if __name__ == "__main__":
    main()
