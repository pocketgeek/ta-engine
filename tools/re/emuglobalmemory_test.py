import unittest
from types import SimpleNamespace
from emuglobalmemory import CapturedFixedGlobalMemory


class FixedGlobalMemoryTest(unittest.TestCase):
    def fixture(self):
        words = {0x5eb0a8: 0x8000, 0x5eb1f0: 0x8100, 0x100: 0x40, 0x104: 60}
        writes, arguments = {}, []
        def allocate(uc, args):
            arguments.append(args)
            return 0, 0x60000000
        p = SimpleNamespace(u32=words.__getitem__, ensure=lambda a, n: None,
                            icd=SimpleNamespace(hooks={}), allocate=allocate,
                            put=lambda a, b: writes.update({a: b}))
        return CapturedFixedGlobalMemory(p), words, writes, arguments

    def test_zeroed_fixed_allocation_and_lock(self):
        host, words, writes, arguments = self.fixture()
        self.assertEqual(host.allocate(None, 0x100), (2, 0x60000000))
        self.assertEqual(arguments, [0x104])
        self.assertEqual(writes[0x60000000], bytes(60))
        words[0x100] = 0x60000000
        self.assertEqual(host.lock(None, 0x100), (1, 0x60000000))
        self.assertEqual(host.allocations, {0x60000000: 60})

    def test_movable_allocation_rejected(self):
        host, words, _, arguments = self.fixture()
        words[0x100] = 2
        with self.assertRaisesRegex(RuntimeError, 'flags'):
            host.allocate(None, 0x100)
        self.assertEqual(arguments, [])

    def test_unknown_lock_rejected_and_null_preserved(self):
        host, words, _, _ = self.fixture()
        with self.assertRaisesRegex(RuntimeError, 'uncaptured'):
            host.lock(None, 0x100)
        words[0x100] = 0
        self.assertEqual(host.lock(None, 0x100), (1, 0))


if __name__ == '__main__':
    unittest.main()
