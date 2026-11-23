#!/usr/bin/env python3
"""Join retail flight-mission call-ins to an attached retail COB VM.

The probe calls the retail mission producers in headless KINGDOMS.icd emulation,
with a real Araarch COB descriptor/bytecode attached at unit+0xbc. Retail's
56c5c0 name dispatcher and scheduler run normally. It then compares the full
COB thread/static/piece checkpoint with the World-side retail_script_test
oracle after the next ordinary VM tick.

The mission host remains controlled: BeginFlight calls 416c50 directly with
its callback-enabled flag; BeginLanding enters 416cd0's stage-2 accepted-site
branch with a synthetic unit, mission, first-site acceptance, controller,
height, and velocity services. These boundaries isolate the call-in/COB state
join; they do not emulate a full match, route search, transport, renderer, or
animation poses from a loaded model. The fixture's Araarch COB has no
EndTransport method, so that native request is observed but naturally resolves
to no local COB thread; BeginLanding is the state compared against World.

No retail GUI is launched.

Example:
    PYTHONPATH=tools/re python3 tools/re/probe_native_flight_mission_cob.py \
        --binary build-o2/retail_script_test
"""
import argparse
import struct
from pathlib import Path

from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_ESP

from emuphase import Phase, GS, TYPE
from probe_native_mover_cob_transitions import local_trace, native_vm
from check_movement_callback_order import SCRIPT_ROOT


ROOT = Path(__file__).resolve().parents[2]


def put(uc, address, value):
    uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))


def get(uc, address):
    return struct.unpack("<I", uc.mem_read(address, 4))[0]


def snapshot(uc, cob):
    """Flatten the same retail COB checkpoint compared by mover parity tests."""
    row = [
        get(uc, cob["vm"] + 0xA60), get(uc, 0x64186C),
        *[get(uc, cob["statics"] + i * 4)
          for i in range(cob["static_count"])],
        *struct.unpack("<656I", uc.mem_read(cob["vm"] + 0x20, 16 * 0xA4)),
    ]
    for piece in range(cob["piece_count"]):
        row.extend(struct.unpack(
            "<19I", uc.mem_read(cob["pieces"] + piece * 76, 76)))
        row.extend(cob["pose"][piece])
    row.extend((len(cob["writes"]), *cob["writes"]))
    return row


def names_at_call(uc):
    esp = uc.reg_read(UC_X86_REG_ESP)
    name_pointer = get(uc, esp + 4)
    return bytes(uc.mem_read(name_pointer, 64)).split(b"\0", 1)[0].decode("ascii")


def observe_native_callins(uc, calls):
    def observe(machine, address, _size, _user_data):
        if address == 0x56C5C0:
            calls.append((names_at_call(machine),
                          machine.reg_read(UC_X86_REG_ECX)))
    uc.hook_add(UC_HOOK_CODE, observe)


def assert_world_match(label, native_row, world_row):
    if native_row != world_row:
        mismatch = next((i for i, pair in enumerate(zip(native_row, world_row))
                         if pair[0] != pair[1]), None)
        raise AssertionError((label, "native/World COB state differs",
                              mismatch,
                              None if mismatch is None else
                              (native_row[mismatch], world_row[mismatch]),
                              len(native_row), len(world_row)))


def flight_producer(binary):
    phase = Phase(24, 24)
    p, uc = phase.icd, phase.uc
    unit = phase.unit(10, 10)
    mission = phase._alloc(0x100)
    mover = unit + 0x300
    put(uc, 0x62D55C, GS)
    put(uc, unit + 0x8, mover)
    put(uc, unit + 0xA8, 0)
    put(uc, unit + 0xB4, TYPE)
    put(uc, mover + 0x36, 0)

    # The surrounding mission services are inert host boundaries. The retail
    # BeginFlight producer and script dispatcher are not substituted.
    p.hooks[0x51E4D0] = lambda _uc, _sp: (2, 0)
    p.hooks[0x4EA3F0] = lambda _uc, _sp: (2, 0)
    p.hooks[0x4DA750] = lambda _uc, _sp: (2, 0)

    cob_path = SCRIPT_ROOT / "aradrag.cob"
    cob = native_vm(p, cob_path, 0)
    put(uc, unit + 0xBC, cob["vm"])
    calls = []
    observe_native_callins(uc, calls)
    result, error = p.call(0x416C50, (unit, mission, 0, 0))
    if error or result != 0:
        raise RuntimeError(("native BeginFlight producer 0x416c50", result, error))
    expected = [("BeginFlight", cob["vm"])]
    if calls != expected:
        raise AssertionError(("0x416c50 call-ins", calls, expected))

    # 56c5c0 services the mission call-in immediately. The script oracle
    # receives that same named event at tick zero, followed by the normal tick.
    _, error = p.call(0x56C870, (1,), ecx=cob["vm"])
    if error:
        raise RuntimeError(("retail BeginFlight follow-up VM tick", error))
    native = snapshot(uc, cob)
    world = local_trace(
        binary, "aradrag", cob_path, 0,
        [(0, "BeginFlight", ())], ticks=1)[-1]
    assert_world_match("BeginFlight", native, world)
    return len(native)


def landing_producer(binary):
    phase = Phase(24, 24)
    p, uc = phase.icd, phase.uc
    unit = phase.unit(10, 10)
    mission = phase._alloc(0x100)
    controller = phase._alloc(0x100)
    mover = unit + 0x300
    put(uc, 0x62D55C, GS)

    # This is the deterministic stage-2 accepted-first-site row from the
    # bounded landing mission matrix (seed 0x416cd0, case 82). Keep its ABI
    # inputs so the native mission reaches the real 0x417089 landing block.
    uc.mem_write(unit + 0x68,
                 struct.pack("<3i", 649497815, -44116041, -441878339))
    uc.mem_write(unit + 0x78, struct.pack("<2H", 7, 13))
    uc.mem_write(unit + 0x7E, struct.pack("<H", 4183))
    put(uc, unit + 0xA4, 1)
    put(uc, unit + 0x8, mover)
    put(uc, unit + 0xB4, TYPE)
    put(uc, TYPE + 0x260, 0x800)  # flying unit
    put(uc, TYPE + 0x264, 0x200)  # transport-capable branch requests EndTransport
    put(uc, mission + 0x0E, unit)
    uc.mem_write(mission + 0x22, bytes(12))
    uc.mem_write(mission + 5, b"\x02")
    put(uc, mission + 6, 3473857355)
    put(uc, mission + 0x4E, 1265153291)
    put(uc, mission + 0x52, 1198115308)
    put(uc, mission + 0x66, 0)
    put(uc, 0x64186C, 559021162)

    allocation_mode = {"controller": False}
    goal, goals = [], []

    def allocate(_uc, stack):
        size = get(_uc, stack)
        if allocation_mode["controller"] and size == 0x36:
            return 0, controller
        return 0, phase._alloc(size)

    def make_goal(_uc, stack):
        goal[:] = [*struct.unpack("<3i", uc.mem_read(get(uc, stack + 4), 12)),
                   0x20, 0]
        return 2, controller

    def set_radius(_uc, stack):
        goal[3] |= 0x10
        goal[4] = get(uc, stack)
        return 1, 0

    def set_altitude(_uc, stack):
        goal[3] |= 8
        offset = struct.unpack("<i", uc.mem_read(stack, 4))[0]
        goal[1] = (130 + offset) * 65536
        return 1, 0

    def install_goal(_uc, _stack):
        goals.append(list(goal))
        return 1, 0

    # 416c50 and 509400 are controlled mission boundaries in this landing
    # fixture: the first supplies the expected initializer result, the second
    # accepts the first candidate.  Call-in dispatch 56c5c0 remains native.
    p.hooks[0x4EB9E0] = allocate
    p.hooks[0x416C50] = lambda _uc, _sp: (4, 0)
    p.hooks[0x509400] = lambda _uc, _sp: (2, 1)
    p.hooks[0x4DC100] = lambda _uc, _sp: (1, 88)
    p.hooks[0x511170] = lambda _uc, _sp: (1, 130)
    p.hooks[0x51D1E0] = lambda _uc, _sp: (3, 0)
    p.hooks[0x4EA3F0] = lambda _uc, _sp: (2, 0)
    p.hooks[0x51E4D0] = lambda _uc, _sp: (2, 0)
    p.hooks[0x4DA750] = lambda _uc, _sp: (2, 0)
    p.hooks[0x4E40E0] = make_goal
    p.hooks[0x4E4540] = set_radius
    p.hooks[0x4E44C0] = set_altitude
    p.hooks[0x4D4D40] = install_goal

    cob_path = SCRIPT_ROOT / "aradrag.cob"
    cob = native_vm(p, cob_path, 0)
    put(uc, unit + 0xBC, cob["vm"])
    allocation_mode["controller"] = True
    calls = []
    observe_native_callins(uc, calls)
    result, error = p.call(0x416CD0, (unit, mission, 0))
    if error or result != 1:
        raise RuntimeError(("native landing mission 0x416cd0", result, error))
    expected = [("EndTransport", cob["vm"]), ("BeginLanding", cob["vm"])]
    if calls != expected:
        raise AssertionError(("0x416cd0 call-ins", calls, expected))
    if not goals:
        raise AssertionError("native 0x416cd0 did not install its controlled landing goal")

    _, error = p.call(0x56C870, (1,), ecx=cob["vm"])
    if error:
        raise RuntimeError(("retail BeginLanding follow-up VM tick", error))
    native = snapshot(uc, cob)
    # Araarch declares BeginLanding but not EndTransport. The shared World-side
    # trace helper filters that absent method, matching the real COB lookup.
    world = local_trace(
        binary, "aradrag", cob_path, 0,
        [(0, "EndTransport", ()), (0, "BeginLanding", ())], ticks=1)[-1]
    assert_world_match("BeginLanding", native, world)
    return len(native)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", default="build-o2/retail_script_test",
                        help="World-side COB state oracle binary")
    args = parser.parse_args()
    binary = str((ROOT / args.binary).resolve()) if not Path(args.binary).is_absolute() else args.binary
    print(f"PASS: native 0x416c50 -> BeginFlight; all {flight_producer(binary)} COB state words match World after the next VM tick")
    print(f"PASS: native 0x416cd0/0x417089 -> EndTransport, BeginLanding; all {landing_producer(binary)} COB state words match World after the next VM tick")


if __name__ == "__main__":
    main()
