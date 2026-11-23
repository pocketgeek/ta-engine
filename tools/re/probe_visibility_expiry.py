#!/usr/bin/env python3
"""Run native temporary sight retention and expiry with real count updates.

No routine substitutions. Uses near-center footprints to isolate lifecycle
from the distant-cell height geometry covered by check_exploration.py.
"""
import random
import struct
import subprocess
import sys
from emu import Icd, HEAP

p = Icd()
p.freeze_hooks()
game, player, unit, counts, mapping, config = [HEAP+i*0x40000 for i in range(6)]
def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))
def word(address):
    return struct.unpack('<I', p.uc.mem_read(address, 4))[0]
def call(address, args=(), ecx=0):
    _, error = p.call(address, args, ecx=ecx)
    assert not error, error

put(0x62d55c, game)
put(0x62d558, config)
put(config+8, config+0x100)
put(game+0x19e98, 32)
put(game+0x19e9c, 32)
put(game+0x19ef4, mapping)
p.uc.mem_write(game+0x306f, b'\xff')
put(player+0x88, counts)
put(player+0x8c, 16)
put(player+0x90, 16)
p.uc.mem_write(player+0xeb, b'\x03')
rng = random.Random(0x4c6750)
rows, expected_rows = [], []
for case in range(256):
    p.uc.mem_write(counts, bytes(256))
    p.uc.mem_write(mapping, bytes(512))
    p.uc.mem_write(config+0x115, b'\x01')
    put(0x62da20, 0)
    now = rng.randrange(100, 10000)
    put(game+0x19f44, now)
    row, expected_row = [now, 21], []
    deadlines = []
    admitted = set()
    for index in range(21):
        x, z = rng.randrange(1, 15), rng.randrange(1, 15)
        p.uc.mem_write(unit+0x84, bytes(32))
        put(unit+0x84, player)
        p.uc.mem_write(unit+0x98, struct.pack('<hhihBB', x, z, 0, 16, 1, 0))
        call(0x4c6800, (1, 1), unit+0x84)
        cells = {zz*16+xx for zz in range(z-1, z+2) for xx in range(x-1, x+2)}
        admitted |= cells
        delay = rng.randrange(0, 31)
        row.extend((x, z, delay))
        call(0x4c6750, (unit, delay))
        assert p.uc.mem_read(unit+0xa3, 1) == b'\x00'
        assert word(0x62da20) == min(index+1, 20)
        if index < 20:
            deadlines.append((now+delay, cells))
        expected = [sum(i in cells for _, cells in deadlines) for i in range(256)]
        assert list(p.uc.mem_read(counts, 256)) == expected
        expected_row.extend((word(0x62da20), *expected))
    # Expiry is strictly AFTER the deadline; compaction retains survivor order.
    for tick in range(now, now+32):
        put(game+0x19f44, tick)
        call(0x4c6b30)
        deadlines = [(end, cells) for end, cells in deadlines if end >= tick]
        assert word(0x62da20) == len(deadlines)
        assert [word(0x62d7a4+i*32) for i in range(len(deadlines))] == [d for d, _ in deadlines]
        expected = [sum(i in cells for _, cells in deadlines) for i in range(256)]
        assert list(p.uc.mem_read(counts, 256)) == expected
        expected_row.extend((word(0x62da20), *expected))
        explored = struct.unpack('<256H', p.uc.mem_read(mapping, 512))
        assert all(value == (8 if i in admitted else 0) for i, value in enumerate(explored))
    rows.append(' '.join(map(str, row)))
    expected_rows.append(expected_row)
if len(sys.argv) > 1:
    result = subprocess.run([sys.argv[1], '--effect-sight-lifecycle'],
        input='\n'.join(rows)+'\n', text=True, capture_output=True, check=True)
    actual = [list(map(int, line.split())) for line in result.stdout.splitlines()]
    assert actual == expected_rows, 'C++ current sight lifecycle differs from native'
    print('PASS: C++ current sight counts and retained-entry counts match every native lifecycle step')
print('PASS: 256 native temporary-sight lifecycles, 20-slot overflow, strict expiry, overlapping counts, stable compaction and persistent exploration')
