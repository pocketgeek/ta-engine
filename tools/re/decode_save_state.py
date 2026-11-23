#!/usr/bin/env python3
"""Export verified movement-related fields from retail version-44 BANK saves.

Partial state only: not a runnable replay, and no inferred RNG seed. Raw saved
positions/speeds are 16.16 integers. Keep exports under gitignored assets/tmp.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from inspect_save import directory, sections


def script_record(blob, num_statics=None, num_pieces=None):
    """Restore the portable part of 56dc00's script snapshot.

    Thread words retain runtime-offset names until their semantics are checked.
    +20 is a transient pointer explicitly cleared by the original loader.
    Counts must come from the owning COB, never be guessed from blob length.
    """
    if len(blob)<0xa48 or (len(blob)-0xa48)%4:
        raise ValueError('truncated saved script state')
    if (num_statics is None)!=(num_pieces is None):
        raise ValueError('both COB counts are required')
    result={'signature':struct.unpack_from('<I',blob)[0], 'threads':[],
            'runtime_a60':struct.unpack_from('<I',blob,0xa44)[0]}
    for index in range(16):
        words=list(struct.unpack_from('<41I',blob,4+index*0xa4))
        words[8]=0
        result['threads'].append({f'{offset*4:02x}':word for offset,word in enumerate(words)})
    if num_statics is None:
        result['static_piece_data_hex']=blob[0xa48:].hex()
    else:
        if num_statics<0 or num_pieces<0 or len(blob)!=0xa48+num_statics*4+num_pieces*0x6c:
            raise ValueError('saved script size disagrees with owning COB')
        result['statics']=list(struct.unpack_from(f'<{num_statics}i',blob,0xa48))
        start=0xa48+num_statics*4
        result['pieces']=[{f'{offset*4:02x}':word for offset,word in enumerate(
            struct.unpack_from('<27I',blob,start+index*0x6c))} for index in range(num_pieces)]
    return result


def movement_record(blob):
    """Saved mover fields, labelled by verified runtime offset where opaque.

    0x4dcae9..0x4dcb9e copies these fields. +2c is deliberately absent:
    the saved +20 word goes to runtime +28, not the movement tick at +2c.
    Only the low 13 flag bits are restored by the loader.
    """
    if len(blob) != 44:
        raise ValueError('unsupported saved movement record')
    fields = {}
    for runtime, source, fmt in (
            *[(r, r - 8, '<i') for r in range(8, 0x24, 4)],
            (0x24, 0x1c, '<h'), (0x26, 0x1e, '<h'),
            (0x28, 0x20, '<I'), (0x30, 0x24, '<I'),
            (0x34, 0x28, '<H')):
        fields[f'{runtime:02x}'] = struct.unpack_from(fmt, blob, source)[0]
    flags = struct.unpack_from('<H', blob, 0x2a)[0]
    return {'speed_raw': fields['20'], 'refusal_deadline_raw': fields['30'],
            'flags': flags, 'restored_flags': flags & 0x1fff,
            'runtime_fields': fields}


def order_record(blob):
    """Decode the common 82-byte mission record using retail's loader.

    Runtime-offset labels are intentional: identifying a field's destination
    in memory does not establish its meaning for every mission type.
    """
    if len(blob) != 82:
        raise ValueError('unsupported saved order record')
    values = {'owner_id': struct.unpack_from('<H', blob, 0)[0],
              'target_id': struct.unpack_from('<H', blob, 2)[0],
              'controller_kind': struct.unpack_from('<I', blob, 4)[0],
              'runtime_fields': {}}
    # 0x4d7107..0x4d718f, source buffer EBP-0x58.
    for runtime, source, fmt in ((0x04, 8, '<B'), (0x05, 9, '<B'),
                                 (0x06, 10, '<I'), (0x0a, 14, '<I'),
                                 *[(r, r - 0x10, '<I') for r in range(0x22, 0x5e, 4)],
                                 (0x6a, 78, '<I')):
        values['runtime_fields'][f'{runtime:02x}'] = struct.unpack_from(fmt, blob, source)[0]
    # +04 is resolved by the loader from the order name before copying these
    # fields; its saved byte is retained only as an observation.
    values['saved_type_byte'] = values['runtime_fields'].pop('04')
    return values


def unit_record(blob):
    # Retail writer 0x513b34..0x513e79; reader 0x513330..0x513944.
    # Writer buffer is EBP-0xf8, reader buffer EBP-0xf4; do not mix bases.
    if len(blob) != 235 or b'\0' not in blob[:32]:
        raise ValueError('unsupported saved unit record')
    return {
        'type': blob[:32].split(b'\0', 1)[0].decode('ascii'),
        'player': blob[0x20],
        'id': struct.unpack_from('<H', blob, 0x21)[0],
        'order_count': struct.unpack_from('<I', blob, 0x23)[0],
        'has_movement': bool(struct.unpack_from('<I', blob, 0x27)[0]),
        'position_raw': list(struct.unpack_from('<iii', blob, 0x2b)),
        'heading': struct.unpack_from('<H', blob, 0x39)[0],
        'footprint_origin': list(struct.unpack_from('<hh', blob, 0x83)),
        'footprint_size': list(struct.unpack_from('<hh', blob, 0x87)),
        'base_speed_raw': struct.unpack_from('<i', blob, 0xa3)[0],
    }


def decode(data):
    chunks = dict(sections(data))
    nodes = {node['name']: node for node in directory(data, chunks)}

    def blob_data(node, entry):
        start = node['offset'] + 32
        body = chunks.get(start)
        if body is None:
            size = struct.unpack_from('<I', data, node['offset'])[0]
            body = data[start:node['offset'] + size]
        return body[entry['body_offset']:entry['body_offset'] + entry['size']]

    units_node = nodes['Units']
    integers = {v['name']: v['value'] for v in units_node['integers']}
    if integers.get('Version') != 44:
        raise ValueError('only saved unit version 44 is decoded')
    strings = {v['name']: v['value'] for v in units_node['strings']}
    named_blobs = {v['name']: v for v in units_node['blobs'] if v['name'] is not None}
    units = []
    for entry in units_node['blobs']:
        if entry['name'] is not None:
            continue
        unit = unit_record(blob_data(units_node, entry))
        unit['save_index'] = entry['metadata']
        script=named_blobs.get(f"Script{unit['save_index']}")
        if script is not None:
            # Script entries precede their unit and carry the previous BANK
            # selection's metadata. The name, not that metadata, is the key.
            unit['script_blob']=script
            unit['script_state']=script_record(blob_data(units_node,script))
        prefix = f"u{unit['id']:04x}"
        unit['orders'] = []
        for index in range(unit['order_count']):
            key = f'{prefix}m{index:04x}'
            if key + '_name' not in strings or key not in named_blobs:
                raise ValueError(f'missing saved order {key}')
            order = order_record(blob_data(units_node, named_blobs[key]))
            if order['owner_id'] != unit['id']:
                raise ValueError(f'saved order owner mismatch: {key}')
            order.update({'index': index, 'type': strings[key + '_name'],
                          'blob': named_blobs[key]})
            if key + 'g' in named_blobs:
                order['controller_blob'] = named_blobs[key + 'g']
                controller = blob_data(units_node, named_blobs[key + 'g'])
                if order['controller_kind'] == 2:
                    if len(controller) != 54:
                        raise ValueError('unsupported saved flight controller')
                    # 4e3c3c..4e3c7b; pointer/handle fields in the prefix are
                    # deliberately excluded from this positional controller.
                    order['flight_controller'] = {
                        'flags': struct.unpack_from('<H',controller,28)[0],
                        'radius': struct.unpack_from('<h',controller,30)[0],
                        'height_offset': struct.unpack_from('<h',controller,32)[0],
                        'heading': struct.unpack_from('<H',controller,34)[0],
                        'point': list(struct.unpack_from('<3i',controller,38))}
                if order['controller_kind'] in (4, 6):
                    offsets = (8, 12, 16) if order['controller_kind'] == 4 else (8, 12, 16, 20)
                    if len(controller) != len(offsets) * 4 + 4:
                        raise ValueError('unsupported saved movement controller')
                    # 0x4e25f9..0x4e2608 restores +08/+0c/+10. The first
                    # word is ignored by the loader (uninitialized stack data
                    # in the writer); never interpret it as a live pointer.
                    order['controller_runtime_fields'] = {
                        f'{offset:02x}': struct.unpack_from('<I', controller, offset - 4)[0]
                        for offset in offsets}
            unit['orders'].append(order)
        if unit['has_movement']:
            movement = blob_data(units_node, named_blobs[prefix + 'mob'])
            unit['movement'] = movement_record(movement)
        units.append(unit)
    if len(units) != integers['Number of Units'] or len({u['id'] for u in units}) != len(units):
        raise ValueError('saved unit count or IDs disagree')
    players = nodes['Players']
    time_entry = next(b for b in players['blobs'] if b['name'] == 'GameTime')
    time = blob_data(players, time_entry)
    if len(time) != 32:
        raise ValueError('unsupported saved GameTime record')
    # 0x4f7750..0x4f775f loads GameTime into G+0x19f34; tick is G+0x19f44.
    tick = struct.unpack_from('<I', time, 16)[0]
    summary = nodes['Summary']
    summary_ints = {v['name']: v['value'] for v in summary['integers']}
    if tick != summary_ints['Game Time']:
        raise ValueError('saved tick disagrees with summary')
    return {'source_sha256': hashlib.sha256(data).hexdigest(), 'partial_state': True,
            'map': next(v['value'] for v in summary['strings'] if v['name'] == 'Map'),
            'tick': tick, 'rng_state': None, 'units': units}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('save', type=Path)
    args = parser.parse_args()
    print(json.dumps(decode(args.save.read_bytes()), indent=2))


if __name__ == '__main__':
    main()
