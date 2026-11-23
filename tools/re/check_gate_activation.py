#!/usr/bin/env python3
"""Observe original 40a020 gate activation decisions on a controlled map.

Map lookup and the whole proximity scan execute natively. The downstream order
builder/dispatcher are observation boundaries; this does not test COB animation
or whether a yard can actually close.
"""
import itertools
import argparse
import struct
import subprocess

from emu import HEAP, Icd
from unicorn.x86_const import UC_X86_REG_EIP


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',default='build-dbg/retail_trace_test')
    args=parser.parse_args()
    p = Icd()
    game, cells, pool, mover, token = (HEAP + n for n in (0, 0x20000, 0x30000, 0x40000, 0x50000))
    gate, body = pool + 312, pool + 624

    def put(address, value):
        p.uc.mem_write(address, struct.pack('<I', value))

    put(0x62d55c, game)
    put(game + 0x19e98, 16)
    put(game + 0x19e9c, 16)
    put(game + 0x19f04, cells)
    put(game + 0x14e84, pool)
    put(game + 0x14e88, body)
    put(gate + 0x130, 0x1000001)
    p.uc.mem_write(gate + 0x74, struct.pack('<4h', 6, 6, 2, 2))
    p.uc.mem_write(token, b'\x17')
    orders, dispatched = [], []

    def order(uc, args):
        values = struct.unpack('<10I', uc.mem_read(args, 40))
        assert values[1:] == (1, 1, gate, 0, 0, 0, 0, 1, 0), values
        assert values[0] in (0x6049dc, 0x604a04)
        orders.append(int(values[0] == 0x604a04))
        return 10, token

    def dispatch(uc, args):
        dispatched.append(struct.unpack('<I', uc.mem_read(args, 4))[0] & 255)
        return 1, 0

    p.hooks[0x4d4bf0] = order
    p.hooks[0x4d7a30] = dispatch
    count = 0
    rows, results = [], []
    for active, same_owner, has_mover, moving, allocated, size, dx, dz in itertools.product(
            (0, 1), (0, 1), (0, 1), (0, 1), (0, 1), (1, 2), range(-2, 5), range(-2, 5)):
        x, z = 6 + dx, 6 + dz
        p.uc.mem_write(gate + 0x114, bytes([active]))
        p.uc.mem_write(body + 0xfd, bytes([0 if same_owner else 1]))
        p.uc.mem_write(body + 0x74, struct.pack('<4h', x, z, size, size))
        put(body + 0x130, 0x1000001 if allocated else 1)
        put(body + 8, mover if has_mover else 0)
        put(mover + 0x20, moving)
        records = bytearray(16 * 16 * 14)
        for cz in range(16):
            for cx in range(16):
                at = (cz * 16 + cx) * 14
                passage = 6 <= cx < 8 and 6 <= cz < 8
                slot = 1 if passage else 0
                if x <= cx < x + size and z <= cz < z + size:
                    slot = 2
                struct.pack_into('<H', records, at, slot)
                records[at + 13] = 32 if passage else 0
        p.uc.mem_write(cells, bytes(records))
        nearby = x < 9 and x + size > 5 and z < 9 and z + size > 5
        overlaps = x < 8 and x + size > 6 and z < 8 and z + size > 6
        desired = int(allocated and same_owner and has_mover and nearby and (moving or overlaps))
        orders.clear()
        dispatched.clear()
        _, error = p.call(0x40a020, (gate,))
        assert error is None, error
        assert p.uc.reg_read(UC_X86_REG_EIP) == 0x6ffff000
        expected = [desired] if active != desired else []
        assert orders == expected, (active, same_owner, has_mover, moving, allocated, size, dx, dz, orders, expected)
        assert dispatched == [23] * len(expected)
        rows.append(' '.join(map(str,(active,same_owner,has_mover,moving,allocated,size,dx,dz))))
        results.append(orders[0] if orders else -1)
        count += 1
    actual=subprocess.run([args.binary,'--automatic-gate'],input='\n'.join(rows)+'\n',
                          text=True,capture_output=True,check=True)
    values=list(map(int,actual.stdout.split()))
    assert len(values)==len(results),(len(values),len(results))
    for row,want,got in zip(rows,results,values):
        assert got==want,(row,'native',want,'World',got)
    print(f'PASS: {count} World/native gate activation decisions, including ownership, motion, overlap and scan border')


if __name__ == '__main__':
    main()
