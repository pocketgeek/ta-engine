#!/usr/bin/env python3
"""Compare integer COB thread execution with 56c870, using synthetic programs.

Engine queries are controlled callbacks; piece animation is excluded here.
All sixteen thread records, statics, RNG and unit-value writes are compared.
"""
import argparse
import random
import struct
import subprocess
from pathlib import Path
from emu import Icd, HEAP


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',default='build-dbg/retail_script_test')
    parser.add_argument('--cob',type=Path,help='optional local COB; execute its Go factory chain')
    parser.add_argument('--state',type=Path,help='optional saved script blob belonging to --cob')
    args=parser.parse_args()
    p=Icd(); vm,descriptor,code,entries,statics,vtable=[HEAP+n*0x10000 for n in range(6)]
    put=lambda a,v:p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    get=lambda a:struct.unpack('<I',p.uc.mem_read(a,4))[0]
    writes=[]
    def set_value(uc,sp):
        writes.extend(struct.unpack('<2i',uc.mem_read(sp,8)))
        return 2,0
    p.hooks[0x56d850]=lambda uc,sp:(1,0) # piece update, outside this test
    p.hooks[0x56a000]=set_value
    p.hooks[0x56a010]=lambda uc,sp:(5,1 if get(sp)==18 else 100)
    p.hooks[0x56a020]=lambda uc,sp:(2,0)
    p.hooks[0x56a030]=lambda uc,sp:(2,0)
    p.hooks[0x56a040]=lambda uc,sp:(2,get(sp+4)) # controlled sound result
    p.freeze_hooks()
    put(vtable+0x50,0x56a000); put(vtable+0x54,0x56a010)
    for offset in (8,12,16,20,24,28): put(vtable+offset,0x56a020)
    put(vtable+0x2c,0x56a030)
    put(vtable+0x38,0x56a040)
    put(descriptor+0x2c,HEAP+0x70000)
    put(HEAP+0x70000,HEAP+0x70100)
    rng=random.Random(0x56c870)
    fixtures=[]
    push=lambda n:[0x10021001,n&0xffffffff]
    ret=push(0)+[0x10065000]
    ops=[0x10031000,0x10032000,0x10033000,0x10035000,0x10036000,0x10037000,
         0x10039000,0x1003a000,*range(0x10051000,0x10059001,0x1000)]
    for _ in range(300):
        program=[]
        for __ in range(8):
            program+=push(rng.getrandbits(32))+push(rng.getrandbits(32))+[rng.choice(ops),0x10023004,0]
        program+=push(rng.randrange(1,500))+[0x10013000]+ret
        fixtures.append((program,[0],[1]*20))
    for delay in (0,1,12,33,34,494,1634,-1,0x7fffffff):
        parent=[0x10062000,1,0]+push(5)+push(1)+[0x10082000]+ret
        child=push(delay)+[0x10013000]+ret
        fixtures.append((parent+child,[0,len(parent)],[1]*60))
    # START arguments survive CREATE_LOCAL; CALL wakes an earlier slot next tick.
    parent=push(37)+[0x10061000,1,1]+push(0)+[0x10013000]+ret
    child=[0x10022000,0x10021002,0,0x10023004,0]+ret
    fixtures.append((parent+child,[0,len(parent)],[1]*4))
    # PLAY_SOUND replaces its priority with the host return value, including
    # zero and signed values; its inline operand is a name, not a piece index.
    for priority in (0,1,-1,0x7fffffff,-0x80000000):
        program=push(priority)+[0x10072000,0,0x10023004,0]+ret
        fixtures.append((program,[0],[1]*2))
    if args.cob:
        data=args.cob.read_bytes()
        header=struct.unpack_from('<10I',data)
        _,scripts,pieces,words,nv,_,index,names,_,code_offset=header
        program=list(struct.unpack_from(f'<{words}I',data,code_offset))
        starts=list(struct.unpack_from(f'<{scripts}I',data,index))
        offsets=struct.unpack_from(f'<{scripts}I',data,names)
        names=[data[offset:data.index(0,offset)].decode('ascii') for offset in offsets]
        # Slot zero begins at Go, retaining the original script-index table.
        fixtures.append((program,starts,[1]*100,nv,names.index('Go')))
        if args.state:
            fixtures.append((program,starts,[1]*10,nv,None))
    lines=[]; expected=[]
    for fixture in fixtures:
        program,starts,ticks=fixture[:3]
        nv,entry=fixture[3:] if len(fixture)>3 else (1,0)
        p.uc.mem_write(vm,bytes(0xa64)); put(vm,vtable); put(vm+4,30)
        put(vm+0xc,descriptor); put(vm+0x14,statics)
        put(vm+0x18,HEAP+0x60000); p.uc.mem_write(HEAP+0x60000,bytes(0x10000))
        put(descriptor+4,len(starts)); put(descriptor+0x18,entries); put(descriptor+0x24,code)
        p.uc.mem_write(code,struct.pack(f'<{len(program)}I',*program))
        p.uc.mem_write(entries,struct.pack(f'<{len(starts)}I',*starts))
        p.uc.mem_write(statics,bytes(nv*4)); put(0x64186c,1)
        if entry is None:
            from decode_save_state import script_record
            state=script_record(args.state.read_bytes(),nv,pieces)
            for i,thread in enumerate(state['threads']):
                for field,value in thread.items(): put(vm+0x20+i*0xa4+int(field,16),value)
            for i,value in enumerate(state['statics']): put(statics+4*i,value)
            put(vm+0xa60,state['runtime_a60'])
        else:
            result,error=p.call(0x56c540,(entry,),ecx=vm)
            if error or result!=0: raise RuntimeError(error or result)
        words=struct.unpack('<656I',p.uc.mem_read(vm+0x20,16*0xa4))
        lines.append(' '.join(map(str,[len(program),len(starts),nv,len(ticks),*program,*starts,
                                      *[get(statics+n*4) for n in range(nv)],*words,get(vm+0xa60),1,*ticks])))
        for elapsed in ticks:
            writes.clear()
            _,error=p.call(0x56c870,(elapsed,),ecx=vm)
            if error: raise RuntimeError(error)
            expected.append([get(vm+0xa60),get(0x64186c),*[get(statics+n*4) for n in range(nv)],
                             *struct.unpack('<656I',p.uc.mem_read(vm+0x20,16*0xa4)),len(writes),*writes])
    proc=subprocess.run([args.binary,'--oracle'],input='\n'.join(lines)+'\n',text=True,capture_output=True,check=True)
    actual=[list(map(int,line.split())) for line in proc.stdout.splitlines()]
    if len(actual)!=len(expected): raise AssertionError(('row count',len(actual),len(expected)))
    for row,(want,got) in enumerate(zip(expected,actual)):
        if want!=got:
            index=next(i for i,pair in enumerate(zip(want,got)) if pair[0]!=pair[1])
            raise AssertionError(('row',row,'word',index,'retail',want[index],'port',got[index]))
    print(f'PASS: {len(fixtures)} script programs, {len(expected)} complete thread/static/RNG boundaries')


if __name__=='__main__': main()
