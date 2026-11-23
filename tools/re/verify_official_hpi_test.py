#!/usr/bin/env python3
"""Asset-free tests; optional read-only corpus checks via TAK_AUTH_RETAIL_DIR."""
import io
import os
from pathlib import Path
import struct
import unittest

from verify_official_hpi import (MAX_DIRECTORY_BYTES, checksum_summary, parse_key,
                                 read_archive, verify_signature)


class VerifierTests(unittest.TestCase):
    # Toy public parameters, not Cavedog data.
    DER = bytes.fromhex('300c02011702010b020102020104')
    KEY = (23, 11, 2, 4)

    def test_der(self):
        self.assertEqual(parse_key(self.DER), self.KEY)

    def test_der_rejects_malformed(self):
        for data in (b'', self.DER[:-1], self.DER + b'\0',
                     self.DER.replace(b'\x01\x17', b'\x01\xff'),
                     bytes.fromhex('30800201170000'),
                     bytes.fromhex('300d0202001702010b020102020104')):
            with self.subTest(data=data), self.assertRaises(ValueError):
                parse_key(data)

    def test_summary_native_observations(self):
        vectors = [(b'', '000000000000000000000000000000000000000000000000'),
                   (b'abc', '030000002660256700000000000000000000000000000000'),
                   (bytes(range(21)), '15000000d2140028282d323710111213322d323714111213'),
                   (bytes(range(256)), '000100008000000080df1f6000000000e0df1f6000000000')]
        for data, expected in vectors:
            self.assertEqual(checksum_summary(data).hex(), expected)

    def test_signature_length_and_zero_r(self):
        for signature in (b'', b'\x01', b'\x01\x02\x03', b'\0\x01'):
            self.assertFalse(verify_signature(self.KEY, b'abc', signature))

    def test_archive_bounds(self):
        for data in (b'', bytes(32),
                     struct.pack('<8I', 0x49504148, 0x20000, 32,
                                 MAX_DIRECTORY_BYTES + 1, 32, 0, 32, 32),
                     struct.pack('<8I', 0x49504148, 0x20000, 1000,
                                 20, 32, 0, 32, 32)):
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                read_archive(io.BytesIO(data), len(data), self.KEY)

    @unittest.skipUnless(os.environ.get('TAK_AUTH_RETAIL_DIR'), 'optional owned retail corpus')
    def test_original_corpus_and_in_memory_negative_checks(self):
        root = Path(os.environ['TAK_AUTH_RETAIL_DIR'])
        key_paths = [p for p in root.iterdir() if p.name.lower() == 'kingdoms.key']
        self.assertEqual(len(key_paths), 1)
        key = parse_key(key_paths[0].read_bytes())
        archives = sorted(p for p in root.iterdir() if p.suffix.lower() == '.hpi')
        self.assertTrue(archives)
        for path in archives:
            with self.subTest(archive=path.name), path.open('rb') as stream:
                result = read_archive(stream, path.stat().st_size, key)
                self.assertTrue(result['directory_signature_valid'])
                self.assertTrue(result['copyright_footer_valid'])
                stream.seek(0)
                header = stream.read(32)
                start, end = result['signed_directory_span']
                stream.seek(start)
                payload = header[8:28] + stream.read(end - start)
                stream.seek(result['signature_offset'])
                signature = stream.read(result['signature_bytes'])
                # Change only detached in-memory inputs; no archive is rewritten.
                for index in (0, 20, len(payload) - 1):
                    changed = bytearray(payload)
                    changed[index] ^= 1
                    self.assertFalse(verify_signature(key, changed, signature))
                changed = bytearray(signature)
                changed[-1] ^= 1
                self.assertFalse(verify_signature(key, payload, changed))


if __name__ == '__main__':
    unittest.main()
