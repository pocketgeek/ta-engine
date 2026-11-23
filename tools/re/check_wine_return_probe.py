#!/usr/bin/env python3
"""Test deferred probes in a new Wine prefix; never attaches to retail."""
import argparse
import json
import os
from pathlib import Path
import resource
import signal
import subprocess
import tempfile
import time
from check_native_return_probe import GDB_SCRIPT
from check_probe_cleanup import driver as cleanup_driver, RECOVERY
from probe_guard import recover_manifest

ASSEMBLY=r'''
.data
.global ready
ready: .long 0
depth: .long 0
callbacks: .long 0
counter: .quad 0
worker_counter: .quad 0
worker_done: .long 0
worker_handle: .long 0
.text
.global _start, dispatcher, reserved1, reserved2, reserved3, completed
_start:
    cmpl $0, ready
    jne begin
    push $10
    call _Sleep@4
    jmp _start
begin:
    push $0
    push $0
    push $0
    push $worker
    push $0
    push $0
    call _CreateThread@24
    mov %eax, worker_handle
    mov $100, %edi
again:
    call reserved1
    call outer
    push $0
    push $locale_callback
    call _EnumSystemLocalesA@8
    call reserved2
    call reserved3
    dec %edi
    jnz again
    cmpl $100, callbacks
    jne fail
    movl $1, worker_done
    push $10000
    push worker_handle
    call _WaitForSingleObject@8
    test %eax,%eax
    jne fail
completed:
    push $42
    call _ExitProcess@4
fail:
    push $43
    call _ExitProcess@4
worker:
    push $worker_counter
    call _QueryPerformanceCounter@4
    push $1
    call _Sleep@4
    cmpl $0, worker_done
    je worker
    xor %eax,%eax
    ret $4
locale_callback:
    incl callbacks
    call outer
    xor %eax, %eax
    ret $4
outer:
    call dispatcher
    nop
    ret
inner:
    call dispatcher
    nop
    ret
dispatcher:
    push $counter
    call _QueryPerformanceCounter@4
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

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wine',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--fault',choices=('none','exception','kill'),default='none')
    parser.add_argument('--native-dispatcher',action='store_true',help='probe Wine actual syscall dispatcher instead of fixture dispatcher')
    parser.add_argument('--mode',choices=('deferred','loop'),default='deferred')
    args=parser.parse_args()
    if args.output.exists(): parser.error('output must be new')
    if args.native_dispatcher and args.fault!='none': parser.error('native dispatcher requires --fault none')
    wine=args.wine.resolve(); server=wine.parent/'wineserver'
    if not server.is_file(): parser.error('matching wineserver must be beside wine')
    resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    report={'passed':False,'wine':str(wine),'fault':args.fault}
    with tempfile.TemporaryDirectory(prefix='tak-wine-probe-') as temp:
        root=Path(temp); fixture=root/'tak_probe_fixture.exe'
        (root/'fixture.s').write_text(ASSEMBLY)
        subprocess.run(['x86_64-w64-mingw32-as','--32','-o',str(root/'fixture.o'),str(root/'fixture.s')],check=True)
        subprocess.run(['x86_64-w64-mingw32-ld','-mi386pe','--image-base','0x400000',
            '--disable-reloc-section','--entry','_start','--subsystem','console','-o',str(fixture),str(root/'fixture.o'),
            '-L/usr/i686-w64-mingw32/sys-root/mingw/lib','-lkernel32'],check=True)
        symbols={}
        for line in subprocess.check_output(['x86_64-w64-mingw32-nm','-n',str(fixture)],text=True).splitlines():
            parts=line.split()
            if len(parts)==3 and parts[2] in ('dispatcher','reserved1','reserved2','reserved3','ready','completed'):
                symbols[parts[2]]=int(parts[0],16)
        if len(symbols)!=6: raise RuntimeError('fixture symbols missing')
        env={k:v for k,v in os.environ.items() if k not in ('WINEPREFIX','WINEARCH','WINESERVER','WINELOADER')}
        env.update(WINEPREFIX=str(root/'prefix'),WINEDEBUG='-all,+seh' if args.fault=='kill' else '-all',WINESERVER=str(server),WINELOADER=str(wine),
                   WINEDLLOVERRIDES='winemenubuilder.exe=d',DISPLAY='',WAYLAND_DISPLAY='',PROBE_MODE=args.mode)
        process=None; debugger=None
        try:
            with (root/'wine.log').open('w') as wine_log:
                process=subprocess.Popen([str(wine),str(fixture)],env=env,stdout=wine_log,stderr=subprocess.STDOUT)
                deadline=time.monotonic()+45; pid=None
                while time.monotonic()<deadline:
                    if process.poll() is not None: raise RuntimeError('Wine exited before fixture was ready')
                    for maps in Path('/proc').glob('[0-9]*/maps'):
                        try: text=maps.read_text()
                        except OSError: continue
                        if str(fixture) in text:
                            pid=int(maps.parent.name); break
                    if pid is not None: break
                    time.sleep(.1)
                if pid is None: raise RuntimeError('Wine fixture did not start within 45 seconds')
                env.update(PROBE_PID=str(pid),PROBE_MANIFEST=str(root/'manifest.json'),PROBE_TOOLS=str(Path(__file__).resolve().parent))
                if args.fault=='none':
                    driver=GDB_SCRIPT.replace("gdb.execute('starti')",f"gdb.execute('attach {pid}')\ngdb.execute('set {{int}}{symbols['ready']} = 1')")
                else:
                    driver=cleanup_driver(args.fault).replace("gdb.execute('attach '+os.environ['PROBE_PID'])",
                        f"gdb.execute('attach {pid}')\ngdb.execute('set {{int}}{symbols['ready']} = 1')")
                driver=driver.replace("entry=int(gdb.parse_and_eval('&dispatcher'))",f"entry={symbols['dispatcher']}")
                driver=driver.replace("int(gdb.parse_and_eval('&'+name))",f"{symbols!r}[name]")
                driver=driver.replace('entries==returns==400','entries==returns==800')
                if args.native_dispatcher:
                    mappings=Path(f'/proc/{pid}/maps').read_text().splitlines()
                    base=next(int(l.split('-')[0],16) for l in mappings
                              if '/i386-unix/ntdll.so' in l and l.split()[2]=='00000000')
                    driver=driver.replace(f"entry={symbols['dispatcher']}",f"entry={base+0x532e4}")
                    driver=driver.replace("inf=gdb.selected_inferior()", "inf=gdb.selected_inferior()\nmain_thread=gdb.selected_thread().global_num")
                    driver=driver.replace("start=EntryProbe('*'+str(entry),internal=True)", "start=EntryProbe('*'+str(entry),internal=True)\nstart.thread=main_thread")
                    driver=driver.replace("probe=ReturnProbe('*'+str(address),type=gdb.BP_HARDWARE_BREAKPOINT,internal=True)", "probe=ReturnProbe('*'+str(address),type=gdb.BP_HARDWARE_BREAKPOINT,internal=True)\n        probe.thread=main_thread")
                    driver=driver.replace('while not exited:',f'''
class CompletedProbe(gdb.Breakpoint):
    def stop(self):
        exited.append(42)
        return True
completed=CompletedProbe('*{symbols['completed']}',internal=True)
prefix=bytes.fromhex('648b0d18020000c701000000008f4108')
if bytes(inf.read_memory(entry,len(prefix)))!=prefix:
    raise RuntimeError('Wine dispatcher prefix differs')
while not exited:''')
                    driver=driver.replace('entries==returns==800 and peak==4','entries==returns and entries>=800 and reserved_hits==300')
                    driver=driver.replace("print('PROBE_RESULT '+json.dumps(result))", """
for bp in list(gdb.breakpoints() or []): bp.delete()
gdb.execute('detach')
print('PROBE_RESULT '+json.dumps(result))
""")
                (root/'driver.py').write_text(driver)
                with (root/'gdb.log').open('w') as log:
                    debugger=subprocess.Popen(['gdb','-nx','-q','-batch','-ex','set debuginfod enabled off',
                        '-ex',f'python exec(open({str(root/"driver.py")!r}).read())'],env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
                    report['debugger_exit']=debugger.wait(timeout=45)
                report['gdb_log']=(root/'gdb.log').read_text()
                results=[json.loads(l[len('PROBE_RESULT '):]) for l in report['gdb_log'].splitlines() if l.startswith('PROBE_RESULT ')]
                report['results']=results
                if args.fault=='kill':
                    report['recovery']=recover_manifest(root/'manifest.json',root/'recovery.log')
                    report['recovery_exit']=0
                    report['recovery_log']=(root/'recovery.log').read_text()
                if args.fault!='none':
                    report['fault_state']=[json.loads(l[len('FAULT_STATE '):]) for l in report['gdb_log'].splitlines() if l.startswith('FAULT_STATE ')]
                if args.fault!='none' and Path(f'/proc/{pid}').exists(): os.kill(pid,signal.SIGCONT)
                report['wine_exit']=process.wait(timeout=10)
                expected={'none':0,'exception':1,'kill':-signal.SIGKILL}[args.fault]
                report['passed']=report['debugger_exit']==expected and report['wine_exit']==42 and report.get('recovery_exit',0)==0
                if args.fault=='none': report['passed'] &= len(results)==1 and results[0]['passed']
                else: report['passed'] &= report['fault_state']==[{'entry_byte':'cc','entries':17}]
        except Exception as error:
            report['error']=str(error)
        finally:
            if debugger is not None and debugger.poll() is None:
                os.killpg(debugger.pid,signal.SIGKILL); debugger.wait()
            # Kill only the server belonging to the fresh temporary prefix.
            subprocess.run([str(server),'-k'],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=10)
            if process is not None and process.poll() is None:
                process.kill(); process.wait()
            report['wine_log']=(root/'wine.log').read_text() if (root/'wine.log').exists() else ''
            if 'gdb_log' not in report and (root/'gdb.log').exists(): report['gdb_log']=(root/'gdb.log').read_text()
            if 'recovery_log' not in report and (root/'recovery.log').exists(): report['recovery_log']=(root/'recovery.log').read_text()
            args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if not k.endswith('_log')}))
    if not report['passed']: raise SystemExit(1)

if __name__=='__main__': main()
