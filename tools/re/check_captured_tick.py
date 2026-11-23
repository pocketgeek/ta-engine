#!/usr/bin/env python3
"""Run retail tick bodies offline from one memory snapshot, stopping on divergence.

No subsequent-frame state is injected. This checks the emulation fixture, not
the C++ port. Windows key polling assumes released keys; the CRT thread record
is recovered from the captured simulation thread's TLS. Initial batch registers
drive the batch countdown and the real simulation epilogue. Outer frame work is
excluded by default; --render-batches attempts the native render path.
--whole-frame instead continues the captured stack to each next tick boundary,
including outer frame work. Unsupported dependencies stop either mode.
Remaining native substitutions prohibit a whole-game parity claim.
"""
import argparse
import json
from pathlib import Path
import struct
from collections import deque

from emu import STACK, STACK_SZ
from emureload import CapturedProcess
from livesample import snapshot
from unicorn import UcError, UC_HOOK_CODE, UC_HOOK_BLOCK
from unicorn.x86_const import (UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_EBX,
                               UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EIP,
                               UC_X86_REG_EAX)


def find_crt_thread(p, stack_address):
    """Identify captured Win32 TEB by self pointer, stack bounds and TLS owner.

    The observed RNG caller's EBP identifies the thread, not future mutable
    state. TLS slot index comes from the executable's captured CRT global.
    """
    index = p.u32(0x629210)
    if index >= 64:
        raise ValueError('CRT TLS expansion slots are not supported')
    found = []
    for page in sorted(p.pages):
        if p.u32(page + 0x18) != page:
            continue
        if not p.u32(page + 8) <= stack_address < p.u32(page + 4):
            continue
        thread = p.u32(page + 0xe10 + index * 4)
        if thread and (thread & ~4095) in p.pages and p.u32(thread) == p.u32(page + 0x24):
            found.append(thread)
    if len(found) != 1:
        raise ValueError(f'expected one captured simulation CRT thread, found {len(found)}')
    return found[0]


def replay(capture, count, crt_seed=None, render_batches=False, native_render=None, whole_frame=False,
           stop_on_difference=True, extend_simulation=False):
    p = CapturedProcess(capture)
    if not p.frame.get('game_memory'):
        raise ValueError('tick replay requires the bounded game-memory capture')
    p.read = lambda address, size: bytes(p.uc.mem_read(address, size))
    clock = None
    cursor = None
    monitor = None
    display = None
    formats = None
    window_loop = None
    window = None
    map_view = None
    unmap = None
    vulkan_images = None
    callbacks = None
    native_history = deque(maxlen=48)
    if native_render:
        from emurender import (CapturedClock, CapturedCursor, CapturedMonitor,
                               CapturedDisplaySettings, CapturedFormatProperties, CapturedUnmap, CapturedMapView,
                               restore_thread_segment)
        restore_thread_segment(p,capture['crt_thread']['teb'])
        clock = CapturedClock(p,capture)
        if native_render.get('cursor'):
            cursor=CapturedCursor(p,capture,native_render['cursor'])
        if native_render.get('monitor'):
            monitor=CapturedMonitor(p,capture,native_render['monitor'])
        if native_render.get('display'):
            display=CapturedDisplaySettings(p,capture,native_render['display'],native_render.get('monitor',0))
        if native_render.get('formats'):
            formats=CapturedFormatProperties(p,capture,native_render['formats'])
        if native_render.get('window_loop'):
            from emuwindow import CapturedWindowLoop
            window_loop=CapturedWindowLoop(p,capture,native_render['window_loop'])
        if native_render.get('window'):
            from emuwindow import CapturedWindowQueries
            window=CapturedWindowQueries(p,capture,*native_render['window'])
        if native_render.get('map_view'):
            map_view=CapturedMapView(p,capture,native_render['map_view'])
        if native_render.get('unmap'):
            unmap=CapturedUnmap(p,capture,native_render['unmap'])
        if native_render.get('vulkan_images'):
            from emuvulkan import CapturedVulkanImages
            vulkan_images=CapturedVulkanImages(p,capture,native_render['vulkan_images'])
        if native_render.get('callbacks'):
            from emucallback import CapturedDispatchCallbacks
            callbacks=CapturedDispatchCallbacks(p,capture,native_render['callbacks'])
        if native_render.get('alert'):
            p.icd.hooks[native_render['alert']] = lambda uc,args:(1,0)
        if native_render.get('keys'):
            p.icd.hooks[native_render['keys']] = lambda uc,args:(1,0)
        if native_render.get('allocate'):
            p.icd.hooks[native_render['allocate']] = lambda uc,args:(3,p.allocate(uc,args+8)[1])
        if native_render.get('destroy_image'):
            # Diagnostic only: captured GPU handles cannot be destroyed in
            # the offline process. This void API has four 32-bit stack words.
            p.icd.hooks[native_render['destroy_image']] = lambda uc,args:(4,0)
        if native_render.get('free'):
            def free_substitute(uc,address,size,data):
                sp=uc.reg_read(UC_X86_REG_ESP)
                pointer=p.u32(sp+12)
                # Only our own substitute allocations lack native heap
                # headers. Original allocations still use real RtlFreeHeap.
                if pointer not in p.allocations: return
                del p.allocations[pointer]
                uc.reg_write(UC_X86_REG_EAX,1)
                uc.reg_write(UC_X86_REG_ESP,sp+16)
                uc.reg_write(UC_X86_REG_EIP,p.u32(sp))
            p.uc.hook_add(UC_HOOK_CODE,free_substitute,
                          begin=native_render['free'],end=native_render['free'])
        def native_instruction(uc,address,size,data):
            native_history.append({'ip':hex(address),'sp':hex(uc.reg_read(UC_X86_REG_ESP)),
                                   'bp':hex(uc.reg_read(UC_X86_REG_EBP)),
                                   'stack':bytes(uc.mem_read(uc.reg_read(UC_X86_REG_ESP),16)).hex(),
                                   'bytes':bytes(uc.mem_read(address,min(size,15))).hex()})
        # Texture conversion executes millions of instructions. Keep bounded
        # basic-block history so diagnostic reads do not dominate the replay.
        p.uc.hook_add(UC_HOOK_BLOCK,native_instruction,begin=0x70000000,end=0xffffffff)
    fixed_global_memory=None
    if not native_render:
        from emuglobalmemory import CapturedFixedGlobalMemory
        fixed_global_memory=CapturedFixedGlobalMemory(p)
    # Imported GetAsyncKeyState is stdcall with one argument. Do not modify
    # gameplay RNG or bypass any mission, movement or search routine.
    key_function = p.u32(0x5eb398)
    p.ensure(key_function, 1)
    p.icd.hooks[key_function] = lambda uc, args: (1, 0)
    # Enter/LeaveCriticalSection: this offline harness executes only one thread.
    for slot in (0x5eb108, 0x5eb110):
        function = p.u32(slot)
        p.ensure(function, 1)
        p.icd.hooks[function] = lambda uc, args: (1, 0)
    # CRT rand also selects newly created unit slots (0x5121d8). It is NOT
    # merely visual RNG. Resolve the captured thread record instead of seeding
    # a substitute. Keep all original fields and execute the actual CRT LCG.
    thread = find_crt_thread(p, capture['rng_calls'][0]['registers']['ebp'])
    if crt_seed is not None:
        p.put(thread + 0x14, struct.pack('<I', crt_seed))
    initial_crt_seed = p.u32(thread + 0x14)
    p.icd.hooks[0x5dc403] = lambda uc, args: (0, thread)
    crt_events = []
    crt_mismatch = None
    def observe_crt(uc, address, size, data):
        nonlocal crt_mismatch
        event={'tick': p.u32(p.game + 0x19f44),
               'return_address': p.u32(uc.reg_read(UC_X86_REG_ESP)),
               'seed_before': p.u32(thread + 0x14)}
        index=len(crt_events)
        crt_events.append(event)
        if capture.get('crt_calls'):
            expected=capture['crt_calls'][index:index+1]
            if expected != [event]:
                crt_mismatch={'index':index,'retail':expected,'emulated':[event]}
                raise RuntimeError('CRT call sequence differs before executing the draw')
    p.uc.hook_add(UC_HOOK_CODE, observe_crt, begin=0x5d4444, end=0x5d4444)
    report = {'complete_state_match': False, 'port_comparison': False,
              'crt_thread': thread, 'initial_crt_seed': initial_crt_seed,
              'crt_seed_overridden': crt_seed is not None,
              'limitations': ['keys assumed released', 'single-threaded critical sections',
                              'outer frame work beyond simulation function excluded',
                              'uncaptured mapped bytes not audited',
                              'new-unit mover +2c and flag padding allocator bytes excluded'],
              'ticks': [], 'crt_calls': crt_events, 'first_mismatch': None}
    initial_ids = {u['id'] for u in p.frame['units']}
    context = p.frame.get('tick_registers', {'ebx': 0, 'esi': 0, 'edi': 1})
    batch_left, batch_size, mode = context['esi'], context['edi'], context['ebx']
    if not 0 <= batch_left < batch_size <= 120:
        raise ValueError('invalid initial tick-batch context')
    report['initial_batch'] = {'remaining_after_current': batch_left, 'size': batch_size, 'mode': mode}
    report['render_batches'] = render_batches
    report['whole_frame'] = whole_frame
    if whole_frame:
        report['limitations'].remove('outer frame work beyond simulation function excluded')
        for name,register in (('esp',UC_X86_REG_ESP),('ebp',UC_X86_REG_EBP),
                              ('ebx',UC_X86_REG_EBX),('esi',UC_X86_REG_ESI),('edi',UC_X86_REG_EDI)):
            p.uc.reg_write(register,context[name])
        boundary_hits = [0]
        def stop_at_next_tick(uc,address,size,data):
            boundary_hits[0] += 1
            if boundary_hits[0] == 2: uc.emu_stop()
        p.uc.hook_add(UC_HOOK_CODE,stop_at_next_tick,begin=0x526351,end=0x526351)
        # Deliberate early stop: opt into emu.Icd.call returning without reaching the return address.
        p.icd.allow_early_stop=True
    if clock:
        report['limitations'].extend(['recorded external clocks replayed in strict caller/tick order',
                                     'native thread alerts report success without running workers',
                                     'bounded native allocation substitute; GPU concurrency not replayed'])
        if native_render.get('destroy_image'):
            report['limitations'].append('vkDestroyImage skipped; captured GPU handles are not live offline')
        if window_loop:
            report['limitations'].append('recorded window messages; kernel side callbacks and shared queue side effects not reconstructed')
        if map_view:
            report['limitations'].append('recorded mapped bytes; shared alias coherence and pre-call map in/out values not audited')
        if unmap:
            report['limitations'].append('recorded unmap results; inert pages retained, mapping lifetime/aliasing not replayed')
        if vulkan_images:
            report['limitations'].append('Vulkan image calls replay recorded outputs; no GPU resource lifecycle or worker execution')
    expected_rng = {}
    p.icd.freeze_hooks()
    for event in capture['rng_calls']:
        expected_rng.setdefault(event['tick'], []).append({
            'caller': hex(event['return_address']), 'bound': event['bound'], 'seed_before': event['seed_before']})
    boundaries=list(capture['frames'][1:count+1])
    if extend_simulation and count>=len(capture['frames']):
        if render_batches or whole_frame or native_render:
            raise ValueError('uncaptured extension supports simulation-only execution')
        report['limitations'].append('extended simulation ticks have no recorded live-frame reference')
        # Only a tick number is expected beyond the capture. No synthetic unit,
        # RNG or path state is supplied; callers can compare snapshot() directly.
        boundaries.extend({'tick':capture['frames'][0]['tick']+i}
                          for i in range(len(capture['frames']),count+1))
    for expected in boundaries:
        first_event = len(p.events)
        first_crt_event = len(crt_events)
        crt_seed_before = p.u32(thread + 0x14)
        # 0x526351 is exactly the recorder's boundary, before tick increment.
        # Stop before the enclosing batch loop/stack epilogue at 0x526515.
        if not whole_frame:
            for register, value in ((UC_X86_REG_ESP, STACK + STACK_SZ - 4096),
                                    (UC_X86_REG_EBP, STACK + STACK_SZ - 2048),
                                    (UC_X86_REG_EBX, mode), (UC_X86_REG_ESI, batch_left), (UC_X86_REG_EDI, batch_size)):
                p.uc.reg_write(register, value)
        try:
            if whole_frame:
                boundary_hits[0] = 0
                p.uc.emu_start(0x526351,0,timeout=30_000_000)
                if boundary_hits[0] != 2:
                    raise RuntimeError("whole-frame replay did not reach next tick boundary")
            else:
                p.uc.emu_start(0x526351, 0x526515, timeout=5_000_000)
                if p.uc.reg_read(UC_X86_REG_EIP) != 0x526515:
                    raise RuntimeError('emulated tick did not reach its boundary (timeout or early stop)')
                if batch_left == 0:
                    # Execute the actual batch epilogue, including effect updates.
                    # It pops the enclosing function's saved EDI/ESI; their caller
                    # values are irrelevant to the remaining calls before 5265bf.
                    stack = STACK + STACK_SZ - 4096
                    p.put(stack, bytes(16))
                    p.uc.reg_write(UC_X86_REG_ESP, stack)
                    p.uc.emu_start(0x526521, 0x5265bf, timeout=30_000_000)
                    if p.uc.reg_read(UC_X86_REG_EIP) != 0x5265bf:
                        raise RuntimeError('batch epilogue did not reach its boundary')
                    if render_batches:
                        # Real caller-side camera/visibility setup precedes the
                        # render call. Calling 4fbb90 alone leaves stale culling.
                        p.uc.reg_write(UC_X86_REG_ESP,STACK+STACK_SZ-4096)
                        p.uc.emu_start(0x526f3c,0x526f66,timeout=30_000_000)
                        if p.uc.reg_read(UC_X86_REG_EIP) != 0x526f66:
                            raise RuntimeError('render frame did not reach its boundary')
                    batch_size = p.u32(p.game + 0x19f38)
                    if not 1 <= batch_size <= 120: raise RuntimeError('invalid subsequent batch size')
                    batch_left = batch_size - 1
                else:
                    batch_left -= 1
        except (UcError, RuntimeError) as error:
            report['first_mismatch'] = {'tick': expected['tick'], 'error': str(error), 'missing': p.missing,
                                        'instruction': hex(p.uc.reg_read(UC_X86_REG_EIP))}
            if native_render: report['first_mismatch']['native_history']=list(native_history)
            if crt_mismatch: report['first_mismatch']['crt_rng']=crt_mismatch
            break
        observed = snapshot(p)
        if observed is None or observed['unreadable_units']:
            raise RuntimeError('incomplete emulated frame')
        differences = []
        if observed['tick'] != expected['tick']:
            differences.append({'field': 'tick', 'retail': expected['tick'], 'emulated': observed['tick']})
        rng = p.events[first_event:]
        units = {u['id']: u for u in observed['units']}
        if 'units' in expected:
            want_rng = expected_rng.get(expected['tick'], [])
            if rng != want_rng:
                first = next((i for i, pair in enumerate(zip(rng, want_rng)) if pair[0] != pair[1]), min(len(rng), len(want_rng)))
                differences.append({'field': 'rng', 'index': first, 'retail': want_rng[first:first+1],
                                    'emulated': rng[first:first+1], 'counts': [len(want_rng), len(rng)]})
            if capture.get('crt_calls'):
                want_crt = [event for event in capture['crt_calls'] if event['tick'] == expected['tick']]
                actual_crt = crt_events[first_crt_event:]
                if want_crt != actual_crt:
                    first = next((i for i, pair in enumerate(zip(actual_crt, want_crt)) if pair[0] != pair[1]),
                                 min(len(actual_crt), len(want_crt)))
                    differences.append({'field': 'crt_rng', 'index': first,
                                        'retail': want_crt[first:first+1], 'emulated': actual_crt[first:first+1],
                                        'counts': [len(want_crt), len(actual_crt)]})
            units = {u['id']: u for u in observed['units']}
            if set(units) != {u['id'] for u in expected['units']}:
                wanted_ids = {u['id'] for u in expected['units']}
                differences.append({'field': 'unit_ids', 'retail_only': sorted(wanted_ids - set(units)),
                                    'emulated_only': sorted(set(units) - wanted_ids)})
            for unit in expected['units']:
                actual = units.get(unit['id'], {})
                fields_to_compare = ['position_raw', 'heading', 'base_speed_raw', 'cell_origin']
                # Ground offsets are comparable only for recognized ground movers.
                # A newly constructed mover can retain allocator bytes at +2c;
                # keep that field outside this semantic comparison for new units.
                if 'route_world' in unit:
                    fields_to_compare += ['speed_raw', 'movement_flags', 'refusal_deadline']
                    if unit['id'] in initial_ids:
                        fields_to_compare.append('movement_tick')
                for field in fields_to_compare:
                    want, got = unit.get(field), actual.get(field)
                    if field == 'movement_flags' and unit['id'] not in initial_ids and got is not None:
                        # Constructor 4dc6f6 preserves bits e000 from allocation;
                        # compare the initialized lower thirteen bits for new units.
                        want, got = want & 0x1fff, got & 0x1fff
                    if want != got:
                        differences.append({'id': unit['id'], 'field': field,
                                            'retail': want, 'emulated': got})
            # Existing entity addresses are stable in this fixture. Search object
            # and heap allocation addresses are deliberately not compared.
            fields = bytes.fromhex(expected['path_state']['fields_hex'])
            for offset in (0x58, 0x5c, 0x60, 0x14, 0x18, 0x48, 0x54, 0x165, 0x191, 0x1ad):
                want = struct.unpack_from('<I', fields, offset)[0]
                got = p.u32(p.obj + offset)
                if got != want:
                    differences.append({'field': f'search+{offset:x}', 'retail': want, 'emulated': got})
        report['ticks'].append({'tick': expected['tick'], 'units': len(units), 'rng_calls': len(rng),
                                'crt_calls': len(crt_events) - first_crt_event,
                                'crt_seed_before': crt_seed_before, 'crt_seed_after': p.u32(thread + 0x14),
                                'differences': differences})
        if differences:
            if report['first_mismatch'] is None:report['first_mismatch'] = report['ticks'][-1]
            # Controlled initial-state variants can compare both executions
            # directly through snapshot(), retaining live-trace differences as
            # diagnostics. Unsupported execution still stops immediately.
            if stop_on_difference:break
    if clock:
        report['clock_inputs_consumed'] = clock.index
        report['clock_inputs_recorded'] = len(clock.events)
    if cursor:
        report['cursor_inputs_consumed'] = cursor.index
        report['cursor_inputs_recorded'] = len(cursor.events)
    if monitor:
        report['monitor_inputs_consumed'] = monitor.index
        report['monitor_inputs_recorded'] = len(monitor.events)
    if display:
        report['display_inputs_consumed'] = display.index
        report['display_inputs_recorded'] = len(display.events)
    if formats:
        report['format_inputs_consumed'] = formats.index
        report['format_inputs_recorded'] = len(formats.events)
    if window_loop:
        report['window_loop_inputs_consumed'] = window_loop.index
        report['window_loop_inputs_recorded'] = len(window_loop.events)
    if window:
        report['window_inputs_consumed'] = window.index
        report['window_inputs_recorded'] = len(window.events)
    if map_view:
        report['map_view_inputs_consumed'] = map_view.index
        report['map_view_inputs_recorded'] = len(map_view.events)
    if unmap:
        report['unmap_inputs_consumed'] = unmap.index
        report['unmap_inputs_recorded'] = len(unmap.events)
    if vulkan_images:
        report['vulkan_image_inputs_consumed'] = vulkan_images.index
        report['vulkan_image_inputs_recorded'] = len(vulkan_images.events)
    if callbacks:
        report['dispatch_calls_completed']=callbacks.index
        report['callbacks_completed']=callbacks.callback_count
        report['callback_native_entries_checked']=callbacks.child_count
        report['limitations'].append('dispatch callback replay restricted to ID 4, captured stack and leaf native children; kernel queue state not reconstructed')
    if fixed_global_memory is not None:
        report['fixed_global_allocations']=list(fixed_global_memory.allocations.values())
        if fixed_global_memory.allocations:
            report['limitations'].append('fixed GlobalAlloc/GlobalLock use the bounded offline allocator')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--ticks', type=int, default=60)
    parser.add_argument('--crt-seed', type=int, help='override captured CRT seed for sensitivity testing only')
    parser.add_argument('--whole-frame',action='store_true',help='continue the captured stack through the actual outer frame loop to the next tick')
    parser.add_argument('--render-batches', action='store_true', help='attempt the retail renderer between batches; unsupported native calls stop replay')
    parser.add_argument('--recorded-clocks',action='store_true')
    parser.add_argument('--nt-alert-thread',type=lambda s:int(s,0))
    parser.add_argument('--nt-async-key-state',type=lambda s:int(s,0))
    parser.add_argument('--rtl-allocate-heap',type=lambda s:int(s,0))
    parser.add_argument('--rtl-free-heap',type=lambda s:int(s,0))
    parser.add_argument('--vk-destroy-image',type=lambda s:int(s,0))
    parser.add_argument('--recorded-cursor',type=lambda s:int(s,0),help='identified NtUserGetCursorPos entry; replay captured POINT outputs')
    parser.add_argument('--recorded-monitor',type=lambda s:int(s,0),help='identified NtUserCallTwoParam entry; replay recorded monitor queries')
    parser.add_argument('--recorded-display-settings',type=lambda s:int(s,0),help='identified NtUserEnumDisplaySettings entry')
    parser.add_argument('--recorded-format-properties',type=lambda s:int(s,0),help='identified vkGetPhysicalDeviceFormatProperties2 entry')
    parser.add_argument('--recorded-window-loop',type=lambda s:int(s,0),help='verified win32u base; replay recorded scalar and message inputs')
    parser.add_argument('--recorded-window',nargs=2,type=lambda s:int(s,0),metavar=('DPI_ENTRY','RECT_ENTRY'),help='NtUserGetProcessDpiAwarenessContext and NtUserCallHwndParam entries')
    parser.add_argument('--recorded-map-view',type=lambda s:int(s,0),help='NtMapViewOfSection entry; replay captured returned view bytes')
    parser.add_argument('--recorded-unmap',type=lambda s:int(s,0),help='diagnostic NtUnmapViewOfSection results; retains inert offline pages')
    parser.add_argument('--recorded-vulkan-images',type=lambda s:int(s,0),help='captured winevulkan PE base; replay recorded image API results without GPU execution')
    parser.add_argument('--recorded-callbacks',type=lambda s:int(s,0),help='NtUserDispatchMessage entry; execute validated callbacks from the captured stack')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.whole_frame and (not args.recorded_clocks or args.render_batches):
        parser.error('--whole-frame requires --recorded-clocks and replaces --render-batches')
    if args.recorded_callbacks and not args.whole_frame:
        parser.error('--recorded-callbacks requires --whole-frame')
    if args.recorded_display_settings and not args.recorded_monitor:
        parser.error('--recorded-display-settings requires --recorded-monitor')
    if args.recorded_vulkan_images and args.vk_destroy_image:
        parser.error('recorded Vulkan image calls replace the diagnostic destroy-image skip')
    capture = json.loads(args.capture.read_text())
    if not 1 <= args.ticks < len(capture['frames']): parser.error('ticks exceed captured sequence')
    if args.crt_seed is not None and not 0 <= args.crt_seed <= 0xffffffff: parser.error('CRT seed must be uint32')
    native = {'alert':args.nt_alert_thread,'keys':args.nt_async_key_state,
              'allocate':args.rtl_allocate_heap,'free':args.rtl_free_heap,
              'destroy_image':args.vk_destroy_image,'cursor':args.recorded_cursor,
              'monitor':args.recorded_monitor,'display':args.recorded_display_settings,
              'formats':args.recorded_format_properties,'unmap':args.recorded_unmap,'map_view':args.recorded_map_view,'window':args.recorded_window,'window_loop':args.recorded_window_loop,
              'vulkan_images':args.recorded_vulkan_images,'callbacks':args.recorded_callbacks} if args.recorded_clocks else None
    if not native and any((args.nt_alert_thread,args.nt_async_key_state,args.rtl_allocate_heap,args.rtl_free_heap,args.vk_destroy_image,args.recorded_cursor,args.recorded_monitor,args.recorded_display_settings,args.recorded_format_properties,args.recorded_unmap,args.recorded_vulkan_images,args.recorded_map_view,args.recorded_window,args.recorded_window_loop)):
        parser.error('native substitutions require --recorded-clocks')
    result = replay(capture, args.ticks, args.crt_seed, args.render_batches,native,args.whole_frame)
    with args.output.open('x') as output:
        json.dump(result, output, indent=2); output.write('\n')
    print(json.dumps({'ticks_checked': len(result['ticks']), 'first_mismatch': result['first_mismatch']}))


if __name__ == '__main__':
    main()
