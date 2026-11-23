"""Decoder checks; no game process, executable, or process permissions needed."""
import struct
import unittest

import livesample as live


class Memory:
    def __init__(self):
        self.data = bytearray(0x100000)
        self.game, self.entities, self.movement, self.owner = 0x1000, 0x30000, 0x40000, 0x50000
        self.ticks = [42, 42]
        self.put(self.game + 0x14e84, '<II', self.entities, self.entities + 2 * live.STRIDE)
        self.put(self.game + 0x19e98, '<II', 640, 640)
        # Deliberately different occupancy table: treating this as the entity
        # table was the original sampler's bug.
        self.put(self.game + 0x19edc, '<I', 0x60000)
        base = self.entities + live.STRIDE
        self.put(base + 2, '<H', 1)
        self.put(base + 8, '<I', self.movement)
        self.put(base + 0x68, '<iii', -32768, 64 * 65536, 24 * 65536)
        self.put(base + 0x74, '<hhhh', -1, 1, 2, 2)
        self.put(base + 0x7e, '<H', 60000)
        self.put(base + 0xb8, '<I', self.owner)
        self.put(base + 0xb4, '<I', 0x70000)
        self.put(0x70000 + 0x162, '<i', 117964)
        self.put(base + 0x12b, '<i', 120000)
        self.put(base + 0x130, '<I', 0x1000000)
        self.put(self.owner + 0xeb, '<B', 3)
        self.put(self.movement + 0x20, '<i', 100000)
        self.put(self.movement + 0x2c, '<II', 40, 44)
        self.put(self.movement + 0x36, '<H', 0x120)

    def put(self, address, fmt, *values):
        struct.pack_into(fmt, self.data, address, *values)

    def read(self, address, size):
        return self.data[address:address + size]

    def u32(self, address):
        if address == live.RNG_STATE:
            return 12345
        if address == live.GAMESTATE_PTR:
            return self.game
        if address == self.game + 0x19f44:
            return self.ticks.pop(0)
        return live.u32(self.read(address, 4))


class DecoderTest(unittest.TestCase):
    def test_actual_entity_layout_signed_positions_unsigned_heading(self):
        frame = live.snapshot(Memory())
        self.assertEqual(frame['tick'], 42)
        self.assertEqual(frame['map_cells'], [640, 640])
        self.assertEqual(len(frame['units']), 1)
        unit = frame['units'][0]
        self.assertEqual((unit['id'], unit['player']), (1, 3))
        self.assertEqual((unit['x'], unit['y'], unit['z']), (-0.5, 64, 24))
        self.assertEqual(unit['heading'], 60000)
        self.assertEqual(unit['cell_origin'], [-1, 1])
        self.assertEqual(unit['footprint'], [2, 2])
        self.assertEqual(unit['speed_raw'], 100000)
        self.assertEqual(unit['nominal_speed_raw'], 117964)
        self.assertEqual((frame['rng_before'], frame['rng_after']), (12345, 12345))
        self.assertEqual(unit['movement_flags'], 0x120)

    def test_reject_tick_crossing(self):
        mem = Memory()
        mem.ticks = [42, 44]
        self.assertIsNone(live.snapshot(mem))

    def test_reject_wrong_layout(self):
        mem = Memory()
        mem.put(mem.entities + live.STRIDE + 2, '<H', 99)
        with self.assertRaisesRegex(RuntimeError, 'slot/id mismatch'):
            live.snapshot(mem)

    def test_reject_bad_table_extent_before_reading(self):
        mem = Memory()
        mem.put(mem.game + 0x14e88, '<I', mem.entities + 1)
        with self.assertRaisesRegex(RuntimeError, 'invalid live entity table'):
            live.snapshot(mem)


if __name__ == '__main__':
    unittest.main()
