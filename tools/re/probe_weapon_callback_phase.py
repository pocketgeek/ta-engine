#!/usr/bin/env python3
"""Verify native combat-script dispatch queues callbacks for the next VM pass.

Executes retail 56c640/56c680 and 56c870 from KINGDOMS.icd. AimWeapon,
FireWeapon, and TargetCleared all call the dispatcher with immediate=0 at
52ff3d, 530205, and 51a86b respectively. Their threads must stay at entry until
the next regular 56c870 pass; an immediate=1 control proves the contrasting path.
The COB body is a single RETURN instruction, so no game/render behavior is used.
"""
import struct

from emu import HEAP, Icd


def put(icd, address, value):
    icd.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))


def make_vm(icd, script_name):
    vm, definition, code, entries, names, name = [HEAP + i * 0x10000 for i in range(6)]
    put(icd, vm + 0x0c, definition)
    put(icd, definition + 4, 1)
    put(icd, definition + 0x18, entries)
    put(icd, definition + 0x1c, names)
    put(icd, definition + 0x24, code)
    put(icd, entries, 0)
    put(icd, names, name)
    icd.uc.mem_write(name, script_name.encode('ascii') + b'\0')
    icd.uc.mem_write(code, struct.pack('<I', 0x10065000))  # RETURN
    icd.hooks[0x56d850] = lambda _uc, _sp: (1, 0)  # piece update, outside callback scheduling
    return vm, code


def run_callback(script_name, args):
    icd = Icd()
    vm, code = make_vm(icd, script_name)
    icd.freeze_hooks()
    padded = tuple(args) + (0,) * (4 - len(args))
    result, error = icd.call(0x56c640, (HEAP + 5 * 0x10000, 0, 0, len(args), *padded), ecx=vm)
    assert not error, error
    assert result == 1

    active = struct.unpack('<I', icd.uc.mem_read(vm + 0xa60, 4))[0]
    thread = vm + 0x20
    flags, pc, stack_top = struct.unpack('<3I', icd.uc.mem_read(thread, 12))
    assert active == 1 and flags == 0x1000000 and pc == 0 and stack_top == len(args) - 1, (
        script_name, active, hex(flags), hex(pc), stack_top)
    stored = struct.unpack('<4I', icd.uc.mem_read(thread + 0x24, 16))
    assert stored == padded, (script_name, stored, padded)

    result, error = icd.call(0x56c870, (0,), ecx=vm)
    assert not error, error
    active = struct.unpack('<I', icd.uc.mem_read(vm + 0xa60, 4))[0]
    assert active == 0, (script_name, active)


for name, args in (
    ('AimWeapon', (0x1234, 0x5678, 2)),
    ('FireWeapon', (2,)),
    ('TargetCleared', (1,)),
):
    run_callback(name, args)

# Immediate mode is not what the three native combat call sites request. Verify
# the dispatcher control so the queued-thread assertion distinguishes the flag.
icd = Icd()
vm, _code = make_vm(icd, 'AimWeapon')
icd.freeze_hooks()
args = (HEAP + 5 * 0x10000, 0, 1, 3, 1, 2, 0, 0)
result, error = icd.call(0x56c640, args, ecx=vm)
assert not error, error
assert result == 1
assert struct.unpack('<I', icd.uc.mem_read(vm + 0xa60, 4))[0] == 0

print('PASS: native AimWeapon, FireWeapon, and TargetCleared start deferred; immediate control runs inline')
