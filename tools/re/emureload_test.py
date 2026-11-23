"""Synthetic memory tests; no retail executable or capture is loaded."""
import struct
import unittest

from emureload import CapturedProcess, initial_fixture
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32


class AllocationTests(unittest.TestCase):
    def setUp(self):
        self.p = CapturedProcess.__new__(CapturedProcess)
        self.p.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.p.pages = set()
        self.p.allocations = {}
        self.p.brk = 0x60000000

    def test_mapping_overlapping_chunks_preserves_existing_bytes(self):
        self.p.put(0x101000, b'kept')
        self.p.ensure(0x100000, 0x4000)
        self.assertEqual(bytes(self.p.uc.mem_read(0x101000, 4)), b'kept')
        self.p.put(0x103fff, b'cross page')
        self.assertEqual(bytes(self.p.uc.mem_read(0x103fff, 10)), b'cross page')

    def test_realloc_preserves_known_contents_on_growth_and_shrink(self):
        self.p.put(0x10000, struct.pack('<II', 0, 8))
        _, first = self.p.reallocate(self.p.uc, 0x10000)
        self.p.put(first, b'12345678')
        self.p.put(0x10000, struct.pack('<II', first, 32))
        _, second = self.p.reallocate(self.p.uc, 0x10000)
        self.assertEqual(bytes(self.p.uc.mem_read(second, 8)), b'12345678')
        self.p.put(0x10000, struct.pack('<II', second, 3))
        _, third = self.p.reallocate(self.p.uc, 0x10000)
        self.assertEqual(bytes(self.p.uc.mem_read(third, 3)), b'123')

    def test_unknown_extent_is_not_guessed(self):
        self.p.put(0x10000, struct.pack('<II', 0x12340000, 16))
        with self.assertRaisesRegex(RuntimeError, 'unknown original realloc extent'):
            self.p.reallocate(self.p.uc, 0x10000)

    def test_zero_size_realloc_does_not_read_old_storage(self):
        self.p.put(0x10000, struct.pack('<II', 0x12340000, 0))
        self.assertEqual(self.p.reallocate(self.p.uc, 0x10000), (0, 0))


class InitialFixtureTests(unittest.TestCase):
    PREFIX = ['TAK_MOVEMENT_PROBE 37 100 7 2 3 1 "map" 1', '0', '5 1 0 0 1', 'body']
    HEIGHTS = ['2', '4 0 0 0 0 0', '9 0 0 0 0 0']
    EXPLORATION = ['8 8 0', ' '.join(['0'] * 64), '2', '4 1 2 3', '9 1 2 3']

    def test_version_37_drops_heights_and_exploration(self):
        got = initial_fixture(self.PREFIX + self.HEIGHTS + self.EXPLORATION, 100)
        self.assertEqual(got, ['TAK_MOVEMENT_PROBE 34 100 7 2 3 1 "map" 1', *self.PREFIX[1:]])

    def test_version_36_drops_heights_only(self):
        lines = [self.PREFIX[0].replace(' 37 ', ' 36 '), *self.PREFIX[1:], *self.HEIGHTS]
        self.assertEqual(initial_fixture(lines, 100)[1:], self.PREFIX[1:])

    def test_version_34_is_unchanged(self):
        lines = [self.PREFIX[0].replace(' 37 ', ' 34 '), *self.PREFIX[1:]]
        self.assertEqual(initial_fixture(lines, 100), lines)

    def test_rejects_other_tick_or_version(self):
        with self.assertRaises(ValueError):
            initial_fixture(self.PREFIX + self.HEIGHTS + self.EXPLORATION, 101)
        with self.assertRaises(ValueError):
            initial_fixture([self.PREFIX[0].replace(' 37 ', ' 33 '), *self.PREFIX[1:]], 100)

    def test_rejects_missing_sections(self):
        with self.assertRaises(ValueError):
            initial_fixture(self.PREFIX + self.EXPLORATION, 100)


if __name__ == '__main__':
    unittest.main()
