#!/usr/bin/env python3
"""Pair World flight integration with retail's native unit mover over many ticks.

The native side executes the real point controller, 0x524af0 flight navigator,
0x4dc800 mover, heading update, and 0x51b2a0 position commit. Only the script
notification is suppressed for this model-free fixture. The flat terrain and
body-sector plane are shared inputs; terrain scanning is held beyond the trace.
"""
import argparse
import struct
import subprocess

from emuphase import ARENA, GS, TYPE, Phase
from unicorn.x86_const import UC_X86_REG_FPCW


def put(uc, address, *values):
    uc.mem_write(address, struct.pack('<' + 'I' * len(values),
                                      *(value & 0xffffffff for value in values)))


def words(uc, address, count):
    return struct.unpack('<' + 'i' * count, uc.mem_read(address, count * 4))


def native_trace(steps, unload=False):
    width = 256 if unload else 64
    start = (3000, 100, 3000) if unload else (160, 100, 160)
    target = (4000, 100, 3000) if unload else (800, 100, 800)
    p = Phase(width, width)
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    unit = p.unit(10, 10)
    mover, navigator = unit + 0x300, ARENA + 0x0d10000
    settings, options = ARENA + 0x0d21000, ARENA + 0x0d20000

    put(p.uc, 0x62d55c, GS)
    put(p.uc, 0x62d558, settings)
    put(p.uc, settings + 8, options)
    p.uc.mem_write(options, bytes(0x100))
    sector_stride = (width + 7) // 8
    sectors = p._alloc(sector_stride * sector_stride * 10)
    put(p.uc, GS + 0x19f18, sectors)
    put(p.uc, GS + 0x19f1c, sector_stride)
    put(p.uc, GS + 0x19f30, 1)
    # Retail's body-sector records expose the terrain height at +1. All sectors
    # use the same 100-unit plane as the World fixture.
    for index in range(sector_stride * sector_stride):
        p.uc.mem_write(sectors + index * 10 + 1, b'\x64')
    sector_x, sector_z = start[0] >> 7, start[2] >> 7
    current_sector = sectors + (sector_z * sector_stride + sector_x) * 10

    put(p.uc, unit + 0xa4, current_sector)
    put(p.uc, current_sector + 6, unit)
    put(p.uc, unit + 0x68, *(value << 16 for value in start))
    p.uc.mem_write(unit + 0x7e, struct.pack('<H', 0))
    put(p.uc, unit + 0x12b, 120000)
    put(p.uc, unit + 0x130, 0x01000000)
    put(p.uc, mover, navigator)
    put(p.uc, mover + 0x20, 0)
    put(p.uc, mover + 0x30, 0x7fffffff)  # pin terrain scan beyond this trace
    p.uc.mem_write(mover + 0x36, struct.pack('<H', 2))

    put(p.uc, navigator, 0x5f34d4)
    put(p.uc, navigator + 8, unit)
    put(p.uc, navigator + 0x0c, *(value << 16 for value in start))
    p.uc.mem_write(navigator + 0x24, struct.pack('<H', 0))

    # Native UnitDef fields: brakerate is the flight kernel's lateral limit,
    # acceleration is its 3D acceleration cap, and 0x800 enables the flyer mover.
    p.uc.mem_write(TYPE + 0x166, struct.pack('<i', 60000))
    p.uc.mem_write(TYPE + 0x16a, struct.pack('<i', 20000))
    p.uc.mem_write(TYPE + 0x16e, struct.pack('<i', 65536))
    p.uc.mem_write(TYPE + 0x172, struct.pack('<i', 65536))
    p.uc.mem_write(TYPE + 0x18e, struct.pack('<H', 10000))
    p.uc.mem_write(TYPE + 0x23a, struct.pack('<h', 100))
    put(p.uc, TYPE + 0x260, 0x800)

    controller, mission, point = (p._alloc(n) for n in (0x100, 0x100, 12))
    put(p.uc, mission + 0x0e, unit)
    put(p.uc, point, *(value << 16 for value in target))
    _, error = p.icd.call(0x4e40e0, (mission, point), ecx=controller)
    assert error is None, error
    # The generic point controller has its native default 0.5px acceptance.
    # VTOL unload installs an explicit transportdistance-34 circle instead.
    if unload:
        _, error = p.icd.call(0x4e4540, (116,), ecx=controller)
        assert error is None, error
    put(p.uc, navigator + 4, controller)

    # This synthetic unit has no COB VM. Keep the native movement/navigation,
    # heading and attitude routines intact while suppressing only that callback.
    p.icd.hooks[0x56c640] = lambda _uc, _args: (8, 0)
    p.icd.freeze_hooks()

    rows, attached = [], []
    for tick in range(1, steps + 1):
        put(p.uc, GS + 0x19f44, tick)
        _, error = p.icd.call(0x4dc800, (unit,), ecx=mover)
        assert error is None, (tick, error)
        position = words(p.uc, unit + 0x68, 3)
        heading = struct.unpack('<H', p.uc.mem_read(unit + 0x7e, 2))[0]
        velocity = words(p.uc, mover + 8, 3)
        speed = struct.unpack('<i', p.uc.mem_read(mover + 0x20, 4))[0]
        nav_out = words(p.uc, navigator + 0x0c, 6)
        nav_heading = struct.unpack('<H', p.uc.mem_read(navigator + 0x24, 2))[0]
        rows.append((tick, *position, heading, speed, *velocity, *nav_out, nav_heading))
        attached.append(struct.unpack('<I', p.uc.mem_read(navigator + 4, 4))[0] == controller)
    return rows, attached


def check(binary, steps, unload=False):
    mode = '--air-unload-flight-trace' if unload else '--air-flight-trace'
    result = subprocess.run([binary, mode, str(steps)],
                            check=True, capture_output=True, text=True)
    world = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]
    retail, attached = native_trace(steps, unload)
    if len(world) != steps:
        raise AssertionError(f'World emitted {len(world)} rows, expected {steps}')
    if not unload:
        if not attached[0]:
            raise AssertionError('native point controller was not attached at the start')
        detached = next((index for index, active in enumerate(attached) if not active), None)
        if detached is None or detached == 0:
            raise AssertionError('native point controller did not detach during the trace')
        before, after = retail[detached - 1], retail[detached]
        if before[1:4] == after[1:4] or after[5] <= 0:
            raise AssertionError('native mover did not commit a final moving step after detach')
    for index, (got, want) in enumerate(zip(world, retail), 1):
        if len(got) < len(want) or got[:len(want)] != want:
            raise AssertionError({'tick': index, 'World': got[:len(want)], 'retail': want})
    case = 'VTOL unload-circle approach' if unload else 'point-flight'
    print(f'PASS: {steps} paired retail/World {case} mover ticks match exactly '
          'for position, altitude, heading, speed, velocity, and navigator outputs')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build-o2/transport_test')
    parser.add_argument('--steps', type=int, default=None,
                        help='ticks to trace (defaults to 600 for point flight, 128 for the single-controller unload approach)')
    parser.add_argument('--unload', action='store_true',
                        help='trace the retail transportdistance-34 VTOL unload circle')
    args = parser.parse_args()
    steps = args.steps if args.steps is not None else (128 if args.unload else 600)
    check(args.binary, max(1, min(steps, 10000)), args.unload)


if __name__ == '__main__':
    main()
