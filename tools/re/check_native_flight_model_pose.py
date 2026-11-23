#!/usr/bin/env python3
"""Join native flight-mover COB callbacks to the resulting shipped 3DO pose.

The probe calls retail's 0x4dc800 mover with controlled flight input, dispatches
its callbacks through the attached shipped COB VM, and captures the post-tick
piece pose. It replays the same callback schedule through the World script
oracle, then compares native 0x4ee620 vertices with model_transform_test using
the native unit heading. This is a headless geometry/state check, not a camera,
texture, projection, or framebuffer comparison. ``--all`` discovers the shipped
canfly FBI/COB pairs with movement callbacks; even a unit with no animated pose
in this generic fixture still receives a static native/World model-transform
comparison and is reported separately.
"""
import argparse
import struct
import subprocess
from pathlib import Path

from emu import HEAP, Icd
from check_movement_callback_order import (
    CALLBACKS, EXTRACTED_ROOT, SCRIPT_ROOT, UNIT_ROOT, cob_methods, fbi_info,
)
from probe_native_mover_cob_transitions import local_trace, native_transition_trace


ROOT = Path(__file__).resolve().parents[2]
MODEL_ROOT = EXTRACTED_ROOT / "objects3d"
SAMPLE_TICK = 5


def signed(value):
    return struct.unpack("<i", struct.pack("<I", value & 0xFFFFFFFF))[0]


def names_at(data, offset, count):
    names = []
    for index in range(count):
        pointer = struct.unpack_from("<I", data, offset + 4 * index)[0]
        names.append(data[pointer:data.index(b"\0", pointer)].decode("ascii"))
    return names


def parse_object(data, offset):
    count = struct.unpack_from("<I", data, offset + 4)[0]
    name_pointer = struct.unpack_from("<I", data, offset + 28)[0]
    name = data[name_pointer:data.index(b"\0", name_pointer)].decode("ascii")
    authored_offset = struct.unpack_from("<3i", data, offset + 16)
    vertex_offset = struct.unpack_from("<I", data, offset + 36)[0]
    vertices = [struct.unpack_from("<3i", data, vertex_offset + 12 * i)
                for i in range(count)]
    child = struct.unpack_from("<I", data, offset + 48)[0]
    children = []
    while child:
        sibling = struct.unpack_from("<I", data, child + 44)[0]
        children.append(parse_object(data, child))
        child = sibling
    return {"name": name, "offset": authored_offset,
            "vertices": vertices, "children": children}


def object_names(obj):
    return [obj["name"], *(name for child in obj["children"]
                            for name in object_names(child))]


def compare_geometry(cob_path, model_path, pose, heading, binary):
    cob_data = cob_path.read_bytes()
    model_data = model_path.read_bytes()
    header = struct.unpack_from("<10I", cob_data)
    piece_names = names_at(cob_data, header[8], header[2])
    piece_index = {name.lower(): index for index, name in enumerate(piece_names)}
    root = parse_object(model_data, 0)

    p = Icd()
    uc = p.uc
    definition_cursor = HEAP + 0x240000
    state_cursor = HEAP + 0x280000
    output_cursor = HEAP + 0x2A0000
    output_vertices = []
    world_rows = []
    model_pieces = []

    def put(address, *values):
        uc.mem_write(address, struct.pack("<" + "I" * len(values),
                                         *(value & 0xFFFFFFFF for value in values)))

    def build(obj, ancestors):
        nonlocal definition_cursor, state_cursor, output_cursor
        definition = definition_cursor
        definition_cursor += 0x60
        authored_vertices = definition_cursor
        definition_cursor += max(12, len(obj["vertices"]) * 12)
        output = output_cursor
        output_cursor += max(12, len(obj["vertices"]) * 12)
        state = state_cursor
        state_cursor += 0x40

        x, y, z = obj["offset"]
        put(definition + 4, len(obj["vertices"]))
        put(definition + 0x10, -x, y, -z)
        put(definition + 0x24, authored_vertices)
        if obj["vertices"]:
            uc.mem_write(authored_vertices, b"".join(
                struct.pack("<3i", -vx, vy, -vz) for vx, vy, vz in obj["vertices"]))

        put(state, definition)
        put(state + 0x24, output)
        put(state + 0x28, 0)  # invalidate the cached piece transform
        piece = piece_index.get(obj["name"].lower())
        current = pose[piece] if piece is not None else [0] * 6
        uc.mem_write(state + 4, struct.pack("<3i", *(signed(v) for v in current[:3])))
        uc.mem_write(state + 0x10, struct.pack("<3H", *(v & 0xFFFF for v in current[3:])))
        model_pieces.append(obj["name"])

        chain = ancestors + [(obj["offset"], current)]
        for vertex_index, vertex in enumerate(obj["vertices"]):
            row = []
            for offset, current_pose in chain:
                row.extend(offset)
                row.extend(current_pose[:3])
                row.extend(current_pose[3:])
                row.extend((0, 0, 0))  # authored vertex, filled at the leaf
                row.extend((0, 0, 0))  # root body pitch, heading, roll
            leaf_base = (len(chain) - 1) * 15
            row[leaf_base + 9:leaf_base + 12] = vertex
            row[12:15] = (0, heading, 0)
            world_rows.append(" ".join(map(str, (len(chain), *row))))
            output_vertices.append((output + vertex_index * 12,
                                    obj["name"], vertex_index))

        children = [build(child, chain) for child in obj["children"]]
        if children:
            put(state + 0x30, children[0]["state"])
        for left, right in zip(children, children[1:]):
            put(left["state"] + 0x2C, right["state"])
        return {"state": state}

    instance = build(root, [])
    wrapper = HEAP + 0x230000
    unit = HEAP + 0x50000
    put(unit + 0xC0, wrapper)
    put(wrapper + 8, 1)
    put(wrapper + 0x1C8, instance["state"])
    uc.mem_write(unit + 0x7C, struct.pack("<3H", 0, heading, 0))
    _, error = p.call(0x4EE620, (unit,))
    if error:
        raise RuntimeError(("native 3DO refresh", error))

    actual = subprocess.run([binary, "--chain"], input="\n".join(world_rows) + "\n",
                            text=True, capture_output=True, check=True)
    world_vertices = [tuple(map(float, line.split()))
                      for line in actual.stdout.splitlines()]
    if len(world_vertices) != len(output_vertices):
        raise AssertionError(("transformed vertex count", len(world_vertices),
                              len(output_vertices)))
    errors = []
    for address, _, _ in output_vertices:
        native = tuple(value / 65536.0 for value in
                       struct.unpack("<3i", uc.mem_read(address, 12)))
        expected = world_vertices[len(errors)]
        errors.append(max(abs(a - b) for a, b in zip(native, expected)))
    worst = max(range(len(errors)), key=errors.__getitem__)
    return (len(model_pieces), len(output_vertices), errors[worst],
            output_vertices[worst], model_pieces)


def discover_flying_cohort():
    """Return all canfly FBI/COB pairs with mover methods and non-mover flyers."""
    cohort = []
    outside = []
    missing = []
    for fbi_path in sorted(UNIT_ROOT.glob("*.fbi")):
        unit_name = fbi_path.stem.lower()
        info = fbi_info(unit_name)
        if not bool(float(info.get("canfly", "0"))):
            continue
        cob_path = SCRIPT_ROOT / f"{unit_name}.cob"
        model_path = MODEL_ROOT / f"{info.get('objectname', unit_name).lower()}.3do"
        if not cob_path.is_file():
            missing.append((unit_name, str(cob_path)))
        if not model_path.is_file():
            missing.append((unit_name, str(model_path)))
        if cob_path.is_file() and cob_methods(unit_name).intersection(CALLBACKS):
            cohort.append(unit_name)
        else:
            outside.append(unit_name)
    if missing:
        raise FileNotFoundError(("missing shipped flying-unit COB/3DO assets", missing))
    return cohort, outside


def check_unit(unit_name, sample_tick, script_binary, model_binary):
    info = fbi_info(unit_name)
    if not bool(float(info.get("canfly", "0"))):
        raise AssertionError(f"{unit_name.upper()} FBI is not a flying unit")
    cob_path = SCRIPT_ROOT / f"{unit_name}.cob"
    object_name = info.get("objectname", unit_name).lower()
    model_path = MODEL_ROOT / f"{object_name}.3do"
    if not cob_path.is_file() or not model_path.is_file():
        raise FileNotFoundError((unit_name, cob_path, model_path))
    height = 100
    max_speed = int(float(info["maxvelocity"]) * 65536)
    sequence = [
        {"target": (800, height, 800)},
        {"target": (800, height, 800)},
        {"target": (-800, height, 800), "heading": 32768},
        {"target": (-800, height, 800), "heading": 32768},
        {"target": (160, height, 160), "speed": 0},
        {"target": (160, height, 160), "speed": 0},
        {"target": (800, height, -800), "speed": max_speed, "heading": 0},
        {"target": (800, height, -800), "mode": 1, "state_mode": 1},
        {"target": (800, height, -800), "mode": 2, "state_mode": 2},
    ]
    if not 1 <= sample_tick < len(sequence):
        raise ValueError(f"--sample-tick must be between 1 and {len(sequence) - 1}")
    native = native_transition_trace(unit_name, sequence)
    if len(native["snapshots"]) != len(sequence) + 1:
        raise AssertionError(("native tick snapshots", len(native["snapshots"])))

    first_calls = [(name, values) for tick, name, values in native["events"] if tick == 0]
    accepted_names = {name for _, name, _ in native["accepted_events"]}
    if not accepted_names:
        raise AssertionError((f"{unit_name} native mover emitted no COB callbacks",
                             native["events"]))

    world = local_trace(script_binary, unit_name, cob_path, 0,
                        native["events"], len(sequence))
    if len(world) != len(native["snapshots"]):
        raise AssertionError(("native/World snapshot count", len(native["snapshots"]),
                              len(world)))
    if world != native["snapshots"]:
        for tick, (expected, actual) in enumerate(zip(native["snapshots"], world)):
            if expected != actual:
                field = next((index for index, values in enumerate(zip(expected, actual))
                              if values[0] != values[1]), None)
                raise AssertionError(("native/World COB state", tick, field,
                                      expected[field] if field is not None else None,
                                      actual[field] if field is not None else None))
        raise AssertionError("native/World COB state mismatch without a differing field")

    header = struct.unpack_from("<10I", cob_path.read_bytes())
    piece_names = names_at(cob_path.read_bytes(), header[8], header[2])
    piece_index = {name.lower(): index for index, name in enumerate(piece_names)}
    piece_start = 2 + header[4] + 16 * 41
    words_per_piece_snapshot = 25  # 19 native records + six captured pose words

    def tick_pose(tick):
        row = native["snapshots"][tick]
        pose = [row[piece_start + index * words_per_piece_snapshot + 19:
                    piece_start + index * words_per_piece_snapshot + 25]
                for index in range(header[2])]
        return pose

    before = tick_pose(sample_tick - 1)
    pose = tick_pose(sample_tick)
    model_root = parse_object(model_path.read_bytes(), 0)
    model_piece_names = object_names(model_root)
    matched = {name.lower() for name in model_piece_names}.intersection(piece_index)
    if not matched:
        raise AssertionError((f"{unit_name} COB/model piece names do not overlap",
                              len(piece_names), len(model_piece_names)))
    animated = sorted(name for name in matched
                      if any(pose[piece_index[name]]) or any(before[piece_index[name]]))
    advanced = sorted(name for name in animated
                      if before[piece_index[name]] != pose[piece_index[name]])

    heading = native["body_angles"][sample_tick][1]
    pieces, vertices, error, worst, _ = compare_geometry(
        cob_path, model_path, pose, heading, model_binary)
    if error >= 0.001:
        raise AssertionError(("native/World model transform mismatch", error, worst))
    return {
        "unit": unit_name,
        "events": first_calls,
        "snapshots": len(native["snapshots"]),
        "animated": advanced,
        "matched_pose_channels": animated,
        "pieces": pieces,
        "vertices": vertices,
        "error": error,
        "worst": worst,
        "heading": heading,
    }


def print_result(result, sample_tick):
    unit_name = result["unit"]
    advanced = result["animated"]
    if advanced:
        print(f"PASS {unit_name.upper()}: pose advances in {len(advanced)} named "
              f"3DO pieces at boundary {sample_tick}; native/World COB state matches "
              f"{result['snapshots']} boundaries; native model transform matches "
              f"World ({result['pieces']} pieces/{result['vertices']} vertices, "
              f"max delta {result['error']:.8f} at {result['worst'][1]} vertex "
              f"{result['worst'][2]}, heading {result['heading']})")
    else:
        names = ",".join(result["matched_pose_channels"][:8]) or "none"
        print(f"NO_MATCHED_POSE {unit_name.upper()}: no named 3DO pose changed at "
              f"boundary {sample_tick} (matched pose channels present: {names}); native/World "
              f"COB state matches {result['snapshots']} boundaries, and the static native "
              f"model transform matches World ({result['pieces']} pieces/"
              f"{result['vertices']} vertices, max delta {result['error']:.8f})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--script-binary", default="build-o2/retail_script_test")
    parser.add_argument("--model-binary", default="build-o2/model_transform_test")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--unit",
                      help="one canfly unit with a shipped COB and 3DO (default: verball)")
    mode.add_argument("--all", action="store_true",
                      help="check every canfly COB that declares a mover callback")
    parser.add_argument("--expect-count", type=int, default=26,
                        help="required --all roster size (0 disables; default: %(default)s)")
    parser.add_argument("--sample-tick", type=int, default=SAMPLE_TICK,
                        help="native COB boundary to inspect (default: %(default)s)")
    args = parser.parse_args()
    if not 1 <= args.sample_tick <= 8:
        parser.error("--sample-tick must be between 1 and 8")

    if args.all:
        cohort, outside = discover_flying_cohort()
        if args.expect_count and len(cohort) != args.expect_count:
            raise SystemExit(f"expected {args.expect_count} callback-bearing flying "
                             f"COB/FBI pairs, found {len(cohort)}: {cohort}")
        print(f"FLYING_MOVER_COB_ROSTER {len(cohort)}: {' '.join(cohort)}", flush=True)
        for name in outside:
            print(f"OUTSIDE_MOVER_COHORT {name.upper()}: no declared native mover "
                  f"callback; shipped COB/3DO assets present")
        results = [check_unit(name, args.sample_tick, args.script_binary,
                              args.model_binary) for name in cohort]
        for result in results:
            print_result(result, args.sample_tick)
        pose_count = sum(bool(result["animated"]) for result in results)
        static_count = len(results) - pose_count
        worst = max(results, key=lambda result: result["error"])
        print(f"ROSTER PASS: {len(results)}/{len(cohort)} native/World COB and model "
              f"transform checks passed; {pose_count} pose-advancing, {static_count} "
              f"without a matched pose change in this fixture; worst transform delta "
              f"{worst['error']:.8f} ({worst['unit'].upper()})")
    else:
        cohort, outside = discover_flying_cohort()
        unit_name = (args.unit or "verball").lower()
        if unit_name not in cohort:
            if unit_name in outside:
                raise SystemExit(f"{unit_name.upper()} is outside the native mover-COB "
                                 "cohort; no mover callback is declared")
            raise SystemExit(f"{unit_name.upper()} is not in the shipped flying mover-COB "
                             "cohort")
        result = check_unit(unit_name, args.sample_tick, args.script_binary,
                            args.model_binary)
        print_result(result, args.sample_tick)
        print("LIMITS: controlled flight mover inputs and unit services; no pathfinding, "
              "camera/projection, texture, framebuffer, or GUI comparison.")


if __name__ == "__main__":
    main()
