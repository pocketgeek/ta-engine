#!/usr/bin/env python3
"""Dependency diagnostic for a single captured entity's retail renderer.

Not a render-order or parity check. Never fabricates RNG calls. Native heap
allocation is a bounded substitute, selected by an explicitly supplied address
whose live module export must first have been identified as RtlAllocateHeap.
"""
import argparse
import json
import struct
from pathlib import Path
from emureload import CapturedProcess
from check_captured_tick import find_crt_thread
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EIP, UC_X86_REG_ESP


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture',type=Path)
    parser.add_argument('--unit',type=int,default=17)
    parser.add_argument('--scope',choices=('unit','scene','frame'),default='unit',
                        help='execute one entity, the scene traversal, or the enclosing frame')
    parser.add_argument('--rtl-allocate-heap',type=lambda s:int(s,0))
    parser.add_argument('--nt-alert-thread',type=lambda s:int(s,0),
                        help='diagnostic-only success substitute for identified NtAlertThreadByThreadId')
    parser.add_argument('--nt-async-key-state',type=lambda s:int(s,0),
                        help='diagnostic-only released-key substitute for identified NtUserGetAsyncKeyState')
    parser.add_argument('--debug-set-mute',type=lambda s:int(s,0),
                        help='live-verified D3D9 DebugSetMute consisting of a single RET')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--readonly-module',type=Path,
                        help='diagnostic-only supplementary read-only module pages')
    args=parser.parse_args()
    capture=json.loads(args.capture.read_text())
    p=CapturedProcess(capture)
    if args.readonly_module:
        module=json.loads(args.readonly_module.read_text())
        for record in module['records']:
            if 'w' in record['permissions']:
                raise ValueError('supplemental module must contain read-only pages only')
            p.load_record(record)
    unit=next(u for u in p.frame['units'] if u['id']==args.unit)
    for slot in (0x5eb108,0x5eb110):
        function=p.u32(slot); p.ensure(function,1)
        p.icd.hooks[function]=lambda uc,args:(1,0)
    thread=find_crt_thread(p,capture['rng_calls'][0]['registers']['ebp'])
    p.icd.hooks[0x5dc403]=lambda uc,args:(0,thread)
    allocations=[]; crt=[]; thread_alerts=[]
    if args.nt_alert_thread:
        def alert(uc,argv):
            thread_alerts.append(p.u32(argv))
            return 1,0
        p.icd.hooks[args.nt_alert_thread]=alert
    if args.nt_async_key_state:
        p.icd.hooks[args.nt_async_key_state]=lambda uc,argv:(1,0)
    if args.debug_set_mute:
        # Live Proton/DXVK export 797ce770 was a bare C3 (RET). Execute that
        # instruction rather than assuming a return value or argument cleanup.
        p.put(args.debug_set_mute,b'\xc3')
    if args.rtl_allocate_heap:
        p.ensure(args.rtl_allocate_heap,1)
        def allocate(uc,argv):
            heap,flags,size=struct.unpack('<III',uc.mem_read(argv,12))
            _,address=p.allocate(uc,argv+8)
            allocations.append({'heap':heap,'flags':flags,'size':size,'result':address})
            return 3,address
        p.icd.hooks[args.rtl_allocate_heap]=allocate
    def observe(uc,address,size,data):
        crt.append({'return_address':p.u32(uc.reg_read(UC_X86_REG_ESP)),
                    'seed_before':p.u32(thread+0x14)})
    p.uc.hook_add(UC_HOOK_CODE,observe,begin=0x5d4444,end=0x5d4444)
    entry={'unit':0x4ee700,'scene':0x4fc5b0,'frame':0x4fbb90}[args.scope]
    _,error=p.icd.call(entry,(unit['entity_address'] if args.scope=='unit' else 1,))
    eip=p.uc.reg_read(UC_X86_REG_EIP)
    report={'diagnostic_only':True,'unit':args.unit,'scope':args.scope,'returned':eip==0x6ffff000,
            'error':error,'eip':hex(eip),'crt_calls':crt,'allocations':allocations,
            'thread_alerts':thread_alerts,
            'missing':p.missing,'limitations':['single-thread critical section substitution',
            'bounded native heap substitute' if args.rtl_allocate_heap else 'native heap unavailable',
            'thread wakeup reports success without running the worker' if args.nt_alert_thread else 'OS thread wakeup not substituted',
            'keyboard state assumed released' if args.nt_async_key_state else 'keyboard state not substituted',
            'live-verified DebugSetMute RET' if args.debug_set_mute else 'DebugSetMute not substituted',
            ('shared device bytes present; GPU execution and concurrent writes not reproduced' if p.frame.get('device_memory') else
             'synchronized native images present; shared device mappings absent' if p.frame.get('native_memory') else
             'later read-only native module pages; no native runtime snapshot' if args.readonly_module else 'native module absent'),
            'no verified render cadence; entry scope does not establish whole-frame parity']}
    args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))

if __name__=='__main__': main()
