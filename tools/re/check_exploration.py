#!/usr/bin/env python3
"""Compare exploration height construction and sight footprints with retail.

Terrain construction runs original 50ea58..50ed53; the unrelated sector-grid
prefix is bypassed after establishing its stack frame. Sight tests execute
complete 4c6800 with independent terrain, player and cached sight inputs.
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.x86_const import UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_FPCW


def compare(binary,mode,rows,expected):
    result=subprocess.run([binary,mode],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual=[list(map(int,line.split())) for line in result.stdout.splitlines()]
    if len(actual)!=len(expected): raise AssertionError((mode,'count',len(actual),len(expected)))
    for i,(want,got) in enumerate(zip(expected,actual)):
        if want!=got: raise AssertionError((mode,i,rows[i],'retail',want,'port',got))
    print(f'PASS: {len(rows)} {mode[2:]} cases')


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-dbg/retail_motion_test')
    args=ap.parse_args();p=Icd();uc=p.uc
    uc.reg_write(UC_X86_REG_FPCW,0x027f)
    game,records,coarse,sight,owner,counts,mapping=[HEAP+i*0x40000 for i in range(7)]
    def put(fmt,a,*v): uc.mem_write(a,struct.pack('<'+fmt,*v))
    def get(fmt,a): return struct.unpack('<'+fmt,uc.mem_read(a,struct.calcsize('<'+fmt)))[0]
    put('I',0x62d55c,game);put('I',game+0x19f04,records)
    p.hooks[0x4eb9e0]=lambda uc,a:(0,coarse)
    p.hooks[0x4eba00]=lambda uc,a:(0,0)
    p.freeze_hooks()
    def coarse_entry(uc,address,size,data):
        # The original prologue has already established EBP and its locals.
        # Preserve the three register slots normally pushed by the prefix.
        sp=uc.reg_read(UC_X86_REG_ESP)-12
        uc.mem_write(sp,bytes(12));uc.reg_write(UC_X86_REG_ESP,sp)
        uc.reg_write(UC_X86_REG_EIP,0x50ea58)
    uc.hook_add(UC_HOOK_CODE,coarse_entry,begin=0x50e746,end=0x50e746)
    rng=random.Random(0x4c6800);rows=[];expected=[]
    for i in range(1024):
        width,height=[rng.randrange(2,33)*2 for _ in range(2)]
        sea=rng.randrange(256)
        values=[rng.randrange(256) for _ in range(width*height)]
        if i%4==0: values=[i%256]*(width*height)
        if i%4==1: values=[min(255,z*8) for z in range(height) for x in range(width)]
        if i%4==2: values=[(x//4%2)*255 for z in range(height) for x in range(width)]
        data=bytearray(width*height*14)
        for index,h in enumerate(values): data[index*14+4]=h
        uc.mem_write(records,bytes(data));put('2I',game+0x19e98,width,height)
        put('B',game+0x19ef8,sea)
        _,error=p.call(0x50e740)
        if error: raise RuntimeError((i,error))
        expected.append(list(uc.mem_read(coarse,(width//2)*(height//2)*2)))
        rows.append(' '.join(map(str,[width,height,sea,*values])))
    compare(args.binary,'--exploration-heights',rows,expected)

    put('I',sight,owner);put('I',owner+0x88,counts)
    put('I',game+0x19f08,coarse);put('I',game+0x19ef4,mapping)
    put('B',game+0x306f,255) # Avoid renderer invalidation; no geometry depends on it.
    visits=[];width=0
    def count_write(uc,access,address,size,value,data):
        assert size==1
        delta=((value-get('B',address)+128)&255)-128
        index=address-counts;visits.extend((index%width,index//width,delta,0))
    def mapping_write(uc,access,address,size,value,data):
        assert size==2
        index=(address-mapping)//2
        assert visits[-4:-2]==[index%width,index//width]
        visits[-1]=value
    uc.hook_add(UC_HOOK_MEM_WRITE,count_write,begin=counts,end=counts+65535)
    uc.hook_add(UC_HOOK_MEM_WRITE,mapping_write,begin=mapping,end=mapping+65535)
    rows=[];expected=[]
    for i in range(8192):
        width,height=[rng.randrange(1,33) for _ in range(2)]
        x,z=[rng.randrange(-8,40) for _ in range(2)]
        eye=rng.randrange(-256,1024);distance=rng.choice([-512,-1,0,16,31,32,64,180,255,512,1024])
        sh=rng.choice([0,1,8,16,32,64,128,255]);old=i%2;active=i//2%2;explore=i//4%2;player=i//8%32
        heights=[rng.randrange(256) for _ in range(width*height*2)]
        put('2I',game+0x19e98,width*2,height*2);put('I',game+0x19f0c,width)
        put('2I',owner+0x8c,width,height);put('B',owner+0xeb,player)
        put('hhihBB',sight+0x14,x,z,eye,distance,sh,old)
        uc.mem_write(coarse,bytes(heights));uc.mem_write(counts,b'\x20'*(width*height))
        uc.mem_write(mapping,bytes(width*height*2));visits.clear()
        _,error=p.call(0x4c6800,(active,explore),ecx=sight)
        if error: raise RuntimeError((i,error))
        expected.append([get('B',sight+0x1f),*visits])
        rows.append(' '.join(map(str,[width,height,x,z,eye,distance,sh,old,active,explore,player,*heights])))
    compare(args.binary,'--sight-footprint',rows,expected)

    unit,world,options=[HEAP+i*0x40000 for i in range(7,10)]
    sight=unit+0x84;put('I',sight,owner)
    put('I',0x62d558,world);put('I',world+8,options)
    rows=[];expected=[]
    for i in range(8192):
        width,height=[rng.randrange(1,33) for _ in range(2)]
        x,z=[rng.randrange(-8,40) for _ in range(2)]
        eye=rng.randrange(-256,1024);distance=rng.choice([-512,-1,0,16,31,32,64,180,255,512,1024])
        sh=rng.choice([0,1,8,16,32,64,128,255]);old=i%2;fog=i//2%2;explore=i//4%2;player=i//8%32
        sea=rng.randrange(256)
        px,pz=[rng.randrange(-64*65536,1200*65536) for _ in range(2)]
        py=rng.randrange(-256*65536,512*65536)
        if i%3==0:
            x=int((px//65536)/32);z=int((pz//65536)/32)
            eye=max(py//65536,sea+1)+sh+rng.choice([-6,-5,-1,0,1,5,6])
        heights=[rng.randrange(256) for _ in range(width*height*2)]
        put('2I',game+0x19e98,width*2,height*2);put('2I',game+0x19f0c,width,height)
        put('B',game+0x19ef8,sea);put('2B',options+0x15,fog,explore)
        put('2I',owner+0x8c,width,height);put('B',owner+0xeb,player)
        put('hhihBB',sight+0x14,x,z,eye,distance,sh,old);put('3i',unit+0x68,px,py,pz)
        uc.mem_write(coarse,bytes(heights));uc.mem_write(counts,b'\x20'*(width*height))
        uc.mem_write(mapping,bytes(width*height*2));visits.clear()
        _,error=p.call(0x4c6c00,(unit,))
        if error: raise RuntimeError((i,error))
        expected.append([get('B',sight+0x1f),get('h',sight+0x14),get('h',sight+0x16),get('i',sight+0x18),*visits])
        rows.append(' '.join(map(str,[width,height,x,z,eye,distance,sh,old,fog,explore,player,px,py,pz,sea,*heights])))
    compare(args.binary,'--sight-update',rows,expected)

    models=HEAP+0x300000;rows=[];expected=[]
    for i in range(2048):
        count=rng.randrange(1,33);row=[count];children=[[] for _ in range(count)]
        for n in range(count):
            parent=rng.randrange(n) if n else -1
            offset=rng.randrange(-512*65536,512*65536)
            vertices=[rng.randrange(-512*65536,512*65536) for _ in range(rng.randrange(0,17))]
            row.extend((parent,offset,len(vertices),*vertices))
            address=models+n*0x800;uc.mem_write(address,bytes(0x40))
            put('I',address+4,len(vertices));put('i',address+0x14,offset);put('I',address+0x24,address+0x100)
            for v,y in enumerate(vertices): put('3i',address+0x100+v*12,0,y,0)
            if n: children[parent].append(n)
        for n,siblings in enumerate(children):
            if siblings: put('I',models+n*0x800+0x30,models+siblings[0]*0x800)
            for a,b in zip(siblings,siblings[1:]):put('I',models+a*0x800+0x2c,models+b*0x800)
        value,error=p.call(0x546f40,(models,))
        if error: raise RuntimeError((i,error))
        expected.append([value if value<0x80000000 else value-0x100000000]);rows.append(' '.join(map(str,row)))
    compare(args.binary,'--sight-model-top',rows,expected)


if __name__=='__main__': main()
