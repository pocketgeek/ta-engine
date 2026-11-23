#!/usr/bin/env python3
"""Join CREAERI's native flight-to-ground mover transition to its COB pose.

CREAERI is a shipped flying transporter but its COB declares no BeginFlight
or BeginLanding methods, so there is no carrier-specific mission call-in to
exercise. Instead this bounded probe drives retail's 0x4dc800 mover from
flight to the ground state. It checks the resulting MoveRate(0) and
setSFXoccupy(4) call-ins, compares every attached-COB boundary with World,
and transforms the post-transition prop pose through native 0x4ee620. The
mover targets, mode change, and unit services are controlled; no retail GUI
is launched.
"""
import argparse
import struct
from pathlib import Path

from check_movement_callback_order import SCRIPT_ROOT, cob_methods, fbi_info
from check_native_flight_model_pose import (
    MODEL_ROOT, compare_geometry, names_at,
)
from probe_native_mover_cob_transitions import local_trace, native_transition_trace


ROOT = Path(__file__).resolve().parents[2]
UNIT = "creaeri"
LANDING_STEP = 18
LANDING_EVENT_TICK = LANDING_STEP - 1  # COB schedule rows are zero-based.
SEQUENCE_PREFIX = [
    *[dict(target=(800, 100, 800)) for _ in range(6)],
    *[dict(target=(800, 100, 800), speed=0) for _ in range(5)],
]


def sequence():
    info = fbi_info(UNIT)
    if not (float(info.get("canfly", "0")) and
            float(info.get("cantransport", "0"))):
        raise AssertionError("CREAERI FBI is no longer a flying transporter")
    speed = int(float(info["maxvelocity"]) * 65536)
    return [
        *SEQUENCE_PREFIX,
        *[dict(target=(-800, 100, 800), speed=speed, heading=32768)
          for _ in range(6)],
        dict(target=(-800, 100, 800), mode=1, state_mode=1),
    ]


def pose_from_row(row, piece_start, piece_count):
    return [row[piece_start + index * 25 + 19:
                piece_start + index * 25 + 25]
            for index in range(piece_count)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--script-binary", default="build-o2/retail_script_test",
                        help="World-side COB state oracle binary")
    parser.add_argument("--model-binary", default="build-o2/model_transform_test",
                        help="World-side model transform helper binary")
    args = parser.parse_args()
    script_binary = str((ROOT / args.script_binary).resolve()) \
        if not Path(args.script_binary).is_absolute() else args.script_binary
    model_binary = str((ROOT / args.model_binary).resolve()) \
        if not Path(args.model_binary).is_absolute() else args.model_binary

    cob_path = SCRIPT_ROOT / f"{UNIT}.cob"
    model_path = MODEL_ROOT / "creaeri.3do"
    if not cob_path.is_file() or not model_path.is_file():
        raise FileNotFoundError((cob_path, model_path))
    methods = {name.lower() for name in cob_methods(UNIT)}
    if not {"turndirection", "setsfxoccupy"}.issubset(methods):
        raise AssertionError(("CREAERI mover COB callback roster changed", sorted(methods)))

    steps = sequence()
    native = native_transition_trace(UNIT, steps)
    if len(native["snapshots"]) != len(steps) + 1:
        raise AssertionError(("native snapshot count", len(native["snapshots"]), len(steps) + 1))

    landing_events = [(name, values) for tick, name, values in native["events"]
                      if tick == LANDING_EVENT_TICK]
    expected = [("MoveRate", (0,)), ("setSFXoccupy", (4,))]
    if landing_events != expected:
        raise AssertionError(("CREAERI native flight-to-ground call-ins",
                              LANDING_EVENT_TICK, landing_events, expected))

    world = local_trace(script_binary, UNIT, cob_path, 0,
                        native["events"], len(steps))
    if len(world) != len(native["snapshots"]):
        raise AssertionError(("native/World boundary count", len(native["snapshots"]),
                              len(world)))
    for index, (retail, expected_world) in enumerate(zip(native["snapshots"], world)):
        if retail != expected_world:
            field = next((i for i, values in enumerate(zip(retail, expected_world))
                          if values[0] != values[1]), None)
            raise AssertionError(("CREAERI native/World COB boundary", index,
                                  field,
                                  None if field is None else
                                  (retail[field], expected_world[field])))

    cob_data = cob_path.read_bytes()
    header = struct.unpack_from("<10I", cob_data)
    piece_names = names_at(cob_data, header[8], header[2])
    piece_start = 2 + header[4] + 16 * 41
    before = pose_from_row(native["snapshots"][LANDING_STEP - 1],
                           piece_start, header[2])
    landing = pose_from_row(native["snapshots"][LANDING_STEP],
                            piece_start, header[2])
    changed = {piece_names[index].lower()
               for index, (left, right) in enumerate(zip(before, landing))
               if left != right}
    expected_propellers = {"lprop", "rprop"}
    if not expected_propellers.issubset(changed):
        raise AssertionError(("CREAERI propellers did not advance at the landing boundary",
                              sorted(expected_propellers), sorted(changed)))

    heading = native["body_angles"][LANDING_STEP][1]
    pieces, vertices, error, worst, _ = compare_geometry(
        cob_path, model_path, landing, heading, model_binary)
    if error >= 0.001:
        raise AssertionError(("CREAERI native/World landed model pose", error, worst))

    print(f"PASS CREAERI flight-to-ground: native 0x4dc800 boundary {LANDING_STEP} "
          f"emits COB tick {LANDING_EVENT_TICK} {landing_events}; all "
          f"{len(native['snapshots'])} native/World COB snapshots match; "
          f"propeller pose advances ({','.join(sorted(expected_propellers))}); "
          f"0x4ee620 matches World for {pieces} pieces/{vertices} vertices "
          f"(max delta {error:.8f} at {worst[1]} vertex {worst[2]})")
    print("LIMITS: controlled flight targets and flight-to-ground mover mode; "
          "no transport mission order, map route, camera, projection, framebuffer, or GUI.")


if __name__ == "__main__":
    main()
