#!/usr/bin/env python3
"""Compare 4139d0 cached/live grade dispatch and ordered special-body probes."""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP
from unicorn import UC_HOOK_MEM_READ, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EIP


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runner'); args=parser.parse_args()
    p=Icd(); uc=p.uc
    obj,grid,plane,unit,kind,record,game=(HEAP,HEAP+0x1000,HEAP+0x2000,
                                      HEAP+0x11000,HEAP+0x12000,HEAP+0x13000,HEAP+0x20000)
    def put(a,v): uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    put(0x62d55c,game); put(obj+0x6c,grid); put(obj+0x58,unit)
    put(grid+0x340,32); put(grid+0x344,32); put(grid+0x348,plane)
    put(game+0x14e84,unit-0x138); put(game+0x14e88,unit)
    put(unit+0x130,0x1000000); put(unit+0xb4,kind); put(kind+0x264,0x40000000)
    uc.mem_write(record,struct.pack('<H',1)); uc.mem_write(record+0xd,b'\x20')
    fixtures=[]; expected=[]; rng=random.Random(0x4139d0)
    for index in range(6000):
        fx,fz=rng.randrange(1,6),rng.randrange(1,6)
        sx,sz=rng.randrange(4,21),rng.randrange(4,21)
        player,retry,probe=rng.randrange(10),rng.randrange(4),rng.randrange(2)
        x,z=rng.randrange(-1,33),rng.randrange(-1,33)
        visible,grade,live=rng.randrange(2),rng.randrange(16),rng.randrange(8)
        bx,bz=x+rng.randrange(fx),z+rng.randrange(fz)
        owner=rng.choice((-1,player,(player+1)%10))
        uc.mem_write(grid+4,struct.pack('<hh',fx,fz))
        uc.mem_write(unit+0x78,struct.pack('<hh',fx,fz))
        uc.mem_write(obj+0x30,struct.pack('<hh',sx,sz))
        uc.mem_write(obj+0x114,bytes([player])); put(obj+0x1ad,retry)
        put(game+player*0x110+0x2484,kind)
        uc.mem_write(plane,struct.pack('<I',grade*0x11111111)*128)
        uc.mem_write(unit+0xfd,bytes([max(0,owner)]))
        queries=[]
        p.hooks[0x413c80]=lambda uc,a:(4,visible)
        p.hooks[0x409fe0]=lambda uc,a:(0,probe)
        def live_query(uc,a):
            who,wx,_,wz=struct.unpack('<Iiii',uc.mem_read(a,16))
            assert who==unit
            queries.extend((1,((wx>>19)-fx)//2,((wz>>19)-fz)//2))
            return 4,live
        def body(uc,a):
            qx,qz=struct.unpack('<ii',uc.mem_read(a,8))
            queries.extend((2,qx,qz))
            return 2,record if owner>=0 and (qx,qz)==(bx,bz) else 0
        p.hooks[0x4db640]=live_query; p.hooks[0x50e600]=body
        value,error=p.call(0x4139d0,(x,z,index%8),ecx=obj)
        assert error is None and uc.reg_read(UC_X86_REG_EIP)==0x6ffff000,error
        fixtures.append(' '.join(map(str,(32,32,fx,fz,sx,sz,player,retry,probe,x,z,visible,grade,live,bx,bz,owner))))
        expected.append(' '.join(map(str,(value,*queries))))
    actual=subprocess.run([args.runner,'--grade'],input='\n'.join(fixtures)+'\n',
                          text=True,capture_output=True,check=True).stdout.splitlines()
    assert actual==expected,next(((i,a,b,fixtures[i]) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),
                                (len(actual),len(expected)))
    print(f'PASS: {len(expected)} cached/live grade results and ordered probes match retail')
    del p.hooks[0x413c80]
    visibility=HEAP+0x40000
    put(game+0x19ef4,visibility)
    fixtures=[]; expected=[]
    def visibility_read(uc,access,address,size,value,data):
        cell=(address-visibility)//2
        queries.extend((cell%(mw//2),cell//(mw//2)))
    uc.hook_add(UC_HOOK_MEM_READ,visibility_read,begin=visibility,end=visibility+4095)
    for _ in range(3000):
        fx,fz=rng.randrange(1,9),rng.randrange(1,9)
        mw,mh=rng.randrange(20,65),rng.randrange(20,65)
        player,x,z=rng.randrange(32),rng.randrange(-1,33),rng.randrange(-1,33)
        mask=rng.randrange(65536)
        uc.mem_write(grid+4,struct.pack('<hh',fx,fz))
        put(game+0x19e98,mw); put(game+0x19e9c,mh)
        uc.mem_write(visibility,struct.pack('<H',mask)*2048)
        queries=[]
        value,error=p.call(0x413c80,(grid,player,x,z),ecx=obj)
        assert error is None and uc.reg_read(UC_X86_REG_EIP)==0x6ffff000,error
        fixtures.append(' '.join(map(str,(32,32,fx,fz,mw,mh,player,x,z,mask))))
        expected.append(' '.join(map(str,(value,*queries))))
    actual=subprocess.run([args.runner,'--visible'],input='\n'.join(fixtures)+'\n',
                          text=True,capture_output=True,check=True).stdout.splitlines()
    assert actual==expected,next(((i,a,b,fixtures[i]) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),
                                (len(actual),len(expected)))
    print(f'PASS: {len(expected)} coarse visibility results and cell reads match retail')
    # Compose the original grade dispatcher with its original visibility query,
    # and compare the production World adapter. Live terrain and special yards
    # are separate fixtures above; these cases isolate owner/map dispatch.
    fixtures=[]; expected=[]
    mw=mh=32
    put(game+0x19e98,mw); put(game+0x19e9c,mh)
    p.hooks[0x409fe0]=lambda uc,a:(0,0)
    for index in range(4096):
        fx,fz=rng.randrange(1,9),rng.randrange(1,9)
        sx,sz=rng.randrange(32),rng.randrange(32)
        player,retry=rng.randrange(8),rng.randrange(4)
        x,z=rng.randrange(-1,33),rng.randrange(-1,33)
        grade=rng.randrange(16)
        if (grade&7)==2: retry=2
        mask=rng.choice((0,65535,1<<player,1<<((player+1)%8),rng.randrange(65536)))
        uc.mem_write(grid+4,struct.pack('<hh',fx,fz))
        uc.mem_write(unit+0x78,struct.pack('<hh',fx,fz))
        uc.mem_write(obj+0x30,struct.pack('<hh',sx,sz))
        uc.mem_write(obj+0x114,bytes([player])); put(obj+0x1ad,retry)
        put(game+player*0x110+0x2484,kind)
        uc.mem_write(plane,struct.pack('<I',grade*0x11111111)*128)
        uc.mem_write(visibility,struct.pack('<H',mask)*2048)
        queries=[]
        value,error=p.call(0x4139d0,(x,z,index%8),ecx=obj)
        assert error is None and uc.reg_read(UC_X86_REG_EIP)==0x6ffff000,error
        fixtures.append(' '.join(map(str,(fx,fz,player,sx,sz,retry,x,z,grade,mask))))
        expected.append(str(value))
    actual=subprocess.run([args.runner,'--world-grade'],input='\n'.join(fixtures)+'\n',
                          text=True,capture_output=True,check=True).stdout.splitlines()
    assert actual==expected,next(((i,a,b,fixtures[i]) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),
                                (len(actual),len(expected)))
    print(f'PASS: {len(expected)} World search grades with owner exploration match retail')
    # 4e0300 updates the packed plane in place, including its independent
    # fourth bit. Observe actual setter calls without replacing the setter.
    def hashed(values):
        h=14695981039346656037
        for value in values:
            for byte in struct.pack('<I',value&0xffffffff):
                h=((h^byte)*1099511628211)&0xffffffffffffffff
        return h
    writes=[]
    def setter(uc,address,size,data):
        from unicorn.x86_const import UC_X86_REG_ESP
        writes.extend(struct.unpack('<iii',uc.mem_read(uc.reg_read(UC_X86_REG_ESP)+4,12)))
    p.uc.hook_add(UC_HOOK_CODE,setter,begin=0x4dfe40,end=0x4dfe40)
    fixtures=[]; expected=[]
    for _ in range(3000):
        width,height=rng.randrange(8,33),rng.randrange(8,33)
        fx,fz=rng.randrange(1,6),rng.randrange(1,6)
        bx,bz=rng.randrange(1,6),rng.randrange(1,6)
        x,z=rng.randrange(width-bx+1),rng.randrange(height-bz+1)
        stale=rng.randrange(2)
        cells=[rng.randrange(16) for _ in range(width*height)]
        packed=[0]*(width*((height+7)//8))
        for qz in range(height):
            for qx in range(width):
                packed[(qz//8)*width+qx]|=cells[qz*width+qx]<<(4*(qz%8))
        put(grid+0x340,width); put(grid+0x344,height)
        uc.mem_write(grid+4,struct.pack('<hh',fx,fz))
        uc.mem_write(plane,struct.pack(f'<{len(packed)}I',*packed))
        writes=[]
        value,error=p.call(0x4e0300,(x|(z<<16),bx|(bz<<16),stale),ecx=grid)
        assert error is None and uc.reg_read(UC_X86_REG_EIP)==0x6ffff000,error
        result=struct.unpack(f'<{len(packed)}I',uc.mem_read(plane,len(packed)*4))
        actual_cells=[(result[(qz//8)*width+qx]>>(4*(qz%8)))&15
                      for qz in range(height) for qx in range(width)]
        fixtures.append(' '.join(map(str,(width,height,fx,fz,x,z,bx,bz,stale,*cells))))
        expected.append(f'{hashed(actual_cells)} {hashed(writes)}')
    actual=subprocess.run([args.runner,'--age-grade'],input='\n'.join(fixtures)+'\n',
                          text=True,capture_output=True,check=True).stdout.splitlines()
    assert actual==expected,next(((i,a,b,fixtures[i]) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),
                                (len(actual),len(expected)))
    print(f'PASS: {len(expected)} aged occupancy planes and ordered writes match retail')
    fixtures=[]; expected=[]
    pool,movers=HEAP+0x80000,HEAP+0x90000
    def refresh(uc,a):
        origin,_=struct.unpack('<II',uc.mem_read(a,8))
        events.extend((1,origin&0xffff)); return 2,0
    def age(uc,a):
        origin,_,stale=struct.unpack('<III',uc.mem_read(a,12))
        events.extend((2,origin&0xffff,int(bool(stale&255)))); return 3,0
    p.hooks[0x4e01d0]=refresh; p.hooks[0x4e0300]=age
    put(game+0x14e84,pool)
    for _ in range(3000):
        count=rng.randrange(1,33); requester=rng.randrange(1,count+1)
        tick=rng.choice((0,9,10,149,150,151,rng.randrange(1000),0xffffffff))
        old_recent=rng.choice((0,max(0,tick-10),rng.randrange(1000)))
        old_stale=rng.choice((0,max(0,tick-150),rng.randrange(1000)))
        last,finish=rng.randrange(2),rng.randrange(2)
        body_fields=[]
        put(game+0x19f44,tick); put(game+0x14e88,pool+count*0x138)
        put(grid+0x34c,old_recent); put(grid+0x350,old_stale)
        for i in range(1,count+1):
            flags=rng.choice((0,1,0x1000000,0x1000001,0x1000002,0x1000003))
            stamp=rng.choice((old_recent,old_stale,max(0,tick-10),max(0,tick-150),rng.randrange(1000)))
            has_mover=1 if i==requester else rng.randrange(2)
            body_fields.extend((flags,stamp,has_mover))
            entity=pool+i*0x138; mover=movers+i*0x100
            put(entity+0x130,flags); put(entity+8,mover if has_mover else 0)
            put(entity+0x74,i); put(entity+0x78,0x10001); put(mover+0x28,stamp)
        events=[]
        entity=pool+requester*0x138
        _,error=p.call(0x4e1ee0,(entity,last),ecx=grid)
        assert error is None and uc.reg_read(UC_X86_REG_EIP)==0x6ffff000,error
        if finish:
            _,error=p.call(0x4e2060,(entity,),ecx=grid)
            assert error is None and uc.reg_read(UC_X86_REG_EIP)==0x6ffff000,error
        recent,stale=struct.unpack('<II',uc.mem_read(grid+0x34c,8))
        current=struct.unpack('<I',uc.mem_read(grid+0x33c,4))[0]
        slot=(current-pool)//0x138 if current else 0
        fixtures.append(' '.join(map(str,(old_recent,old_stale,tick,requester,last,count,finish,*body_fields))))
        expected.append(' '.join(map(str,(recent,stale,slot,*events))))
    actual=subprocess.run([args.runner,'--prepare-grade'],input='\n'.join(fixtures)+'\n',
                          text=True,capture_output=True,check=True).stdout.splitlines()
    assert actual==expected,next(((i,a,b,fixtures[i]) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),
                                (len(actual),len(expected)))
    print(f'PASS: {len(expected)} grid preparation/cleanup schedules match retail')


if __name__=='__main__': main()
