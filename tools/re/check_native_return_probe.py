#!/usr/bin/env python3
"""Exercise nested return probes on a disposable 32-bit program, never retail.

The callback mode reproduces the retired recorder's breakpoint mutations.
The loop mode makes those mutations only after GDB's continue command returns.
"""
import argparse
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import tempfile


ASSEMBLY = r'''
.section .data
depth: .long 0
.section .text
.global _start, dispatcher, reserved1, reserved2, reserved3
_start:
    mov $100, %edi
again:
    call reserved1
    call outer
    call reserved2
    call reserved3
    dec %edi
    jnz again
    mov $1, %eax
    mov $42, %ebx
    int $0x80
outer:
    call dispatcher
    nop
    ret
inner:
    call dispatcher
    nop
    ret
dispatcher:
    incl depth
    cmpl $4, depth
    je finish
    call inner
finish:
    decl depth
    ret
reserved1: nop; ret
reserved2: nop; ret
reserved3: nop; ret
'''


GDB_SCRIPT = r'''
import gdb, json, os, struct
mode=os.environ['PROBE_MODE']
gdb.execute('set pagination off')
gdb.execute('set confirm off')
gdb.execute('set displaced-stepping off')
gdb.execute('starti')
gdb.execute('set scheduler-locking step')
inf=gdb.selected_inferior()
entry=int(gdb.parse_and_eval('&dispatcher'))
reserved_hits=0
class ReservedProbe(gdb.Breakpoint):
    def stop(self):
        global reserved_hits
        if int(gdb.parse_and_eval('$pc'))!=int(self.location[1:]):
            raise RuntimeError('reserved hardware probe reported at wrong PC')
        reserved_hits+=1
        return False
reserved=[ReservedProbe('*'+str(int(gdb.parse_and_eval('&'+name))),
    type=gdb.BP_HARDWARE_BREAKPOINT,internal=True) for name in ('reserved1','reserved2','reserved3')]
stack=[]; entries=0; returns=0; peak=0; probe=None; probe_address=None; exited=[]; pending=False
gdb.events.exited.connect(lambda event:exited.append(event.exit_code) if hasattr(event,'exit_code') else None)
def arm():
    global probe,probe_address
    address=stack[-1][0] if stack else None
    if address==probe_address: return
    if probe is not None: probe.delete()
    probe=None; probe_address=address
    if address is not None:
        probe=ReturnProbe('*'+str(address),type=gdb.BP_HARDWARE_BREAKPOINT,internal=True)
def observe(is_entry,mutate=True):
    global entries,returns,peak,pending
    sp=int(gdb.parse_and_eval('$esp'))
    if is_entry:
        address=struct.unpack('<I',inf.read_memory(sp,4))[0]
        stack.append((address,sp+4)); entries+=1; peak=max(peak,len(stack))
    else:
        if not stack or sp!=stack[-1][1]: raise RuntimeError('return stack mismatch')
        stack.pop(); returns+=1
    if mutate: arm()
    else: pending=True
class EntryProbe(gdb.Breakpoint):
    def stop(self):
        if int(gdb.parse_and_eval('$pc'))!=entry: raise RuntimeError('entry probe reported at wrong PC')
        if mode=='callback': observe(True); return False
        if mode=='deferred': observe(True,False)
        return True
class ReturnProbe(gdb.Breakpoint):
    def stop(self):
        if int(gdb.parse_and_eval('$pc'))!=probe_address: raise RuntimeError('return probe reported at wrong PC')
        if mode=='callback': observe(False); return False
        if mode=='deferred': observe(False,False)
        return True
start=EntryProbe('*'+str(entry),internal=True)
while not exited:
    gdb.execute('continue',to_string=True)
    if exited: break
    if mode=='loop':
        pc=int(gdb.parse_and_eval('$pc'))
        if pc!=entry and pc!=probe_address: raise RuntimeError('unexpected stop')
        observe(pc==entry)
    elif mode=='deferred':
        if not pending: raise RuntimeError('missing deferred action')
        pending=False
        arm()
result={'mode':mode,'entries':entries,'returns':returns,'max_depth':peak,
        'open_calls':len(stack),'exit_codes':exited}
result['passed']=(entries==returns==400 and peak==4 and not stack and exited==[42])
print('PROBE_RESULT '+json.dumps(result))
'''


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode',choices=('loop','deferred','callback'),default='deferred')
    parser.add_argument('--log',type=Path,required=True)
    args=parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    with tempfile.TemporaryDirectory(prefix='tak-probe-test-') as tmp:
        root=Path(tmp)
        (root/'fixture.s').write_text(ASSEMBLY)
        (root/'driver.py').write_text(GDB_SCRIPT)
        subprocess.run(['as','--32','-o',str(root/'fixture.o'),str(root/'fixture.s')],check=True)
        subprocess.run(['ld','-m','elf_i386','-o',str(root/'fixture'),str(root/'fixture.o')],check=True)
        command=['gdb','-nx','-q','-batch','-ex','set debuginfod enabled off',
                 str(root/'fixture'),'-ex',f'python exec(open({str(root/"driver.py")!r}).read())']
        with args.log.open('x') as output:
            process=subprocess.Popen(command,stdout=output,stderr=subprocess.STDOUT,
                env=dict(os.environ,PROBE_MODE=args.mode),start_new_session=True)
            try: code=process.wait(timeout=45)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid,signal.SIGKILL); process.wait()
                raise RuntimeError('isolated probe timed out; disposable process group killed')
        lines=args.log.read_text().splitlines()
        results=[json.loads(line[len('PROBE_RESULT '):]) for line in lines if line.startswith('PROBE_RESULT ')]
        print(json.dumps({'debugger_exit':code,'results':results,'log':str(args.log)}))
        if code or len(results)!=1 or not results[0]['passed']: raise SystemExit(1)


if __name__=='__main__': main()
