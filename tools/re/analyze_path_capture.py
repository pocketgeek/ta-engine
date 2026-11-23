#!/usr/bin/env python3
"""Decode scheduler/RNG observations without treating a partial capture as a replay.

Output contains retail-derived state; keep it beside the gitignored capture.
"""
import argparse
import base64
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import zlib


def analyze(capture):
    frames = capture['frames']
    if capture['status'] != 'captured' or not frames:
        raise ValueError('a successful capture is required')
    if any(b['tick'] != a['tick'] + 1 for a, b in zip(frames, frames[1:])):
        raise ValueError('nonconsecutive frames')
    addresses = {u['entity_address']: u['id'] for u in frames[0]['units']
                 if 'entity_address' in u}
    initial = frames[0]['path_state']
    players = bytes.fromhex(initial['players_hex'])
    if len(players) != 10 * 0x110:
        raise ValueError('invalid player records')
    buffers = []
    for entry in initial['buffers']:
        size = entry['size']
        if not 0 <= size <= 32 * 1024 * 1024:
            raise ValueError('oversize path buffer')
        decoder = zlib.decompressobj()
        raw = decoder.decompress(base64.b64decode(entry['zlib_base64'], validate=True), size + 1)
        if len(raw) != size or not decoder.eof or decoder.unused_data:
            raise ValueError('invalid compressed path buffer')
        buffers.append({'name': entry['name'], 'size': size,
                        'sha256': hashlib.sha256(raw).hexdigest()})
    scheduler = []
    routes = []
    previous = {}
    for frame in frames:
        state = frame['path_state']
        raw = bytes.fromhex(state['fields_hex'])
        if len(raw) != 0x22b:
            raise ValueError('invalid scheduler record')
        u32 = lambda offset: struct.unpack_from('<I', raw, offset)[0]
        i32 = lambda offset: struct.unpack_from('<i', raw, offset)[0]
        scheduler.append({
            'tick': frame['tick'], 'active_id': addresses.get(u32(0x58)),
            'active_address': u32(0x58), 'phase': u32(0x5c),
            'contour_phase': u32(0x60), 'player_cursor': raw[0x114],
            'entity_cursors': [addresses.get(u32(0x115 + p*4)) for p in range(10)],
            'player_work_remaining': [i32(0x13d + p*4) for p in range(10)],
            'total_work_remaining': i32(0x165), 'last_iteration_work': u32(0x48),
            'pending': state['pending_per_player'],
            'heap_count': u32(0x14), 'heap_removed': u32(0x18),
            'processed': u32(0x191), 'node_limit': u32(0xec),
            'retries': u32(0x1ad), 'adjusted_budget': u32(0x225),
        })
        for unit in frame['units']:
            identity = unit['id']
            route = {k: unit.get(k) for k in ('route_world', 'route_tick', 'route_flags', 'route_outcome')}
            if identity in previous and route != previous[identity]:
                routes.append({'tick': frame['tick'], 'id': identity, **route,
                               # Stamp changes identify deliveries/re-asks; point
                               # changes alone also include waypoint consumption.
                               'stamp_changed': route['route_tick'] != previous[identity]['route_tick']})
            previous[identity] = route
    rng = []
    for call in capture['rng_calls']:
        refs = {reg: addresses[value] for reg, value in call.get('registers', {}).items()
                if value in addresses}
        # Register roles are proven at these call sites only. Other matches are
        # candidates, not an attribution (scratch registers can retain pointers).
        owner_reg = {0x402dc2: 'esi', 0x407873: 'edi', 0x4e5482: 'edi'}.get(call['return_address'])
        rng.append({'tick': call['tick'], 'caller': hex(call['return_address']),
                    'bound': call['bound'], 'seed_before': call['seed_before'],
                    'id': refs.get(owner_reg), 'entity_register_matches': refs})
    counts = Counter((event['caller'], event['bound'], event['id']) for event in rng)
    return {'schema': 1, 'source_sha256': capture['source_sha256'],
            'complete_state_match': False, 'buffers': buffers,
            'scheduler': scheduler, 'route_changes': routes, 'rng_calls': rng,
            'rng_totals': [{'caller': caller, 'bound': bound, 'id': identity, 'count': count}
                           for (caller, bound, identity), count in counts.items()]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = analyze(json.loads(args.capture.read_text()))
    with args.output.open('x') as output:
        json.dump(result, output, indent=2)
        output.write('\n')
    print(json.dumps({'frames': len(result['scheduler']), 'rng_calls': len(result['rng_calls']),
                      'route_changes': len(result['route_changes']),
                      'initial_search': result['scheduler'][0], 'first_rng': result['rng_calls'][:1]}, indent=2))


if __name__ == '__main__':
    main()
