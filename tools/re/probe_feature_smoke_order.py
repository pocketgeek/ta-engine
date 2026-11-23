#!/usr/bin/env python3
"""Probe retail burn-list order and the CRT seed used by feature smoke.

Calls the native burn-slot allocator/list mover (4949c0/494a80), then follows
the active linked list in the order consumed by 4959c0. It also executes the
native CRT TLS seed initializer (5dc3f0) and rand (5d4444). The feature-to-draw
comparison is a controlled case with no pre-existing smoke particles and
three accepted random-consuming draws per emission; it is not a whole-tick
replay or a port parity claim.
"""
import struct

from emu import HEAP, Icd


def put32(p, address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def get16(p, address):
    return struct.unpack('<h', p.uc.mem_read(address, 2))[0]


def get32(p, address):
    return struct.unpack('<I', p.uc.mem_read(address, 4))[0]


p = Icd()
game = HEAP + 0x1000
entries = HEAP + 0x10000
put32(p, 0x62d55c, game)
put32(p, game + 0x19e74, entries)

# The free burn-slot list is 13 -> 2 -> 9. Each allocation moves its head to
# the active list head, exactly as native 4949c0 calls 494a80.
allocation_order = [13, 2, 9]
put32(p, game + 0x19e7c, 0xffffffff)  # active fires
put32(p, game + 0x19e80, 0xffffffff)  # other live burn-list head
put32(p, game + 0x19e84, allocation_order[0])
for offset, feature_id in enumerate(allocation_order):
    previous = -1 if offset == 0 else allocation_order[offset - 1]
    following = allocation_order[offset + 1] if offset + 1 < len(allocation_order) else -1
    p.uc.mem_write(entries + feature_id * 96,
                   struct.pack('<hh', following, previous) + bytes(96 - 4))

allocated = []
for expected in allocation_order:
    result, error = p.call(0x4949c0)
    assert not error, error
    assert result == expected, (result, expected)
    allocated.append(result)

native_order = []
index = struct.unpack('<i', p.uc.mem_read(game + 0x19e7c, 4))[0]
while index != -1:
    native_order.append(index)
    index = get16(p, entries + index * 96)
assert native_order == list(reversed(allocated)), (native_order, allocated)
assert get32(p, game + 0x19e84) == 0xffffffff
local_order = sorted(native_order)
assert native_order != local_order
print(f'PASS: native burn allocations {allocated} traverse newest-first as {native_order}')
print(f'PASS: local feature-vector/map order for these ids is {local_order}')

# Native TLS initialization sets the per-thread CRT seed at +0x14 to one.
thread = HEAP + 0x50000
_, error = p.call(0x5dc3f0, (thread,))
assert not error, error
assert get32(p, thread + 0x14) == 1
p.hooks[0x5dc403] = lambda uc, args: (0, thread)
p.freeze_hooks()


def draws_by_feature(order):
    _, error = p.call(0x5dc3f0, (thread,))
    assert not error, error
    grouped = {}
    for feature_id in order:
        values = []
        for _ in range(3):
            value, error = p.call(0x5d4444)
            assert not error, error
            values.append(value)
        grouped[feature_id] = values
    return grouped


native_draws = draws_by_feature(native_order)
local_draws = draws_by_feature(local_order)
assert native_draws != local_draws
print('PASS: native CRT initializer sets seed 1; native 5d4444 is seeded and deterministic')
print(f'PASS: with the same seed and three draws per accepted smoke, feature draw assignments differ: {native_draws} vs {local_draws}')
print('LIMIT: other CRT consumers and expiring existing smoke particles are excluded from this controlled comparison')
