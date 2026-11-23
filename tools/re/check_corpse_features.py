#!/usr/bin/env python3
"""Compare corpse/statue feature placement and removal with native execution.

Orders/combat/death animation timing are excluded: each operation supplies a
corpse-placement event at a captured unit's original position. Native 512ee0
selects/places its corpse, stone or frozen feature; World uses its production
corpse placement. Both execute real map mutations, including overlap refusals.
Every changed feature cell is compared after each placement and retirement.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX
from emureload import CapturedProcess
from balance_inputs import set_balance_inputs


def changes(before, after, names):
    result = []
    stride = 14
    block = stride * 512
    for start in range(0, len(before), block):
        end = min(len(before), start + block)
        if before[start:end] == after[start:end]:
            continue
        for offset in range(start, end, stride):
            previous, current = (struct.unpack_from('<H', data, offset+8)[0]
                                 for data in (before, after))
            if previous == current and (current != 0xfffe or
                                        before[offset+10:offset+12] == after[offset+10:offset+12]):
                continue
            name = names[current] if current < len(names) else str(current)
            result.append([offset//stride, name,
                           after[offset+11] if current == 0xfffe else 0,
                           after[offset+10] if current == 0xfffe else 0])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('fixture', type=Path)
    parser.add_argument('--retail-root', required=True, type=Path)
    parser.add_argument('--runner', default=Path('build-dbg/retail_replay_probe'), type=Path)
    parser.add_argument('--balance', choices=('standard','crusades'))
    parser.add_argument('--save', type=Path, help='matching save, required for controlled balance inputs')
    parser.add_argument('--cases', type=int, help='default: every captured unit in all three modes')
    parser.add_argument('--synthesize-missing-corpses', action='store_true',
                        help='append a native record for a controlled corpse the capture never loaded, '
                             'cloned from a record with identical placement inputs')
    args = parser.parse_args()
    capture=json.loads(args.capture.read_text())
    p = CapturedProcess(capture)
    if args.balance:
        if not args.save: raise ValueError('--save is required for --balance')
        set_balance_inputs(p,capture,args.save,args.retail_root,args.balance=='crusades',corpses=True,
                           synthesize_corpses=args.synthesize_missing_corpses)
    read = lambda address, size: bytes(p.uc.mem_read(address, size))
    unpack = lambda fmt, address: struct.unpack('<'+fmt, read(address, struct.calcsize('<'+fmt)))
    base, table = p.u32(p.game+0x19f04), p.u32(p.game+0x19edc)
    count = p.u32(p.game+0x19ec0)
    width, height = p.frame['map_cells']
    names = [read(table+i*320,32).split(b'\0')[0].decode('ascii').lower() for i in range(count)]
    placement_results = []

    def placement_return(uc, address, size, data):
        placement_results.append(bool(uc.reg_read(UC_X86_REG_EAX)))

    for address in (0x512fc3, 0x513004):
        p.uc.hook_add(UC_HOOK_CODE,placement_return,begin=address,end=address)
    # CapturedProcess owns emulated allocations. Native delete/free reaches
    # the captured process's OS heap locks; retain the backing pages here.
    p.icd.hooks[0x5ba5d0] = lambda uc,args: (0,0)
    p.icd.freeze_hooks()
    subjects = p.runtime['units']
    if args.cases is None: args.cases=len(subjects)*3
    if args.cases <= 0 or args.cases > len(subjects)*3:
        raise ValueError(f'cases must be in 1..{len(subjects)*3}')
    operations, expected = [], []
    before = read(base,width*height*14)
    placed_count = 0
    changed_count = 0
    for case in range(args.cases):
        unit = subjects[case//3]
        mode = case % 3
        entity = unit['address']
        kind = p.u32(entity+0xb4)
        origin = unpack('2h',entity+0x74)
        adjustment = unpack('2h',kind+0x25c)
        x,z = (a+b for a,b in zip(origin,adjustment))
        placement_results.clear()
        _,error = p.icd.call(0x512ee0,(entity,1,0,int(mode==1),int(mode==2)))
        if error or p.missing:
            raise AssertionError((case,'place',error,p.missing))
        placed = any(placement_results)
        placed_count += placed
        for action in (mode,3):
            if action == 3 and placed:
                _,error = p.icd.call(0x496380,(base+(z*width+x)*14,1))
                if error or p.missing:
                    raise AssertionError((case,'remove',error,p.missing))
            after = read(base,width*height*14)
            delta = changes(before,after,names)
            changed_count += len(delta)
            expected.append({'kind':'corpse_feature','id':unit['id'],
                             'placed':int(placed and action!=3),'cells':delta})
            operations.append((unit['id'],action))
            before = after
    fixture = args.fixture.read_text().splitlines()
    header = fixture[0].split()
    if args.balance: header[-1]=str(int(args.balance=='crusades'))
    if int(header[1]) < 34 or int(header[2]) != p.frame['tick'] or int(header[-1]) != read(0x641144,1)[0]:
        raise ValueError('fixture must match initial capture tick and balance selection (probe 34+)')
    header[5] = '0'
    fixture[0] = ' '.join(header)
    with tempfile.TemporaryDirectory(prefix='tak-corpse-features-') as directory:
        root = Path(directory)
        (root/'input').write_text('\n'.join(fixture)+'\n')
        (root/'operations').write_text('\n'.join(f'{id} {mode}' for id,mode in operations)+'\n')
        subprocess.run([str(args.runner),str(args.retail_root),str(root/'input'),str(root/'output'),
                        '--corpse-features',str(root/'operations')],check=True)
        actual = [event for line in (root/'output').read_text().splitlines()
                  if (event := json.loads(line))['kind']=='corpse_feature']
    if len(actual) != len(expected):
        raise AssertionError(('event count',len(actual),len(expected)))
    for index,(observed,wanted) in enumerate(zip(actual,expected)):
        if observed != wanted:
            raise AssertionError((index,operations[index],'port',observed,'native',wanted))
    print(f'PASS: {args.cases} native corpse/stone/frozen placements ({placed_count} accepted), '
          f'{args.cases} retirements, {changed_count} changed feature cells; '
          f'crusades={bool(read(0x641144,1)[0])}, controlled_balance={bool(args.balance)}; '
          'timing and combat excluded'
          +(f"; synthesized native records: {', '.join(getattr(p,'synthesized_features',[]))}"
            if getattr(p,'synthesized_features',None) else ''))


if __name__ == '__main__':
    main()
