#!/usr/bin/env python3
"""Compare full muzzle/SweetSpot piece queries with an offline retail capture.

4dd2a0 (SweetSpot) bounds the unit's per-piece vertex buffer, which the draw
path rewrites in place (4ee620 -> 4ee920/4eea20). A unit drawn since it was
created or loaded is checked against `--bounds` (the drawn pose the simulation
uses) to within --tolerance raw units: retail transforms in x87 floats, the port
in integers. A unit never drawn (every buffer still equals its definition, as
in the reload captures) is checked exactly against `--local-bounds`.

Reads user-owned assets and memory locally. Temporary script state is deleted;
no executable, asset or captured-memory bytes are exported into the repository.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from emu import HEAP
from emureload import CapturedProcess

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('capture', type=Path)
ap.add_argument('--unit', type=int, required=True)
ap.add_argument('--cob', type=Path, required=True)
ap.add_argument('--model', type=Path, required=True)
ap.add_argument('--binary', default='build-o2/retail_script_test')
ap.add_argument('--tolerance', type=int, default=1024,
                help='largest drawn-bounds difference per axis, raw 16.16 units')
args = ap.parse_args()
p = CapturedProcess(json.loads(args.capture.read_text()))
p.icd.freeze_hooks()
unit = next(u['address'] for u in p.runtime['units'] if u['id'] == args.unit)
vm = p.u32(unit + 0xbc)
vtable = p.u32(vm)
header = struct.unpack_from('<10I', args.cob.read_bytes())
count, statics = header[2], header[4]
assert p.u32(p.u32(vm + 0xc) + 8) == count, 'captured script piece count differs'
state = bytearray(0xa48 + statics * 4 + count * 108)
for piece in range(count):
    for method, offset in ((24, 72), (28, 84)):
        for axis in range(3):
            value, error = p.icd.call(p.u32(vtable + method), (piece, axis), ecx=vm)
            if error:
                raise RuntimeError(error)
            struct.pack_into('<I', state, 0xa48 + statics * 4 + piece * 108 + offset + axis * 4, value)
roll, heading, pitch = struct.unpack('<3H', p.uc.mem_read(unit + 0x7c, 6))
model = p.u32(unit + 0xc0)
drawn = False
for index in range(p.u32(model)):
    entry = model + 0x1cc + index * 0x38
    definition = p.u32(entry)
    size = p.u32(definition + 4) * 12
    if size and p.uc.mem_read(p.u32(entry + 0x24), size) != p.uc.mem_read(p.u32(definition + 0x24), size):
        drawn = True
worst = 0
position = struct.unpack('<3i', p.uc.mem_read(unit + 0x68, 12))
with tempfile.TemporaryDirectory(prefix='tak-captured-aim-') as directory:
    saved = Path(directory) / 'poses.state'
    saved.write_bytes(state)
    for piece in range(count):
        for mode, entry, call_args in (
                ('origin', 0x4dd0f0, (HEAP + 0x300000, unit, piece)),
                ('bounds', 0x4dd2a0, (unit, HEAP + 0x300000, piece))):
            _, error = p.icd.call(entry, call_args)
            if error:
                raise RuntimeError(error)
            native = struct.unpack('<3i', p.uc.mem_read(HEAP + 0x300000, 12))
            if mode == 'bounds':
                native = tuple(a - b for a, b in zip(native, position))
            option = mode if mode == 'origin' or drawn else 'local-bounds'
            result = subprocess.check_output([args.binary, '--' + option, str(args.cob),
                str(args.model), str(saved), str(heading), str(piece), str(pitch), str(roll)], text=True)
            actual = tuple(map(int, result.split()))
            if option == 'bounds':
                error = max(abs(a - b) for a, b in zip(actual, native))
                worst = max(worst, error)
                assert error <= args.tolerance, (args.unit, piece, mode, actual, native)
            else:
                assert actual == native, (args.unit, piece, mode, actual, native)
kind = f'drawn (worst {worst} raw)' if drawn else 'undrawn'
print(f'PASS: all {count} animated piece origins and {count} {kind} SweetSpot bounds match captured retail unit {args.unit}')
