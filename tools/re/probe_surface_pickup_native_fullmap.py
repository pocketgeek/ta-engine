#!/usr/bin/env python3
"""Continue a native Lake Lokken sea pickup over full TNT grades and real IDs.

Unlike the corridor probe, this grades all 480x480 TNT cells with retail's
0x508cd0 and advances the real Vertrans mover on those map records. Its Araarch
and Vertrans records live in the native game's global contiguous 0x138-byte
entity pool, so 0x51b4f0 -> 0x51b5a0 resolves their IDs through the real lookup.
Only mover-side game services, player feedback, and transfer effects remain
controlled boundaries. No retail GUI is launched.

Run from the repository root:
    PYTHONPATH=tools/re python3 tools/re/probe_surface_pickup_native_fullmap.py
"""
import argparse
import struct
from pathlib import Path

from emuphase import GS
from probe_surface_pickup_native_map_route import (
    CARRIER, PASSENGER, START, TARGET, run as run_route,
)
from probe_surface_pickup_native_mission import run_native, run_world
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP


def install_native_entity_array(state):
    """Move the live fixture units into retail's ID-indexed global entity pool.

    Native 0x51b5a0 computes game+0x14e84 + 312*unitId, rejects addresses
    beyond game+0x14e88, then checks the unit's alive bit at +0x130. Entity ID
    0 is reserved here; Vertrans is ID 1 and Araarch is ID 2.
    """
    phase, live = state["phase"], state["live"]
    uc = phase.uc
    old_carrier, old_passenger = live.carrier, live.passenger
    pool = phase._alloc(3 * 0x138)
    carrier, passenger = pool + 0x138, pool + 2 * 0x138

    for unit_id, src, dst in ((1, old_carrier, carrier),
                              (2, old_passenger, passenger)):
        uc.mem_write(dst, bytes(uc.mem_read(src, 0x138)))
        live.put(dst + 2, unit_id)

    # This is the exact lookup layout read by the native attachment routine.
    live.put(GS + 0x14E84, pool)
    live.put(GS + 0x14E88, passenger)  # inclusive last entity address

    # Preserve the existing native mission/controller graph while rebasing
    # pointers that referred to the original heap fixture units.
    live.carrier, live.passenger = carrier, passenger
    live.put(live.mission + 0x0E, carrier)
    live.put(live.mission + 0x16, passenger)
    live.put(live.passenger_mission + 0x16, carrier)
    live.put(live.nav + 8, carrier)

    # Replace the synthetic player fixture with game slot 0. The attached-unit
    # helper tests this record's first dword and then may inspect +0xea; retail's
    # worker fixture populated the dword with a non-pointer sentinel, so clear
    # it after route delivery. Keep the actual one-player marker and a valid,
    # empty player-owned entity range for the transfer handler's fallback scan.
    owner = GS + 0x2404
    live.owner = owner
    live.put(carrier + 0xB8, owner)
    live.put(passenger + 0xB8, owner)
    live.put(owner, 0)
    live.byte(owner + 0xEA, 1)
    live.put(owner + 0x74, pool + 0x138)
    live.put(owner + 0x78, pool + 0x138 - 1)

    assert live.get(GS + 0x14E84) == pool
    assert live.get(GS + 0x14E88) == passenger
    assert live.get(carrier + 2) & 0xFFFF == 1
    assert live.get(passenger + 2) & 0xFFFF == 2
    assert live.get(carrier + 0x130) & 0x01000000
    assert live.get(passenger + 0x130) & 0x01000000
    assert 0x51B4F0 not in phase.icd.hooks, "native attachment was intercepted"
    return pool, carrier, passenger


def observe_attachment_calls(state):
    """Observe, without replacing, both native attachment entrypoints."""
    uc = state["phase"].uc
    calls = []

    def on_code(machine, address, _size, _user):
        if address not in (0x51B4F0, 0x51B5A0):
            return
        esp = machine.reg_read(UC_X86_REG_ESP)
        if address == 0x51B4F0:
            args = struct.unpack("<5I", machine.mem_read(esp + 4, 20))
            calls.append((address, args))
        else:
            record, mode = struct.unpack("<II", machine.mem_read(esp + 4, 8))
            payload = bytes(machine.mem_read(record, 7))
            calls.append((address, (record, mode, payload)))

    uc.hook_add(UC_HOOK_CODE, on_code,
                begin=0x51B4F0, end=0x51B5A0)
    return calls


def dispatch_completed_passenger_order(state, carrier, passenger):
    """Stage and retire a real passenger order after native boarding.

    The route fixture keeps a synthetic reciprocal order on the passenger so
    the carrier's pickup handler can find it. Boarding does not consume that
    placeholder in the native-attachment mode. Clear only that fixture queue
    head, then use retail's code-2 constructor, insertion, dispatcher, and
    mission/reference cleanup. This is a completion check after attachment;
    it does not exercise passenger approach or movement during pickup.
    """
    phase, live, uc = state["phase"], state["live"], state["phase"].uc
    put, get = live.put, live.get
    placeholder = get(passenger + 0x60)
    assert placeholder == live.passenger_mission, hex(placeholder)
    assert get(placeholder + 4) == 1
    assert get(placeholder + 0x16) == carrier
    assert get(carrier + 0x60) == 0, "carrier pickup order still owns its queue"
    assert get(carrier + 0xC4) == 0, "carrier already has a target reference"
    carrier_ref_before = get(carrier + 0xC4)
    passenger_ref_before = get(passenger + 0xC4)

    definitions = get(0x62DB84)
    assert definitions, "retail mission descriptor table is missing"
    # The full-map fixture's table is controlled. Install only the retail
    # descriptor row needed to dispatch this staged Move_Seek_Pickup order.
    put(definitions + 2 * 25 + 4, 0x403430)
    put(definitions + 2 * 25 + 0x11, 0x200)

    # The fixture owner was zeroed for the carrier transfer fallback. Restore
    # the valid player-row fields required by real queue insertion.
    put(live.owner, 1)
    live.byte(live.owner + 0xEA, 1)
    put(passenger + 0x60, 0)  # discard the synthetic reciprocal-order head

    passenger_order = phase._alloc(0x80)
    constructed, error = phase.icd.call(
        0x4D6C40,
        (2, carrier, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        ecx=passenger_order)
    assert error is None, ("native code-2 passenger order constructor", error)
    assert constructed == passenger_order
    assert get(passenger_order) == 0x5F2814
    assert uc.mem_read(passenger_order + 4, 1) == b"\x02"
    assert get(passenger_order + 0x16) == carrier
    assert get(passenger_order + 0x5A) == 0x200

    _, error = phase.icd.call(0x4D7750, (passenger, passenger_order))
    assert error is None, ("native passenger queue insertion", error)
    assert get(passenger + 0x60) == passenger_order
    assert get(passenger_order + 0x0E) == passenger
    assert get(passenger_order + 0x66) == 0
    assert get(carrier + 0x60) == 0
    assert get(carrier + 0xC4) == passenger_order + 0x12
    assert get(passenger + 0xC4) == passenger_ref_before

    # Only this one fixture override is removed: native 4d6ad0 must perform
    # the passenger queue unlink, and the existing no-op free sink remains.
    hooks = dict(phase.icd.hooks)
    fixture_remove = hooks.pop(0x4D6AD0, None)
    assert fixture_remove is not None and fixture_remove.__name__ == "remove_order"
    assert 0x4EBA00 in hooks, "controlled no-op free sink was unexpectedly removed"
    phase.icd.hooks = hooks

    native_entries = []
    remove_args = []

    def observe_native_cleanup(machine, address, _size, _user):
        if address in (0x403430, 0x4D6AD0, 0x4D6DA0, 0x519950):
            native_entries.append(address)
        if address == 0x4D6AD0:
            esp = machine.reg_read(UC_X86_REG_ESP)
            remove_args.append(struct.unpack("<2I", machine.mem_read(esp + 4, 8)))

    uc.hook_add(UC_HOOK_CODE, observe_native_cleanup)
    live.put(GS + 0x19F44, state["native_tick"] + 1)
    _, error = phase.icd.call(0x4D8450, (passenger,))
    assert error is None, ("native attached-passenger dispatch", error)

    assert remove_args == [(passenger, passenger_order)], remove_args
    assert native_entries == [0x403430, 0x4D6AD0, 0x4D6DA0, 0x519950], (
        [hex(address) for address in native_entries])
    assert get(passenger + 0x60) == 0, "completed passenger order remains queued"
    assert get(carrier + 0x60) == 0, "passenger cleanup changed carrier order ownership"
    assert get(carrier + 0xC4) == carrier_ref_before, (
        "native passenger cleanup left a stale carrier reference",
        hex(get(carrier + 0xC4)))
    assert get(passenger + 0xC4) == passenger_ref_before
    assert get(carrier + 0xAC) == passenger, "passenger cleanup cleared carrier cargo"
    assert get(passenger + 0xA8) == carrier, "passenger cleanup cleared its carrier link"
    return passenger_order, native_entries


def main():
    repo = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--retail-root", default="/home/pocket_geek/tak_data")
    parser.add_argument("--hpitool", default=str(repo / "build-o2/hpitool"))
    parser.add_argument("--world-binary", default=str(repo / "build-o2/transport_test"))
    parser.add_argument("--max-ticks", type=int, default=5000)
    args = parser.parse_args()

    retail_root = Path(args.retail_root).resolve()
    state = run_route(retail_root, Path(args.hpitool).resolve(),
                      full_map=True, native_attachment=True)
    pool, carrier, passenger = install_native_entity_array(state)
    state["native_attachment"] = True
    calls = observe_attachment_calls(state)
    native_tick, arrival_tick, native_trace = run_native(state, args.max_ticks)
    state["native_tick"] = native_tick

    native_outer = [entry for entry in calls if entry[0] == 0x51B4F0]
    native_inner = [entry for entry in calls if entry[0] == 0x51B5A0]
    assert len(native_outer) == 1, native_outer
    assert len(native_inner) == 1, native_inner
    assert native_outer[0][1][:2] == (passenger, carrier), native_outer
    assert native_outer[0][1][2:] == (0xFFFFFFFF, 0, 1), native_outer
    assert native_inner[0][1][1:] == (1, bytes((7, 2, 0, 1, 0, 0xFF, 0))), native_inner

    world_tick, world_output = run_world(Path(args.world_binary).resolve(), retail_root)
    cargo = state["live"].get(carrier + 0xAC)
    parent = state["live"].get(passenger + 0xA8)
    passenger_mission = state["live"].get(passenger + 0x60)
    passenger_flags = state["live"].get(passenger + 0x130)
    assert passenger_mission == state["live"].passenger_mission, (
        "retail attachment unexpectedly changed the fixture passenger order",
        hex(passenger_mission))
    passenger_order, passenger_cleanup = dispatch_completed_passenger_order(
        state, carrier, passenger)
    print(
        f"PASS: retail-grade Lake Lokken sea pickup covered all "
        f"{state['width'] * state['height']} TNT cells; the native worker "
        f"installed {len(state['route'])} route waypoints at tick "
        f"{state['delivered_at']}, then Vertrans arrived at tick "
        f"{arrival_tick} and attached Araarch at tick {native_tick} "
        f"({native_tick - state['delivered_at']} mover ticks after delivery)."
    )
    print(
        f"  Real entity array: base {pool:#010x}, inclusive end "
        f"{state['live'].get(GS + 0x14E88):#010x}, IDs 1/2 at "
        f"{carrier:#010x}/{passenger:#010x}; native 0x51b4f0 and 0x51b5a0 "
        "both executed once."
    )
    print(
        f"  Native terminal state: cargo={cargo:#010x}, reciprocal parent="
        f"{parent:#010x}, passenger order pointer={passenger_mission:#010x}, "
        f"passenger flags={passenger_flags:#010x}, "
        f"transfer effects={state['live'].transfer_effects}."
    )
    print(
        f"  Post-boarding passenger check: native code-2 order "
        f"{passenger_order:#010x} completed through 0x4d8450/0x403430; "
        f"retail cleanup entries "
        f"{[hex(address) for address in passenger_cleanup]}; passenger and "
        "carrier queues and target references are clear, cargo links remain. "
        "This does not test passenger approach or movement during pickup."
    )
    print(
        f"  World full-map shipped-map roundtrip boarded at tick {world_tick} "
        "and completed shore unload/placement/coast. Native ticks are not "
        "asserted equal to World ticks."
    )
    assert "PASS: boat completed routed shore unload on the shipped map" in world_output


if __name__ == "__main__":
    main()
