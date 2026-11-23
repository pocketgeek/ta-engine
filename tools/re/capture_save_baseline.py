#!/usr/bin/env python3
"""Read a paused retail state and compare movement fields with a preserved save.

Read-only polling, not an atomic process snapshot. Two identical observations
are required. A later tick is reported as a mismatch, never silently aligned.
"""
import argparse
import json
from pathlib import Path
import time

from decode_save_state import decode
from livesample import Mem, find_pid, snapshot, verify_code


def compare(saved, live):
    differences = []
    if saved['tick'] != live['tick']:
        differences.append({'field': 'tick', 'saved': saved['tick'], 'live': live['tick']})
    expected = {u['id']: u for u in saved['units']}
    observed = {u['id']: u for u in live['units']}
    for uid in sorted(expected.keys() | observed.keys()):
        if uid not in expected or uid not in observed:
            differences.append({'id': uid, 'field': 'presence',
                                'saved': uid in expected, 'live': uid in observed})
            continue
        unit, actual = expected[uid], observed[uid]
        for field, live_field in (('player', 'player'), ('position_raw', 'position_raw'),
                                  ('heading', 'heading'), ('base_speed_raw', 'base_speed_raw'),
                                  ('footprint_origin', 'cell_origin'), ('footprint_size', 'footprint')):
            if unit[field] != actual[live_field]:
                differences.append({'id': uid, 'field': field,
                                    'saved': unit[field], 'live': actual[live_field]})
        for field, live_field in (('speed_raw', 'speed_raw'),
                                  ('refusal_deadline_raw', 'refusal_deadline'),
                                  ('flags', 'movement_flags')):
            if 'movement' in unit and unit['movement'][field] != actual.get(live_field):
                differences.append({'id': uid, 'field': 'movement.' + field,
                                    'saved': unit['movement'][field], 'live': actual.get(live_field)})
    return differences


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('save', type=Path)
    parser.add_argument('--pid', type=int)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists() or not args.output.parent.is_dir():
        parser.error('output must be a new file in an existing directory')
    saved = decode(args.save.read_bytes())
    mem = Mem(args.pid if args.pid is not None else find_pid())
    try:
        code = verify_code(mem)
        first = snapshot(mem)
        time.sleep(0.15)
        second = snapshot(mem)
        if first is None or first != second or first['unreadable_units']:
            raise RuntimeError('retail state changed during capture or units were unreadable; pause and retry')
        if first['rng_before'] != first['rng_after']:
            raise RuntimeError('RNG changed during capture; pause and retry')
        differences = compare(saved, first)
        report = {'schema': 1, 'source_sha256': saved['source_sha256'], 'pid': mem.pid,
                  'capture': 'two equal read-only observations; not atomic', 'code': code,
                  'live': first, 'differences': differences,
                  'movement_fields_match': not differences,
                  'complete_state_match': False}
    finally:
        mem.close()
    with args.output.open('x') as output:
        json.dump(report, output, indent=2)
        output.write('\n')
    print(json.dumps({'tick': first['tick'], 'rng': first['rng_before'],
                      'differences': len(differences), 'output': str(args.output)}))


if __name__ == '__main__':
    main()
