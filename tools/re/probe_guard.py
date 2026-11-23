"""Persist probe originals and restore them with a separate debugger after failure.

Restoration validates process start time and every complete instruction prefix
before writing any byte. This module never selects a target process itself.
"""
import json
import os
from pathlib import Path
import subprocess


def hardware_debug_state(tid):
    """Read Linux x86 debug registers; caller must already be this TID's tracer."""
    import ctypes
    import platform
    if platform.system()!='Linux' or platform.machine()!='x86_64' or ctypes.sizeof(ctypes.c_void_p)!=8:
        raise RuntimeError('debug-register recovery requires Linux x86_64 tracer ABI')
    libc=ctypes.CDLL(None,use_errno=True); libc.ptrace.restype=ctypes.c_long
    values={}
    # Verified offsetof(struct user, u_debugreg)=848, sizeof(element)=8 on
    # the tracer ABI, including when the tracee is a compat i386 process.
    for index in (0,1,2,3,6,7):
        ctypes.set_errno(0)
        value=libc.ptrace(3,tid,ctypes.c_void_p(848+index*8),ctypes.c_void_p())
        if value==-1 and ctypes.get_errno(): raise OSError(ctypes.get_errno(),'read debug register')
        values[str(index)]=value&0xffffffffffffffff
    return values


def restore_hardware_debug_state(tid,values):
    import ctypes
    hardware_debug_state(tid)  # Validate ABI and tracer access before writes.
    libc=ctypes.CDLL(None,use_errno=True); libc.ptrace.restype=ctypes.c_long
    for index,value in [(7,0)]+[(i,values[str(i)]) for i in (0,1,2,3,6,7)]:
        if libc.ptrace(6,tid,ctypes.c_void_p(848+index*8),ctypes.c_void_p(value))==-1:
            raise OSError(ctypes.get_errno(),'restore debug register')
    actual=hardware_debug_state(tid)
    if any(actual[str(i)]!=values[str(i)] for i in (0,1,2,3,7)):
        raise RuntimeError('hardware debug-register restoration differs')


def process_start_time(pid):
    text=Path(f'/proc/{pid}/stat').read_text()
    return text[text.rfind(')')+2:].split()[19]


def validate_manifest(record):
    if (type(record.get('pid')) is not int or record['pid'] <= 0
            or not isinstance(record.get('start_time'),str)
            or not record['start_time'].isdigit()
            or type(record.get('resume')) is not bool
            or record.get('state') not in ('armed','restored')):
        raise ValueError('invalid probe manifest identity or state')
    probes=record.get('probes')
    if not isinstance(probes,list) or not 1 <= len(probes) <= 4:
        raise ValueError('invalid probe manifest count')
    sites=[]
    for probe in probes:
        address=probe['address']; raw=bytes.fromhex(probe['bytes'])
        if (type(address) is not int or not 0 < address <= 0xffffffff-15
                or len(raw)!=16 or raw[0]==0xcc
                or any(abs(address-other)<16 for other in sites)):
            raise ValueError('invalid or overlapping probe prefix')
        sites.append(address)
    hardware=record.get('hardware')
    if not isinstance(hardware,dict) or not hardware:
        raise ValueError('missing initial hardware register snapshot')
    for tid,registers in hardware.items():
        if (not isinstance(tid,str) or not tid.isdigit() or int(tid)<=0
                or not isinstance(registers,dict) or set(registers)!={'0','1','2','3','6','7'}
                or any(type(v) is not int or not 0<=v<=0xffffffffffffffff for v in registers.values())):
            raise ValueError('invalid initial hardware register snapshot')
    return record


def save_manifest(path,pid,probes,resume=True,hardware=None):
    if hardware is None: raise ValueError('pre-probe hardware register snapshot required')
    record={'pid':pid,'start_time':process_start_time(pid),'resume':resume,
            'state':'armed','probes':probes,'hardware':hardware}
    validate_manifest(record)
    with Path(path).open('x') as output:
        json.dump(record,output); output.flush(); os.fsync(output.fileno())
    return record


def mark_restored(path):
    path=Path(path); record=json.loads(path.read_text()); record['state']='restored'
    temporary=path.with_name(path.name+'.restored')
    with temporary.open('x') as output:
        json.dump(record,output); output.flush(); os.fsync(output.fileno())
    os.replace(temporary,path)


RECOVERY = r'''
import gdb,json,os,sys
from pathlib import Path
sys.path.insert(0,os.environ['PROBE_GUARD_TOOLS'])
from probe_guard import validate_manifest,hardware_debug_state,restore_hardware_debug_state
m=validate_manifest(json.loads(Path(os.environ['PROBE_MANIFEST']).read_text()))
pid=int(os.environ['PROBE_PID'])
if m['pid']!=pid: raise RuntimeError('target PID differs')
def identity():
    text=Path('/proc/'+str(pid)+'/stat').read_text()
    return text[text.rfind(')')+2:].split()[19]
if identity()!=m['start_time']: raise RuntimeError('target identity changed')
gdb.execute('attach '+str(pid))
inf=gdb.selected_inferior()
if identity()!=m['start_time']: raise RuntimeError('target identity changed during attach')
selected=gdb.selected_thread(); thread_states=[]
for t in inf.threads():
    t.switch()
    try: signal_info=str(gdb.parse_and_eval('$_siginfo'))
    except gdb.error: signal_info=None
    thread_states.append({'thread':t.global_num,'ptid':t.ptid,
        'pc':int(gdb.parse_and_eval('$pc')),'eflags':int(gdb.parse_and_eval('$eflags')),
        'signal':signal_info,'debug':hardware_debug_state(t.ptid[1])})
selected.switch()
print('RECOVERY_THREADS '+json.dumps(thread_states))
for p in m['probes']:
    raw=bytes.fromhex(p['bytes']); got=bytes(inf.read_memory(p['address'],len(raw)))
    if got not in (raw,b'\xcc'+raw[1:]): raise RuntimeError('probe bytes differ; refusing repair')
for p in m['probes']:
    raw=bytes.fromhex(p['bytes'])
    inf.write_memory(p['address'],raw[:1])
    if bytes(inf.read_memory(p['address'],len(raw)))!=raw: raise RuntimeError('probe restoration failed')
for t in inf.threads():
    original=m['hardware'].get(str(t.ptid[1]))
    if original is None:
        # Threads born under the recorder inherit debugger-owned hardware
        # slots. A clean thread has no enabled application debug registers.
        original={str(i):0 for i in (0,1,2,3,6,7)}
    restore_hardware_debug_state(t.ptid[1],original)
gdb.execute('queue-signal 0')
print('RECOVERY_RESULT '+json.dumps({'restored':len(m['probes']),'pc':int(gdb.parse_and_eval('$pc'))}))
gdb.execute('detach')
'''


def recover_manifest(path,log_path):
    path=Path(path); manifest=validate_manifest(json.loads(path.read_text()))
    if manifest.get('state')=='restored': return {'needed':False}
    pid=manifest['pid']
    if process_start_time(pid)!=manifest['start_time']:
        raise RuntimeError('probe recovery target identity changed')
    env=dict(os.environ,PROBE_PID=str(pid),PROBE_MANIFEST=str(path.resolve()),
             PROBE_GUARD_TOOLS=str(Path(__file__).resolve().parent))
    with Path(log_path).open('x') as output:
        result=subprocess.run(['gdb','-nx','-q','-batch','-ex','set debuginfod enabled off',
            '-ex','set confirm off','-ex','python exec('+repr(RECOVERY)+')'],
            env=env,stdout=output,stderr=subprocess.STDOUT,timeout=30)
    if result.returncode:
        raise RuntimeError(f'probe recovery failed; target not resumed; see {log_path}')
    mark_restored(path)
    if manifest.get('resume',True) and process_start_time(pid)==manifest['start_time']:
        import signal
        os.kill(pid,signal.SIGCONT)
    return {'needed':True,'restored':len(manifest['probes']),'log':str(log_path)}
