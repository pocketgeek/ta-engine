#!/usr/bin/env python3
"""Fault-inject debugger cleanup on a disposable, independently launched process.

Never accepts a target PID. The only attach targets are children created here.
"""
import argparse
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import tempfile
import time

from check_native_return_probe import ASSEMBLY, GDB_SCRIPT


START = r'''
    mov $172, %eax
    mov $0x59616d61, %ebx
    mov $-1, %ecx
    xor %edx, %edx
    xor %esi, %esi
    xor %edi, %edi
    int $0x80
    mov $20, %eax
    int $0x80
    mov %eax, %ebx
    mov $37, %eax
    mov $19, %ecx
    int $0x80
'''

from probe_guard import RECOVERY, recover_manifest



def driver(fault):
    s=GDB_SCRIPT.replace("gdb.execute('starti')", "gdb.execute('attach '+os.environ['PROBE_PID'])\ngdb.execute('handle SIGSTOP nostop noprint nopass')")
    s=s.replace('inf=gdb.selected_inferior()', '''inf=gdb.selected_inferior()
import sys
sys.path.insert(0,os.environ['PROBE_TOOLS'])
from probe_guard import hardware_debug_state
initial_hardware={str(t.ptid[1]):hardware_debug_state(t.ptid[1]) for t in inf.threads()}
''')
    s=s.replace("start=EntryProbe('*'+str(entry),internal=True)",r'''
from pathlib import Path
import sys
sys.path.insert(0,os.environ['PROBE_TOOLS'])
from probe_guard import save_manifest
manifest=save_manifest(os.environ['PROBE_MANIFEST'],inf.pid,
    [{'address':entry,'bytes':bytes(inf.read_memory(entry,16)).hex()}],hardware=initial_hardware)
gdb.execute('set breakpoint always-inserted on')
start=EntryProbe('*'+str(entry),internal=True)
''')
    begin=s.index('while not exited:')
    end=s.index("result={'mode':",begin)
    loop=s[begin:end]
    loop += "    if entries==17 and not exited:\n"
    loop += "        fd=os.open('/proc/'+str(inf.pid)+'/mem',os.O_RDONLY)\n"
    loop += "        try: actual=os.pread(fd,1,entry).hex()\n        finally: os.close(fd)\n"
    loop += "        print('FAULT_STATE '+json.dumps({'entry_byte':actual,'entries':entries}),flush=True)\n"
    loop += ("        raise RuntimeError('injected observer failure')\n" if fault=='exception'
             else "        os.kill(inf.pid,19)\n        os.kill(os.getpid(),9)\n" if fault=='kill'
             else "        pass\n")
    s=s[:begin]+"try:\n"+''.join('    '+l+'\n' for l in loop.rstrip().splitlines())+"""finally:
    if inf.is_valid() and inf.pid:
        for bp in list(gdb.breakpoints() or []): bp.delete()
        for p in manifest['probes']:
            if bytes(inf.read_memory(p['address'],16)).hex()!=p['bytes']:
                raise RuntimeError('normal cleanup failed to restore probe')
        print('CLEANUP_RESTORED')
        gdb.execute('queue-signal 0')
        gdb.execute('detach')
"""+s[end:]
    return s


def run_gdb(script,env,log):
    with log.open('x') as output:
        p=subprocess.Popen(['gdb','-nx','-q','-batch','-ex','set debuginfod enabled off',
            '-ex','set confirm off','-ex',f'python exec(open({str(script)!r}).read())'],
            stdout=output,stderr=subprocess.STDOUT,env=env,start_new_session=True)
        try: return p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid,signal.SIGKILL); p.wait()
            raise RuntimeError('disposable debugger timed out')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fault',choices=('none','exception','kill'),required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if args.output.exists(): parser.error('output must be new')
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    report={'fault':args.fault,'passed':False}
    with tempfile.TemporaryDirectory(prefix='tak-cleanup-test-') as temp:
        root=Path(temp)
        (root/'fixture.s').write_text(ASSEMBLY.replace('_start:\n','_start:\n'+START))
        subprocess.run(['as','--32','-o',str(root/'fixture.o'),str(root/'fixture.s')],check=True)
        subprocess.run(['ld','-m','elf_i386','-o',str(root/'fixture'),str(root/'fixture.o')],check=True)
        (root/'driver.py').write_text(driver(args.fault))
        (root/'recovery.py').write_text(RECOVERY)
        target=subprocess.Popen([str(root/'fixture')])
        try:
            deadline=time.monotonic()+5
            while time.monotonic()<deadline:
                if '\nState:\tT' in Path(f'/proc/{target.pid}/status').read_text(): break
                time.sleep(.01)
            else: raise RuntimeError('fixture did not stop itself')
            env=dict(os.environ,PROBE_MODE='deferred',PROBE_PID=str(target.pid),PROBE_MANIFEST=str(root/'manifest.json'),PROBE_TOOLS=str(Path(__file__).resolve().parent))
            report['debugger_exit']=run_gdb(root/'driver.py',env,root/'debugger.log')
            report['debugger_log']=(root/'debugger.log').read_text()
            if args.fault=='kill':
                report['recovery']=recover_manifest(root/'manifest.json',root/'recovery.log')
                report['recovery_exit']=0
                report['recovery_log']=(root/'recovery.log').read_text()
            if args.fault!='none':
                report['fault_state']=[json.loads(l[len('FAULT_STATE '):]) for l in report['debugger_log'].splitlines() if l.startswith('FAULT_STATE ')]
            if target.poll() is None: os.kill(target.pid,signal.SIGCONT)
            report['fixture_exit']=target.wait(timeout=5)
            expected={'none':0,'exception':1,'kill':-signal.SIGKILL}[args.fault]
            report['passed']=(report['debugger_exit']==expected and report['fixture_exit']==42
                              and report.get('recovery_exit',0)==0)
            if args.fault!='none': report['passed'] &= report['fault_state']==[{'entry_byte':'cc','entries':17}]
        finally:
            if target.poll() is None:
                target.kill(); target.wait()
            args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if not k.endswith('_log')}))
    if not report['passed']: raise SystemExit(1)


if __name__=='__main__': main()
