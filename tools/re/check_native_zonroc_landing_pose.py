#!/usr/bin/env python3
"""Join ZONROC's native BeginLanding mission producer to its shipped model pose.

The probe enters retail's accepted-site branch in 0x416cd0/0x417089, attaches
the shipped ZONROC COB, and observes native EndTransport/BeginLanding method
dispatch. It compares the complete COB state before and after the next VM tick
with the World script oracle, then checks the post-landing pose through retail
0x4ee620 against the World geometry helper. Mission services and the accepted
site are controlled; this is not a full transport order, movement, camera, or
render trace. No retail GUI is launched.
"""
import argparse
import struct
from pathlib import Path

import emu
from check_movement_callback_order import SCRIPT_ROOT, fbi_info
from check_native_flight_model_pose import (
    MODEL_ROOT, compare_geometry, names_at,
)
from emuphase import GS, TYPE, Phase
from probe_native_flight_mission_cob import assert_world_match, snapshot
from probe_native_mover_cob_transitions import local_trace, native_vm
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_ESP


ROOT = Path(__file__).resolve().parents[2]
UNIT = "zonroc"
EVENTS = [(0, "EndTransport", ()), (0, "BeginLanding", ())]
LANDING_TICK = 1
EXPECTED_WINGS = {"wingl1", "wingr1", "wingl2", "wingr2", "wingl3", "wingr3"}


def put(uc, address, value):
    uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))


def get(uc, address):
    return struct.unpack("<I", uc.mem_read(address, 4))[0]


def make_native_landing():
    info = fbi_info(UNIT)
    if not (float(info.get("canfly", "0")) and
            float(info.get("cantransport", "0"))):
        raise AssertionError("ZONROC FBI is no longer a flying transporter")

    phase = Phase(24, 24)
    p, uc = phase.icd, phase.uc
    unit = phase.unit(10, 10)
    mission = phase._alloc(0x100)
    controller = phase._alloc(0x100)
    mover = unit + 0x300

    put(uc, 0x62D55C, GS)
    uc.mem_write(unit + 0x68,
                 struct.pack("<3i", 649497815, -44116041, -441878339))
    uc.mem_write(unit + 0x78, struct.pack("<2H", 7, 13))
    uc.mem_write(unit + 0x7E, struct.pack("<H", 4183))
    put(uc, unit + 0xA4, 1)
    put(uc, unit + 0x08, mover)
    put(uc, unit + 0xB4, TYPE)
    put(uc, TYPE + 0x260, 0x800)  # shipped canfly flag
    put(uc, TYPE + 0x264, 0x200)  # shipped cantransport flag
    put(uc, mission + 0x0E, unit)
    uc.mem_write(mission + 0x22, bytes(12))
    uc.mem_write(mission + 5, b"\x02")  # accepted-site stage
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

    # Retain the established accepted-site mission fixture's narrow services.
    # The native 0x416cd0 producer, script name lookup and COB VM stay live.
    p.hooks.update({
        0x4EB9E0: allocate,
        0x416C50: lambda _uc, _sp: (4, 0),
        0x509400: lambda _uc, _sp: (2, 1),
        0x4DC100: lambda _uc, _sp: (1, 88),
        0x511170: lambda _uc, _sp: (1, 130),
        0x51D1E0: lambda _uc, _sp: (3, 0),
        0x4EA3F0: lambda _uc, _sp: (2, 0),
        0x51E4D0: lambda _uc, _sp: (2, 0),
        0x4DA750: lambda _uc, _sp: (2, 0),
        0x4E40E0: make_goal,
        0x4E4540: set_radius,
        0x4E44C0: set_altitude,
        0x4D4D40: install_goal,
    })

    cob_path = SCRIPT_ROOT / f"{UNIT}.cob"
    cob = native_vm(p, cob_path, profile=0)
    put(uc, unit + 0xBC, cob["vm"])
    return p, uc, unit, mission, cob_path, cob, allocation_mode, goals


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--script-binary", default="build-o2/retail_script_test",
                        help="World-side COB state oracle binary")
    parser.add_argument("--model-binary", default="build-o2/model_transform_test",
                        help="World-side model transform helper binary")
    parser.add_argument("--icd", default=str(ROOT / "assets/game/KINGDOMS.icd"),
                        help="installed retail executable used by headless emulation")
    args = parser.parse_args()
    emu.ICD = str(Path(args.icd).resolve())
    script_binary = str((ROOT / args.script_binary).resolve()) \
        if not Path(args.script_binary).is_absolute() else args.script_binary
    model_binary = str((ROOT / args.model_binary).resolve()) \
        if not Path(args.model_binary).is_absolute() else args.model_binary

    cob_path = SCRIPT_ROOT / f"{UNIT}.cob"
    model_path = MODEL_ROOT / "zonroc.3do"
    if not cob_path.is_file() or not model_path.is_file():
        raise FileNotFoundError((cob_path, model_path))
    p, uc, unit, mission, cob_path, cob, allocation_mode, goals = \
        make_native_landing()
    native_before = snapshot(uc, cob)
    pose_before = [piece[:] for piece in cob["pose"]]

    calls = []

    def observe(machine, address, _size, _user_data):
        if address != 0x56C5C0:
            return
        esp = machine.reg_read(UC_X86_REG_ESP)
        name_pointer = get(machine, esp + 4)
        name = bytes(machine.mem_read(name_pointer, 64)).split(b"\0", 1)[0]
        calls.append((name.decode("ascii"), machine.reg_read(UC_X86_REG_ECX)))

    uc.hook_add(UC_HOOK_CODE, observe, begin=0x56C5C0, end=0x56C5C0)
    allocation_mode["controller"] = True
    result, error = p.call(0x416CD0, (unit, mission, 0))
    if error or result != 1:
        raise RuntimeError(("native 0x416cd0 accepted-site BeginLanding producer",
                            result, error))
    expected_calls = [("EndTransport", cob["vm"]), ("BeginLanding", cob["vm"])]
    if calls != expected_calls:
        raise AssertionError(("native landing call-ins", calls, expected_calls))
    if not goals:
        raise AssertionError("native landing producer did not install its accepted-site goal")

    _, error = p.call(0x56C870, (LANDING_TICK,), ecx=cob["vm"])
    if error:
        raise RuntimeError(("native BeginLanding follow-up COB tick", error))
    native_after = snapshot(uc, cob)
    pose_after = [piece[:] for piece in cob["pose"]]

    local_before = local_trace(script_binary, UNIT, cob_path, 0, [], ticks=0)[-1]
    local_after = local_trace(script_binary, UNIT, cob_path, 0, EVENTS,
                              ticks=LANDING_TICK)[-1]
    assert_world_match("ZONROC Create", native_before, local_before)
    assert_world_match("ZONROC BeginLanding", native_after, local_after)

    header = struct.unpack_from("<10I", cob_path.read_bytes())
    piece_names = names_at(cob_path.read_bytes(), header[8], header[2])
    changed = {piece_names[index].lower()
               for index, (before, after) in enumerate(zip(pose_before, pose_after))
               if before != after}
    if not EXPECTED_WINGS.issubset(changed):
        raise AssertionError(("ZONROC landing pose did not advance all wing pieces",
                              sorted(EXPECTED_WINGS), sorted(changed)))

    heading = struct.unpack("<H", uc.mem_read(unit + 0x7E, 2))[0]
    pieces, vertices, error, worst, _ = compare_geometry(
        cob_path, model_path, pose_after, heading, model_binary)
    if error >= 0.001:
        raise AssertionError(("ZONROC native/World landing model pose", error, worst))
    print(f"PASS ZONROC BeginLanding: native 0x416cd0 accepted-site producer "
          f"dispatches {calls}; all {len(native_after)} COB state words match "
          f"World after one VM tick; named wings advance "
          f"({','.join(sorted(EXPECTED_WINGS))}); native 0x4ee620 matches "
          f"World transforms for {pieces} pieces/{vertices} vertices "
          f"(max delta {error:.8f} at {worst[1]} vertex {worst[2]})")
    print("LIMITS: controlled accepted-site mission services and GET profile; "
          "no transport order, movement, camera, projection, framebuffer, or GUI.")


if __name__ == "__main__":
    main()
