#!/usr/bin/env python3
"""Observe retail effect animation startup and single/batched clock updates.

Executes 537390, 5373d0, 537430 and their real frame-duration lookup. Synthetic
animation metadata avoids depending on asset contents or substituting timing.
"""
import random
import subprocess
import sys
import struct
from emu import Icd, HEAP

p = Icd()
p.freeze_hooks()
state, animation = HEAP, HEAP + 0x10000
rng = random.Random(0x537390)
checks = 0
rows, traces = [], []


def call(address, *args):
    result, error = p.call(address, args)
    assert not error, error
    return result


def snapshot():
    frame, remaining, loop = struct.unpack('<HHB', p.uc.mem_read(state, 5))
    active = struct.unpack('<I', p.uc.mem_read(state + 8, 4))[0] != 0
    return frame, remaining, loop, active


for case in range(1280):
    count = rng.randrange(1, 17)
    durations = [rng.randrange(1, 16) for _ in range(count)]
    # Exercise low-word duration extremes too; zero is a one-update frame in
    # the single-tick routine used by caster nimbus and wandering effects.
    if case >= 1024:
        durations[rng.randrange(count)] = (0, 300, 301, 32767, 32768, 65535)[case % 6]
    looping = rng.randrange(2)
    initial = rng.randrange(count + 3)
    metadata = bytearray(0x30 + count * 8)
    struct.pack_into('<HB', metadata, 0, count, looping)
    for index, duration in enumerate(durations):
        struct.pack_into('<H', metadata, 0x2c + index * 8, duration)
    p.uc.mem_write(animation, bytes(metadata))
    for batched in (False, True):
        if batched and case >= 1024:
            # Batch updates have separate signed-duration behavior. Edge values
            # here target the single-update clock used by nimbus and storms.
            continue
        p.uc.mem_write(state, bytes(12))
        call(0x537390, state, animation, initial)
        frame = initial if initial < count else 0
        remaining, active = durations[frame], True
        assert snapshot() == (frame, remaining, looping, active), case
        trace = [frame, remaining, int(active)]
        for step in range(64):
            elapsed = rng.randrange(1, 17) if batched else 1
            changed = False
            if active and (not batched or count > 1):
                if batched:
                    remaining -= elapsed
                    while remaining <= 0 and active:
                        frame += 1
                        if frame == count:
                            if looping:
                                frame = 0
                            else:
                                active = False
                                break
                        remaining += durations[frame]
                elif remaining < 2:
                    changed = True
                    frame += 1
                    if frame == count:
                        if looping:
                            frame = 0
                        else:
                            active = False
                    if active:
                        remaining = durations[frame]
                else:
                    remaining -= 1
            if batched:
                call(0x537430, state, elapsed)
            else:
                result = call(0x5373d0, state)
                assert result == int(changed), (case, step, result, changed)
            assert snapshot() == (frame, remaining & 65535, looping, active), (case, batched, step, snapshot(), frame, remaining, active)
            checks += 1
            trace.extend((frame, remaining & 65535, int(active)))
        if not batched:
            rows.append(" ".join(map(str, [count, looping, initial, 64] + durations)))
            traces.append(trace)
print(f'PASS: {checks} native effect clock updates; authored durations, looping, expiration, startup bounds and single-frame batch hold')

if len(sys.argv) > 1:
    result = subprocess.run([sys.argv[1], '--effect-clock'], input='\n'.join(rows)+'\n', text=True, capture_output=True, check=True)
    actual = [list(map(int, line.split())) for line in result.stdout.splitlines()]
    assert actual == traces, 'live effect clock differs from native single-tick traces'
    print(f'PASS: shared C++ effect clock matches {len(rows)} native 64-update timelines')
