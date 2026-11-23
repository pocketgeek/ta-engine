import base64
import struct
import unittest
import zlib

from decode_features import initial_feature_presence


class FeaturePresenceTest(unittest.TestCase):
    def fixture(self, indices=(0xffff, 0, 0xfffe, 1), name=b'Tree'):
        base = 0x100000
        blob = bytearray(0x21000)
        struct.pack_into('<I', blob, 0x19ec0, 2)
        struct.pack_into('<I', blob, 0x19edc, base + 0x20000)
        struct.pack_into('<I', blob, 0x19f04, base + 0x20800)
        blob[0x20000:0x20000 + len(name)] = name
        blob[0x20140:0x20144] = b'Rock'
        for i, index in enumerate(indices):
            struct.pack_into('<H', blob, 0x20800 + 14 * i + 8, index)
        return {'map_cells': [2, 2],
                'runtime_state': {'world_buffers': [{'name': 'game_fields', 'address': base}]},
                'game_memory': [{'address': base, 'size': len(blob),
                                 'zlib_base64': base64.b64encode(zlib.compress(blob)).decode()}]}

    def test_origins_only_and_normalized_names(self):
        self.assertEqual(initial_feature_presence(self.fixture()), [(1, 'tree'), (3, 'rock')])

    def test_missing_capture_is_not_an_empty_map(self):
        self.assertIsNone(initial_feature_presence({}))
        self.assertEqual(initial_feature_presence(self.fixture((0xffff,) * 4)), [])

    def test_invalid_index_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'index outside'):
            initial_feature_presence(self.fixture((2, 0xffff, 0xffff, 0xffff)))

    def test_unterminated_name_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'unterminated'):
            initial_feature_presence(self.fixture(name=b'T' * 32))

    def test_missing_memory_is_rejected(self):
        frame = self.fixture()
        frame['game_memory'][0]['address'] += 0x30000
        with self.assertRaisesRegex(ValueError, 'memory absent'):
            initial_feature_presence(frame)


if __name__ == '__main__':
    unittest.main()
