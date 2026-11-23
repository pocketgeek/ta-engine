import base64
import struct
import unittest
import zlib

from decode_player_cache import initial_build_planner


class BuildPlannerRestoreTest(unittest.TestCase):
    def fixture(self, mutate=lambda blobs: None):
        game, manager, types, extra = 0x100000, 0x200000, 0x300000, 0x400000
        blobs = {game: bytearray(0x18000), manager: bytearray(0x100),
                 types: bytearray(676*3), extra: bytearray(0x1000), 0x62a33c: bytearray(4)}
        def put(base, offset, value):
            struct.pack_into('<I', blobs[base], offset, value)
        put(game, 0x175b8, 3); put(game, 0x175c4, types)
        put(0x62a33c, 0, manager); put(manager, 0, game+0x2404)
        put(manager, 0x7d, 17); put(manager, 0xc5, extra)
        blobs[game][0x24e7] = 1
        blobs[extra][1:3] = bytes([25, 100])
        blobs[extra][0x100:0x107] = b'VERUNA\0'
        blobs[extra][0x200] = 128
        for i in (1, 2):
            put(types, i*676+0x8a, extra+0x100)
        put(types, 676+0x12a, extra+0x200)
        put(types, 676+0x12e, 3); put(types, 676+0x132, extra+0x300)
        struct.pack_into('<3H', blobs[extra], 0x300, 2, 1, 2)
        mutate(blobs)
        return {'runtime_state': {'world_buffers': [{'name': 'game_fields', 'address': game}]},
                'game_memory': [{'address': a, 'size': len(b),
                                 'zlib_base64': base64.b64encode(zlib.compress(b)).decode()}
                                for a, b in blobs.items()]}

    def test_order_duplicates_flags_and_owner_weights(self):
        state = initial_build_planner(self.fixture(), 1)
        self.assertEqual(state['types'][0], {'choices': [2, 1, 2], 'faction': 'VERUNA', 'special': True})
        self.assertEqual(state['types'][1], {'choices': [], 'faction': 'VERUNA', 'special': False})
        self.assertEqual(state['owners'], [{'population': 17, 'limited': True, 'weights': [25, 100]}])

    def test_invalid_menu_type(self):
        frame = self.fixture(lambda b: struct.pack_into('<H', b[0x400000], 0x300, 3))
        with self.assertRaisesRegex(ValueError, 'outside catalogue'):
            initial_build_planner(frame, 1)

    def test_missing_menu(self):
        frame = self.fixture(lambda b: struct.pack_into('<I', b[0x300000], 676+0x132, 0))
        with self.assertRaisesRegex(ValueError, 'build menu'):
            initial_build_planner(frame, 1)

    def test_unterminated_faction(self):
        def mutate(blobs):
            blobs[0x400000][0x100:0x200] = b'A'*256
        with self.assertRaisesRegex(ValueError, 'unterminated'):
            initial_build_planner(self.fixture(mutate), 1)

    def test_absent_capture_and_owner(self):
        self.assertIsNone(initial_build_planner({}, 1))
        frame = self.fixture(lambda b: struct.pack_into('<I', b[0x62a33c], 0, 0))
        self.assertEqual(initial_build_planner(frame, 1)['owners'], [None])


if __name__ == '__main__':
    unittest.main()
