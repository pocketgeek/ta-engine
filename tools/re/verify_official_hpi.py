#!/usr/bin/env python3
"""Read-only Kingdoms HPI v2 directory-signature verifier; not a payload audit.

See docs/research/official-archive-verification.md. Requires an independently
obtained KINGDOMS.KEY; no retail data or signing functionality is included.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

MAX_KEY_BYTES = 4096
MAX_DIRECTORY_BYTES = 64 * 1024 * 1024
MASK32 = (1 << 32) - 1


def parse_key(data):
    """Strict DER parsing, deliberately stricter than the retail decoder."""
    if len(data) > MAX_KEY_BYTES:
        raise ValueError('key exceeds research-tool limit')

    def tlv(pos, tag):
        if pos + 2 > len(data) or data[pos] != tag:
            raise ValueError('missing DER tag')
        length = data[pos + 1]
        pos += 2
        if length & 128:
            count = length & 127
            if not 1 <= count <= 2 or pos + count > len(data):
                raise ValueError('invalid DER length')
            if data[pos] == 0:
                raise ValueError('noncanonical DER length')
            length = int.from_bytes(data[pos:pos + count], 'big')
            pos += count
            if length < 128:
                raise ValueError('noncanonical DER length')
        end = pos + length
        if end > len(data):
            raise ValueError('truncated DER value')
        return pos, end

    pos, end = tlv(0, 0x30)
    if end != len(data):
        raise ValueError('trailing key data')
    values = []
    for _ in range(4):
        start, pos = tlv(pos, 2)
        raw = data[start:pos]
        if not raw or raw[0] & 128 or (len(raw) > 1 and raw[0] == 0 and raw[1] < 128):
            raise ValueError('invalid positive DER integer')
        values.append(int.from_bytes(raw, 'big'))
    if pos != end:
        raise ValueError('expected exactly four key integers')
    p, q, g, y = values
    if not (2 < q < p and 1 < g < p and 1 < y < p):
        raise ValueError('invalid public-key parameters')
    if p.bit_length() > 4096 or q.bit_length() > 512:
        raise ValueError('key exceeds research-tool arithmetic limit')
    if (p - 1) % q or pow(g, q, p) != 1 or pow(y, q, p) != 1:
        raise ValueError('public-key subgroup checks failed')
    return tuple(values)


def checksum_summary(data):
    """24-byte summary consumed as a big-endian integer by the verifier."""
    if len(data) > MASK32:
        raise ValueError('summary input exceeds uint32 length')
    a = b = c = d = 0
    for i, value in enumerate(data):
        a = (a + value) & 255
        b ^= value
        c = (c + (value ^ (i & 255))) & 255
        d ^= (value + i) & 255
    n = len(data) // 4
    s0 = s1 = s2 = s3 = 0
    for i, (word,) in enumerate(struct.iter_unpack('<I', memoryview(data)[:n * 4])):
        j = n - 1 - i
        s0 = (s0 + word) & MASK32
        s1 ^= word
        s2 = (s2 + (word ^ j)) & MASK32
        s3 ^= (word + j) & MASK32
    return struct.pack('<6I', len(data), a | b << 8 | c << 16 | d << 24,
                       s0, s1, s2, s3)


def verify_signature(key, payload, signature):
    p, q, g, y = key
    width = (q.bit_length() + 7) // 8
    if len(signature) != 2 * width:
        return False
    r = int.from_bytes(signature[:width], 'big')
    s = int.from_bytes(signature[width:], 'big')
    if r == 0:
        return False
    m = int.from_bytes(checksum_summary(payload), 'big')
    return r == (pow(g, s, p) * pow(y, r, p) % p + m) % q


def read_archive(stream, size, key):
    """Only bounded reads; never decompresses or extracts archive contents."""
    def read_at(offset, length):
        if offset < 0 or length < 0 or offset + length > size:
            raise ValueError('archive span outside file')
        stream.seek(offset)
        result = stream.read(length)
        if len(result) != length:
            raise ValueError('short archive read')
        return result

    header = read_at(0, 32)
    magic, version, directory, directory_size, names, names_size, data_start, sig = struct.unpack('<8I', header)
    if magic != 0x49504148 or version != 0x20000:
        raise ValueError('expected Kingdoms HPI v2')
    if directory_size > MAX_DIRECTORY_BYTES:
        raise ValueError('directory exceeds research-tool limit')
    if names + names_size > size or data_start > size:
        raise ValueError('archive header span outside file')
    payload = header[8:28] + read_at(directory, directory_size)
    width = (key[1].bit_length() + 7) // 8
    signature = read_at(sig, 2 * width)
    footer = read_at(size - 36, 36)
    footer_ok = (footer[:10] + b'0000' + footer[14:] ==
                 b'Copyright 0000 Cavedog Entertainment')
    return {'directory_signature_valid': verify_signature(key, payload, signature),
            'copyright_footer_valid': footer_ok,
            'signed_header_span': [8, 28],
            'signed_directory_span': [directory, directory + directory_size],
            'signature_offset': sig, 'signature_bytes': len(signature),
            'summary_hex': checksum_summary(payload).hex(),
            'file_payloads_checked': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--key', type=Path, required=True)
    parser.add_argument('archives', type=Path, nargs='+')
    args = parser.parse_args()
    try:
        with args.key.open('rb') as stream:
            key_data = stream.read(MAX_KEY_BYTES + 1)
        key = parse_key(key_data)
    except (OSError, ValueError) as error:
        parser.exit(2, f'key error: {error}\n')
    failed = False
    for path in args.archives:
        result = {'archive': str(path), 'key_sha256': hashlib.sha256(key_data).hexdigest()}
        try:
            with path.open('rb') as stream:
                stream.seek(0, 2)
                size = stream.tell()
                result.update(read_archive(stream, size, key))
                stream.seek(0)
                result['sha256'] = hashlib.file_digest(stream, 'sha256').hexdigest()
                result['bytes'] = size
            failed |= not (result['directory_signature_valid'] and result['copyright_footer_valid'])
        except (OSError, ValueError) as error:
            result['error'] = str(error)
            failed = True
        print(json.dumps(result, sort_keys=True))
    return int(failed)


if __name__ == '__main__':
    raise SystemExit(main())
