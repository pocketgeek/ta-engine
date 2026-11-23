#!/usr/bin/env python3
"""Headlessly verify native attached-SFX list cleanup during owner teardown.

Executes the retail owner-model destructor and its real SFX-list destructor
with a synthetic one-node attached list. The emulated alloc/free boundaries are
substituted only to keep the probe's memory resident; list unlinking and pool
return run in KINGDOMS.icd. This checks cleanup semantics, not the game tick at
which a particular death reaches owner teardown.
"""
import struct

from emu import HEAP, Icd


p = Icd()
owner = HEAP
manager = HEAP + 0x400
sentinel = HEAP + 0x800
node = HEAP + 0x1000
frees = []


def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def get(address):
    return struct.unpack('<I', p.uc.mem_read(address, 4))[0]


def retain_allocation(uc, sp):
    frees.append(get(sp))
    return 0, 0


# Native C++ deletion only releases the wrapper allocations. Leave their bytes
# mapped so the probe can inspect native unlinking/pool insertion afterward.
p.hooks[0x4eba00] = retain_allocation
p.hooks[0x5ba5d0] = retain_allocation
p.hooks[0x4ebaa0] = lambda uc, sp: (0, 0)
p.freeze_hooks()

put(owner + 0x17c, manager)
put(manager, 0x5f03a4)
put(manager + 4, 0)
put(manager + 8, sentinel)
put(manager + 12, 1)
put(manager + 16, 40)
put(sentinel, node)
put(sentinel + 4, node)
put(node, sentinel)
put(node + 4, sentinel)
put(0x640210, 0)

_, error = p.call(0x4ee560, (owner,))
assert not error, error

# 4ee560 synchronously calls the +0x17c manager's destructor. 497380 unlinks
# every entry via 497400, then returns both the node and sentinel to the native
# pool before its wrapper allocation is released.
assert get(manager) == 0x5f03b8
assert get(manager + 8) == 0
assert get(manager + 12) == 0
assert get(0x640210) == sentinel
assert get(sentinel) == node
assert get(node) == 0
assert len(frees) == 2, frees
assert manager in frees, frees
assert owner in frees, frees

print('PASS: native owner teardown synchronously drains attached SFX list; '
      'node and sentinel return to the native pool')
print('LIMIT: this isolates destructor semantics; it does not drive the native '
      'death dispatcher to measure the teardown tick relative to Killed/Dying')
