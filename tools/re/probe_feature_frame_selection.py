#!/usr/bin/env python3
"""Observe native feature body/shadow clock selection; final lookup is a sink.

Does not cover clock advancement, visibility, projection or GPU blending.
"""
import struct
from emu import Icd, HEAP, STACK
from unicorn.x86_const import *

p = Icd()
base, definition, instance = STACK + 0x8000, HEAP, HEAP + 0x1000
settings, options = HEAP + 0x2000, HEAP + 0x3000
calls = []
def put(address, value):
    p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))
def clock(uc, sp):
    address, = struct.unpack('<I', uc.mem_read(sp, 4))
    calls.append(('clock', address))
    return 1, address + 0x10000
def frame(uc, sp):
    sequence, index = struct.unpack('<2I', uc.mem_read(sp, 8))
    calls.append(('frame', sequence, index))
    return 2, sequence + 0x10000
p.hooks[0x536400] = clock
p.hooks[0x5367a0] = frame
p.freeze_hooks()
put(0x62d558, settings)
put(settings + 0x18, options)
count = 0
for animated in (False, True):
    for enabled in (False, True):
        for body in (0, HEAP + 0x4000):
            for shadow in (0, HEAP + 0x5000):
                put(definition + 0x13c, 2 if animated else 0)
                put(definition + 0xd8, body)
                put(definition + 0xdc, shadow)
                p.uc.mem_write(options + 0xf, bytes([enabled]))
                put(base + 0x10, 0)
                for reg, value in ((UC_X86_REG_EBP, base), (UC_X86_REG_ESP, base-0x100),
                                   (UC_X86_REG_ESI, definition), (UC_X86_REG_EDX, 0)):
                    p.uc.reg_write(reg, value)
                calls.clear()
                # No body jumps directly past the body assignment.
                p.uc.emu_start(0x4fd93f, 0x4fd9bb if not body else 0x4fd9b8)
                expected = []
                if shadow and enabled:
                    expected.append(('clock', definition + 0xfc) if animated else ('frame', shadow, 0))
                if body:
                    expected.append(('clock', definition + 0xf0) if animated else ('frame', body, 0))
                assert calls == expected, (animated, enabled, body, shadow, calls, expected)
                count += 1
for enabled in (False, True):
    for mask in range(8):
        put(instance + 0x5d, 4 if mask & 1 else 0)
        put(instance + 0x59, 1 if mask & 2 else 0)
        put(instance + 0x4d, 1 if mask & 4 else 0)
        put(base + 0xc, instance)
        p.uc.mem_write(options + 0xf, bytes([enabled]))
        for reg, value in ((UC_X86_REG_EBP, base), (UC_X86_REG_ESP, base-0x100),
                           (UC_X86_REG_ECX, instance)):
            p.uc.reg_write(reg, value)
        calls.clear()
        p.uc.emu_start(0x4fd7cc, 0x4fd9b8)
        expected = []
        if enabled and mask & 1: expected.append(('clock', instance + 0x10))
        if mask & 2: expected.append(('clock', instance + 0x51))
        if mask & 4: expected.append(('clock', instance + 0x45))
        expected.append(('clock', instance + 4))
        assert calls == expected, (enabled, mask, calls, expected)
        count += 1
print(f'PASS: {count} native feature selections: independent animated body/shadow clocks, static frame zero, burn-instance clocks and shadow toggle')

# Ignition chooses a new burn shadow only when neither flame overlay exists.
p = Icd()
calls = []
def init(uc, sp):
    calls.append(struct.unpack('<3I', uc.mem_read(sp, 12)))
    return 3, 0
p.hooks[0x537390] = init
p.freeze_hooks()
for present in range(4):
    for burn_shadow in (0, HEAP + 0x7000):
        for old_flag in (0, 4):
            body, front, back = HEAP + 0x4000, HEAP + 0x5000, HEAP + 0x6000
            put(definition + 0xe0, body)
            put(definition + 0xe4, burn_shadow)
            put(definition + 0x108, front if present & 1 else 0)
            put(definition + 0x10c, back if present & 2 else 0)
            p.uc.mem_write(instance + 0x5d, bytes([old_flag]))
            for reg, value in ((UC_X86_REG_ESP, base-0x100),
                               (UC_X86_REG_ESI, instance), (UC_X86_REG_EDI, definition)):
                p.uc.reg_write(reg, value)
            calls.clear()
            p.uc.emu_start(0x494c1f, 0x494ca8)
            expected = [(instance + 4, body, 0)]
            if present:
                if present & 1: expected.append((instance + 0x45, front, 0))
                if present & 2: expected.append((instance + 0x51, back, 0))
                flag = old_flag
            else:
                if burn_shadow: expected.append((instance + 0x10, burn_shadow, 0))
                flag = 4 if burn_shadow else 0
            assert calls == expected, (present, burn_shadow, calls, expected)
            assert p.uc.mem_read(instance + 0x5d, 1)[0] == flag
print('PASS: 16 native ignition setups replace/disable burn shadows without overlays, preserve prior shadow state with overlays')

# Burn-shadow loading overrides every authored loop value with nonlooping.
p = Icd()
for authored in range(256):
    animation = HEAP + 0x7000
    p.uc.mem_write(animation + 2, bytes([authored]))
    p.uc.reg_write(UC_X86_REG_EAX, animation)
    p.uc.reg_write(UC_X86_REG_EBX, definition)
    p.uc.emu_start(0x493ea5, 0x493ebf)
    assert p.uc.mem_read(animation + 2, 1) == b'\0'
    assert struct.unpack('<I', p.uc.mem_read(definition + 0xe4, 4))[0] == animation
print('PASS: 256 authored burn-shadow loop values are forced nonlooping by native feature loading')

# The per-tick feature pass advances both type clocks, independently of visibility.
p = Icd()
calls = []
def advance(uc, sp):
    calls.append(struct.unpack('<I', uc.mem_read(sp, 4))[0])
    return 1, 0
p.hooks[0x5373d0] = advance
p.freeze_hooks()
world, definitions = HEAP + 0x10000, HEAP + 0x30000
put(0x62d55c, world)
put(world + 0x19edc, definitions)
for mask in range(32):
    put(world + 0x19ec0, 5)
    for index in range(5):
        put(definitions + index*0x140 + 0x13c, 2 if mask & (1 << index) else 0)
    p.uc.reg_write(UC_X86_REG_ESP, base-0x100)
    calls.clear()
    p.uc.emu_start(0x4959c0, 0x495a12)
    expected = [definitions + index*0x140 + offset for index in range(5)
                if mask & (1 << index) for offset in (0xf0, 0xfc)]
    assert calls == expected, (mask, calls, expected)
print('PASS: 32 feature-type masks advance animated body/shadow clocks once in each native tick pass')
