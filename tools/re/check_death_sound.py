#!/usr/bin/env python3
"""Compare death-audio admission and unit sound routing with native routines.

Headless Unicorn only. Reads the locally owned retail image through emu.Icd;
no retail audio device/game process is started and no assets are copied.
"""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build-o2/death_sound_test')
    args = parser.parse_args()
    p = Icd()
    manager = HEAP
    put = lambda address, value: p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))
    get = lambda address: struct.unpack('<I', p.uc.mem_read(address, 4))[0]
    stopped = []

    def stop(uc, sp):
        stopped.append(get(sp))
        return 1, 0

    p.hooks[0x56EA10] = stop
    p.freeze_hooks()
    rng = random.Random(0x56EA90)
    cases, expected = [], []
    for _ in range(500):
        priority = rng.randrange(8)
        voices = [(True, rng.randrange(4) == 0, rng.randrange(8), rng.randrange(1000))
                  for _ in range(32)]
        for i, (_, looping, level, age) in enumerate(voices):
            record = manager + i * 20
            put(record + 0x2c, HEAP + 0x10000)
            put(record + 0x30, looping)
            put(record + 0x34, level)
            put(record + 0x38, age)
        stopped.clear()
        result, error = p.call(0x56EA90, [priority], ecx=manager)
        assert error is None, error
        assert bool(result & 255) == bool(stopped)
        expected.append(stopped[0] if stopped else -1)
        cases.append(str(priority) + ' ' + ' '.join(str(int(x)) for voice in voices for x in voice))
    actual = subprocess.check_output([args.binary, '--victims'], input='\n'.join(cases)+'\n', text=True)
    assert list(map(int, actual.split())) == expected
    print('PASS: 500 native priority/age/looping victim comparisons')

    p = Icd()
    vm, view, unit, game = [HEAP + n * 0x10000 for n in range(4)]
    put = lambda address, value: p.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))
    get = lambda address: struct.unpack('<I', p.uc.mem_read(address, 4))[0]
    put(vm + 0xa64, view)
    put(view + 0xc, unit)
    put(0x62d55c, game)
    visible, selected, free = True, True, True
    played = []
    p.hooks[0x4F7210] = lambda uc, sp: (2, int(visible))
    p.hooks[0x51FB10] = lambda uc, sp: (1, int(selected))
    p.hooks[0x56EB60] = lambda uc, sp: (0, int(free))

    def positional(uc, sp):
        played.append(1)
        return 3, 123

    def global_sound(uc, sp):
        played.append(2)
        return 4, 123

    p.hooks[0x50A9C0] = positional
    p.hooks[0x50A720] = global_sound
    p.freeze_hooks()
    cases, expected = [], []
    for flags in range(64):
        for visible in (False, True):
            for selected in (False, True):
                for free in (False, True):
                    played.clear()
                    result, error = p.call(0x50DEF0, [HEAP + 0x50000, flags], ecx=vm)
                    assert error is None and result == 0, (error, result)
                    expected.append(played[0] if played else 0)
                    cases.append(f'{flags} {int(visible)} {int(selected)} {int(free)}')
    actual = subprocess.check_output([args.binary, '--routes'], input='\n'.join(cases)+'\n', text=True)
    assert list(map(int, actual.split())) == expected
    print('PASS: 512 native unit-sound visibility/selection/class/free-voice routes and zero returns')


if __name__ == '__main__':
    main()
