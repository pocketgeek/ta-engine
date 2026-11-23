#!/usr/bin/env python3
"""Compare native mover callback requests with the shared World helper.

The native side executes retail's 0x4dc800 mover and captures its calls into
the script dispatcher. This small roster matrix uses the shipped ARAARCH,
VERBALL and VERTRANS FBI/COB pairs to cover ground, flying and water units.
All profiles follow the native point-route controller, with mover mode and
terrain/water level set for their authored movement class. No retail GUI is
launched.

The capture is at the script-dispatch boundary, before COB method lookup. The
comparison therefore checks movement-generated callback requests and ordering;
the shipped COB declarations are also verified, but absent script methods are
not executed by this probe.
"""
import argparse
import os
import re
import struct
import subprocess
from pathlib import Path

from emuphase import ARENA, GS, TYPE, Phase
from unicorn.x86_const import UC_X86_REG_FPCW


ROOT = Path(__file__).resolve().parents[2]
# The extracted retail files: TAK_TEST_EXTRACTED when ctest sets it (a worktree has
# no assets/ of its own), else the source tree's.
EXTRACTED_ROOT = Path(os.environ.get('TAK_TEST_EXTRACTED') or ROOT / 'assets/extracted/all')
UNIT_ROOT = EXTRACTED_ROOT / 'units'
SCRIPT_ROOT = EXTRACTED_ROOT / 'scripts'
CALLBACKS = ('TurnDirection', 'MoveRate', 'setSFXoccupy')


PROFILES = (
    # Mode 1 is a surface mover; the raised height and zero sea level produce
    # dry-ground occupancy 4. This authored ground script has TurnDirection.
    dict(unit='araarch', mode=1, y=120, sea=0, waterline=0, model_top=0,
         movement_class='ground',
         expected_methods={'TurnDirection'}, world_inputs=((-40, 1, 4), (-40, 1, 4)),
         expected=[[('TurnDirection', (-40,)), ('MoveRate', (1,)),
                    ('setSFXoccupy', (4,))], []]),
    # VERTRANS is a real floater on WATER4. Height=99, sea=100, its authored
    # waterline=1 and controlled model top=5 select the floating band (2).
    dict(unit='vertrans', mode=1, y=99, sea=100, waterline=1, model_top=5,
         movement_class='water',
         expected_methods={'TurnDirection', 'MoveRate'},
         world_inputs=((-40, 1, 2), (-40, 1, 2)),
         expected=[[('TurnDirection', (-40,)), ('MoveRate', (1,)),
                    ('setSFXoccupy', (2,))], []]),
    # VERBALL is a shipped canfly unit with both turn and speed call-ins.
    dict(unit='verball', mode=2, y=100, sea=0, waterline=0, model_top=0,
         movement_class='flying',
         expected_methods={'TurnDirection', 'MoveRate'},
         world_inputs=((-135, 3, 5), (-135, 3, 5)),
         expected=[[('TurnDirection', (-135,)), ('MoveRate', (3,)),
                    ('setSFXoccupy', (5,))], []]),
)


def put(uc, address, *values):
    uc.mem_write(address, struct.pack('<' + 'I' * len(values),
                                      *(value & 0xffffffff for value in values)))


def fixed(value):
    # Native FBI fixed-point parsing truncates after multiplying by 65536.
    return int(float(value) * 65536)


def fbi_info(unit):
    text = (UNIT_ROOT / f'{unit}.fbi').read_text(encoding='latin1')
    match = re.search(r'\[UNITINFO\]\s*\{(.*?)^\}', text, re.S | re.M)
    if not match:
        raise ValueError(f'{unit}.fbi lacks UNITINFO')
    fields = {}
    for key, value in re.findall(r'^\s*([\w]+)\s*=\s*([^;\r\n]*?)\s*;',
                                 match.group(1), re.M | re.I):
        fields[key.lower()] = value.strip()
    return fields


def cob_methods(unit):
    data = (SCRIPT_ROOT / f'{unit}.cob').read_bytes()
    header = struct.unpack_from('<10I', data)
    names = []
    for index in range(header[1]):
        offset = struct.unpack_from('<I', data, header[7] + 4 * index)[0]
        names.append(data[offset:].split(b'\0', 1)[0].decode('ascii'))
    return set(names)


def profile_type_fields(uc, info, profile):
    def number(name, default):
        return float(info.get(name, default))

    maximum = number('maxvelocity', 0)
    put(uc, TYPE + 0x162, fixed(maximum))
    put(uc, TYPE + 0x166, fixed(number('brakerate', 0.5)))
    put(uc, TYPE + 0x16a, fixed(number('acceleration', 0.5)))
    put(uc, TYPE + 0x16e, fixed(number('watermultiplier', 1)))
    put(uc, TYPE + 0x172, fixed(number('roadmultiplier', 1.2)))
    put(uc, TYPE + 0x182, fixed(number('moverate1', 2 * maximum)))
    put(uc, TYPE + 0x186, fixed(number('moverate2', 2 * maximum)))
    uc.mem_write(TYPE + 0x18e, struct.pack('<H', int(number('turnrate', 500)) & 0xffff))
    uc.mem_write(TYPE + 0x190, struct.pack('<H', int(number('turninplacerate', 0)) & 0xffff))
    uc.mem_write(TYPE + 0x14c, struct.pack('<h', profile['model_top']))
    uc.mem_write(TYPE + 0x248, bytes((profile['waterline'] & 0xff,)))
    put(uc, TYPE + 0x260, 0x800 if 'canfly' in info and number('canfly', 0) else 0)


def native_trace(profile):
    p = Phase(64, 64)
    p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    unit = p.unit(10, 10)
    mover, navigator = unit + 0x300, ARENA + 0x0d10000
    settings, options = ARENA + 0x0d21000, ARENA + 0x0d20000
    info = fbi_info(profile['unit'])
    methods = cob_methods(profile['unit'])
    if not profile['expected_methods'].issubset(methods):
        raise AssertionError((profile['unit'], sorted(methods),
                              sorted(profile['expected_methods'])))
    declared_methods = methods.intersection(CALLBACKS)
    if declared_methods != profile['expected_methods']:
        raise AssertionError((profile['unit'], sorted(declared_methods),
                              sorted(profile['expected_methods'])))
    if (('canfly' in info and bool(float(info['canfly']))) !=
            (profile['movement_class'] == 'flying')):
        raise AssertionError((profile['unit'], 'FBI mobility class changed'))
    authored_waterline = int(float(info.get('waterline', 0)))
    if authored_waterline != profile['waterline']:
        raise AssertionError((profile['unit'], 'FBI waterline changed', authored_waterline))
    authored_class = info.get('movementclass', '').upper()
    if profile['movement_class'] == 'water' and not authored_class.startswith('WATER'):
        raise AssertionError((profile['unit'], 'FBI water class changed', authored_class))
    if profile['movement_class'] == 'ground' and not authored_class.startswith('GROUND'):
        raise AssertionError((profile['unit'], 'FBI ground class changed', authored_class))

    put(p.uc, 0x62d55c, GS)
    put(p.uc, 0x62d558, settings)
    put(p.uc, settings + 8, options)
    p.uc.mem_write(options, bytes(0x100))
    p.uc.mem_write(GS + 0x19ef8, bytes((profile['sea'],)))
    sector_stride = 8
    sectors = p._alloc(sector_stride * sector_stride * 10)
    put(p.uc, GS + 0x19f18, sectors)
    put(p.uc, GS + 0x19f1c, sector_stride)
    put(p.uc, GS + 0x19f30, 1)
    for index in range(sector_stride * sector_stride):
        p.uc.mem_write(sectors + index * 10 + 1, b'\x64')
    current_sector = sectors + ((10 * 16 >> 7) * sector_stride +
                                (10 * 16 >> 7)) * 10

    start_y = profile['y']
    start, target = (160, start_y, 160), (800, start_y, 800)
    put(p.uc, unit + 0xa4, current_sector)
    put(p.uc, current_sector + 6, unit)
    put(p.uc, unit + 0x68, *(value << 16 for value in start))
    p.uc.mem_write(unit + 0x7e, struct.pack('<H', 0))
    max_velocity = fixed(float(info.get('maxvelocity', 1.25)))
    put(p.uc, unit + 0x12b, max_velocity)
    put(p.uc, unit + 0x130, 0x01000000)
    put(p.uc, mover, navigator)
    put(p.uc, mover + 0x20, max_velocity)
    put(p.uc, mover + 0x30, 0x7fffffff)

    p.uc.mem_write(mover + 0x36, struct.pack('<H', profile['mode']))

    put(p.uc, navigator, 0x5f34d4)
    put(p.uc, navigator + 8, unit)
    put(p.uc, navigator + 0x0c, *(value << 16 for value in start))
    p.uc.mem_write(navigator + 0x24, struct.pack('<H', 0))

    profile_type_fields(p.uc, info, profile)

    controller, mission, point = (p._alloc(n) for n in (0x100, 0x100, 12))
    put(p.uc, mission + 0x0e, unit)
    put(p.uc, point, *(value << 16 for value in target))
    _, error = p.icd.call(0x4e40e0, (mission, point), ecx=controller)
    assert error is None, error
    _, error = p.icd.call(0x4e4540, (16,), ecx=controller)
    assert error is None, error
    put(p.uc, navigator + 4, controller)

    calls = []

    def capture(uc, sp):
        args = struct.unpack('<8I', uc.mem_read(sp, 32))
        name = bytes(uc.mem_read(args[0], 64)).split(b'\0')[0].decode('ascii')
        values = tuple(struct.unpack('<i', struct.pack('<I', value))[0]
                       for value in args[4:4 + args[3]])
        calls.append((name, values))
        return 8, 0

    p.icd.hooks[0x56c640] = capture
    p.icd.freeze_hooks()

    traces = []
    for tick in (1, 2):
        put(p.uc, GS + 0x19f44, tick)
        calls.clear()
        _, error = p.icd.call(0x4dc800, (unit,), ecx=mover)
        assert error is None, (profile['unit'], tick, error)
        traces.append(list(calls))
    return traces, methods


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build-o2/retail_movement_animation_test')
    args = parser.parse_args()

    for profile in PROFILES:
        retail, methods = native_trace(profile)
        if retail != profile['expected']:
            raise AssertionError({'unit': profile['unit'], 'expected retail': profile['expected'],
                                  'actual retail': retail})
        # These are independent World helper inputs for each scenario, held
        # constant on the second tick to verify native edge suppression.
        rows = ''.join('%d %d %d\n' % row for row in profile['world_inputs'])
        result = subprocess.run([args.binary, '--trace'], input=rows,
                                check=True, capture_output=True, text=True)
        client = result.stdout.splitlines()
        expected = [' '.join(f'{name}({arguments[0]})' for name, arguments in events)
                    if events else '-' for events in profile['expected']]
        if client != expected:
            raise AssertionError({'unit': profile['unit'], 'retail': retail,
                                  'World helper': client, 'expected': expected})
        declared = ', '.join(name for name in CALLBACKS if name in methods) or '(none)'
        print(f"PASS: {profile['unit'].upper()} ({profile['movement_class']}) native 0x4dc800 ticks 1–2; "
              f"COB declares {declared}; requests="
              f"{retail}")

    print('PASS: 6 native mover ticks match updateRetailMovementAnimationCallbacks')


if __name__ == '__main__':
    main()
