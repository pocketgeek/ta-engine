#!/usr/bin/env python3
"""Trace LIFBIRD's shipped idle and flying ambient animation headlessly.

Retail's LIFBIRD Create starts its FlightControl, AnimationControl, and
StaticAnim threads. This runs those scripts through KINGDOMS.icd's native COB
scheduler with controlled unit-value reads, compares every VM boundary with
the World COB VM, then checks representative resulting 3DO poses through the
native model refresh and World transform helper. No retail GUI is launched.
"""
import argparse
import struct
from pathlib import Path

import emu
from check_native_flight_model_pose import compare_geometry
from check_native_mover_cob_join import (
    local_cob_trace,
    native_cob_trace,
)
from check_movement_callback_order import EXTRACTED_ROOT, SCRIPT_ROOT


ROOT = Path(__file__).resolve().parents[2]
COB = SCRIPT_ROOT / "lifbird.cob"
MODEL = EXTRACTED_ROOT / "objects3d/lifbird.3do"
TICKS = 500
GEOMETRY_TICKS = {
    "idle": (1, 3, 10, 60, TICKS),
    "flying": (0, 3, 10, 30, 60, 120, TICKS),
}


def load_piece_names(data, header):
    names = []
    for index in range(header[2]):
        pointer = struct.unpack_from("<I", data, header[8] + index * 4)[0]
        names.append(data[pointer:data.index(b"\0", pointer)].decode("ascii"))
    return names


def load_script_names(data, header):
    names = []
    for index in range(header[1]):
        pointer = struct.unpack_from("<I", data, header[7] + index * 4)[0]
        names.append(data[pointer:data.index(b"\0", pointer)].decode("ascii"))
    return names


def pose_from_row(row, piece_start, piece_count):
    return [row[piece_start + index * 25 + 19:
                piece_start + index * 25 + 25]
            for index in range(piece_count)]


def check_profile(label, profile, script_binary, model_binary, names, header):
    native = native_cob_trace(COB, profile, [], ticks=TICKS)
    world = local_cob_trace(script_binary, COB, profile, [], ticks=TICKS)
    if len(native) != TICKS + 1 or len(world) != len(native):
        raise AssertionError((label, "snapshot count", len(native), len(world)))
    if native != world:
        tick = next(index for index, (left, right) in
                    enumerate(zip(native, world)) if left != right)
        left, right = native[tick], world[tick]
        word = next(index for index, pair in enumerate(zip(left, right))
                    if pair[0] != pair[1])
        raise AssertionError((label, "native/World COB state", tick, word,
                              left[word], right[word]))

    # FlightControl owns these switches. With no speed it stays in the neutral
    # ambient path; with speed it selects the fly loop. Static 3 latches the
    # one-time restore path used by the neutral controller.
    expected = ({2: 0, 3: 0, 5: 1} if label == "idle" else {2: 1, 3: 0})
    if not any(all(row[column] == value for column, value in expected.items())
               for row in native[:10]):
        raise AssertionError((label, "Create did not select its ambient state",
                              [[row[column] for column in expected]
                               for row in native[:10]]))

    piece_start = 2 + header[4] + 16 * 41
    advanced = set()
    for previous, current in zip(native, native[1:]):
        before = pose_from_row(previous, piece_start, header[2])
        after = pose_from_row(current, piece_start, header[2])
        advanced.update(name for index, name in enumerate(names)
                        if before[index] != after[index])
    # Fail if the fixture stops exercising the shipped bird's actual wings.
    expected_wings = {"wingl1", "wingr1", "wingl2", "wingr2",
                      "wingl3", "wingr3"}
    if label == "flying":
        missing = expected_wings - {name.lower() for name in advanced}
        if missing:
            raise AssertionError((label, "native fly loop stopped animating named wings",
                                  sorted(missing), sorted(advanced)))
    elif not {"body", "head"}.issubset({name.lower() for name in advanced}):
        raise AssertionError((label, "native idle animation did not move body/head",
                              sorted(advanced)))

    results = []
    for tick in GEOMETRY_TICKS[label]:
        pose = pose_from_row(native[tick], piece_start, header[2])
        pieces, vertices, error, worst, _ = compare_geometry(
            COB, MODEL, pose, 0, model_binary)
        if error >= 0.001:
            raise AssertionError((label, tick, "native/World 3DO pose", error, worst))
        results.append((tick, pieces, vertices, error))

    print(f"PASS LIFBIRD {label}: {TICKS + 1} native/World COB boundaries match; "
          f"animated pieces={','.join(sorted(advanced))}; "
          f"native/World 3DO transforms match at {len(results)} poses "
          f"({max(row[3] for row in results):.8f} max delta)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--script-binary", default="build-o2/retail_script_test")
    parser.add_argument("--model-binary", default="build-o2/model_transform_test")
    parser.add_argument("--icd", default=str(ROOT / "assets/game/KINGDOMS.icd"),
                        help="path to the installed retail executable")
    args = parser.parse_args()
    emu.ICD = str(Path(args.icd).resolve())
    if not COB.is_file() or not MODEL.is_file():
        raise FileNotFoundError((COB, MODEL))
    data = COB.read_bytes()
    header = struct.unpack_from("<10I", data)
    names = load_piece_names(data, header)
    if header[1] != 11 or header[2] != 14 or header[4] != 5:
        raise AssertionError(("LIFBIRD COB layout changed", header))
    if not {"flightcontrol", "animationcontrol", "staticanim"}.issubset(
            {name.lower() for name in load_script_names(data, header)}):
        raise AssertionError("LIFBIRD Create-owned animation controllers are missing")

    check_profile("idle", 0, args.script_binary, args.model_binary, names, header)
    check_profile("flying", 1, args.script_binary, args.model_binary, names, header)
    print("PASS: LIFBIRD shipped ambient idle/flight animation is covered headlessly")


if __name__ == "__main__":
    main()
