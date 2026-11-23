#!/usr/bin/env python3
"""Inventory SQSH sections of a retail HAPI BANK save, without modifying it.

This decodes BANK named fields and blob boundaries, NOT complete game state.
Section strings are observations; directory fields have decoded associations. Extracted retail
data belongs under gitignored assets/tmp, never in source control.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


def lzss(data, expected):
    window, output = bytearray(4096), bytearray()
    cursor, write = 0, 1
    while cursor < len(data):
        flags = data[cursor]
        cursor += 1
        for bit in range(8):
            if flags & (1 << bit):
                if cursor + 2 > len(data):
                    raise ValueError('truncated LZSS reference')
                word = struct.unpack_from('<H', data, cursor)[0]
                cursor += 2
                read, length = word >> 4, (word & 15) + 2
                if read == 0:
                    if len(output) != expected:
                        raise ValueError('LZSS decoded size mismatch')
                    return bytes(output)
                if len(output) + length > expected:
                    raise ValueError('LZSS output overrun')
                for _ in range(length):
                    value = window[read]
                    read = (read + 1) & 4095
                    output.append(value)
                    window[write] = value
                    write = (write + 1) & 4095
            else:
                if cursor >= len(data):
                    raise ValueError('truncated LZSS literal')
                if len(output) >= expected:
                    raise ValueError('LZSS output overrun')
                value = data[cursor]
                cursor += 1
                output.append(value)
                window[write] = value
                write = (write + 1) & 4095
    raise ValueError('missing LZSS terminator')


def sections(data):
    if not data.startswith(b'HAPIBANK'):
        raise ValueError('expected HAPI BANK save')
    cursor = 8
    while (cursor := data.find(b'SQSH', cursor)) >= 0:
        if cursor + 19 > len(data):
            raise ValueError('truncated SQSH header')
        version, method, encrypted, size, expected, checksum = struct.unpack_from('<BBBIII', data, cursor + 4)
        end = cursor + 19 + size
        if end > len(data) or expected > 64 * 1024 * 1024:
            raise ValueError('invalid SQSH extent')
        if version != 2 or method != 1 or encrypted:
            raise ValueError('unsupported SQSH variant; this inspector handles the observed save format')
        payload = data[cursor + 19:end]
        if sum(payload) & 0xffffffff != checksum:
            raise ValueError('SQSH checksum mismatch')
        decoded = lzss(payload, expected)
        yield cursor, decoded
        cursor = end


def directory(data, chunks):
    """Decode the observed BANK directory; preserve unknown blob metadata.

    Blob references address the uncompressed body relative to its on-disk
    header, so they must not be used as physical file offsets.
    """
    if len(data) < 34 or not data.startswith(b'HAPIBANK'):
        raise ValueError('truncated or invalid BANK header')
    names_offset, offset = struct.unpack_from('<II', data, 12)
    if not 34 <= offset <= names_offset < len(data) or names_offset not in chunks:
        raise ValueError('invalid BANK directory extent')
    names = chunks[names_offset]

    def name(at):
        if at >= len(names) or (at and names[at - 1] != 0):
            raise ValueError('invalid BANK name reference')
        end = names.find(b'\0', at)
        if end < 0:
            raise ValueError('unterminated BANK name')
        return names[at:end].decode('ascii')

    result = []
    while offset < names_offset:
        if offset + 32 > names_offset:
            raise ValueError('truncated BANK node header')
        size, key, ni, nd, ns, nb, compressed, unknown = struct.unpack_from('<8I', data, offset)
        end = offset + size
        if size < 32 or end > names_offset or compressed not in (0, 1):
            raise ValueError('invalid BANK node extent or compression')
        if compressed:
            if offset + 32 not in chunks:
                raise ValueError('missing BANK node chunk')
            packed_size = struct.unpack_from('<I', data, offset + 39)[0]
            if offset + 32 + 19 + packed_size != end:
                raise ValueError('BANK chunk extent mismatch')
            body = chunks[offset + 32]
        else:
            body = data[offset + 32:end]
        payload = ni * 8 + nd * 12 + ns * 8 + nb * 16
        if payload > len(body):
            raise ValueError('BANK fields overrun node')
        node = {'name': name(key), 'offset': offset, 'unknown': unknown,
                'integers': [], 'doubles': [], 'strings': [], 'blobs': []}
        cursor = 0
        for kind, count, fmt, width in (('integers', ni, '<Ii', 8),
                                        ('doubles', nd, '<Id', 12),
                                        ('strings', ns, '<II', 8)):
            for _ in range(count):
                field, value = struct.unpack_from(fmt, body, cursor)
                cursor += width
                node[kind].append({'name': name(field),
                                   'value': name(value) if kind == 'strings' else value})
        for _ in range(nb):
            field, metadata, reference, length = struct.unpack_from('<4I', body, cursor)
            cursor += 16
            if reference != offset + 32 + payload or payload + length > len(body):
                raise ValueError('invalid BANK blob extent')
            blob = body[payload:payload + length]
            node['blobs'].append({'name': None if field == 0xffffffff else name(field),
                                  'metadata': metadata, 'body_offset': payload,
                                  'size': length, 'sha256': hashlib.sha256(blob).hexdigest()})
            payload += length
        if payload != len(body):
            raise ValueError('unconsumed BANK node bytes')
        result.append(node)
        offset = end
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('save', type=Path)
    parser.add_argument('--extract', type=Path, help='write decoded sections and index to this directory')
    args = parser.parse_args()
    data = args.save.read_bytes()
    report = {'source': str(args.save), 'size': len(data),
              'sha256': hashlib.sha256(data).hexdigest(), 'sections': []}
    chunks = dict(sections(data))
    report['directory'] = directory(data, chunks)
    for offset, decoded in chunks.items():
        entry = {'offset': offset, 'decoded_size': len(decoded),
                 'sha256': hashlib.sha256(decoded).hexdigest(),
                 'strings': [{'offset': m.start(), 'text': m.group().decode('ascii')}
                             for m in re.finditer(rb'[\x20-\x7e]{5,}', decoded)]}
        report['sections'].append(entry)
        if args.extract:
            args.extract.mkdir(parents=True, exist_ok=True)
            target = args.extract / f'chunk-{offset:06x}.bin'
            if target.exists():
                if target.read_bytes() != decoded:
                    raise ValueError(f'refusing to overwrite different data: {target}')
            else:
                with target.open('xb') as output:
                    output.write(decoded)
    encoded = json.dumps(report, indent=2) + '\n'
    if args.extract:
        # Exclusive output: captures and prior reports are never overwritten.
        with (args.extract / 'inspection.json').open('x') as output:
            output.write(encoded)
    else:
        print(encoded, end='')


if __name__ == '__main__':
    main()
