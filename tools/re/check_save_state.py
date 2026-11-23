#!/usr/bin/env python3
"""Compare decoded save order fields with executed retail loader instructions.

Requires local KINGDOMS.icd and Unicorn, like check_motion.py. Executes the
field-copy block only; does not claim full save reload or simulation parity.
"""
import argparse
from pathlib import Path
import struct

from unicorn.x86_const import UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_ESP, UC_X86_REG_EBX
from emu import Icd, HEAP, STACK, STACK_SZ
from inspect_save import directory, sections
from decode_save_state import decode, order_record, movement_record, script_record
from probe_saved_movement import movement_goal


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('save', type=Path)
    args = parser.parse_args()
    data = args.save.read_bytes()
    chunks = dict(sections(data))
    node = next(n for n in directory(data, chunks) if n['name'] == 'Units')
    body = chunks[node['offset'] + 32]
    icd = Icd()
    ebp = STACK + STACK_SZ - 0x2000
    scripts=0
    for entry in node['blobs']:
        if not (entry['name'] or '').startswith('Script'):
            continue
        blob=body[entry['body_offset']:entry['body_offset']+entry['size']]
        expected=script_record(blob)
        icd.uc.mem_write(ebp-0xa58,blob[:0xa48])
        icd.uc.mem_write(HEAP,bytes([0xa5])*0xa64)
        icd.uc.reg_write(UC_X86_REG_EBP,ebp)
        icd.uc.reg_write(UC_X86_REG_EBX,HEAP)
        icd.uc.emu_start(0x56dc90,0x56dcd5)
        for index,thread in enumerate(expected['threads']):
            actual=struct.unpack('<41I',icd.uc.mem_read(HEAP+0x20+index*0xa4,0xa4))
            want=[thread[f'{offset*4:02x}'] for offset in range(41)]
            if list(actual)!=want:
                raise AssertionError((entry['name'],index,'thread restore mismatch'))
        actual=struct.unpack('<I',icd.uc.mem_read(HEAP+0xa60,4))[0]
        if actual!=expected['runtime_a60']:
            raise AssertionError((entry['name'],'script header trailer mismatch'))
        scripts+=1
    print(f'{scripts} script records: all 16 threads match executed retail loader block')
    count = 0
    for entry in node['blobs']:
        name = entry['name'] or ''
        if len(name) != 10 or name[0] != 'u' or name[5] != 'm':
            continue
        blob = body[entry['body_offset']:entry['body_offset'] + entry['size']]
        expected = order_record(blob)
        icd.uc.mem_write(ebp - 0x58, blob)
        icd.uc.mem_write(HEAP, bytes([0xa5]) * 0x80)
        icd.uc.reg_write(UC_X86_REG_EBP, ebp)
        icd.uc.reg_write(UC_X86_REG_ESI, HEAP)
        icd.uc.emu_start(0x4d7107, 0x4d718f)
        for field, value in expected['runtime_fields'].items():
            offset = int(field, 16)
            fmt, size = ('<B', 1) if offset == 5 else ('<I', 4)
            actual = struct.unpack(fmt, icd.uc.mem_read(HEAP + offset, size))[0]
            if actual != value:
                raise AssertionError(f'{name} +{field}: decoder={value}, retail={actual}')
        count += 1
    if not count:
        raise ValueError('no order records checked')
    print(f'{count} order records: decoded fields match executed retail loader block')
    movers = 0
    for entry in node['blobs']:
        if not (entry['name'] or '').endswith('mob'):
            continue
        blob = body[entry['body_offset']:entry['body_offset'] + entry['size']]
        expected = movement_record(blob)
        for initial_flags in (0, 0xffff, 0xa5a5):
            icd.uc.mem_write(ebp - 0x50, blob + bytes(4))
            icd.uc.mem_write(HEAP, bytes([0xa5]) * 0x80)
            icd.uc.mem_write(HEAP + 0x36, struct.pack('<H', initial_flags))
            icd.uc.reg_write(UC_X86_REG_EBP, ebp)
            icd.uc.reg_write(UC_X86_REG_ESI, HEAP)
            icd.uc.reg_write(UC_X86_REG_ESP, ebp - 0x100)
            icd.uc.emu_start(0x4dcae9, 0x4dcb9e)
            for field, value in expected['runtime_fields'].items():
                offset = int(field, 16)
                fmt = '<h' if offset in (0x24, 0x26) else '<H' if offset == 0x34 else '<I' if offset in (0x28, 0x30) else '<i'
                actual = struct.unpack(fmt, icd.uc.mem_read(HEAP + offset, struct.calcsize(fmt)))[0]
                assert actual == value, (entry['name'], field, value, actual)
            actual_flags = struct.unpack('<H', icd.uc.mem_read(HEAP + 0x36, 2))[0]
            assert actual_flags == (initial_flags & 0xe000) | expected['restored_flags']
            assert bytes(icd.uc.mem_read(HEAP + 0x2c, 4)) == bytes([0xa5]) * 4
        movers += 1
    print(f'{movers} movers: all saved fields and flag preservation match executed retail loader')
    goals = 0
    for unit in decode(data)['units']:
        for order in unit['orders']:
            if order['controller_kind'] != 4:
                continue
            # Execute retail's controller-to-world navigation point conversion.
            icd.uc.mem_write(HEAP + 4, struct.pack('<I', HEAP + 0x100))
            icd.uc.mem_write(HEAP + 8, struct.pack('<I', order['controller_runtime_fields']['08']))
            icd.uc.mem_write(HEAP + 0x10e, struct.pack('<I', HEAP + 0x200))
            icd.uc.mem_write(HEAP + 0x278, struct.pack('<hh', *unit['footprint_size']))
            _, error = icd.call(0x4e2820, (HEAP + 0x300,), ecx=HEAP)
            assert error is None, error
            x, _, z = struct.unpack('<iii', icd.uc.mem_read(HEAP + 0x300, 12))
            assert (x, z) == movement_goal(unit, order), (unit['id'], x, z)
            goals += 1
    print(f'{goals} cell goals: navigation points match executed retail controller')
    rectangles=0
    for unit in decode(data)['units']:
        for order in unit['orders']:
            if order['controller_kind'] != 6:
                continue
            entry=order['controller_blob']
            blob=body[entry['body_offset']:entry['body_offset']+entry['size']]
            icd.uc.mem_write(ebp-0x14,blob)
            icd.uc.mem_write(HEAP,bytes([0xa5])*0x20)
            icd.uc.reg_write(UC_X86_REG_EBP,ebp)
            icd.uc.reg_write(UC_X86_REG_ESI,HEAP)
            icd.uc.emu_start(0x4e3632,0x4e364a)
            for key,value in order['controller_runtime_fields'].items():
                assert struct.unpack('<I',icd.uc.mem_read(HEAP+int(key,16),4))[0]==value
            rectangles+=1
    print(f'{rectangles} rectangle goals: bounds match executed retail loader')


if __name__ == '__main__':
    main()
