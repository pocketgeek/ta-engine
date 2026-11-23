#!/usr/bin/env python3
"""Compare World's raw pathfinding grades with retail on identical initial state.

AI and tick execution are excluded. The port loads terrain/features from the
install and units from the initial fixture. Only rectangle queries and cache
requester/age parameters are passed in; no native grade results are injected.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import random
import struct
import subprocess
import tempfile

from emureload import CapturedProcess
from balance_inputs import set_balance_inputs


def compare(capture, fixture, runner, retail_root, cases, vary_cache_state=False, live=False, balance=None, save=None):
    p = CapturedProcess(capture)
    selected = set_balance_inputs(p,capture,save,retail_root,balance=='crusades') if balance else {}
    width, height = p.frame['map_cells']
    cells = bytes(p.uc.mem_read(p.u32(p.game+0x19f04), width*height*14))
    word = lambda at: int.from_bytes(p.uc.mem_read(at, 2), 'little')
    grids = {g['address'] for g in p.runtime['grids']} | set(selected.values())
    subjects = []
    for u in p.frame['units']:
        mover = u.get('mover_address', 0)
        if mover and p.u32(mover+4) in grids:
            subjects.append((u, p.u32(mover+4)))
    if not subjects:
        raise ValueError('capture has no surface navigation grids')
    movement_classes = {
        u['id']: bytes(p.uc.mem_read(p.u32(grid),64)).split(b'\0')[0].decode('ascii')
        for u,grid in subjects
    }
    occupied, featured, steep, ramps = [], [], [], []
    for i in range(width*height):
        if struct.unpack_from('<H', cells, i*14)[0]: occupied.append(i)
        if struct.unpack_from('<H', cells, i*14+8)[0] != 0xffff: featured.append(i)
        spread = cells[i*14+5]-cells[i*14+6]
        if spread > 30: steep.append(i)
        if 0 < spread <= 30: ramps.append(i)
    rng = random.Random(0x5088f0)
    queries, expected, labels = [], [], []
    p.icd.freeze_hooks()
    for case in range(cases):
        unit, grid = subjects[case % len(subjects)]
        group = case % 6
        pool = (occupied, featured, steep, steep, [], ramps)[group]
        if pool:
            index = rng.choice(pool)
            x, z = index % width, index // width
            if group == 3: z -= rng.randint(1, 4)
        else:
            x, z = rng.randrange(-1, width+1), rng.randrange(-1, height+1)
        fx, fz = (1, 1) if not live and (case // 6) % 2 else tuple(unit['footprint'])
        requester = p.u32(grid+0x33c)
        requester_id = word(requester+2) if requester else 0
        recent, stale = p.u32(grid+0x34c), p.u32(grid+0x350)
        original = requester, recent, stale
        if vary_cache_state:
            requester_id = (0,unit['id'],subjects[(case+1)%len(subjects)][0]['id'])[(case // 6) % 3]
            requester = p.u32(p.game+0x14e84)+requester_id*312 if requester_id else 0
            recent = rng.randrange(0,p.frame['tick']+151)
            stale = rng.randrange(recent+1)
            for offset,value in ((0x33c,requester),(0x34c,recent),(0x350,stale)):
                p.uc.mem_write(grid+offset,struct.pack('<I',value))
        if live:
            center_x,center_z=x+fx//2,z+fz//2
            pointer=p.u32(p.game+0x14e84)+unit['id']*312
            value,error=p.icd.call(0x4db640,(pointer,center_x<<20,0,center_z<<20))
            if value >= 0x80000000: value -= 0x100000000
        else:
            value, error = p.icd.call(0x5088f0, (grid, x, z, fx, fz))
        if vary_cache_state:
            for offset,saved in zip((0x33c,0x34c,0x350),original):
                p.uc.mem_write(grid+offset,struct.pack('<I',saved))
        if error or p.missing:
            raise AssertionError((case, error, p.missing))
        queries.append((unit['id'],center_x,center_z) if live else
                       (unit['id'],x,z,fx,fz,requester_id,recent,stale))
        expected.append(value)
        labels.append(('occupied','feature','steep','north-of-steep','arbitrary','ramp')[group])
    with tempfile.TemporaryDirectory(prefix='tak-world-grade-') as directory:
        root = Path(directory)
        lines = fixture.read_text().splitlines()
        header = lines[0].split()
        if int(header[1]) < 34:
            raise ValueError('regenerate fixture with captured standard/Crusades selection (probe 34+)')
        captured_crusades=bool(bytes(p.uc.mem_read(0x641144,1))[0])
        if balance: header[-1]=str(int(balance=='crusades'))
        if bool(int(header[-1])) != captured_crusades:
            raise ValueError('fixture and capture use different standard/Crusades rosters')
        if int(header[2]) != p.frame['tick']:
            raise ValueError('fixture and capture start at different ticks')
        header[5] = '0'
        lines[0] = ' '.join(header)
        (root/'input').write_text('\n'.join(lines)+'\n')
        (root/'queries').write_text('\n'.join(' '.join(map(str,q)) for q in queries)+'\n')
        subprocess.run([str(runner),str(retail_root),str(root/'input'),str(root/'output'),
                        '--live-grade-queries' if live else '--raw-grade-queries',str(root/'queries')], check=True)
        events = [json.loads(line) for line in (root/'output').read_text().splitlines()]
        actual = [e['result'] for e in events if e['kind'] == ('live_grade' if live else 'raw_grade')]
    if len(actual) != cases:
        raise AssertionError(('result count', len(actual), cases))
    differences = [(i,labels[i],queries[i],got,want)
                   for i,(got,want) in enumerate(zip(actual,expected)) if got != want]
    counts = Counter(labels)
    failures = Counter(d[1] for d in differences)
    for label, count in counts.items():
        accepted = sum(value >= 4 if live else value != 0 for kind,value in zip(labels,expected) if kind == label)
        print(f'{label}: {count-failures[label]}/{count} matching; '
              f'retail accepts {accepted}, rejects {count-accepted}')
    class_labels = [movement_classes[q[0]] for q in queries]
    for movement,count in sorted(Counter(class_labels).items()):
        matched = sum(a==b for name,a,b in zip(class_labels,actual,expected) if name==movement)
        accepted = sum(value>=4 if live else value!=0
                       for name,value in zip(class_labels,expected) if name==movement)
        print(f'{movement}: {matched}/{count} matching; retail accepts {accepted}, rejects {count-accepted}')
    if live:
        print('Step acceptance differences:',sum((a>=4)!=(b>=4) for a,b in zip(actual,expected)))
    if differences:
        print('Mismatches by sample class:', dict(Counter(d[1] for d in differences)))
        for difference in differences[:20]: print('query, port, retail:', difference)
        raise AssertionError(f'{len(differences)} of {cases} World grades differ (live={live})')
    print(f'PASS: {cases} World grades (live={live}, crusades={captured_crusades}, controlled_balance={bool(balance)}), {len(subjects)} surface units; '
          'occupied, feature, steep, north-of-steep, arbitrary and ramp cells')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture', type=Path)
    ap.add_argument('fixture', type=Path)
    ap.add_argument('--runner', type=Path, default=Path('build-dbg/retail_replay_probe'))
    ap.add_argument('--retail-root', type=Path, required=True)
    ap.add_argument('--balance', choices=('standard','crusades'),
                    help='controlled balance inputs from source FBI/MOVEINFO assets, not a fresh capture')
    ap.add_argument('--save', type=Path, help='matching retail save, required with --balance')
    ap.add_argument('--cases', type=int, default=3000)
    ap.add_argument('--live', action='store_true', help='compare live steering grades at unit footprint centers')
    ap.add_argument('--vary-cache-state', action='store_true',
                    help='compare controlled requester and occupancy-age inputs')
    args = ap.parse_args()
    if bool(args.balance) != bool(args.save): ap.error('--balance and --save are required together')
    if args.live and args.vary_cache_state: ap.error('--live does not use cached grade state')
    if not 1 <= args.cases <= 100000: ap.error('cases must be 1..100000')
    compare(json.loads(args.capture.read_text()),args.fixture,args.runner,args.retail_root,args.cases,args.vary_cache_state,args.live,args.balance,args.save)


if __name__ == '__main__': main()
