#!/usr/bin/env python3
"""Capture retail before a save's first simulation tick, or the next live tick.

Uses up to four hardware breakpoints, never patches game code/data. Optional
Wine callback observation adds four temporary software breakpoints in Wine.
All-stop GDB
briefly pauses the process, captures the matching tick, then detaches. Optional
follow-up ticks include ordered RNG observations (timing is perturbed). The user
loads the save while this bounded observer is armed, or unpauses for
--next-tick. Optional native images include writable globals captured at that
same boundary, but exclude shared device mappings. Output is exclusive.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


def game_memory_ranges(maps):
    """Bounded low-address private memory, excluding emulator-reserved space.

    This includes script heaps and writable globals omitted by field snapshots.
    It is not an OS checkpoint: shared mappings and high Wine/driver mappings
    are excluded. Fail rather than silently truncate an oversized capture.
    """
    ranges = []
    total = 0
    regions = []
    for line in maps.splitlines():
        columns = line.split()
        if len(columns) < 5:
            raise ValueError('invalid process memory map')
        lo, hi = (int(part, 16) for part in columns[0].split('-'))
        permissions = columns[1]
        if len(permissions) != 4 or hi <= lo:
            raise ValueError('invalid process memory range')
        if permissions[0] != 'r' or permissions[3] != 'p':
            continue
        lo, hi = max(lo, 0x10000), min(hi, 0x60000000)
        if hi <= lo:
            continue
        total += hi - lo
        regions.append((hi - lo, lo, hi))
        # Small independent records bound decompression and debugger reads.
        for address in range(lo, hi, 1024 * 1024):
            ranges.append((address, min(1024 * 1024, hi - address)))
    if total > 768 * 1024 * 1024:
        largest = ', '.join(f'{lo:#x}-{hi:#x}: {size // 1024 // 1024} MiB'
                            for size, lo, hi in sorted(regions, reverse=True)[:5])
        raise ValueError(f'game memory exceeds 768 MiB capture limit ({total} bytes; largest: {largest})')
    if not ranges:
        raise ValueError('no private game memory found')
    return ranges


def game_memory_state(mem, maps):
    import base64
    import zlib
    return [{'address': address, 'size': size,
             'zlib_base64': base64.b64encode(zlib.compress(mem.read(address, size), 1)).decode('ascii')}
            for address, size in game_memory_ranges(maps)]


def native_memory_ranges(maps):
    """High-address 32-bit PE images, including their writable globals.

    This supplements the low game snapshot, not an OS/GPU checkpoint. Refuse
    overlap with the offline emulator's allocator, stack and scratch heap.
    """
    records = []
    total = 0
    reserved = ((0x60000000, 0x64000000), (0x70000000, 0x70100000),
                (0x71000000, 0x71400000))
    for line in maps.splitlines():
        columns = line.split(maxsplit=5)
        if len(columns) < 5:
            raise ValueError('invalid native memory map')
        lo, hi = (int(part, 16) for part in columns[0].split('-'))
        permissions = columns[1]
        if len(permissions) != 4 or hi <= lo:
            raise ValueError('invalid native memory range')
        if len(columns) < 6 or not columns[5].lower().endswith(('.dll', '.exe')):
            continue
        if permissions[0] != 'r' or permissions[3] != 'p':
            continue
        lo, hi = max(lo, 0x60000000), min(hi, 0x100000000)
        if hi <= lo:
            continue
        if any(lo < end and hi > start for start, end in reserved):
            raise ValueError('native image overlaps offline emulator address space')
        total += hi - lo
        if total > 256 * 1024 * 1024:
            raise ValueError('native images exceed 256 MiB capture limit')
        for address in range(lo, hi, 1024 * 1024):
            records.append({'address': address, 'size': min(1024 * 1024, hi - address),
                            'permissions': permissions, 'module': columns[5]})
    if not records:
        raise ValueError('no high-address native PE images found')
    return records


def device_memory_ranges(maps):
    """Bounded NVIDIA and Wine shared CPU buffers, not GPU execution state.

    Glide's D3D texture lock can return a /memfd:wine-mapping buffer rather
    than a /dev/nvidia mapping. Both must be read at the stopped boundary.
    """
    records=[]
    total=0
    for line in maps.splitlines():
        columns=line.split(maxsplit=5)
        if len(columns)<6 or not (columns[5].startswith('/dev/nvidia') or
                                 columns[5] in ('/memfd:wine-mapping', '/memfd:wine-mapping (deleted)')):
            continue
        lo,hi=(int(v,16) for v in columns[0].split('-'))
        permissions=columns[1]
        if permissions[0]!='r' or permissions[3]!='s':
            continue
        lo,hi=max(lo,0x10000),min(hi,0x100000000)
        if hi<=lo:
            continue
        if any(lo<end and hi>start for start,end in
               ((0x60000000,0x64000000),(0x70000000,0x70100000),(0x71000000,0x71400000))):
            raise ValueError('device mapping overlaps offline emulator address space')
        total+=hi-lo
        if total>384*1024*1024:
            raise ValueError('device mappings exceed 384 MiB capture limit')
        for address in range(lo,hi,1024*1024):
            records.append({'address':address,'size':min(1024*1024,hi-address),
                            'permissions':permissions,'device':columns[5]})
    if not records:
        raise ValueError('no readable shared NVIDIA or Wine mappings found')
    return records


def device_memory_state(mem,maps):
    import base64
    import zlib
    records=device_memory_ranges(maps)
    for record in records:
        record['zlib_base64']=base64.b64encode(zlib.compress(
            mem.read(record['address'],record['size']),1)).decode('ascii')
    return records


def native_memory_state(mem, maps):
    import base64
    import zlib
    records = native_memory_ranges(maps)
    for record in records:
        record['zlib_base64'] = base64.b64encode(zlib.compress(
            mem.read(record['address'], record['size']), 1)).decode('ascii')
    return records


def captured_crt_thread(mem, records, stack):
    """Resolve the stopped Win32 thread's CRT TLS from the bounded snapshot."""
    import base64
    import struct
    import zlib
    index = mem.u32(0x629210)
    if index >= 64:
        raise RuntimeError('unsupported CRT TLS expansion slot')
    found = []
    for record in records:
        if not 0 < record['size'] <= 1024 * 1024:
            raise RuntimeError('invalid game-memory chunk')
        decoder = zlib.decompressobj()
        raw = decoder.decompress(base64.b64decode(record['zlib_base64'], validate=True), record['size'] + 1)
        if len(raw) != record['size'] or not decoder.eof or decoder.unused_data:
            raise RuntimeError('invalid compressed game-memory chunk')
        for offset in range(0, len(raw) - 0x1b, 4096):
            page = record['address'] + offset
            if struct.unpack_from('<I', raw, offset + 0x18)[0] != page:
                continue
            high, low = struct.unpack_from('<II', raw, offset + 4)
            if not low <= stack < high:
                continue
            thread = mem.u32(page + 0xe10 + index * 4)
            if thread and mem.u32(thread) == mem.u32(page + 0x24):
                found.append({'teb': page, 'address': thread, 'tls_index': index,
                              'thread_id': mem.u32(thread), 'seed': mem.u32(thread + 0x14)})
    if len(found) != 1:
        raise RuntimeError(f'expected one stopped CRT thread, found {len(found)}')
    return found[0]


def runtime_state(mem, game, units, initial):
    """Read bounded mission chains and the ground search's cached grade planes.

    Called only while all threads are stopped. Addresses describe this capture,
    not stable identities. Unknown controller classes are explicitly reported.
    """
    import base64
    import struct
    import zlib
    from livesample import STRIDE, MAX_SLOTS

    result = {'units': [], 'missions': [], 'controllers': [], 'grids': [],
              'world_buffers': [], 'type_prefixes': [], 'mission_definitions': []}
    entities = mem.u32(game + 0x14e84)
    seen_missions, seen_controllers, seen_grids = set(), set(), set()
    definitions = mem.u32(0x62db84)
    seen_types, seen_definitions = set(), set()
    controller_sizes = {0x5f28d8: 0x14, 0x5f290c: 0x18}
    if initial:
        # Scheduler counts empty slots as well as occupied ones.
        last = mem.u32(game + 0x14e88)
        slots = (last - entities) // STRIDE + 1
        if not entities or last < entities or (last - entities) % STRIDE or not 1 <= slots <= MAX_SLOTS:
            raise RuntimeError('invalid runtime entity pool')
        config = mem.u32(mem.u32(0x62d558) + 8)
        result['scan_slot_limit'] = mem.u32(config + 0xc)
        raw = mem.read(entities, slots * STRIDE)
        result['entity_pool'] = {'address': entities, 'size': len(raw),
                                'zlib_base64': base64.b64encode(zlib.compress(raw)).decode('ascii')}
        width, height = struct.unpack('<II', mem.read(game + 0x19e98, 8))
        occupancy_count = mem.u32(game + 0x19ec0)
        if not 1 <= width <= 4096 or not 1 <= height <= 4096 or not 0 <= occupancy_count <= MAX_SLOTS:
            raise RuntimeError('invalid bounded search world')
        # Cached grades are still gated by the coarse per-player visibility
        # plane (0x413c80). Local body queries additionally read map cell records
        # and the separate 0x140-stride occupancy records. Capture those inputs
        # rather than assuming all terrain is visible or all occupancy is empty.
        for name, pointer, size in (
            ('game_fields', game, 0x19f74),
            ('coarse_visibility', mem.u32(game + 0x19ef4), (width // 2) * (height // 2) * 2),
            ('map_cells', mem.u32(game + 0x19f04), width * height * 14),
            ('occupancy', mem.u32(game + 0x19edc), occupancy_count * 0x140),
        ):
            if size > 32*1024*1024 or (size and not pointer):
                raise RuntimeError('invalid bounded world buffer: ' + name)
            payload = mem.read(pointer, size) if size else b''
            result['world_buffers'].append({'name': name, 'address': pointer, 'size': size,
                'zlib_base64': base64.b64encode(zlib.compress(payload)).decode('ascii')})
    for unit in units:
        address = entities + unit['id'] * STRIDE
        primary, secondary = mem.u32(address + 0x60), mem.u32(address + 0x64)
        result['units'].append({'id': unit['id'], 'address': address,
                                'primary': primary, 'secondary': secondary,
                                'events': mem.u32(address + 0xd0)})
        type_address = mem.u32(address + 0xb4)
        if initial and type_address and type_address not in seen_types:
            seen_types.add(type_address)
            # Prefix containing navigation dimensions, costs and flags; this
            # deliberately does not claim to capture the complete unit type.
            result['type_prefixes'].append({'address': type_address,
                                          'fields_hex': mem.read(type_address, 0x280).hex()})
        for head in (primary, secondary):
            visited = set()
            mission = head
            while mission:
                if mission in visited:
                    raise RuntimeError('cyclic runtime mission queue')
                visited.add(mission)
                if mission in seen_missions:
                    break
                if len(seen_missions) >= 16384:
                    raise RuntimeError('runtime mission capture limit reached')
                seen_missions.add(mission)
                raw = mem.read(mission, 0x72)
                controller = struct.unpack_from('<I', raw, 0x6e)[0]
                result['missions'].append({'address': mission, 'unit_id': unit['id'],
                    'fields_hex': raw.hex(), 'handler': mem.u32(definitions + raw[4] * 25 + 4)})
                if initial and raw[4] not in seen_definitions:
                    seen_definitions.add(raw[4])
                    entry = definitions + raw[4] * 25
                    result['mission_definitions'].append({'type_id': raw[4], 'address': entry,
                                                          'fields_hex': mem.read(entry, 25).hex()})
                if initial and controller and controller not in seen_controllers:
                    seen_controllers.add(controller)
                    vtable = mem.u32(controller)
                    size = controller_sizes.get(vtable, 0)
                    result['controllers'].append({'address': controller, 'vtable': vtable,
                        'known_size': bool(size), 'fields_hex': mem.read(controller, size).hex() if size else None})
                if not initial:
                    break  # each tick captures the two active heads, not every queued order
                mission = struct.unpack_from('<I', raw, 0x66)[0]
        mover = mem.u32(address + 8)
        if not initial or not mover:
            continue
        navigator = mem.u32(mover)
        if not navigator or mem.u32(navigator) != 0x5f2a24:
            continue
        grid = mem.u32(mover + 4)
        if not grid or grid in seen_grids:
            continue
        seen_grids.add(grid)
        raw = mem.read(grid, 0x354)
        width, height, plane = struct.unpack_from('<III', raw, 0x340)
        size = width * ((height + 7) // 8) * 4
        if not 1 <= width <= 4096 or not 1 <= height <= 4096 or not plane or size > 32*1024*1024:
            raise RuntimeError('invalid bounded navigation grade plane')
        result['grids'].append({'address': grid, 'fields_hex': raw.hex(), 'width': width,
            'height': height, 'plane_address': plane, 'plane_size': size,
            'zlib_base64': base64.b64encode(zlib.compress(mem.read(plane, size))).decode('ascii')})
    return result


def captured_performance_counter(mem, stack, status):
    """NtQueryPerformanceCounter's completed output, before its RET 8."""
    result={'return_address':mem.u32(stack),'status':status,
            'counter_address':mem.u32(stack+4),'frequency_address':mem.u32(stack+8)}
    if status==0:
        if not result['counter_address']:
            raise ValueError('successful counter query has no output pointer')
        import struct
        result['counter']=struct.unpack('<q',mem.read(result['counter_address'],8))[0]
        if result['frequency_address']:
            result['frequency']=struct.unpack('<q',mem.read(result['frequency_address'],8))[0]
    return result


def captured_mapped_view(mem, event, remaining=128*1024*1024):
    """Capture a verified NtMapViewOfSection's successful current-process view."""
    import base64
    import zlib
    args=event['arguments']
    if len(args)!=10: raise ValueError('map-view argument count differs')
    if event['result'] & 0x80000000: return None
    if args[1]!=0xffffffff: raise ValueError('map-view target is not current process')
    address,size=mem.u32(args[2]),mem.u32(args[6])
    if not 0<size<=min(32*1024*1024,remaining):
        raise ValueError('map-view capture limit exceeded')
    if address<0x10000 or address+size>0x100000000 or address%4096 or size%4096:
        raise ValueError('invalid map-view address or size')
    for lo,hi in ((0x60000000,0x64000000),(0x70000000,0x70100000),(0x71000000,0x71400000)):
        if address<hi and address+size>lo: raise ValueError('map-view overlaps emulator reserved space')
    raw=mem.read(address,size)
    if len(raw)!=size: raise ValueError('short map-view read')
    return {'address':address,'size':size,
            'section_offset':mem.read(args[5],8).hex() if args[5] else None,
            'zlib_base64':base64.b64encode(zlib.compress(raw,1)).decode('ascii')}


def captured_user_callback(mem, stack, returning=False):
    """Bounded KiUserCallbackDispatcher inputs or NtCallbackReturn outputs."""
    import struct
    args=struct.unpack('<III',mem.read(stack+4,12))
    if returning:
        pointer,size,status=args
        fields={'phase':'return','status':status,'return_address':mem.u32(stack)}
    else:
        number,pointer,size=args
        if number>255: raise ValueError('callback number exceeds capture limit')
        fields={'phase':'entry','number':number}
    if size>65536 or (size and not pointer) or pointer+size>0x100000000:
        raise ValueError('callback payload exceeds capture limit')
    raw=mem.read(pointer,size) if size else b''
    if len(raw)!=size: raise ValueError('short callback payload')
    return dict(fields,stack=stack,address=pointer,size=size,payload_hex=raw.hex())


def captured_syscall_return(mem, frame, result):
    """Wine i386 normal dispatcher exit, before restoring the user stack.

    Pointer samples are observations only. Replayers must identify each API's
    output contract and copy only its defined output fields, never the whole
    sample. No kernel state or side effects are reconstructed here.
    """
    event=captured_syscall_arguments(mem,mem.u32(frame+8),mem.u32(frame+12),mem.u32(frame+0x1c))
    event['result']=result
    return event


def captured_syscall_arguments(mem,stub_return,stack,register_number):
    import struct
    stub=mem.read(stub_return-12,15)
    if stub[0]!=0xb8 or stub[5]!=0xba or stub[10:13]!=b'\xff\xd2\xc2':
        raise ValueError('unsupported Wine syscall return stub')
    byte_count=struct.unpack_from('<H',stub,13)[0]
    if byte_count>256 or byte_count%4:
        raise ValueError('invalid bounded syscall argument count')
    number=struct.unpack_from('<I',stub,1)[0]
    if number!=register_number:
        raise ValueError('Wine syscall frame and stub numbers differ')
    args=list(struct.unpack(f'<{byte_count//4}I',mem.read(stack+4,byte_count)))
    samples={}
    for index,pointer in enumerate(args):
        if not 0x10000<=pointer<0xfffff000: continue
        try:
            samples[str(index)]=mem.read(pointer,min(256,4096-(pointer&4095))).hex()
        except (OSError,RuntimeError):
            pass  # Scalar/handle arguments are not necessarily readable pointers.
    return {'stub_return':stub_return,'number':number,'return_address':mem.u32(stack),
            'stack':stack,'arguments':args,'pointer_samples':samples}


def captured_unix_return(mem,frame,result):
    """Observe Wine's Unix bridge outputs; samples do not imply replay support."""
    return captured_unix_arguments(mem,mem.u32(frame+12)-16,mem.u32(frame+8),result)


def captured_unix_arguments(mem,stack,return_address,result,arguments=None):
    import struct
    # At the actual caller return Wine has reused the old parameter slot for
    # its return address. Use the entry snapshot when observing that site.
    lo,hi,number,params=(struct.unpack('<4I',mem.read(stack,16))
                         if arguments is None else arguments)
    raw=mem.read(params,min(256,4096-(params&4095)))
    samples={}
    for offset in range(0,len(raw)-3,4):
        pointer=struct.unpack_from('<I',raw,offset)[0]
        # Bounded direct pointees, plus Vulkan extension chains. Unknown API
        # layouts remain unsupported in replay even when samples are present.
        for depth in range(8):
            if len(samples)>=64 or not 0x10000<=pointer<0xfffff000 or str(pointer) in samples: break
            try: sample=mem.read(pointer,min(256,4096-(pointer&4095)))
            except (OSError,RuntimeError): break
            samples[str(pointer)]=sample.hex()
            if len(sample)<8: break
            kind,next_pointer=struct.unpack_from('<II',sample)
            if kind==14 and not (number==342 and offset==4 and depth==0): break
            # These input structures contain arrays beyond their pNext
            # chain. Capture their exact bounded contents, not just the
            # pointer values or unrelated neighboring stack bytes.
            array_fields={14:(56,60),1000147000:(8,12),
                          1000001000:(56,60),1000275002:(8,12),1000274002:(8,12)}
            if kind in array_fields:
                count_offset,pointer_offset=array_fields[kind]
                if len(sample)<pointer_offset+4:
                    raise ValueError('truncated Vulkan array descriptor')
                count=struct.unpack_from('<I',sample,count_offset)[0]
                array=struct.unpack_from('<I',sample,pointer_offset)[0]
                if count>256: raise ValueError('Vulkan input array exceeds capture limit')
                if count and not array and kind!=1000274002:
                    raise ValueError('Vulkan input array has no pointer')
                if count and array:
                    if len(samples)>=64 and str(array) not in samples:
                        raise ValueError('Vulkan input pointer capture limit reached')
                    samples[str(array)]=mem.read(array,count*4).hex()
            if kind!=14 and not 1000000000<=kind<1001000000: break
            pointer=next_pointer
    return {'return_address':return_address,'number':number,'handle':lo|(hi<<32),
            'result':result,'parameters':params,'parameter_bytes':raw.hex(),
            'pointer_samples':samples}


def inside_gdb():
    import gdb
    import struct
    sys.dont_write_bytecode = True
    sys.path.insert(0, os.environ['TAK_RELOAD_TOOLS'])
    from decode_save_state import decode
    from capture_save_baseline import compare
    from livesample import GAMESTATE_PTR, REFERENCE, STRIDE, snapshot, verify_code

    inferior = gdb.selected_inferior()
    from probe_guard import hardware_debug_state
    initial_hardware={str(t.ptid[1]):hardware_debug_state(t.ptid[1]) for t in inferior.threads()}

    class Memory:
        pid = inferior.pid

        def read(self, address, size):
            try:
                return bytes(inferior.read_memory(address, size))
            except gdb.MemoryError as error:
                raise OSError(str(error)) from error

        def u32(self, address):
            return struct.unpack('<I', self.read(address, 4))[0]

    mem = Memory()
    report = {'schema': 1, 'pid': mem.pid, 'capture': 'all-stop hardware breakpoint',
              'complete_state_match': False, 'status': 'not captured'}
    breakpoint = None
    rng_breakpoint = None
    steering_breakpoint = None
    crt_breakpoint = None
    clock_breakpoint = None
    callback_breakpoints = []
    native_return_probe = None
    active_native = []
    pending_actions = {"install": False, "return": False}
    def verify_probe_pc(address):
        pc=int(gdb.parse_and_eval('$pc'))&0xffffffff
        if pc!=address:
            raise RuntimeError(f'debugger attributed probe {address:#x} to PC {pc:#x}')
    stop_events=[]
    def on_stop(event):
        stop_events.append(event)
    gdb.events.stop.connect(on_stop)
    crt_context = {}
    tick_count = int(os.environ.get('TAK_RELOAD_TICKS', '0'))
    next_tick = os.environ.get('TAK_RELOAD_NEXT_TICK') == '1'
    report['capture_basis'] = 'next simulation tick' if next_tick else 'save reload'
    report['frames'] = []
    report['rng_calls'] = []
    report['steering_calls'] = []
    report['crt_calls'] = []
    report['native_order'] = []
    def path_state(game, initial):
        import base64
        import zlib
        address = mem.u32(game + 0x19e70)
        if not address:
            return None
        raw = mem.read(address, 0x22b)
        u32 = lambda offset: struct.unpack_from('<I', raw, offset)[0]
        state = {'address': address, 'fields_hex': raw.hex(),
                 'active_entity': u32(0x58), 'phase': u32(0x5c),
                 'pending_per_player': list(struct.unpack('<10I', mem.read(0x634674, 40)))}
        if initial:
            state['players_hex'] = mem.read(game + 0x2404, 10 * 0x110).hex()
            state['buffers'] = []
            # Constructor 0x415f80 allocates cell records and dirty-block bits;
            # the search's node pool and pointer heap are separate allocations.
            for name, pointer, size in (
                ('cells', u32(0x1c), u32(0x28) * 4),
                ('dirty_blocks', u32(0x2c), ((u32(0x28) + 255) // 256) * 4),
                ('nodes', u32(0), u32(0xc) * 20),
                ('heap', u32(4), u32(0x10) * 4),
            ):
                if not 0 <= size <= 32 * 1024 * 1024 or (size and not pointer):
                    raise RuntimeError('invalid bounded path buffer: ' + name)
                payload = mem.read(pointer, size) if size else b''
                state['buffers'].append({'name': name, 'address': pointer, 'size': size,
                    'zlib_base64': base64.b64encode(zlib.compress(payload)).decode('ascii')})
        return state
    try:
        saved = decode(Path(os.environ['TAK_RELOAD_SAVE']).read_bytes())
        report['source_sha256'] = saved['source_sha256']
        report['code'] = verify_code(mem)
        binary = REFERENCE.read_bytes()
        if mem.read(0x526310, 0x55) != binary[0x126310:0x126365]:
            raise RuntimeError('simulation tick entry differs from reference')
        if os.environ.get('TAK_RELOAD_RUNTIME_STATE') == '1':
            game = mem.u32(GAMESTATE_PTR)
            # The main menu has no entity table yet. The first matching tick
            # below still validates the complete loaded runtime state.
            empty = not game or mem.read(game + 0x14e84, 8) == bytes(8)
            if empty and not next_tick:
                print('Runtime preflight deferred until save load (no active entity table)', flush=True)
            else:
                current = snapshot(mem)
                if current is None or current['unreadable_units']:
                    raise RuntimeError('incomplete runtime preflight snapshot')
                checked = runtime_state(mem, game, current['units'], True)
                print(f"Runtime preflight: {len(checked['missions'])} missions, "
                      f"{len(checked['grids'])} cached grade planes", flush=True)
        if os.environ.get('TAK_RELOAD_GAME_MEMORY') == '1':
            ranges = game_memory_ranges(Path(f'/proc/{inferior.pid}/maps').read_text())
            print(f"Game memory preflight: {sum(size for _, size in ranges)} bytes in {len(ranges)} chunks", flush=True)
        if os.environ.get('TAK_RELOAD_NATIVE_MEMORY') == '1':
            ranges = native_memory_ranges(Path(f'/proc/{inferior.pid}/maps').read_text())
            print(f"Native image preflight: {sum(r['size'] for r in ranges)} bytes in {len(ranges)} chunks", flush=True)

        if os.environ.get('TAK_RELOAD_DEVICE_MEMORY') == '1':
            ranges = device_memory_ranges(Path(f'/proc/{inferior.pid}/maps').read_text())
            print(f"Device mapping preflight: {sum(r['size'] for r in ranges)} bytes in {len(ranges)} chunks", flush=True)

        class Observe(gdb.Breakpoint):
            def __init__(self):
                super().__init__('*0x526351', type=gdb.BP_HARDWARE_BREAKPOINT, internal=True)

            def observe(self):
                try:
                    verify_probe_pc(0x526351)
                    game = mem.u32(GAMESTATE_PTR)
                    if not game:
                        return False
                    tick = mem.u32(game + 0x19f44)
                    if not report['frames'] and not next_tick and tick != saved['tick']:
                        return False
                    if report['frames'] and tick != report['frames'][-1]['tick'] + 1:
                        raise RuntimeError(f'nonsequential simulation tick {tick} after '
                                           f'{report["frames"][-1]["tick"]}')
                    live = snapshot(mem)
                    if live is None or live['unreadable_units']:
                        raise RuntimeError('incomplete stopped snapshot')
                    if live['rng_before'] != live['rng_after']:
                        raise RuntimeError('RNG changed during stopped capture')
                    if os.environ.get('TAK_RELOAD_PATH_STATE') == '1':
                        live['path_state'] = path_state(game, not report['frames'])
                    if os.environ.get('TAK_RELOAD_RUNTIME_STATE') == '1':
                        live['runtime_state'] = runtime_state(mem, game, live['units'], not report['frames'])
                    if os.environ.get('TAK_RELOAD_GAME_MEMORY') == '1' and not report['frames']:
                        live['game_memory'] = game_memory_state(mem, Path(f'/proc/{inferior.pid}/maps').read_text())
                    if os.environ.get('TAK_RELOAD_NATIVE_MEMORY') == '1' and not report['frames']:
                        live['native_memory'] = native_memory_state(mem, Path(f'/proc/{inferior.pid}/maps').read_text())
                    if os.environ.get('TAK_RELOAD_DEVICE_MEMORY') == '1' and not report['frames']:
                        live['device_memory'] = device_memory_state(mem, Path(f'/proc/{inferior.pid}/maps').read_text())
                    live['tick_registers'] = {r: int(gdb.parse_and_eval('$' + r)) & 0xffffffff
                                              for r in ('ebx', 'esi', 'edi', 'ebp', 'esp')}
                    if os.environ.get('TAK_RELOAD_CRT') == '1':
                        if not crt_context:
                            crt_context.update(captured_crt_thread(mem, live['game_memory'], live['tick_registers']['esp']))
                            crt_context['gdb_thread'] = gdb.selected_thread().global_num
                            report['crt_thread'] = dict(crt_context)
                        live['crt_seed'] = mem.u32(crt_context['address'] + 0x14)
                    if os.environ.get('TAK_RELOAD_STEERING') == '1':
                        entities = mem.u32(game + 0x14e84)
                        for unit in live['units']:
                            mover = mem.u32(entities + unit['id'] * STRIDE + 8)
                            if not mover:
                                continue
                            navigator = mem.u32(mover)
                            unit['route_outcome'] = mem.u32(entities + unit['id'] * STRIDE + 0x134)
                            if navigator and mem.u32(navigator) == 0x5f2a24:
                                count = mem.u32(navigator + 0x10c)
                                if not 0 <= count <= 64:
                                    raise RuntimeError('invalid stopped route size')
                                unit['route_world'] = [list(struct.unpack('<hh', mem.read(navigator + 0xc + i*4, 4)))
                                                       for i in range(count)]
                                unit['route_tick'] = mem.u32(navigator + 0x110)
                                unit['route_flags'] = mem.read(navigator + 0x114, 1)[0]
                                if os.environ.get('TAK_RELOAD_PATH_STATE') == '1' and not report['frames']:
                                    unit['entity_address'] = entities + unit['id'] * STRIDE
                                    unit['entity_hex'] = mem.read(unit['entity_address'], STRIDE).hex()
                                    unit['mover_address'] = mover
                                    unit['mover_hex'] = mem.read(mover, 0x38).hex()
                                    unit['navigator_address'] = navigator
                                    unit['navigator_hex'] = mem.read(navigator, 0x115).hex()
                    if not report['frames']:
                        report['live'] = live
                        report['differences'] = compare(saved, live)
                        report['movement_fields_match'] = not report['differences']
                    report['frames'].append(live)
                    print(f"TRACE: captured tick {tick} ({len(report['frames'])}/{tick_count+1} boundaries)",flush=True)
                    if len(report['frames'])==1 and callback_entry and tick_count:
                        # Mutation must occur after continue returns, never
                        # inside a GDB Python breakpoint callback.
                        pending_actions['install']=True
                        return True
                    if tick < report['frames'][0]['tick'] + tick_count:
                        return False
                    report['status'] = 'captured'
                except Exception as error:
                    report['status'] = 'error'
                    report['error'] = str(error)
                return True

        class RandomObserve(gdb.Breakpoint):
            def __init__(self):
                super().__init__('*0x535cc0', type=gdb.BP_HARDWARE_BREAKPOINT, internal=True)

            def observe(self):
                if not report['frames']:
                    return False
                try:
                    if len(report['rng_calls']) >= 16384:
                        raise RuntimeError('RNG capture limit reached')
                    sp = int(gdb.parse_and_eval('$esp')) & 0xffffffff
                    bound = struct.unpack('<i', mem.read(sp + 4, 4))[0]
                    game = mem.u32(GAMESTATE_PTR)
                    report['rng_calls'].append({'tick': mem.u32(game + 0x19f44),
                                                'return_address': mem.u32(sp), 'bound': bound,
                                                'seed_before': mem.u32(0x64186c),
                                                'registers': {r: int(gdb.parse_and_eval('$' + r)) & 0xffffffff
                                                              for r in ('esi', 'edi', 'ebx', 'ecx', 'ebp')}})
                    return False
                except Exception as error:
                    report['status'] = 'error'
                    report['error'] = str(error)
                    return True

        class CrtObserve(gdb.Breakpoint):
            def __init__(self):
                super().__init__('*0x5d4444', type=gdb.BP_HARDWARE_BREAKPOINT, internal=True)

            def observe(self):
                if not report['frames'] or gdb.selected_thread().global_num != crt_context.get('gdb_thread'):
                    return False
                try:
                    if len(report['crt_calls']) >= 262144:
                        raise RuntimeError('CRT RNG capture limit reached')
                    sp = int(gdb.parse_and_eval('$esp')) & 0xffffffff
                    report['crt_calls'].append({'tick': mem.u32(mem.u32(GAMESTATE_PTR) + 0x19f44),
                        'return_address': mem.u32(sp), 'seed_before': mem.u32(crt_context['address'] + 0x14)})
                    return False
                except Exception as error:
                    report['status'] = 'error'
                    report['error'] = str(error)
                    return True

        class ClockObserve(gdb.Breakpoint):
            def __init__(self,address):
                super().__init__(f'*{address:#x}',type=gdb.BP_HARDWARE_BREAKPOINT,internal=True)

            def observe(self):
                if not report['frames'] or gdb.selected_thread().global_num!=crt_context.get('gdb_thread'):
                    return False
                try:
                    events=report.setdefault('performance_calls',[])
                    if len(events)>=65536:
                        raise RuntimeError('performance counter capture limit reached')
                    event=captured_performance_counter(mem,int(gdb.parse_and_eval('$esp'))&0xffffffff,
                                                      int(gdb.parse_and_eval('$eax'))&0xffffffff)
                    event['tick']=mem.u32(mem.u32(GAMESTATE_PTR)+0x19f44)
                    events.append(event)
                    return False
                except Exception as error:
                    report['status']='error'; report['error']=str(error)
                    return True

        class SyscallObserve(gdb.Breakpoint):
            def __init__(self,address):
                super().__init__(f'*{address:#x}',type=gdb.BP_HARDWARE_BREAKPOINT,internal=True)

            def observe(self):
                if not report['frames'] or gdb.selected_thread().global_num!=crt_context.get('gdb_thread'):
                    return False
                try:
                    events=report.setdefault('syscall_calls',[])
                    if len(events)>=65536: raise RuntimeError('syscall capture limit reached')
                    event=captured_syscall_return(mem,int(gdb.parse_and_eval('$esp'))&0xffffffff,
                                                 int(gdb.parse_and_eval('$eax'))&0xffffffff)
                    event['tick']=mem.u32(mem.u32(GAMESTATE_PTR)+0x19f44)
                    if map_return and event['stub_return']==map_return:
                        event['mapped_view']=captured_mapped_view(mem,event,
                            128*1024*1024-report.get('mapped_view_bytes',0))
                        if event['mapped_view']:
                            report['mapped_view_bytes']=report.get('mapped_view_bytes',0)+event['mapped_view']['size']
                    events.append(event)
                    if event['stub_return']==clock_return:
                        counter=captured_performance_counter(mem,event['stack'],event['result'])
                        counter['tick']=event['tick']
                        report.setdefault('performance_calls',[]).append(counter)
                    report['native_order'].append({'kind':'syscall','index':len(events)-1})
                    return False
                except Exception as error:
                    report['status']='error'; report['error']=str(error)
                    return True

        class NativeReturnObserve(gdb.Breakpoint):
            def __init__(self,frame,software_site=0):
                self.software=bool(software_site)
                if software_site:
                    super().__init__(f'*{software_site:#x}',type=gdb.BP_BREAKPOINT,internal=True)
                else:
                    super().__init__(f'*(unsigned int*){frame+8:#x}',type=gdb.BP_WATCHPOINT,
                                     wp_class=gdb.WP_READ,internal=True)
                self.frame=frame

            def observe(self):
                ip=int(gdb.parse_and_eval('$eip'))&0xffffffff
                if ip==(syscall_return if self.software else syscall_return+8): return SyscallObserve.stop(self)
                if ip!=(unix_return if self.software else unix_return+4) or not report['frames'] or gdb.selected_thread().global_num!=crt_context.get('gdb_thread'):
                    return False
                try:
                    current_frame=int(gdb.parse_and_eval('$esp'))&0xffffffff
                    if not self.software and current_frame != self.frame:
                        raise RuntimeError('Wine Unix return frame moved')
                    events=report.setdefault('unix_calls',[])
                    if len(events)>=16384: raise RuntimeError('Unix bridge capture limit reached')
                    event=captured_unix_return(mem,current_frame,int(gdb.parse_and_eval('$eax'))&0xffffffff)
                    event['tick']=mem.u32(mem.u32(GAMESTATE_PTR)+0x19f44)
                    events.append(event)
                    report['native_order'].append({'kind':'unix','index':len(events)-1})
                    return False
                except Exception as error:
                    report['status']='error'; report['error']=str(error)
                    return True

        def arm_native_return():
            nonlocal native_return_probe
            address=active_native[-1]['site'] if active_native else 0
            if native_return_probe is not None:
                if native_return_probe.is_valid():
                    if address==native_return_probe.address: return
                    native_return_probe.delete()
                native_return_probe=None
            if address:
                native_return_probe=ActualNativeReturn(address)
                native_return_probe.thread=crt_context['gdb_thread']

        class ActualNativeReturn(gdb.Breakpoint):
            def __init__(self,address):
                super().__init__(f'*{address:#x}',type=gdb.BP_HARDWARE_BREAKPOINT,internal=True)
                self.address=address
                self.silent=True

            def observe(self):
                if not active_native: return False
                try:
                    verify_probe_pc(self.address)
                    current=active_native[-1]
                    sp=int(gdb.parse_and_eval('$esp'))&0xffffffff
                    if sp!=current['return_stack']:
                        raise RuntimeError(f'native user return stack {sp:#x} differs from '
                            f'entry {current["index"]} expected {current["return_stack"]:#x} '
                            f'at {current["site"]:#x}')
                    entry=report['native_entries'][current['index']]
                    result=int(gdb.parse_and_eval('$eax'))&0xffffffff
                    if entry['kind']=='syscall':
                        event=captured_syscall_arguments(mem,current['site'],sp,entry['number'])
                        event['result']=result
                        events=report.setdefault('syscall_calls',[])
                        if event['stub_return']==clock_return:
                            counter=captured_performance_counter(mem,sp,result)
                            counter['tick']=mem.u32(mem.u32(GAMESTATE_PTR)+0x19f44)
                            report.setdefault('performance_calls',[]).append(counter)
                        if map_return and event['stub_return']==map_return:
                            event['mapped_view']=captured_mapped_view(mem,event,
                                128*1024*1024-report.get('mapped_view_bytes',0))
                            if event['mapped_view']:
                                report['mapped_view_bytes']=report.get('mapped_view_bytes',0)+event['mapped_view']['size']
                    else:
                        event=captured_unix_arguments(mem,entry['stack']+4,entry['return_address'],result,
                            (entry['handle']&0xffffffff,entry['handle']>>32,
                             entry['number'],entry['parameters']))
                        events=report.setdefault('unix_calls',[])
                    if len(events)>=65536: raise RuntimeError('native return event limit reached')
                    event['tick']=mem.u32(mem.u32(GAMESTATE_PTR)+0x19f44)
                    event['entry_index']=current['index']
                    events.append(event)
                    report['native_order'].append({'kind':entry['kind'],'index':len(events)-1})
                    active_native.pop()
                    pending_actions['return']=True
                    return True
                except Exception as error:
                    report['status']='error'; report['error']=str(error)
                    return True

        class NativeEntryObserve(gdb.Breakpoint):
            def __init__(self,address,unix):
                super().__init__(f'*{address:#x}',type=gdb.BP_BREAKPOINT,internal=True)
                self.unix=unix
                self.address=address
                self.silent=True

            def observe(self):
                if not report['frames'] or gdb.selected_thread().global_num!=crt_context.get('gdb_thread'):
                    return False
                try:
                    verify_probe_pc(self.address)
                    stack=int(gdb.parse_and_eval('$esp'))&0xffffffff
                    if not self.unix and mem.u32(stack)==callback_return+12:
                        return False  # NtCallbackReturn has its own nonlocal-return event.
                    events=report.setdefault('native_entries',[])
                    if len(events)>=65536: raise RuntimeError('native entry limit reached')
                    if self.unix:
                        ret,lo,hi,number,params=struct.unpack('<5I',mem.read(stack,20))
                        event={'kind':'unix','return_address':ret,'handle':lo|(hi<<32),
                               'number':number,'parameters':params,'stack':stack,
                               'parameter_bytes':mem.read(params,min(256,4096-(params&4095))).hex()}
                    else:
                        event=captured_syscall_arguments(mem,mem.u32(stack),stack+4,
                            int(gdb.parse_and_eval('$eax'))&0xffffffff)
                        event['kind']='syscall'
                    event['tick']=mem.u32(mem.u32(GAMESTATE_PTR)+0x19f44)
                    site=event['return_address'] if self.unix else event['stub_return']
                    return_stack=stack+20 if self.unix else stack+4
                    key=(event['kind'],site,return_stack)
                    if active_native and active_native[-1]['key']==key:
                        # Same still-active user stack: an observed dispatcher
                        # restart, not an inferred second application call.
                        event['phase']='restart'
                        event['restarts_entry']=active_native[-1]['index']
                    else:
                        event['phase']='entry'
                        active_native.append({'key':key,'site':site,'return_stack':return_stack,'index':len(events)})
                    events.append(event)
                    report['native_order'].append({'kind':'entry','index':len(events)-1})
                    pending_actions['return']=True
                    return True
                except Exception as error:
                    report['status']='error'; report['error']=str(error)
                    return True

        class UserCallbackObserve(gdb.Breakpoint):
            def __init__(self,address,returning):
                super().__init__(f'*{address:#x}',type=gdb.BP_BREAKPOINT,internal=True)
                self.returning=returning
                self.address=address
                self.silent=True

            def observe(self):
                if not report['frames'] or gdb.selected_thread().global_num!=crt_context.get('gdb_thread'):
                    return False
                try:
                    verify_probe_pc(self.address)
                    events=report.setdefault('user_callbacks',[])
                    if len(events)>=16384: raise RuntimeError('callback event limit reached')
                    event=captured_user_callback(mem,int(gdb.parse_and_eval('$esp'))&0xffffffff,self.returning)
                    total=report.get('callback_bytes',0)+event['size']
                    if total>16*1024*1024: raise RuntimeError('callback byte limit reached')
                    report['callback_bytes']=total
                    event['tick']=mem.u32(mem.u32(GAMESTATE_PTR)+0x19f44)
                    events.append(event)
                    report['native_order'].append({'kind':'callback','index':len(events)-1})
                    pending_actions['return']=True
                    return True
                except Exception as error:
                    report['status']='error'; report['error']=str(error)
                    return True

        class SteeringObserve(gdb.Breakpoint):
            def __init__(self):
                super().__init__('*0x4d9cf8', type=gdb.BP_HARDWARE_BREAKPOINT, internal=True)

            def observe(self):
                if not report['frames']:
                    return False
                try:
                    if len(report['steering_calls']) >= 16384:
                        raise RuntimeError('steering capture limit reached')
                    entity = int(gdb.parse_and_eval('$esi')) & 0xffffffff
                    bp = int(gdb.parse_and_eval('$ebp')) & 0xffffffff
                    mover = mem.u32(entity + 8)
                    navigator = mem.u32(mover)
                    event = {'tick': mem.u32(mem.u32(GAMESTATE_PTR) + 0x19f44),
                             'id': struct.unpack('<H', mem.read(entity + 2, 2))[0],
                             'position_raw': list(struct.unpack('<iii', mem.read(entity + 0x68, 12))),
                             'heading': struct.unpack('<H', mem.read(entity + 0x7e, 2))[0],
                             'requested_heading': int(gdb.parse_and_eval('$eax')) & 65535,
                             'fctrl': int(gdb.parse_and_eval('$fctrl')),
                             'aim_raw': list(struct.unpack('<iii', mem.read(bp - 0x34, 12))),
                             'movement_flags': struct.unpack('<H', mem.read(mover + 0x36, 2))[0],
                             'navigator_vtable': mem.u32(navigator)}
                    if event['navigator_vtable'] == 0x5f2a24:
                        count = mem.u32(navigator + 0x10c)
                        if not 1 <= count <= 64:
                            raise RuntimeError('invalid active ground route size')
                        event['route_world'] = [list(struct.unpack('<hh', mem.read(navigator + 0xc + i*4, 4)))
                                                for i in range(count)]
                    report['steering_calls'].append(event)
                    return False
                except Exception as error:
                    report['status'] = 'error'
                    report['error'] = str(error)
                    return True

        breakpoint = Observe()
        if tick_count:
            rng_breakpoint = RandomObserve()
        clock_return=int(os.environ.get('TAK_RELOAD_CLOCK_RETURN','0'))
        syscall_return=int(os.environ.get('TAK_RELOAD_SYSCALL_RETURN','0'))
        map_return=int(os.environ.get('TAK_RELOAD_MAP_RETURN','0'))
        if map_return:
            stub=mem.read(map_return-12,15)
            if stub[0]!=0xb8 or stub[5]!=0xba or stub[10:]!=bytes.fromhex('ffd2c22800'):
                raise RuntimeError('NtMapViewOfSection return stub differs')
            report['map_view_return']=map_return
        unix_return=int(os.environ.get('TAK_RELOAD_UNIX_RETURN','0'))
        native_teb=int(os.environ.get('TAK_RELOAD_NATIVE_RETURN_TEB','0'))
        callback_entry=int(os.environ.get('TAK_RELOAD_CALLBACK_ENTRY','0'))
        callback_return=int(os.environ.get('TAK_RELOAD_CALLBACK_RETURN','0'))
        syscall_entry=int(os.environ.get('TAK_RELOAD_SYSCALL_ENTRY','0'))
        unix_entry=int(os.environ.get('TAK_RELOAD_UNIX_ENTRY','0'))
        if syscall_entry:
            for address,prefix in ((syscall_entry,'648b0d18020000c701000000008f4108'),
                                   (unix_entry,'648b0d18020000c701008000008f4108')):
                expected=bytes.fromhex(prefix)
                if mem.read(address,len(expected))!=expected:
                    raise RuntimeError('native dispatcher entry differs')
            report['native_entry_sites']=[syscall_entry,unix_entry]
        if callback_entry:
            prologue=bytes.fromhex('5589e583ec0c8b4508894424088b4510894424048b450c890424')
            if mem.read(callback_entry,len(prologue))!=prologue:
                raise RuntimeError('Wine callback dispatcher prologue differs')
            code=mem.read(callback_return,15)
            if code[0]!=0xb8 or code[5]!=0xba or code[10:]!=bytes.fromhex('ffd2c20c00'):
                raise RuntimeError('Wine callback return stub differs')
            report['callback_entry']=callback_entry; report['callback_return']=callback_return
            report['capture']='all-stop hardware ticks/RNG plus four temporary Wine software probes'
            if syscall_entry:
                report['capture']='all-stop hardware ticks/RNG/user returns plus four temporary Wine software probes'
                report['native_return_observation']='actual user return sites'
        if clock_return:
            if mem.read(clock_return,3)!=b'\xc2\x08\x00':
                raise RuntimeError('performance counter observation site is not RET 8')
            report['performance_counter_return']=clock_return
            if not syscall_return: clock_breakpoint=ClockObserve(clock_return)
        if syscall_return:
            if mem.read(syscall_return,14)!=bytes.fromhex('8b5c24208b4c24088b64240c51c3'):
                raise RuntimeError('Wine normal syscall exit differs from reference')
            report['syscall_return']=syscall_return
            if native_teb:
                if mem.u32(native_teb+0x18)!=native_teb:
                    raise RuntimeError('native return TEB self pointer differs')
                if mem.read(unix_return,10)!=bytes.fromhex('8b4c24088b64240c51c3'):
                    raise RuntimeError('Wine Unix return differs from reference')
                frame=mem.u32(native_teb+0x218)
                report['native_return_frame']=frame; report['unix_return']=unix_return
                if not callback_entry: clock_breakpoint=NativeReturnObserve(frame)
            else:
                clock_breakpoint=SyscallObserve(syscall_return)
        if os.environ.get('TAK_RELOAD_STEERING') == '1' and not (clock_return or syscall_return):
            if mem.read(0x4d9cf3, 9) != binary[0xd9cf3:0xd9cfc]:
                raise RuntimeError('steering observation site differs from reference')
            steering_breakpoint = SteeringObserve()
        if os.environ.get('TAK_RELOAD_CRT') == '1':
            if mem.read(0x5d4444, 0x22) != binary[0x1d4444:0x1d4466]:
                raise RuntimeError('CRT RNG routine differs from reference')
            crt_breakpoint = CrtObserve()
        print('ARMED: waiting for the next simulation tick' if next_tick else
              f"ARMED: load the save; waiting for tick {saved['tick']}", flush=True)
        while True:
            stop_events.clear()
            gdb.execute('continue',to_string=True)
            should_stop=False
            if len(stop_events)!=1 or not isinstance(stop_events[0],gdb.BreakpointEvent):
                raise RuntimeError('capture interrupted outside an observed breakpoint')
            for probe in stop_events[0].breakpoints:
                if not hasattr(probe,'observe'):
                    raise RuntimeError('unexpected debugger breakpoint')
                should_stop=probe.observe() or should_stop
                if report['status']=='error': break
            if report['status']=='error': break
            if pending_actions['install']:
                pending_actions['install']=False
                from probe_guard import save_manifest
                sites=[callback_entry,callback_return,
                       syscall_entry or syscall_return,unix_entry or unix_return]
                save_manifest(os.environ['TAK_RELOAD_PROBE_GUARD'],int(inferior.pid),
                    [{'address':a,'bytes':mem.read(a,16).hex()} for a in sites],
                    os.environ.get('TAK_RELOAD_RESUME_AFTER_RECOVERY')=='1',hardware=initial_hardware)
                gdb.execute('set displaced-stepping off')
                gdb.execute('set scheduler-locking step')
                gdb.execute('set breakpoint always-inserted off')
                report['software_probe_step_policy']='displaced off; scheduler-locking step'
                callback_breakpoints.append(UserCallbackObserve(callback_entry,False))
                callback_breakpoints.append(UserCallbackObserve(callback_return,True))
                if not syscall_entry:
                    callback_breakpoints.append(NativeReturnObserve(frame,syscall_return))
                    callback_breakpoints.append(NativeReturnObserve(frame,unix_return))
                if syscall_entry:
                    callback_breakpoints.append(NativeEntryObserve(syscall_entry,False))
                    callback_breakpoints.append(NativeEntryObserve(unix_entry,True))
                for probe in callback_breakpoints:
                    probe.thread=crt_context['gdb_thread']
                continue
            if pending_actions['return']:
                pending_actions['return']=False
                arm_native_return()
                continue
            if should_stop: break
    except (Exception, KeyboardInterrupt) as error:
        report['error'] = str(error)
    finally:
        gdb.events.stop.disconnect(on_stop)
        if native_return_probe is not None and native_return_probe.is_valid(): native_return_probe.delete()
        for probe in callback_breakpoints:
            if probe.is_valid(): probe.delete()
        if breakpoint is not None and breakpoint.is_valid():
            breakpoint.delete()
        if rng_breakpoint is not None and rng_breakpoint.is_valid():
            rng_breakpoint.delete()
        if steering_breakpoint is not None and steering_breakpoint.is_valid():
            steering_breakpoint.delete()
        if crt_breakpoint is not None and crt_breakpoint.is_valid():
            crt_breakpoint.delete()
        if clock_breakpoint is not None and clock_breakpoint.is_valid():
            clock_breakpoint.delete()
        guard_path=Path(os.environ['TAK_RELOAD_PROBE_GUARD'])
        if guard_path.exists():
            from probe_guard import mark_restored
            guard=json.loads(guard_path.read_text())
            for probe in guard['probes']:
                expected=bytes.fromhex(probe['bytes'])
                if mem.read(probe['address'],len(expected))!=expected:
                    raise RuntimeError('probe cleanup did not restore original bytes')
            mark_restored(guard_path)
        try:
            gdb.execute('detach')
        except gdb.error as error:
            report['detach_error'] = str(error)
        with open(os.environ['TAK_RELOAD_OUTPUT'], 'x') as output:
            json.dump(report, output, indent=2)
            output.write('\n')
        print(json.dumps({'status': report['status'],
                          'differences': len(report.get('differences', []))}), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('save', type=Path)
    parser.add_argument('--pid', type=int)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=120)
    parser.add_argument('--ticks', type=int, default=0, help='also capture 0..120 subsequent ticks and RNG calls')
    parser.add_argument('--steering', action='store_true', help='also observe mover aim, route points and x87 control word')
    parser.add_argument('--path-state', action='store_true', help='also capture bounded initial path-search buffers and scheduler state')
    parser.add_argument('--runtime-state', action='store_true', help='also capture mission queues, entity slots and cached ground grade planes')
    parser.add_argument('--game-memory', action='store_true', help='also capture bounded private low-address memory, including script heaps and globals')
    parser.add_argument('--crt', action='store_true', help='also trace simulation-thread CRT RNG calls using the fourth hardware breakpoint')
    parser.add_argument('--native-memory', action='store_true', help='also capture high-address PE images and writable globals at the baseline tick')
    parser.add_argument('--device-memory', action='store_true', help='also read bounded shared NVIDIA and Wine CPU buffers; not a GPU checkpoint')
    parser.add_argument('--clock-return',type=lambda s:int(s,0),default=0,help='identified NtQueryPerformanceCounter RET site; replaces steering-call observation with completed external clock reads')
    parser.add_argument('--syscall-return',type=lambda s:int(s,0),default=0,help='Wine normal syscall dispatcher exit; captures completed OS calls and clocks using one hardware slot')
    parser.add_argument('--map-view-return',type=lambda s:int(s,0),default=0,help='verified NtMapViewOfSection RET; capture returned view bytes (32 MiB/view, 128 MiB total)')
    parser.add_argument('--user-callbacks',nargs=2,type=lambda s:int(s,0),metavar=('ENTRY','RETURN'),help='Wine callback dispatcher and NtCallbackReturn entries; uses four temporary software probes after baseline capture')
    parser.add_argument('--native-entries',nargs=2,type=lambda s:int(s,0),metavar=('SYSCALL','UNIX'),help='record dispatcher entries and actual user return sites; replaces shared-exit probes and uses the fourth hardware slot')
    parser.add_argument('--native-return-teb',type=lambda s:int(s,0),default=0,help='identified simulation TEB; use one read watchpoint for OS and Unix bridge results')
    parser.add_argument('--unix-return',type=lambda s:int(s,0),default=0,help='Wine Unix bridge normal return site for --native-return-teb')
    parser.add_argument('--next-tick', action='store_true', help='capture the next tick without requiring a save reload; the save remains reference metadata')
    args = parser.parse_args()
    if bool(args.user_callbacks)!=bool(args.native_entries):
        parser.error('Wine callback capture requires native entries and guarded actual-return observation together')
    if not 1 <= args.seconds <= 180:
        parser.error('capture window must be 1..180 seconds')
    if not 0 <= args.ticks <= 120:
        parser.error('follow-up ticks must be 0..120')
    if args.path_state and not args.steering:
        parser.error('--path-state requires --steering')
    if args.runtime_state and not args.path_state:
        parser.error('--runtime-state requires --path-state')
    if args.game_memory and not args.runtime_state:
        parser.error('--game-memory requires --runtime-state')
    if args.crt and not args.game_memory:
        parser.error('--crt requires --game-memory')
    if args.native_memory and not args.game_memory:
        parser.error('--native-memory requires --game-memory')
    if args.clock_return and not args.crt:
        parser.error('--clock-return requires --crt for thread identity')
    if args.syscall_return and not (args.crt and args.clock_return):
        parser.error('--syscall-return requires --crt and --clock-return')
    if args.map_view_return and not args.syscall_return:
        parser.error('--map-view-return requires --syscall-return')
    if args.user_callbacks and not args.native_return_teb:
        parser.error('--user-callbacks requires --native-return-teb')
    if args.native_entries and not args.user_callbacks:
        parser.error('--native-entries requires --user-callbacks')
    if bool(args.native_return_teb)!=bool(args.unix_return) or (args.native_return_teb and not args.syscall_return):
        parser.error('--native-return-teb and --unix-return require each other and --syscall-return')
    if args.device_memory and not args.native_memory:
        parser.error('--device-memory requires --native-memory')
    if args.output.exists() or not args.output.parent.is_dir():
        parser.error('output must be a new file in an existing directory')
    from decode_save_state import decode
    from livesample import find_pid
    decode(args.save.read_bytes())  # Reject unsupported saves before attaching.
    pid = args.pid if args.pid is not None else find_pid()
    if pid < 1:
        parser.error('PID must be positive')
    script = str(Path(__file__).resolve())
    guard_path=args.output.with_suffix('.probes.json')
    recovery_log=args.output.with_suffix('.recovery.log')
    if guard_path.exists() or recovery_log.exists():
        parser.error('probe guard and recovery paths must be new')
    process_state=Path(f'/proc/{pid}/status').read_text().split('State:',1)[1].strip()[0]
    env = dict(os.environ, TAK_RELOAD_SAVE=str(args.save.resolve()),
               TAK_RELOAD_OUTPUT=str(args.output.resolve()),
               TAK_RELOAD_PROBE_GUARD=str(guard_path.resolve()),
               TAK_RELOAD_RESUME_AFTER_RECOVERY=str(int(process_state not in ('T','t'))),
               TAK_RELOAD_TOOLS=str(Path(script).parent), TAK_RELOAD_TICKS=str(args.ticks),
               TAK_RELOAD_STEERING=str(int(args.steering)), TAK_RELOAD_PATH_STATE=str(int(args.path_state)),
               TAK_RELOAD_RUNTIME_STATE=str(int(args.runtime_state)),
               TAK_RELOAD_GAME_MEMORY=str(int(args.game_memory)), TAK_RELOAD_CRT=str(int(args.crt)),
               TAK_RELOAD_DEVICE_MEMORY=str(int(args.device_memory)), TAK_RELOAD_CLOCK_RETURN=str(args.clock_return),
               TAK_RELOAD_SYSCALL_RETURN=str(args.syscall_return),
               TAK_RELOAD_MAP_RETURN=str(args.map_view_return),
               TAK_RELOAD_CALLBACK_ENTRY=str(args.user_callbacks[0] if args.user_callbacks else 0),
               TAK_RELOAD_CALLBACK_RETURN=str(args.user_callbacks[1] if args.user_callbacks else 0),
               TAK_RELOAD_SYSCALL_ENTRY=str(args.native_entries[0] if args.native_entries else 0),
               TAK_RELOAD_UNIX_ENTRY=str(args.native_entries[1] if args.native_entries else 0),
               TAK_RELOAD_NATIVE_RETURN_TEB=str(args.native_return_teb), TAK_RELOAD_UNIX_RETURN=str(args.unix_return),
               TAK_RELOAD_NATIVE_MEMORY=str(int(args.native_memory)), TAK_RELOAD_NEXT_TICK=str(int(args.next_tick)))
    commands = ['gdb', '-nx', '-q', '-batch', '-ex', 'set debuginfod enabled off',
                '-ex', 'set pagination off', '-ex', 'set print thread-events off',
                '-ex', 'set non-stop off', '-ex', 'handle SIGUSR1 nostop noprint pass',
                '-ex', f'attach {pid}',
                '-ex', f'python exec(compile(open({script!r}).read(), {script!r}, "exec"))']
    try:
        result = subprocess.run(['timeout', '--signal=INT', '--kill-after=5s',
                                 f'{args.seconds}s', *commands], env=env)
    finally:
        if guard_path.exists():
            from probe_guard import recover_manifest
            recovery=recover_manifest(guard_path,recovery_log)
            if recovery['needed']: print(json.dumps({'probe_recovery':recovery}),flush=True)
    if not args.output.exists():
        raise RuntimeError(f'no capture report; debugger status {result.returncode}')
    report = json.loads(args.output.read_text())
    if report['status'] != 'captured' or 'detach_error' in report:
        raise RuntimeError(f'capture unsuccessful: {report.get("error", report["status"])}')


if 'gdb' in sys.modules:
    inside_gdb()
elif __name__ == '__main__':
    main()
