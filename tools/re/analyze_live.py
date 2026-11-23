#!/usr/bin/env python3
"""Summarize decoded livesample JSONL; no retail executable is needed.

Usage: python3 tools/re/analyze_live.py trace.jsonl
Origin comparisons use signed fixed-point coordinates without float rounding.
Tick deltas are observations, not counts of missed simulation frames.
"""
import argparse
from collections import Counter, defaultdict
import json


def analyze(records):
    frames = observations = retail_errors = port_errors = 0
    deltas = Counter()
    speeds = defaultdict(set)
    previous_tick = None
    for frame in records:
        if frame['kind'] != 'frame':
            continue
        frames += 1
        if previous_tick is not None:
            deltas[frame['tick'] - previous_tick] += 1
        previous_tick = frame['tick']
        for unit in frame['units']:
            observations += 1
            x, _, z = unit['position_raw']
            fx, fz = unit['footprint']
            retail = [(p - (f - 1) * 8 * 65536) // (16 * 65536)
                      for p, f in ((x, fx), (z, fz))]
            # Recorded units are at positive coordinates; this models the
            # port's floor-to-pixels, /16, subtract-half-footprint convention.
            port = [int((p // 65536) / 16) - f // 2
                    for p, f in ((x, fx), (z, fz))]
            retail_errors += retail != unit['cell_origin']
            port_errors += port != unit['cell_origin']
            speeds[unit['type_address']].add(unit['base_speed_raw'])
    return {'frames': frames, 'unit_observations': observations,
            'retail_origin_mismatches': retail_errors,
            'legacy_port_origin_mismatches': port_errors,
            'observed_tick_deltas': dict(sorted(deltas.items())),
            'base_speeds_by_type_address': {
                hex(key): sorted(values) for key, values in sorted(speeds.items())}}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('trace')
    args = parser.parse_args()
    with open(args.trace) as source:
        print(json.dumps(analyze(json.loads(line) for line in source if line.strip()),
                         indent=2))
