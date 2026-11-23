#!/usr/bin/env python3
"""Compare captured surface units' composed movement and shared asynchronous search.

Both implementations retain their own evolving positions, occupancy, search
state and delivered routes. --also-unit adds concurrent requesters in allocation
order; unselected units remain stationary. All movers share one worker tick.
Boat scan visibility uses one fixed viewer, the lowest selected slot's owner,
on both sides. Exploration, mission
creation and the height clock are controlled initial/host inputs; the mission
handler, combat, scripts and render cadence do not advance. Native formation is
disabled. This is a movement/search integration comparison, not full game parity.
"""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
from types import SimpleNamespace
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_ESP

from balance_inputs import set_balance_inputs
from check_captured_tick import find_crt_thread
from emureload import CapturedProcess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture', type=Path)
    ap.add_argument('--fixture', type=Path, required=True)
    ap.add_argument('--save', type=Path, required=True)
    ap.add_argument('--balance', choices=('standard', 'crusades'), required=True)
    ap.add_argument('--unit', type=int, required=True)
    ap.add_argument('--also-unit', type=int, action='append', default=[], help='additional concurrent surface requester; repeatable')
    ap.add_argument('--rounds', type=int, default=160)
    ap.add_argument('--budget', type=int, default=503)
    ap.add_argument('--dx', type=int, default=512)
    ap.add_argument('--dz', type=int, default=0)
    ap.add_argument('--retail-root', type=Path, default=Path('assets/game'))
    ap.add_argument('--runner', type=Path, default=Path('build-dbg/retail_replay_probe'))
    ap.add_argument('--diagnostic-dir', type=Path, help='retain initial fixture, results and ordered grade traces')
    args = ap.parse_args()
    if not 1 <= args.rounds <= 5000 or args.budget < 1:
        ap.error('positive budget and 1..5000 rounds required')
    capture = json.loads(args.capture.read_text())
    p = CapturedProcess(capture)
    selected = set_balance_inputs(p, capture, args.save, args.retail_root,
                                  args.balance == 'crusades', surface=True, motion=True)
    addresses = {u['id']: u['address'] for u in p.runtime['units']}
    identities=sorted([args.unit,*args.also_unit])
    if len(set(identities))!=len(identities): ap.error('requester IDs must be distinct')
    for identity in identities:
        if identity not in selected:
            raise ValueError('selected unit is not a captured surface mover')
        unit=addresses[identity]
        if p.u32(unit+0xa8) or p.u32(unit+0x130)&3!=1:
            raise ValueError('selected unit must be an unattached surface mover')

    def read(fmt, address):
        return struct.unpack('<' + fmt, p.uc.mem_read(address, struct.calcsize('<' + fmt)))

    def write(fmt, address, *values):
        p.put(address, struct.pack('<' + fmt, *values))

    def alloc(size):
        address = p.brk
        p.brk = (address + size + 15) & ~15
        p.put(address, bytes(size))
        return address

    fixture = args.fixture.read_text().splitlines()
    header = fixture[0].split()
    version=int(header[1])
    if version not in (34,36,37) or int(header[2]) != p.frame['tick']:
        raise ValueError('matching version-34, version-36 or version-37 fixture required')
    header[1] = '37'; header[5] = '0'; header[-1] = str(int(args.balance == 'crusades'))
    fixture[0] = ' '.join(header)
    if version==34:
        fixture.append(str(len(p.frame['units'])))
        for u in p.frame['units']:
            pointer = addresses[u['id']]
            mv = u.get('mover_address', 0)
            phase = read('H', pointer + 0x82)[0]
            stamp = p.u32(mv + 0x2c) if mv else 0
            pitch, roll = read('H', pointer + 0x80)[0], read('H', pointer + 0x7c)[0]
            fixture.append(f'{u["id"]} {u["position_raw"][1]} {stamp} {phase} {pitch} {roll}')
    width, height = read('2I', p.game + 0x19e98)
    # A balance switch can change a representative's movement class (e.g.
    # GROUND5 -> GROUND4). Rebind initial caches by the selected native class;
    # retaining the old representative ID would silently relabel its old plane.
    packed_length = width * ((height + 7) // 8) * 8
    plane_rows = [i for i, line in enumerate(fixture)
                  if len(parts := line.split()) == 4 and len(parts[-1]) == packed_length]
    if not plane_rows or plane_rows != list(range(plane_rows[0], plane_rows[-1] + 1)):
        raise ValueError('contiguous captured grade planes required')
    if int(fixture[plane_rows[0] - 1]) != len(plane_rows):
        raise ValueError('captured grade plane count mismatch')
    representatives = {}
    for identity, grid in sorted(selected.items()):
        representatives.setdefault(grid, identity)
    planes = []
    for grid, identity in representatives.items():
        gw, gh, pointer = read('3I', grid + 0x340)
        if (gw, gh) != (width, height) or not pointer:
            raise ValueError('selected movement class has no captured grade plane')
        packed = bytes(p.uc.mem_read(pointer, gw * ((gh + 7) // 8) * 4)).hex()
        recent, stale = read('2I', grid + 0x34c)
        planes.append(f'{identity} {recent} {stale} {packed}')
    fixture[plane_rows[0] - 1:plane_rows[-1] + 1] = [str(len(planes)), *planes]
    width //= 2; height //= 2
    viewer=read('B',addresses[identities[0]]+0xfd)[0]
    if version==37:
        count=len(p.frame['units'])
        if int(fixture[-count-1])!=count or tuple(map(int,fixture[-count-3].split()[:2]))!=(width,height):
            raise ValueError('invalid existing exploration tail')
        del fixture[-count-3:]
    fixture.append(f'{width} {height} {viewer}')
    fixture.append(' '.join(map(str, read('H' * (width * height), p.u32(p.game + 0x19ef4)))))
    fixture.append(str(len(p.frame['units'])))
    for u in p.frame['units']:
        fixture.append(' '.join(map(str, [u['id'], *read('hhihBB', addresses[u['id']] + 0x98)])))

    # Start a fresh shared worker, retaining the captured terrain,
    # explored map, units and grade-cache inputs on both sides.
    for u in p.runtime['units']:
        mv = p.u32(u['address'] + 8)
        if mv and p.u32(mv):
            nv = p.u32(mv)
            write('B', nv + 0x114, read('B', nv + 0x114)[0] & ~2)
    write('10I', 0x634674, *([0] * 10))
    obj = alloc(0x400)
    _, error = p.icd.call(0x415f80, ecx=obj)
    assert error is None, error
    p.obj = obj
    write('I', p.game + 0x19e70, obj); write('I', obj + 0x225, args.budget)
    actors=[]
    for identity in identities:
        unit=addresses[identity];mover=p.u32(unit+8);nav=p.u32(mover)
        player = p.u32(unit + 0xb8)
        owner = read('B', unit + 0xfd)[0]
        first, last = read('2I', player + 0x74)
        pool_first = identity - (unit - first) // 0x138
        pool_count = (last - first) // 0x138 + 1
        scan_limit = p.u32(p.u32(p.u32(0x62d558) + 8) + 12)
        if pool_count != scan_limit or not 0 <= identity - pool_first < pool_count:
            raise ValueError('captured pool and scheduler scan limit disagree')
        write('I', obj + 0x115 + 4 * owner, first)
        point = alloc(12)
        x, y, z = read('3i', unit + 0x68)
        gx, gz = x + args.dx * 65536, z + args.dz * 65536
        write('3i', point, gx, y, gz)
        mission = alloc(0x72)
        _, error = p.icd.call(0x4d6c40, (28, 0, point, 0, 0, 0, 0, 0, 0, 0, 0, 0), ecx=mission)
        assert error is None, error
        write('I', mission + 0xe, unit); write('I', unit + 0x60, mission)
        write('I', nav + 4, 0); write('I', nav + 0x10c, 0); write('B', nav + 0x114, 0)
        write('I', nav + 0x110, 0)  # same fresh admission stamp as the World fixture
        write('I', unit + 0x130, p.u32(unit + 0x130) & ~0x4000)
        write('H', mover + 0x36, read('H', mover + 0x36)[0] & ~0xf0)
        write('I', mover + 0x30, 0); write('B', unit + 0x134, 0)
        p.icd.hooks[p.u32(p.u32(nav) + 0x34)] = lambda uc, a: (0, 0)
        actors.append(SimpleNamespace(identity=identity,unit=unit,mover=mover,nav=nav,mission=mission,
                                      owner=owner,gx=gx,gz=gz,first=pool_first,count=pool_count,position=(x,z)))
    write('B',p.game+0x306f,viewer)
    thread = find_crt_thread(p, capture['rng_calls'][0]['registers']['ebp'])
    p.icd.hooks[0x5dc403] = lambda uc, a: (0, thread)
    clock = p.frame['tick']
    p.icd.hooks[0x53ff20] = lambda uc, a: (0, clock)
    grades = []
    query = []
    deliveries = {identity:[] for identity in identities}
    nav_owners={actor.nav:actor.identity for actor in actors}
    def delivered(uc, address, size, data):
        identity=nav_owners.get(uc.reg_read(UC_X86_REG_ECX))
        if identity is not None: deliveries[identity].append(clock)
    p.uc.hook_add(UC_HOOK_CODE, delivered, begin=0x4e4ea0, end=0x4e4ea0)
    if args.diagnostic_dir:
        def entry(uc, address, size, data):
            query[:] = read('2i', uc.reg_read(UC_X86_REG_ESP) + 4)

        def returned(uc, address, size, data):
            value = uc.reg_read(UC_X86_REG_EAX)
            if value >= 2**31: value -= 2**32
            grades.append([clock, *query, value])

        p.uc.hook_add(UC_HOOK_CODE, entry, begin=0x4139d0, end=0x4139d0)
        for address in (0x4139fe, 0x413a51, 0x413a9a, 0x413ae1, 0x413bb9, 0x413bd7, 0x413bf7, 0x413c36, 0x413c77):
            p.uc.hook_add(UC_HOOK_CODE, returned, begin=address, end=address)
    p.icd.freeze_hooks()
    for actor in actors:
        _, error = p.icd.call(0x4d4da0, (actor.mission + 0x22, 4), ecx=actor.mission)
        assert error is None and not p.missing, (error, p.missing)
    expected = []
    workers = []
    for n in range(1, args.rounds + 1):
        clock = p.frame['tick'] + n
        write('I', p.game + 0x19f44, clock)
        # Retail 5263aa/526411: unit movement/height precedes worker stepping.
        for actor in actors:
            for routine in (0x4dc800,0x51b2a0):
                _,error=p.icd.call(routine,(actor.unit,),ecx=actor.mover)
                if error or p.missing: raise RuntimeError((n,actor.identity,hex(routine),error,p.missing))
        _,error=p.icd.call(0x416430,(1,),ecx=obj)
        if error or p.missing: raise RuntimeError((n,'worker',error,p.missing))
        for actor in actors:
            unit,mover,nav,mission,owner=actor.unit,actor.mover,actor.nav,actor.mission,actor.owner
            flags = read('H', mover + 0x36)[0]
            navflags = read('B', nav + 0x114)[0]
            count = p.u32(nav + 0x10c)
            if args.diagnostic_dir:
                workers.append([clock, *[p.u32(obj + offset) for offset in
                                (0x5c, 0x40, 0xb0, 0x54, 0xec, 0x191, 0x14, 0x44, 0x1ad)],
                                read('B', obj + 0x114)[0], p.u32(obj + 0x169 + 4 * owner),
                                p.u32(obj + 0x115 + 4 * owner), p.u32(0x634674 + 4 * owner),
                                read('B', unit + 0x134)[0]])
            expected.append([*read('3i', unit + 0x68), read('i', mover + 0x20)[0],
                             read('H', unit + 0x7e)[0], read('H', unit + 0x80)[0], read('H', unit + 0x7c)[0],
                             flags & 0x1800, 2 if flags & 4 else 1 if flags & 8 else 0, p.u32(mover + 0x2c),
                             (flags >> 5) & 7, p.u32(mover + 0x30), p.u32(0x64186c), (flags >> 8) & 7,
                             int(bool(navflags & 2)), p.u32(mission + 0x6a), navflags & 1, count,
                             *read('h' * (count * 2), nav + 12), read('B', unit + 0x134)[0] & 15])
    with tempfile.TemporaryDirectory(prefix='tak-search-movement-') as temp:
        root = Path(temp)
        (root / 'input').write_text('\n'.join(fixture) + '\n')
        (root / 'command').write_text(''.join(f'{a.identity} {args.budget} {args.rounds} {a.gx} {a.gz} {a.first} {a.count}\n' for a in actors))
        run = subprocess.run([str(args.runner), str(args.retail_root), str(root / 'input'), str(root / 'output'),
                              '--search-movement', str(root / 'command')], check=True, text=True, capture_output=True,
                             env=dict(os.environ, **({'TAK_SEARCH_DIAG': '1'} if args.diagnostic_dir else {})))
        actual = [e['result'] for line in (root / 'output').read_text().splitlines()
                  if (e := json.loads(line))['kind'] == 'search_movement']
        if args.diagnostic_dir:
            args.diagnostic_dir.mkdir(parents=True, exist_ok=True)
            for name in ('input', 'command', 'output'):
                (args.diagnostic_dir / name).write_bytes((root / name).read_bytes())
            (args.diagnostic_dir / 'world-grades.log').write_text(run.stderr)
            (args.diagnostic_dir / 'native-grades.json').write_text(json.dumps(grades))
            (args.diagnostic_dir / 'native-results.json').write_text(json.dumps(expected))
            (args.diagnostic_dir / 'native-workers.json').write_text(json.dumps(workers))
            (args.diagnostic_dir / 'native-deliveries.json').write_text(json.dumps(deliveries))
            (args.diagnostic_dir / 'requesters.json').write_text(json.dumps([
                {'id':a.identity,'owner':a.owner,'grid':selected[a.identity],'viewer':viewer} for a in actors]))
            world_grades=[list(map(int,line.split()[1:])) for line in run.stderr.splitlines() if line.startswith('GRADE ')]
            if grades!=world_grades:
                index=next((i for i,(want,got) in enumerate(zip(grades,world_grades)) if want!=got),min(len(grades),len(world_grades)))
                raise AssertionError(('grade query',index,grades[index:index+1],world_grades[index:index+1]))
    assert len(actual) == len(expected), (len(actual), len(expected))
    for n, (want, got) in enumerate(zip(expected, actual), 1):
        assert want == got, {'step': (n-1)//len(actors)+1, 'unit': identities[(n-1)%len(actors)], 'retail': want, 'world': got}
    for index,actor in enumerate(actors):
        previous=actor.position
        moves=pending_moves=delivered_moves=refusals=0
        received=deliveries[actor.identity]
        for n,state in enumerate(expected[index::len(actors)],1):
            position=(state[0],state[2]);moved=position!=previous
            moves+=moved;pending_moves+=moved and bool(state[14])
            delivered_moves+=moved and bool(received) and p.frame['tick']+n>received[0]
            refusals+=bool(state[8]);previous=position
        print(f'PASS: {args.rounds} composed search/movement updates for unit {actor.identity}, {args.balance}; '
              f'moves={moves}, moving-pending={pending_moves}, deliveries={len(received)}, '
              f'moves-after-delivery={delivered_moves}, refusal-ticks={refusals}, concurrent={len(actors)}')


if __name__ == '__main__':
    main()
