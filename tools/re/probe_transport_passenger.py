#!/usr/bin/env python3
"""Observe Move_Seek_Pickup scheduling in the user-owned retail executable.

Runs the native handler and sleep routine. Eligibility, name lookup and navigator
installation are controlled sinks. Also compares the port scheduling core;
World passenger movement integration remains a separate requirement.
"""
import struct
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_ECX

p = Icd()
# Mission descriptors end with the script-visible name. The handler preceding
# Move_Seek_Pickup is 403430; 407f90 belongs to the following GUARD record.
assert struct.unpack('<I', p.uc.mem_read(0x5eb88a, 4))[0] == 0x403430
assert struct.unpack('<I', p.uc.mem_read(0x5eb89b, 4))[0] == 0x604cf8
assert bytes(p.uc.mem_read(0x604cf8, 17)) == b'Move_Seek_Pickup\0'
passenger, carrier, mission, carrier_mission, kind, game = [HEAP+i*0x10000 for i in range(6)]
def put(a, v): p.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
def read(a): return struct.unpack('<I', p.uc.mem_read(a, 4))[0]
def byte(a, v): p.uc.mem_write(a, bytes([v]))
trace = []
eligible = True

def name(uc, sp):
    dest = uc.reg_read(UC_X86_REG_ECX)
    key = read(sp)
    assert key in (0x604c00, 0x604de4), hex(key)
    byte(dest, 1 if key == 0x604c00 else 2)
    return 1, dest

def approach(uc, sp):
    point, radius = read(sp), read(sp+4)
    trace.append(('approach', tuple(struct.unpack('<3i', uc.mem_read(point, 12))), radius))
    return 2, 0

def detach(uc, sp):
    assert read(sp) == 0
    trace.append(('detach',))
    return 1, 0

p.hooks.update({0x519f50: lambda uc, sp: (1, int(eligible)),
                0x4d4bf0: name, 0x4d4da0: approach, 0x4d4d40: detach})
p.freeze_hooks()
put(0x62d55c, game)
put(game+0x19f44, 100)
put(carrier+0xb4, kind)
position = (123*65536+5, 17*65536, -45*65536-9)
p.uc.mem_write(carrier+0x68, struct.pack('<3i', *position))
p.uc.mem_write(kind+0x23e, struct.pack('<H', 150))
count = 0

def reset(stage=0, chain=(1,), attached=0):
    p.uc.mem_write(mission, bytes(0x80))
    put(mission+0x16, carrier)
    byte(mission+5, stage)
    put(passenger+0xa8, attached)
    put(carrier+0x60, carrier_mission if chain else 0)
    for i, code in enumerate(chain):
        node = carrier_mission+i*0x100
        byte(node+4, code)
        put(node+0x66, node+0x100 if i+1<len(chain) else 0)
    trace.clear()

def invoke(events=0):
    global count
    result, error = p.call(0x403430, (passenger, mission, events))
    assert not error, error
    count += 1
    return result

# Completion takes precedence over eligibility and carrier mission lookup.
for attached, expected in ((carrier, 5), (carrier+0x1000, 8)):
    reset(attached=attached, chain=())
    assert invoke() == expected and not trace
reset(chain=()); put(mission+0x16, 0)
# The native equality check treats two null references as completion too.
assert invoke() == 5
eligible = False
reset(); assert invoke() == 8
eligible = True
for chain in ((), (3,), (3, 4)):
    reset(chain=chain); assert invoke() == 8 and not trace

# Searches the carrier's mission chain for either pickup class. It does not
# require the pickup to target this particular passenger at this point.
for chain in ((1,), (2,), (3, 1), (3, 2), (2, 1), (1, 2)):
    reset(chain=chain)
    assert invoke() == 1
    ground = next(code for code in chain if code in (1, 2)) == 1
    assert trace == ([('approach', position, 134)] if ground else [('detach',)])
    assert read(mission+6) == (0x789 if ground else 0x89)
    assert read(mission+0xa) == 130
    assert tuple(struct.unpack('<3i', p.uc.mem_read(mission+0x22, 12))) == position

for air in (False, True):
    for events in (0, 1, 8, 0x80, 0x100, 0x200, 0x400, 0x700, 0x780):
        reset(stage=1, chain=(2 if air else 1,))
        put(mission+0x4e, 4)
        result = invoke(events)
        if events & 0x80:
            assert result == 1 and trace == [('detach',)]
            assert read(mission+0x4e) == 0
        elif events & 0x700:
            assert result == 2 and trace == [('detach',)]
            assert read(mission+6) == 0x89 and read(mission+0xa) == 130
        else:
            assert result == 4 and not trace
            assert p.uc.mem_read(mission+5, 1) == b'\0'
            assert read(mission+6) == 1 and read(mission+0xa) == 105
    for retries in range(8):
        reset(stage=2, chain=(2 if air else 1,))
        put(mission+0x4e, retries)
        result = invoke()
        assert read(mission+0x4e) == retries+1 and not trace
        assert result == (2 if retries < 5 else 8)
        if retries < 5:
            assert read(mission+0xa) == 130 and read(mission+6) == 1
    reset(stage=3, chain=(2 if air else 1,))
    assert invoke() == 7
print(f'PASS: {count} native passenger pickup cases: attachment, carrier mission chain, air/sea approach, event precedence, waits and retry exhaustion')

# Compare the port's scheduling core against native calls with retained masks,
# deadlines, counter wraparound and combined events, not only zeroed fixtures.
import random
import subprocess
import sys
rng = random.Random(0x403430)
rows, expected = [], []
for _ in range(4096):
    stage = rng.choice((0, 1, 1, 2, 2, 3, 255))
    retries = rng.choice((0, 4, 5, 6, 0x7fffffff, 0x80000000, 0xffffffff))
    now = rng.choice((0, 100, 0xfffffff0, rng.getrandbits(32)))
    events = rng.getrandbits(12)
    surface = rng.randrange(2)
    mask, deadline = rng.getrandbits(16), rng.getrandbits(32)
    reset(stage=stage, chain=(1 if surface else 2,))
    put(mission+0x4e, retries)
    put(mission+6, mask)
    put(mission+0xa, deadline)
    put(game+0x19f44, now)
    result = invoke(events)
    rows.append(f'{stage} {retries} {now} {events} {surface} {mask} {deadline}')
    expected.append((result, p.uc.mem_read(mission+5, 1)[0], read(mission+0x4e),
                     read(mission+6), read(mission+0xa),
                     sum(t[0] == 'approach' for t in trace),
                     sum(t[0] == 'detach' for t in trace)))
result = subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-o2/transport_test',
                         '--passenger-pickup'], input='\n'.join(rows)+'\n',
                        text=True, capture_output=True, check=True)
actual = [tuple(map(int, line.split())) for line in result.stdout.splitlines()]
assert len(actual) == len(expected), (len(actual), len(expected))
for i, (a, e) in enumerate(zip(actual, expected)):
    assert a == e, (i, rows[i], a, e)
print(f'PASS: {len(rows)} native/port passenger scheduler comparisons, including retained masks, event precedence and integer wraparound')

# Retain the same mission memory over a blocked approach and its successive
# waits. Stage installation is explicit here; the full native dispatcher and
# actual navigator remain separate integration gates.
for surface in (False, True):
    reset(chain=(1 if surface else 2,))
    put(game+0x19f44,100)
    assert invoke()==1
    byte(mission+5,1)
    trace.clear()
    assert invoke(0x100)==2 and trace==[('detach',)]
    byte(mission+5,2)
    deadlines=[]
    for retry in range(6):
        put(game+0x19f44,read(mission+0xa))
        trace.clear()
        result=invoke()
        assert read(mission+0x4e)==retry+1 and not trace
        assert result==(2 if retry<5 else 8)
        if retry<5:deadlines.append(read(mission+0xa))
    assert deadlines==[160,190,220,250,280],deadlines
    # Reusing the exhausted mission cannot override a completed attachment.
    put(passenger+0xa8,carrier)
    trace.clear()
    assert invoke()==5 and not trace
print('PASS: retained air/sea passenger missions preserve five 30-tick retry waits, exhaust on retry six, and prioritize completed attachment')
