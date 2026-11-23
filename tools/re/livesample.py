#!/usr/bin/env python3
"""Observe a running retail KINGDOMS.icd under Wine/Proton, without writing it.

  python3 tools/re/livesample.py [pid]
  python3 tools/re/livesample.py [pid] --watch --duration 60 --output /tmp/retail.jsonl

The live entity table is G+0x14e84 .. G+0x14e88, stride 0x138. G+0x19edc,
stride 0x140, is a DIFFERENT table used by the terrain/occupancy raters; the
old sampler read that table with guessed position offsets. Actual entity world
coordinates are signed 16.16 at +0x68/+0x6c/+0x70; heading is uint16 at +0x7e.
These fields and the movement object at +8 are read by retail's mover/query.

JSONL contains decoded game state, never executable code. Tick-before/after
checks reject reads spanning a tick, but these are polling observations, NOT
atomic debugger snapshots or a guarantee of capturing every simulation tick.
The footer reports observed tick deltas. The game is never paused or modified.
"""
import argparse
from collections import Counter
import glob
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import time

GAMESTATE_PTR = 0x62d55c
RNG_STATE = 0x64186c
STRIDE = 0x138
MAX_SLOTS = 20000
REFERENCE = Path(__file__).resolve().parents[2] / 'assets/game/KINGDOMS.icd'
CODE_RANGES = ((0x4db640, 0x2a8), (0x507d10, 0x29a), (0x4daee1, 0x45b),
               (0x535cc0, 0x84), (0x512430, 0x2b))


def u32(data, offset=0):
    return struct.unpack_from('<I', data, offset)[0]


def find_pid():
    candidates = []
    for directory in glob.glob('/proc/[0-9]*'):
        try:
            # Inspect mappings, not launcher command lines that mention the game.
            mappings = Path(directory, 'maps').read_text().lower()
            if any('kingdoms.icd' in line and line.startswith('00400000-')
                   for line in mappings.splitlines()):
                candidates.append(int(Path(directory).name))
        except OSError:
            continue
    if len(candidates) != 1:
        raise RuntimeError(f'expected one mapped retail process, found {candidates}; specify its PID')
    return candidates[0]


class Mem:
    def __init__(self, pid):
        self.pid = pid
        self.fd = os.open(f'/proc/{pid}/mem', os.O_RDONLY)

    def close(self):
        os.close(self.fd)

    def read(self, address, size):
        data = os.pread(self.fd, size, address)
        if len(data) != size:
            raise RuntimeError(f'short game-memory read at {address:#x}')
        return data

    def u32(self, address):
        return u32(self.read(address, 4))


def verify_code(mem):
    # These ranges are all in .text, whose RVA equals its file offset in this
    # reference PE. Compare bytes transiently; emit only addresses and hashes.
    binary = REFERENCE.read_bytes()
    result = []
    for address, size in CODE_RANGES:
        expected = binary[address - 0x400000:address - 0x400000 + size]
        actual = mem.read(address, size)
        if actual != expected:
            raise RuntimeError(f'retail code differs at {address:#x}; do not use these field offsets')
        result.append({'address': address, 'size': size,
                       'sha256': hashlib.sha256(actual).hexdigest()})
    return result


def header(mem):
    game = mem.u32(GAMESTATE_PTR)
    if not game:
        raise RuntimeError('retail has no active game state')
    base, end = struct.unpack('<II', mem.read(game + 0x14e84, 8))
    width, height = struct.unpack('<II', mem.read(game + 0x19e98, 8))
    count, remainder = divmod(end - base, STRIDE)
    if not base or not 0 < count <= MAX_SLOTS or remainder:
        raise RuntimeError(f'invalid live entity table {base:#x}..{end:#x}')
    if not (0 < width <= 4096 and 0 < height <= 4096):
        raise RuntimeError(f'invalid map dimensions {width}x{height}')
    return game, base, count, width, height


def snapshot(mem):
    game, base, count, width, height = header(mem)
    tick = mem.u32(game + 0x19f44)
    rng_before = mem.u32(RNG_STATE)
    records = mem.read(base, count * STRIDE)
    units, unreadable, types = [], 0, {}
    for slot in range(count):
        record = memoryview(records)[slot * STRIDE:(slot + 1) * STRIDE]
        flags = u32(record, 0x130)
        if not flags & 0x1000000:
            continue
        uid = struct.unpack_from('<H', record, 2)[0]
        if uid != slot:
            raise RuntimeError(f'entity slot/id mismatch: {slot}/{uid}')
        x, y, z = struct.unpack_from('<iii', record, 0x68)
        fx, fz = struct.unpack_from('<hh', record, 0x78)
        cx, cz = struct.unpack_from('<hh', record, 0x74)
        unit = {'id': uid, 'position_raw': [x, y, z],
                'x': x / 65536, 'y': y / 65536, 'z': z / 65536,
                'footprint': [fx, fz], 'cell_origin': [cx, cz],
                'heading': struct.unpack_from('<H', record, 0x7e)[0],
                'flags': flags, 'type_address': u32(record, 0xb4),
                'base_speed_raw': struct.unpack_from('<i', record, 0x12b)[0]}
        movement, owner = u32(record, 8), u32(record, 0xb8)
        try:
            type_address = unit['type_address']
            if type_address and type_address not in types:
                types[type_address] = struct.unpack('<i', mem.read(type_address + 0x162, 4))[0]
            unit['nominal_speed_raw'] = types.get(type_address)
            unit['player'] = mem.read(owner + 0xeb, 1)[0] if owner else None
            if movement:
                nav = mem.read(movement + 0x20, 0x18)
                unit.update(speed_raw=struct.unpack_from('<i', nav)[0],
                            movement_tick=u32(nav, 0xc), refusal_deadline=u32(nav, 0x10),
                            movement_flags=struct.unpack_from('<H', nav, 0x16)[0])
            else:
                unit['speed_raw'] = None
        except OSError:
            # The unit may have been removed while the game updates this frame.
            unreadable += 1
            continue
        units.append(unit)
    rng_after = mem.u32(RNG_STATE)
    if mem.u32(game + 0x19f44) != tick or mem.u32(GAMESTATE_PTR) != game:
        return None
    return {'kind': 'frame', 'tick': tick, 'map_cells': [width, height],
            'rng_before': rng_before, 'rng_after': rng_after,
            'unreadable_units': unreadable, 'units': units}


def record(mem, output, duration, hz):
    emit = lambda value: print(json.dumps(value, separators=(',', ':')), file=output, flush=True)
    emit({'kind': 'metadata', 'schema': 2, 'pid': mem.pid,
          'unix_time': time.time(), 'code': verify_code(mem),
          'capture': 'read-only polling; not an atomic snapshot', 'sample_hz': hz})
    start = time.monotonic()
    last_tick = None
    frames = torn = 0
    tick_deltas = Counter()
    moved = set()
    first_positions = {}
    while time.monotonic() - start < duration:
        sample_start = time.monotonic()
        frame = snapshot(mem)
        if frame is None:
            torn += 1
        elif frame['tick'] != last_tick:
            if last_tick is not None:
                if frame['tick'] < last_tick:
                    raise RuntimeError('game tick reset; start a new capture for this match')
                # Retail's counter advanced by TWO in almost every captured
                # frame of the live terrain run. A delta of two is not evidence
                # of a missed frame; retain the distribution without assuming Hz.
                tick_deltas[frame['tick'] - last_tick] += 1
            for unit in frame['units']:
                point = unit['position_raw']
                first = first_positions.setdefault(unit['id'], point)
                if point != first:
                    moved.add(unit['id'])
            last_tick = frame['tick']
            frame['elapsed'] = sample_start - start
            emit(frame)
            frames += 1
        time.sleep(max(0, 1 / hz - (time.monotonic() - sample_start)))
    footer = {'kind': 'end', 'frames': frames, 'tick_delta_counts': dict(tick_deltas),
              'rejected_torn_samples': torn, 'moved_unit_ids': sorted(moved)}
    emit(footer)
    print(json.dumps(footer), file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pid', nargs='?', type=int)
    parser.add_argument('--watch', action='store_true')
    parser.add_argument('--duration', type=float, default=30)
    parser.add_argument('--hz', type=float, default=60)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if not 0 < args.duration <= 3600 or not 0 < args.hz <= 240:
        parser.error('duration must be 0..3600 seconds and hz 0..240')
    mem = None
    try:
        mem = Mem(args.pid if args.pid is not None else find_pid())
        if args.watch:
            if args.output:
                # Never overwrite a previous observation accidentally.
                with args.output.open('x') as output:
                    record(mem, output, args.duration, args.hz)
            else:
                record(mem, sys.stdout, args.duration, args.hz)
        else:
            print(json.dumps({'pid': mem.pid, 'code': verify_code(mem)}, indent=2))
            for _ in range(3):
                frame = snapshot(mem)
                if frame is not None:
                    print(json.dumps(frame, indent=2))
                    break
            else:
                raise RuntimeError('could not obtain a frame within one game tick')
    except (OSError, RuntimeError, ValueError, struct.error) as exc:
        print(f'livesample: {exc}', file=sys.stderr)
        return 1
    finally:
        if mem is not None:
            mem.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
