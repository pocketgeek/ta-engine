import base64
import struct
import unittest
import zlib
from decode_player_cache import initial_build_caches


class BuildCacheRestoreTest(unittest.TestCase):
    def fixture(self, change=lambda blobs: None):
        game, manager, resource, types, arrays, unit = [0x100000+n*0x100000 for n in range(6)]
        blobs = {game: bytearray(0x18000), manager: bytearray(0x110), resource: bytearray(0x190),
                 types: bytearray(1352), arrays: bytearray(0x400), unit: bytearray(312),
                 0x62a33c: bytearray(4),0x61a01c:bytearray(20)}
        def put(base, offset, value): struct.pack_into('<I', blobs[base], offset, value)
        put(game, 0x175b8, 2); put(game, 0x175c4, types)
        put(game, 0x2404+0x10c, resource)
        put(0x62a33c, 0, manager); put(manager, 0, game+0x2404)
        for offset, pointer in [(0x85, arrays), (0x95, arrays+0x100), (0xe5, arrays+0x200), (0x69, arrays+0x300)]:
            put(manager, offset, pointer)
        struct.pack_into('<h', blobs[arrays], 2, 1)
        struct.pack_into('<h', blobs[arrays], 0x102, 1)
        struct.pack_into('<i', blobs[arrays], 0x204, 12)
        blobs[arrays][0x301] = 42
        blobs[types][708:712] = b'TEST'
        struct.pack_into('<f', blobs[types], 676+0x20e, 2040)
        struct.pack_into('<2f', blobs[types], 676+0x206, 5000, 15)
        put(unit, 0xb4, types+676); put(unit, 0xb8, game+0x2404)
        struct.pack_into('<2f', blobs[resource], 0, 50, 100)
        struct.pack_into('<2f', blobs[resource], 0x188, 3, 7)
        change(blobs)
        return {'runtime_state': {'world_buffers': [{'name': 'game_fields', 'address': game}],
                                  'units': [{'id': 1, 'address': unit}]},
                'game_memory': [{'address': a, 'size': len(b), 'zlib_base64': base64.b64encode(zlib.compress(b)).decode()}
                                for a, b in blobs.items()]}

    def test_counts_catalogue_and_newest_resource_sample(self):
        state = initial_build_caches(self.fixture(), 1)[0]
        self.assertEqual(state['samples'][-1], [3, 7])
        self.assertEqual(state['mana'], 50)
        self.assertEqual((state['entries'][0]['income'],state['entries'][0]['storage']), (15,5000))
        self.assertEqual((state['entries'][0]['name'], state['entries'][0]['desired'], state['entries'][0]['priority']),
                         ('test', 12, 42))

    def test_construction_count_disagreement_rejected(self):
        frame = self.fixture(lambda b: struct.pack_into('<f', b[0x600000], 0x108, 0.5))
        with self.assertRaisesRegex(ValueError, 'construction'): initial_build_caches(frame, 1)

    def test_nonfinite_history_rejected(self):
        frame = self.fixture(lambda b: struct.pack_into('<f', b[0x300000], 0x188, float('nan')))
        with self.assertRaisesRegex(ValueError, 'history'): initial_build_caches(frame, 1)

    def test_wrong_owner_rejected(self):
        frame = self.fixture(lambda b: struct.pack_into('<I', b[0x200000], 0, 0x102514))
        with self.assertRaisesRegex(ValueError, 'owner'): initial_build_caches(frame, 1)

    def test_nonfinite_type_economy_rejected(self):
        frame = self.fixture(lambda b: struct.pack_into('<f', b[0x400000], 676+0x20a, float('nan')))
        with self.assertRaisesRegex(ValueError, 'economy'): initial_build_caches(frame, 1)

    def test_missing_or_corrupt_memory(self):
        self.assertIsNone(initial_build_caches({}, 1))
        frame = self.fixture(); frame['game_memory'][0]['size'] += 1
        with self.assertRaisesRegex(ValueError, 'compressed'): initial_build_caches(frame, 1)


if __name__ == '__main__': unittest.main()
