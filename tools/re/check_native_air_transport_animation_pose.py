#!/usr/bin/env python3
"""Join the native air-carrier BeginFlight producer to its shipped model pose.

Retail's 0x416c50 dispatches BeginFlight into the attached ZONROC COB. The
probe advances its real COB scheduler once, compares the complete native
thread/static/piece checkpoint with the World script oracle, and transforms
the resulting named pose through retail's 0x4ee620 against the World geometry
helper. This adds a carrier-specific mission-animation case to the generic
flyer-mover callback roster. Mission setup and unit services remain controlled;
it does not run a complete transport order, map movement, camera, or renderer.
No retail GUI is launched.
"""
import argparse
import struct
from pathlib import Path

import emu
from emuphase import GS, TYPE, Phase
from check_movement_callback_order import SCRIPT_ROOT
from check_native_flight_model_pose import (
    MODEL_ROOT, compare_geometry, names_at, object_names, parse_object,
)
from probe_native_flight_mission_cob import assert_world_match, snapshot
from probe_native_mover_cob_transitions import local_trace, native_vm
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_ESP


ROOT = Path(__file__).resolve().parents[2]
UNIT = "zonroc"
EVENTS = [(0, "BeginFlight", ())]
FLIGHT_TICKS = 1
EXPECTED_WINGS = {"wingl1", "wingr1", "wingl2", "wingr2", "wingl3", "wingr3"}


def put(uc, address, value):
    uc.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))


def make_native_flight():
    phase = Phase(24, 24)
    p, uc = phase.icd, phase.uc
    unit = phase.unit(10, 10)
    mission = phase._alloc(0x100)
    mover = unit + 0x300

    put(uc, 0x62D55C, GS)
    put(uc, unit + 0x08, mover)
    put(uc, unit + 0xA8, 0)
    put(uc, unit + 0xB4, TYPE)
    put(uc, mover + 0x36, 0)
    put(uc, TYPE + 0x260, 0x800)  # shipped canfly flag
    put(uc, TYPE + 0x264, 0x200)  # shipped cantransport flag

    # The producer calls these surrounding movement/controller services; keep
    # their host effects controlled while leaving 0x416c50 and COB dispatch live.
    p.hooks[0x51E4D0] = lambda _uc, _sp: (2, 0)
    p.hooks[0x4EA3F0] = lambda _uc, _sp: (2, 0)
    p.hooks[0x4DA750] = lambda _uc, _sp: (2, 0)

    cob_path = SCRIPT_ROOT / f"{UNIT}.cob"
    cob = native_vm(p, cob_path, profile=0)
    put(uc, unit + 0xBC, cob["vm"])
    return p, uc, unit, mission, cob_path, cob, snapshot(uc, cob), \
        [piece[:] for piece in cob["pose"]]


def native_begin_flight(p, uc, unit, mission, cob):
    calls = []

    def observe(machine, address, _size, _user_data):
        if address != 0x56C5C0:
            return
        esp = machine.reg_read(UC_X86_REG_ESP)
        name_pointer = struct.unpack("<I", machine.mem_read(esp + 4, 4))[0]
        name = bytes(machine.mem_read(name_pointer, 64)).split(b"\0", 1)[0]
        calls.append((name.decode("ascii"), machine.reg_read(UC_X86_REG_ECX)))

    uc.hook_add(UC_HOOK_CODE, observe, begin=0x56C5C0, end=0x56C5C0)
    result, error = p.call(0x416C50, (unit, mission, 0, 0))
    if error or result != 0:
        raise RuntimeError(("native 0x416c50 BeginFlight producer", result, error))
    if calls != [("BeginFlight", cob["vm"])]:
        raise AssertionError(("native BeginFlight call-in", calls, cob["vm"]))

    _, error = p.call(0x56C870, (FLIGHT_TICKS,), ecx=cob["vm"])
    if error:
        raise RuntimeError(("native BeginFlight follow-up COB tick", error))
    return snapshot(uc, cob), [piece[:] for piece in cob["pose"]]


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
    cob_data = cob_path.read_bytes()
    header = struct.unpack_from("<10I", cob_data)
    piece_names = names_at(cob_data, header[8], header[2])
    if not {"beginflight", "flightcontrol"}.issubset(
            {name.lower() for name in
             [bytes(cob_data[struct.unpack_from("<I", cob_data, header[7] + 4 * i)[0]:]
                    .split(b"\0", 1)[0]).decode("ascii")
              for i in range(header[1])]}):
        raise AssertionError("ZONROC COB no longer declares BeginFlight and FlightControl")

    p, uc, unit, mission, cob_path, cob, native_before, pose_before = make_native_flight()
    # Compare the startup/Create checkpoint before the mission call-in too.
    native_after, pose_after = native_begin_flight(p, uc, unit, mission, cob)
    local_before = local_trace(script_binary, UNIT, cob_path, 0, [], ticks=0)[-1]
    local_after = local_trace(script_binary, UNIT, cob_path, 0, EVENTS,
                              ticks=FLIGHT_TICKS)[-1]
    assert_world_match("ZONROC Create", native_before, local_before)
    assert_world_match("ZONROC BeginFlight", native_after, local_after)

    changed = {piece_names[index].lower()
               for index, (before, after) in enumerate(zip(pose_before, pose_after))
               if before != after}
    model_piece_names = {name.lower() for name in object_names(
        parse_object(model_path.read_bytes(), 0))}
    matched_wings = EXPECTED_WINGS & changed & model_piece_names
    if matched_wings != EXPECTED_WINGS:
        raise AssertionError(("ZONROC BeginFlight did not animate all named wing pieces",
                              sorted(EXPECTED_WINGS), sorted(matched_wings),
                              sorted(changed)))

    heading = struct.unpack("<H", uc.mem_read(unit + 0x7E, 2))[0]
    pieces, vertices, error, worst, _ = compare_geometry(
        cob_path, model_path, pose_after, heading, model_binary)
    if error >= 0.001:
        raise AssertionError(("ZONROC native/World 3DO pose", pieces, vertices,
                              error, worst))
    print(f"PASS ZONROC BeginFlight: native 0x416c50 call-in and one COB tick "
          f"match World across {len(native_after)} state words; named wing pose "
          f"advances ({','.join(sorted(matched_wings))}); native 0x4ee620 "
          f"matches World transforms for {pieces} pieces/{vertices} vertices "
          f"(max delta {error:.8f} at {worst[1]} vertex {worst[2]})")
    print("LIMITS: controlled BeginFlight mission services and GET profile; no "
          "full carrier movement/order, camera, projection, framebuffer, or GUI.")


if __name__ == "__main__":
    main()
