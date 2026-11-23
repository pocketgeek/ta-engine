#!/usr/bin/env python3
"""Execute a captured retail mission, mover or pending search in offline Unicorn.

Restores captured inputs at their original addresses. Allocator/free calls and
process-exit callback registration are replaced. Missing mapped memory stops
execution; uncaptured bytes in mapped pages are not validated, so this is not
a complete game replay.
All reports must stay with the gitignored retail fixtures.
"""
import argparse
import base64
import json
from pathlib import Path
import struct
import zlib

from emu import Icd
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED, UcError
from unicorn.x86_const import (UC_X86_REG_EIP, UC_X86_REG_ESP, UC_X86_REG_FPCW,
                              UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EAX)


def initial_fixture(lines, tick):
    """Return a probe fixture as its version-34 initial-state prefix.

    Checks that author their own surface heights (probe 35/36) and exploration
    (probe 37) from the capture need the fixture without those trailing sections.
    probe_saved_movement.py now writes version 37, so strip them here rather
    than requiring a separately kept version-34 file.
    """
    header = lines[0].split()
    if len(header) < 5 or header[0] != 'TAK_MOVEMENT_PROBE' or int(header[2]) != tick:
        raise ValueError('expected matching version-34 initial fixture')
    version, end = int(header[1]), len(lines)
    if not 34 <= version <= 37:
        raise ValueError('expected matching version-34 initial fixture')
    if version >= 37:
        # width/height/viewer, explored masks, sight count, then one row per sight cache.
        rows = next((n for n in range(end) if end-n-1 >= 2 and lines[end-n-1] == str(n)), None)
        if rows is None or len(lines[end-rows-3].split()) != 3:
            raise ValueError('malformed version-37 exploration section')
        end -= rows + 3
    if version >= 35:
        # One surface-height row per captured unit, preceded by their count.
        count = int(header[4])
        if end-count-1 < 1 or lines[end-count-1] != str(count):
            raise ValueError('malformed surface-height section')
        end -= count + 1
    header[1] = '34'
    return [' '.join(header), *lines[1:end]]


class CapturedProcess:
    def __init__(self, capture):
        if capture['status'] != 'captured':
            raise ValueError('successful capture required')
        self.frame = capture['frames'][0]
        self.runtime = self.frame['runtime_state']
        if not self.runtime.get('world_buffers'):
            raise ValueError('capture lacks search world buffers (including coarse visibility)')
        self.icd = Icd()
        self.uc = self.icd.uc
        self.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
        self.pages = {page for lo, hi, _ in self.uc.mem_regions()
                      for page in range(lo, hi + 1, 4096)}
        self.events = []
        self.missing = []
        self.brk = 0x60000000  # above the bounded original game-memory snapshot
        self.allocations = {}
        self.obj = self.frame['path_state']['address']
        for record in self.frame.get('game_memory', []):
            self.load_record(record)
        for record in self.frame.get('device_memory', []):
            self.load_record(record)
        for record in self.frame.get('native_memory', []):
            self.load_record(record)
        self.load_record(self.runtime['entity_pool'])
        for record in self.runtime['world_buffers']:
            self.load_record(record)
            if record['name'] == 'game_fields': self.game = record['address']
        for name in ('type_prefixes', 'mission_definitions', 'missions', 'controllers', 'grids'):
            for record in self.runtime[name]:
                if record.get('fields_hex'):
                    self.put(record['address'], bytes.fromhex(record['fields_hex']))
        for record in self.runtime['grids']:
            self.load_record({'address': record['plane_address'], 'size': record['plane_size'],
                              'zlib_base64': record['zlib_base64']})
        for unit in self.frame['units']:
            for name in ('entity', 'mover', 'navigator'):
                if name + '_hex' in unit:
                    self.put(unit[name + '_address'], bytes.fromhex(unit[name + '_hex']))
        path = self.frame['path_state']
        self.put(self.obj, bytes.fromhex(path['fields_hex']))
        for record in path['buffers']: self.load_record(record)
        # The node buffer captures used records, while allocation capacity can
        # exceed that count. New records are initialized by the real kernel.
        self.ensure(self.u32(self.obj), self.u32(self.obj + 0x10) * 20)
        self.allocations[self.u32(self.obj)] = self.u32(self.obj + 0x10) * 20
        self.allocations[self.u32(self.obj + 4)] = self.u32(self.obj + 0x10) * 4
        self.put(0x62d55c, struct.pack('<I', self.game))
        self.put(0x64186c, struct.pack('<I', self.frame['rng_before']))
        self.put(0x634674, struct.pack('<10I', *path['pending_per_player']))
        definition = self.runtime['mission_definitions'][0]
        self.put(0x62db84, struct.pack('<I', definition['address'] - definition['type_id'] * 25))
        self.icd.hooks[0x4eb9e0] = self.allocate
        self.icd.hooks[0x5ba3d0] = lambda uc, args: self.allocate(uc, args + 4)
        self.icd.hooks[0x5ba3e0] = self.allocate
        self.icd.hooks[0x5ba4f0] = self.reallocate
        self.icd.hooks[0x4eba00] = lambda uc, args: (0, 0)
        # cdecl atexit: the caller removes its argument. No process shutdown is
        # executed by this harness; retain the real lazy container initialization.
        self.icd.hooks[0x5d3d12] = lambda uc, args: (0, 0)
        self.uc.hook_add(UC_HOOK_MEM_UNMAPPED, self.unmapped)
        self.uc.hook_add(UC_HOOK_CODE, self.observe, begin=0x535cc0, end=0x535cc0)

    def ensure(self, address, size):
        if not 0 <= size <= 32*1024*1024 or address + size > 0x100000000:
            raise ValueError('invalid memory record')
        end = (address + size + 4095) & ~4095
        page = address & ~4095
        while page < end:
            if page in self.pages:
                page += 4096
                continue
            first = page
            while page < end and page not in self.pages:
                self.pages.add(page)
                page += 4096
            self.uc.mem_map(first, page - first)

    def put(self, address, data):
        self.ensure(address, len(data)); self.uc.mem_write(address, data)

    def load_record(self, record):
        size = record['size']
        if not 0 <= size <= 32*1024*1024: raise ValueError('oversized compressed record')
        decoder = zlib.decompressobj()
        data = decoder.decompress(base64.b64decode(record['zlib_base64'], validate=True), size + 1)
        if len(data) != size or not decoder.eof or decoder.unused_data:
            raise ValueError('invalid compressed record')
        self.put(record['address'], data)

    def u32(self, address):
        return struct.unpack('<I', self.uc.mem_read(address, 4))[0]

    def allocate(self, uc, args):
        size = self.u32(args)
        if size > 8*1024*1024 or self.brk + size > 0x64000000:
            raise ValueError('emulated allocation limit')
        address = self.brk
        self.brk = (address + max(16, size) + 15) & ~15
        self.ensure(address, max(16, size))
        self.allocations[address] = size
        return 0, address

    def reallocate(self, uc, args):
        pointer, size = self.u32(args), self.u32(args + 4)
        if not size:
            return 0, 0
        if pointer and pointer not in self.allocations:
            raise RuntimeError(f'unknown original realloc extent: {pointer:#x}, requested {size}')
        _, result = self.allocate(uc, args + 4)
        if pointer:
            self.put(result, bytes(uc.mem_read(pointer, min(size, self.allocations[pointer]))))
        return 0, result

    def unmapped(self, uc, access, address, size, value, data):
        try:
            stack_top = hex(self.u32(uc.reg_read(UC_X86_REG_ESP)))
        except UcError:
            stack_top = None
        self.missing.append({'instruction': hex(uc.reg_read(UC_X86_REG_EIP)),
                             'address': hex(address), 'size': size, 'access': access,
                             'stack_top': stack_top})
        return False

    def observe(self, uc, address, size, data):
        stack = uc.reg_read(UC_X86_REG_ESP)
        self.events.append({'caller': hex(self.u32(stack)), 'bound': self.u32(stack + 4),
                            'seed_before': self.u32(0x64186c)})

    def run_mission(self, identity):
        unit = next(u for u in self.runtime['units'] if u['id'] == identity)
        active_before = self.u32(self.obj + 0x58)
        controller_before = self.u32(unit['primary'] + 0x6e)
        dispatches = []
        active_dispatches = []

        def observe_dispatch(uc, address, size, data):
            mission = uc.reg_read(UC_X86_REG_ESI)
            if address == 0x4d84fe:
                definition = uc.reg_read(UC_X86_REG_EAX)
                record = {
                    'mission': mission, 'handler': hex(self.u32(definition + 4)),
                    'stage_before': uc.mem_read(mission + 5, 1)[0],
                    'consumed_events': uc.reg_read(UC_X86_REG_EDI),
                    'pending_before': self.u32(mission + 0x6a),
                    'controller_before': self.u32(mission + 0x6e),
                    'active_search_before': self.u32(self.obj + 0x58),
                    'rng_index_before': len(self.events)}
                dispatches.append(record)
                active_dispatches.append(record)
            else:
                if not active_dispatches or active_dispatches[-1]['mission'] != mission:
                    raise RuntimeError('mission handler return without matching entry')
                active_dispatches.pop().update({
                    'result': uc.reg_read(UC_X86_REG_EAX),
                    'stage_after_handler': uc.mem_read(mission + 5, 1)[0],
                    'wait_mask_after': self.u32(mission + 6),
                    'deadline_after': self.u32(mission + 0xa),
                    'pending_after': self.u32(mission + 0x6a),
                    'controller_after': self.u32(mission + 0x6e),
                    'active_search_after': self.u32(self.obj + 0x58),
                    'rng_index_after': len(self.events)})

        hooks = [self.uc.hook_add(UC_HOOK_CODE, observe_dispatch, begin=a, end=a)
                 for a in (0x4d84fe, 0x4d8501)]
        self.put(self.game + 0x19f44, struct.pack('<I', self.frame['tick'] + 1))
        try:
            value, error = self.icd.call(0x4d8450, (unit['address'],))
        finally:
            for hook in hooks:
                self.uc.hook_del(hook)
        return {'value': value, 'error': error, 'missing': self.missing, 'rng_calls': self.events,
                'dispatches': dispatches,
                'complete_state_match': False,
                'active_search_before': active_before,
                'active_search_after': self.u32(self.obj + 0x58),
                'controller_before': controller_before,
                'controller_after': self.u32(unit['primary'] + 0x6e),
                'mission_hex': bytes(self.uc.mem_read(unit['primary'], 0x72)).hex()}

    def run_mover(self, identity):
        """One original mover update, including navigator and position commit.

        Other units and mission dispatch are not advanced. This is a component
        replay, even when the next captured unit's motion fields agree.
        """
        unit = next(u for u in self.runtime['units'] if u['id'] == identity)
        entity = unit['address']
        mover = self.u32(entity + 8)
        if not mover:
            raise ValueError(f'unit {identity} has no mover')

        def state():
            def unpack(fmt, address):
                return list(struct.unpack('<' + fmt, self.uc.mem_read(address, struct.calcsize('<' + fmt))))
            return {'position_raw': unpack('3i', entity + 0x68),
                    'velocity_raw': unpack('3i', mover + 8),
                    'filtered_acceleration_raw': unpack('3i', mover + 0x14),
                    'speed_raw': unpack('i', mover + 0x20)[0],
                    'heading': unpack('H', entity + 0x7e)[0]}

        before = state()
        phases = []

        def observe_phase(uc, address, size, data):
            record = {'routine': hex(address), **state()}
            if address == 0x4da7d0:
                navigator, kind = self.u32(mover), self.u32(entity + 0xb4)
                flags = struct.unpack('<H', uc.mem_read(mover + 0x36, 2))[0]
                def signed(address):
                    return struct.unpack('<i', uc.mem_read(address, 4))[0]
                multiplier = signed(kind + 0x172) if flags & 0x800 else (
                    signed(kind + 0x16e) if flags & 0x1000 else 65536)
                record['flight_inputs'] = {
                    'destination_raw': list(struct.unpack('<3i', uc.mem_read(navigator + 0xc, 12))),
                    'destination_velocity_raw': list(struct.unpack('<3i', uc.mem_read(navigator + 0x18, 12))),
                    'maximum_speed_raw': signed(entity + 0x12b) * multiplier >> 16,
                    'acceleration_raw': signed(kind + 0x16a) * multiplier >> 16,
                    'lateral_limit_raw': signed(kind + 0x166) * multiplier >> 16,
                    'direct_control': self.u32(entity + 0xa4) == self.u32(self.game + 0x19f30),
                    'movement_mode': flags & 3}
            phases.append(record)

        hooks = [self.uc.hook_add(UC_HOOK_CODE, observe_phase, begin=a, end=a)
                 for a in (0x4da7d0, 0x4d9ad0, 0x4dad30, 0x4db350, 0x4dc600)]
        self.put(self.game + 0x19f44, struct.pack('<I', self.frame['tick'] + 1))
        try:
            value, error = self.icd.call(0x4dc800, (entity,), ecx=mover)
        finally:
            for hook in hooks:
                self.uc.hook_del(hook)
        return {'id': identity, 'tick': self.frame['tick'] + 1,
                'value': value, 'error': error, 'missing': self.missing,
                'before': before, 'phases': phases, 'after': state(),
                'rng_calls': self.events, 'complete_state_match': False}

    def run_search(self, pops):
        if self.u32(self.obj + 0x5c) != 2: raise ValueError('initial search must be in phase 2')
        results = []
        for index in range(pops):
            if self.u32(self.obj + 0x14) == self.u32(self.obj + 0x18): break
            value, error = self.icd.call(0x4142c0, ecx=self.obj)
            results.append({'pop': index+1, 'value': value, 'error': error,
                            'processed': self.u32(self.obj + 0x191), 'heap_count': self.u32(self.obj + 0x14)})
            if error or value: break
            self.put(self.obj + 0x44, struct.pack('<I', 2 + int(self.u32(self.obj + 0x1ad) > 0)))
        return {'pops': results, 'missing': self.missing, 'complete_state_match': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--mission', type=int)
    action.add_argument('--mover', type=int)
    action.add_argument('--pops', type=int)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.pops is not None and not 1 <= args.pops <= 10000: parser.error('pops must be 1..10000')
    process = CapturedProcess(json.loads(args.capture.read_text()))
    if args.mission is not None:
        result = process.run_mission(args.mission)
    elif args.mover is not None:
        result = process.run_mover(args.mover)
    else:
        result = process.run_search(args.pops)
    with args.output.open('x') as output:
        json.dump(result, output, indent=2); output.write('\n')
    print(json.dumps(result if args.pops is None else {'last': result['pops'][-1:], 'missing': result['missing']}))


if __name__ == '__main__':
    main()
