#!/usr/bin/env python3
"""Trace shipped SET26/SET31 death callbacks and owner lifetimes in retail.

Retail's COB VM host, 0x51d3e0 unit update, 0x51e380 timer, 0x512ae0
retirement, and 0x4ee560/0x497380 owner/effect-list destructors execute from
KINGDOMS.icd. `--native-death-state` additionally enters 0x512610 and 0x512860
to construct and dispatch a real death state before following owner updates.

The fixture supplies synthetic game/player/unit/FBI/model/owner records,
the unit-id lookup table, and a one-node attached-SFX list. The position query
returns a fixed point; effect creation, model allocation, unrelated graphics
and global callbacks, and VM shutdown are controlled seams (the model allocator
returns null, and the SFX sink only records requests). The native death builder,
COB event routing/host, unit updater/timer/removal, and list destructors execute
from the retail binary. This is a lifecycle trace, not a rendered corpse test.
"""
import argparse
import json
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
    parser.add_argument("--script", default="tarmage")
    parser.add_argument("--world-binary", type=Path,
                        default=Path("build-o2/animation_roster_test"))
    parser.add_argument("--native-death-state", action="store_true",
                        help="build the death event through retail 0x512610/0x512860")
    parser.add_argument("--native-set26-roster", action="store_true",
                        help="start native Killed/Dying COB handlers and trace SET26 owner teardown")
    parser.add_argument("--get-unit-value-17", type=int, default=0,
                        help="override COB GET_UNIT_VALUE 17 for conditional SET26 branches")
    parser.add_argument("--death-type", type=int, default=1,
                        help="native death-state type (for --native-death-state; default: 1)")
    parser.add_argument("--instant-corpse", action="store_true", help="verify immediate bitmap-only building death")
    parser.add_argument("--corpse-request", action="store_true", help="verify corpse 1 after the authored owner lifetime")
    parser.add_argument("--get-unit-value-28", type=int, default=0,
                        help="override COB water-depth query")
    parser.add_argument("--warm-create", action="store_true",
                        help="execute Create and one VM update before starting death")
    parser.add_argument("--trace-json", action="store_true",
                        help="report native corpse decision and retirement without unit-specific assertions")
    parser.add_argument("--construction-remaining", type=float, default=0.0,
                        help="native unit construction fraction (zero means complete)")
    args = parser.parse_args()

    script = args.scripts / f"{args.script}.cob"
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

    def host_hook(slot, count):
        def invoke(uc, sp):
            raw = struct.unpack("<" + "I" * count, uc.mem_read(sp, count * 4))
            if slot == 0x54:
                value = (args.get_unit_value_17 if raw[0] == 17 else
                         args.get_unit_value_28 if raw[0] == 28 else
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
        return 3, 0

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
    corpse_requests=[]
    if args.instant_corpse or args.corpse_request or args.trace_json:
        def corpse_request(uc, sp):
            corpse_requests.append(struct.unpack("<5I",uc.mem_read(sp,20)))
            return 5,1
        # Keep the native retirement decision; map stamping is covered separately.
        p.hooks[0x512EE0]=corpse_request
        put(p,vtable+0x2C,0x50DD20)
        p.hooks[0x492C80]=lambda uc,sp:(3,0)
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
    if args.native_death_state:
        # 0x512610 creates a native death event containing the unit's numeric
        # id; 0x512860 resolves it through this inclusive retail unit table.
        # Put the sole fixture unit at id 1, leaving id 0 null as in retail.
        put(p, unit + 2, 1)
        put(p, game + 0x14E84, unit - 0x138)
        put(p, game + 0x14E88, unit)
        p.uc.mem_write(unit + 0x10C, struct.pack("<h", -1))
        p.uc.mem_write(unit + 0x111, b"\x64")
    f32(p, unit + 0x108, args.construction_remaining)
    put(p, owner + 0x17C, sfx_list)
    put(p, sfx_list, sfx_vtable)
    put(p, sfx_list + 8, sentinel)
    put(p, sfx_list + 12, 1)
    put(p, sfx_list + 16, 40)
    put(p, sentinel, node)
    put(p, sentinel + 4, node)
    put(p, node, sentinel)
    put(p, node + 4, sentinel)
    put(p, 0x640210, 0)
    f32(p, owner + 0x18, 0.0)
    f32(p, 0x5F03CC, 0.03)

    trace = []
    recent = []
    watched = {0x50D450, 0x56C870, 0x51E380, 0x51DE30, 0x512610, 0x512860,
               0x512AE0, 0x4EE560, 0x497380}
    def watch(uc, address, size, _):
        if address in watched:
            trace.append((tick[0], address))
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

    if args.warm_create and "create" in lower_names:
        result, error = start("Create", [])
        assert not error, ("Create", error)
        result, error = p.call(0x56C870, (1,), ecx=vm)
        assert not error, ("Create update", error)
        trace.clear(); writes.clear(); callback_dispatches.clear()
    if args.native_death_state:
        result, error = p.call(0x512610, (unit, args.death_type))
        if error:
            raise RuntimeError(("native death state 0x512610", error,
                                hex(p.uc.reg_read(UC_X86_REG_EIP)),
                                [hex(a) for a in recent[-40:]]))
    else:
        for name, values in (("Killed", [100, 0, 1]), ("Dying", [1])):
            if name not in names:
                continue
            result, error = start(name, values)
            assert not error and result == 1, (name, result, error)

    # cremomb's retail Dying callback intentionally runs a long countdown,
    # with 32-ms sleeps inside its decrement loop, before it writes SET26.
    # Allow that actual callback to finish, but keep the trace bounded
    # well below the separate 600-tick direct-VM sweep.
    delayed_set26_case = args.native_death_state and args.script.lower() == "crebomb"
    set26_owner_case = args.native_death_state and args.script.lower() in ("crefire", "crebomb")
    update_horizon = 1500 if args.native_set26_roster or args.corpse_request or args.trace_json else (256 if delayed_set26_case else 35)
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

    if args.trace_json:
        print(json.dumps({"script":args.script,"death_type":args.death_type,
            "construction_remaining":args.construction_remaining,"warm_create":args.warm_create,
            "corpse_requests":[list(row[1:]) for row in corpse_requests],
            "retirement_ticks":[t for t,a in trace if a==0x512AE0],
            "writes":writes,"callbacks":callback_dispatches,
            "owner_alive":bool(u32(p,unit+0xC0)),
            "owner_timer":f32(p,owner+0x18),
            "vm_update_ticks":[t for t,a in trace if a==0x56C870]}))
        return
    if args.corpse_request:
        assert args.native_death_state
        assert u32(p,unit+0xC0)==0,"owner did not retire within the bounded trace"
        assert len(corpse_requests)==1 and corpse_requests[0][1]==1,corpse_requests
        print(f"PASS: {args.script} death type {args.death_type}: native corpse 1, owner retired by tick {tick[0]}")
        return
    if args.instant_corpse:
        assert args.native_death_state and "Dying" not in names
        assert not writes,writes
        assert (0,0x512AE0) in trace,trace
        assert u32(p,unit+0xC0)==0
        assert len(corpse_requests)==1 and corpse_requests[0][1]==1,corpse_requests
        print(f"PASS: {args.script} death type {args.death_type}: immediate native retirement and corpse 1")
        return
    expected_native_write = (writes if args.native_set26_roster else
                             ([(146, 26, 1)] if delayed_set26_case else
                              [(0, 26 if set26_owner_case else 31, 1)]))
    if args.native_death_state and writes != expected_native_write:
        print("native-death diagnostics:", {
            "writes": writes, "trace": [(t, hex(a)) for t, a in trace],
            "native_sfx": native_sfx, "effects": events,
            "callback_names": callback_names,
            "callback_dispatches": callback_dispatches,
            "unit_state": hex(u32(p, unit + 0x130)), "owner_timer": f32(p, owner + 0x18),
            "recent": [hex(a) for a in recent[-40:]],
        })
    assert writes == expected_native_write, writes
    if args.native_set26_roster:
        # The direct native callback starts isolate each script's own Killed and
        # Dying bytecode. The real 0x51d3e0 owner updater, removal edge and list
        # destructors then run until the callback's SET26 write is consumed.
        assert "Dying" in names, names
        if not writes:
            print(f"NO SET26: native {args.script} Killed/Dying did not write a unit value "
                  f"within {len(snapshots)} owner updates")
            return
        assert len(writes) == 1 and writes[0][1:] == (26, 1), writes
        write_tick = writes[0][0]
        retirement_tick = write_tick + 1
        assert len(snapshots) == retirement_tick, (write_tick, retirement_tick, snapshots[-3:])
        assert snapshots[-1][0] == retirement_tick and snapshots[-1][4] == 0, snapshots[-1]
        assert snapshots[-1][5] == 0 and u32(p, unit + 0xC0) == 0, snapshots[-1]
        assert [(t, address) for t, address in trace
                if address in (0x512AE0, 0x4EE560, 0x497380)] == [
                    (retirement_tick, 0x512AE0),
                    (retirement_tick, 0x4EE560),
                    (retirement_tick, 0x497380)
                ], trace
        vm_updates = [(t, address) for t, address in trace if address == 0x56C870]
        assert not any(t > write_tick for t, _ in vm_updates), vm_updates
        print(f"PASS: native {args.script} Killed/Dying writes SET26 at tick {write_tick}; "
              f"0x51d3e0 retires owner/list at tick {retirement_tick}")
        return
    if set26_owner_case:
        write_tick = expected_native_write[0][0]
        retirement_tick = write_tick + 1
        if delayed_set26_case:
            # This unit's Dying script takes its ordinary long countdown path
            # (unit value 17 is clear), then requests removal at tick 146. No
            # damage flame is attached by this COB; the fixture's preexisting
            # owner-list node lets the native destructor path be observed.
            assert not native_sfx and not events, (native_sfx, events)
            assert len(snapshots) == retirement_tick, (len(snapshots), snapshots[-3:])
            assert [t for t, address in trace if address == 0x56C870] == list(range(0, retirement_tick)), trace
            assert [t for t, address in trace if address == 0x51E380] == list(range(1, retirement_tick)), trace
            assert all(snapshot[3] == sfx_list and snapshot[4] == sentinel and snapshot[5] == 1
                       and snapshot[7] == node and snapshot[8] == node
                       for snapshot in snapshots[:-1]), snapshots
        else:
            assert native_sfx == [(0, 16, 260)], native_sfx
            assert events == [(0, 0, (100, 200, 300), view)], events
            assert len(snapshots) == 1, snapshots
        assert snapshots[-1][0] == retirement_tick, snapshots[-1]
        assert snapshots[-1][4] == 0 and snapshots[-1][5] == 0, snapshots[-1]
        assert snapshots[-1][6] == sentinel and u32(p, unit + 0xC0) == 0, snapshots[-1]
        assert [(t, address) for t, address in trace
                if address in (0x512AE0, 0x4EE560, 0x497380)] == [
                    (retirement_tick, 0x512AE0),
                    (retirement_tick, 0x4EE560),
                    (retirement_tick, 0x497380)
                ], trace
        vm_updates = [(t, address) for t, address in trace if address == 0x56C870]
        if not delayed_set26_case:
            assert vm_updates == [(0, 0x56C870)], vm_updates
        assert [(address, name) for address, name, _ in callback_dispatches] == [
            ("0x56c720", "Killed"), ("0x56c640", "Dying")
        ], callback_dispatches
        if not delayed_set26_case:
            assert not [t for t, address in trace if address == 0x51E380], trace
            print("PASS: retail crefire death dispatcher writes SET26 with one attached flame at tick 0; "
                  "0x51d3e0 removes the unit and drains its native owner/list on tick 1")
        else:
            print("PASS: retail cremomb Killed/Dying runs the long Dying countdown, writes SET26 at tick 146, "
                  "then 0x51d3e0 removes the unit and drains its native owner/list on tick 147")
        return
    assert native_sfx and all(row[0] == 0 and row[2] == 260 for row in native_sfx), native_sfx
    assert len(events) == len(native_sfx), (native_sfx, events)
    # Retail's dispatcher passes the view/model object into the creation sink;
    # the separate attached SFX list is owned by the model record.
    assert all(point == (100, 200, 300) and effect_owner == view
               for _, _, point, effect_owner in events), events
    assert len(snapshots) == 35, len(snapshots)
    assert abs(snapshots[0][2] - 0.97) < 0.00001, snapshots[0]
    assert abs(snapshots[32][2] - 0.01) < 0.00001, snapshots[32]
    assert snapshots[33][1] & 0x20000000 and snapshots[33][2] == 0, snapshots[33]
    assert snapshots[34][4] == 0 and snapshots[34][5] == 0, snapshots[34]
    assert snapshots[34][6] == sentinel, snapshots[34]
    vm_updates = [(t, address) for t, address in trace if address == 0x56C870]
    if args.native_death_state:
        assert [(t, address) for t, address in trace if address in (0x512610, 0x512860)] == [
            (0, 0x512610), (0, 0x512860)
        ], trace
        assert vm_updates == [(0, 0x56C870)], vm_updates
        assert [(address, name) for address, name, _ in callback_dispatches] == [
            ("0x56c720", "Killed"), ("0x56c640", "Dying")
        ], callback_dispatches
    else:
        assert vm_updates == [(0, 0x56C870), (0, 0x56C870)], vm_updates
    retirement = [(t, address) for t, address in trace
                  if address in (0x512AE0, 0x4EE560, 0x497380)]
    assert retirement == [(35, 0x512AE0), (35, 0x4EE560), (35, 0x497380)], retirement
    timer_updates = [(t, address) for t, address in trace if address == 0x51E380]
    assert [t for t, _ in timer_updates] == list(range(1, 35)), timer_updates
    assert u32(p, unit + 0xC0) == 0

    world = subprocess.run(
        [str(args.world_binary), "--death-sfx-timeline", str(script), "100", "1"],
        text=True, capture_output=True, check=True,
    )
    render_host = subprocess.run(
        [str(args.world_binary), "--death-render-timeline", str(script), "100", "1"],
        text=True, capture_output=True, check=True,
    )
    world_sfx, world_writes = [], []
    for line in world.stdout.splitlines():
        fields = line.split()
        if fields[0] == "E":
            world_sfx.append(tuple(map(int, fields[1:])))
        elif fields[0] == "U":
            world_writes.append(tuple(map(int, fields[1:])))
    world_initial = [row for row in world_sfx if row[0] == 0]
    world_late = [row for row in world_sfx if row[0] > 0]
    assert native_sfx == world_initial, (native_sfx, world_initial)
    assert world_writes == [(0, 31, 1)], world_writes
    late_ticks = {frame: sum(row[0] == frame for row in world_late) for frame in (14, 28)}
    assert late_ticks == {14: 8, 28: 8} and len(world_late) == 16, (late_ticks, world_late)
    render_sfx, render_writes = [], []
    for line in render_host.stdout.splitlines():
        fields = line.split()
        if fields[0] == "E":
            render_sfx.append(tuple(map(int, fields[1:])))
        elif fields[0] == "U":
            render_writes.append(tuple(map(int, fields[1:])))
    assert render_sfx == world_initial, (render_sfx, world_initial)
    assert render_writes == [(0, 31, 1)], render_writes

    # SET26 is the sibling native owner edge: retail marks the unit for removal
    # on its next owner update. `crefire` writes it immediately, then its direct
    # COB timeline emits later attached-death effects; GameView's render host must
    # stop those later display-VM ticks just like SET31.
    set26_script = args.scripts / "crefire.cob"
    set26_direct = subprocess.run(
        [str(args.world_binary), "--death-sfx-timeline", str(set26_script)],
        text=True, capture_output=True, check=True,
    )
    set26_render = subprocess.run(
        [str(args.world_binary), "--death-render-timeline", str(set26_script)],
        text=True, capture_output=True, check=True,
    )

    def parse_timeline(output):
        effects, writes = [], []
        for line in output.splitlines():
            fields = line.split()
            if fields[0] == "E":
                effects.append(tuple(map(int, fields[1:])))
            elif fields[0] == "U":
                writes.append(tuple(map(int, fields[1:])))
        return effects, writes

    set26_direct_sfx, set26_direct_writes = parse_timeline(set26_direct.stdout)
    set26_render_sfx, set26_render_writes = parse_timeline(set26_render.stdout)
    assert (0, 26, 1) in set26_direct_writes, set26_direct_writes
    assert any(row[0] > 0 for row in set26_direct_sfx), set26_direct_sfx
    assert set26_render_writes == [(0, 26, 1)], set26_render_writes
    assert set26_render_sfx == [row for row in set26_direct_sfx if row[0] == 0], (
        set26_render_sfx, set26_direct_sfx
    )

    callback_summary = (f"retail death builder ({args.death_type}) dispatches {callback_dispatches}"
                        if args.native_death_state else "manual Killed and Dying starts")
    print(f"PASS: native {args.script} SET31 at tick 0 via {callback_summary}; real 0x56c870 runs "
          f"{len(vm_updates)} time(s) at tick 0, then real 0x51e380 decrements to 0.97 at tick 1 and "
          "expires on tick 34; 0x51d3e0 consumes removal on tick 35; native owner/list destructors "
          "clear the attached node (0x502da0 creation sink controlled)")
    print(f"PASS: render-host timeline records SET31 and stops future VM ticks after immediate death "
          f"callbacks ({len(render_sfx)} tick-0 callbacks); direct-VM timeline continues to emit "
          f"{len(world_late)} later callbacks at ticks 14 and 28")
    print(f"PASS: render-host SET26 timeline for {set26_script.name} keeps only its tick-0 effects "
          f"while the direct-VM timeline emits {sum(row[0] > 0 for row in set26_direct_sfx)} later callbacks")


if __name__ == "__main__":
    main()
