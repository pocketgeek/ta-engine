#!/usr/bin/env python3
"""Compare native COB display poses through retail's real 3DO model refresh.

This headless check runs Zonhunt's shipped COB in KINGDOMS.icd, captures the
current piece poses emitted by native 56d850, maps those values by piece name
onto the shipped Zonhunt 3DO hierarchy, and calls native 0x4ee620. Its output
vertices are compared with the World model transform helper for identical
authored geometry, poses, and body heading. This checks the native COB-to-3DO
geometry seam; it does not claim framebuffer, texture, or raster parity.
"""
import argparse
import struct
import subprocess
from pathlib import Path

from emu import HEAP
from check_zhon_construction_flight_trace import native_persistent_trace
from probe_zhon_construction_pose_join import native_display_root_state
from unicorn.x86_const import UC_X86_REG_FPCW


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_COB = ROOT / "assets/extracted/all/scripts/zonhunt.cob"
DEFAULT_MODEL = ROOT / "assets/extracted/all/objects3d/zonhunt.3do"


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


def compare(cob_path, model_path, tick, seed, binary):
    cob_data = cob_path.read_bytes()
    cob_header = struct.unpack_from("<10I", cob_data)
    piece_count = cob_header[2]
    piece_names_offset = cob_header[8]
    piece_names = names_at(cob_data, piece_names_offset, piece_count)
    piece_index = {name.lower(): index for index, name in enumerate(piece_names)}

    metadata, movement, _, callbacks = native_persistent_trace(tick, seed)
    if metadata["terrain"] != 100:
        raise AssertionError(("unexpected native construction fixture terrain",
                              metadata["terrain"]))
    sampled, p, native_piece_index, unit = native_display_root_state(
        cob_path, movement, callbacks, (tick,), capture_context=True)
    if native_piece_index != piece_index:
        raise AssertionError("native COB piece table changed between probes")
    pose = sampled[tick]["piece_pose"]
    heading = movement[tick - 1][4] & 0xFFFF

    model_data = model_path.read_bytes()
    root = parse_object(model_data, 0)
    # The installed renderer stores 3DO-authored vertices in its mirrored X/Z
    # basis. Reserve disjoint emulated ranges for definitions, instance pieces,
    # and transformed output vertices.
    definition_cursor = HEAP + 0x240000
    state_cursor = HEAP + 0x280000
    output_cursor = HEAP + 0x2A0000
    output_vertices = []
    world_rows = []
    model_pieces = []

    def put(address, *values):
        p.uc.mem_write(address, struct.pack("<" + "I" * len(values),
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
            p.uc.mem_write(authored_vertices, b"".join(
                struct.pack("<3i", -vx, vy, -vz)
                for vx, vy, vz in obj["vertices"]))

        put(state, definition)
        put(state + 0x24, output)
        put(state + 0x28, 0)  # invalidate the piece cache before native refresh
        piece = piece_index.get(obj["name"].lower())
        current = pose[piece] if piece is not None else [0] * 6
        p.uc.mem_write(state + 4, struct.pack("<3i", *(signed(v) for v in current[:3])))
        p.uc.mem_write(state + 0x10, struct.pack("<3H", *(v & 0xFFFF for v in current[3:])))

        piece_chain = ancestors + [(obj["offset"], current)]
        for vertex_index, vertex in enumerate(obj["vertices"]):
            row = []
            for offset, current_pose in piece_chain:
                row.extend(offset)
                row.extend(current_pose[:3])
                row.extend(current_pose[3:])
                row.extend((0, 0, 0))  # authored vertex; filled on the leaf
                row.extend((0, 0, 0))  # root body pitch, heading, roll
            leaf_base = (len(piece_chain) - 1) * 15
            row[leaf_base + 9:leaf_base + 12] = vertex
            row[12:15] = (0, heading, 0)
            world_rows.append(" ".join(map(str, (len(piece_chain), *row))))
            output_vertices.append((output + vertex_index * 12,
                                    obj["name"], vertex_index))

        model_pieces.append(obj["name"])
        children = [build(child, piece_chain) for child in obj["children"]]
        if children:
            put(state + 0x30, children[0]["state"])
        for left, right in zip(children, children[1:]):
            put(left["state"] + 0x2C, right["state"])
        return {"state": state}

    instance = build(root, [])
    wrapper = HEAP + 0x230000
    put(unit + 0xC0, wrapper)
    put(wrapper + 8, 1)
    put(wrapper + 0x1C8, instance["state"])
    # Retail Unit body angles are roll, heading, pitch.
    p.uc.mem_write(unit + 0x7C, struct.pack("<3H", 0, heading, 0))
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027F)
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
                       struct.unpack("<3i", p.uc.mem_read(address, 12)))
        expected = world_vertices[len(errors)]
        errors.append(max(abs(a - b) for a, b in zip(native, expected)))
    worst = max(range(len(errors)), key=errors.__getitem__)
    return len(model_pieces), len(errors), errors[worst], output_vertices[worst], \
        sum(any(item) for item in pose), piece_count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cob", type=Path, default=DEFAULT_COB)
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("--tick", type=int, default=101)
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=1)
    parser.add_argument("--binary", default="build-o2/model_transform_test")
    args = parser.parse_args()
    if args.cob.stem.lower() != "zonhunt" or args.model.stem.lower() != "zonhunt":
        parser.error("this focused construction-flight check requires Zonhunt COB and 3DO")
    if not 1 <= args.tick <= 103:
        parser.error("--tick must be in 1..103 for the bounded native construction trace")
    pieces, vertices, error, worst, animated, total = compare(
        args.cob, args.model, args.tick, args.seed, args.binary)
    if error >= 0.001:
        raise AssertionError(("native/World model transform mismatch", error, worst))
    print(f"PASS: native Zonhunt COB tick {args.tick} -> shipped 3DO 0x4ee620; "
          f"{pieces} pieces, {vertices} vertices, {animated}/{total} COB poses emitted; "
          f"max World-helper delta {error:.8f} at {worst[1]} vertex {worst[2]}")
    print("LIMITS: controlled flat-terrain flight fixture; this checks 3D geometry only, "
          "not final projection, textures, or framebuffer output.")


if __name__ == "__main__":
    main()
