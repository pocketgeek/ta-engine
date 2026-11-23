#!/usr/bin/env python3
"""Compare first captured turns with retail arithmetic aimed at saved cell goals.

This is a diagnostic counterfactual, not a replay: retail may steer at a path
point instead. Execute its angle routine to distinguish target selection from
CORDIC error. Read-only; redirect output into gitignored assets/tmp.
"""
import argparse
import json
from pathlib import Path

from decode_save_state import decode
from emu import Icd
from probe_saved_movement import movement_goal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('save', type=Path)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--turn-rate', type=int, required=True,
                        help='verified BAM/tick turn rate for the selected type')
    parser.add_argument('--type', default='zonter')
    args = parser.parse_args()
    if not 0 < args.turn_rate < 32768:
        parser.error('turn rate must be between 1 and 32767')
    saved = decode(args.save.read_bytes())
    capture = json.loads(args.capture.read_text())
    if capture['status'] != 'captured' or capture['source_sha256'] != saved['source_sha256']:
        raise ValueError('capture/save mismatch')
    first, second = capture['frames'][:2]
    if first['tick'] != saved['tick'] or second['tick'] != first['tick'] + 1:
        raise ValueError('capture must begin at saved tick with consecutive frames')
    before = {u['id']: u for u in first['units']}
    after = {u['id']: u for u in second['units']}
    icd = Icd()
    rows = []
    for unit in saved['units']:
        if unit['type'] != args.type or not unit['orders'] or unit['orders'][0]['type'] != 'Move_Ground':
            continue
        start, end = before[unit['id']], after[unit['id']]
        gx, gz = movement_goal(unit, unit['orders'][0])
        x, _, z = start['position_raw']
        # 0x51b340 passes source minus target to 0x53612a.
        angle, error = icd.call(0x53612a, (x - gx, z - gz))
        if error:
            raise RuntimeError(error)
        angle &= 65535
        diff = ((angle - start['heading'] + 32768) & 65535) - 32768
        predicted = (start['heading'] + max(-args.turn_rate, min(args.turn_rate, diff))) & 65535
        rows.append({'id': unit['id'], 'goal_raw': [gx, gz],
                     'initial_heading': start['heading'], 'direct_goal_heading': angle,
                     'direct_goal_next_heading': predicted, 'captured_next_heading': end['heading'],
                     'matches_direct_goal': predicted == end['heading']})
    print(json.dumps({'tick': second['tick'], 'complete_state': False,
                      'assumption': 'unmodified turn rate and direct cell-goal steering',
                      'units': rows}, indent=2))


if __name__ == '__main__':
    main()
