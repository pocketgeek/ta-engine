#!/usr/bin/env python3
"""Check native inactive-navigation braking against World.brakeGround.

Inputs exercise every signed-pitch bucket boundary, speed modes, and fractional
speeds, pending facing targets and pivot rates. Formation limits are disabled. Displacements remain
inside one footprint cell; this is not a collision or full-route oracle. Native
execution reads its original rules; no speed table is imported into the port
or the test runner.
"""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP
from unicorn.x86_const import UC_X86_REG_FPCW


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-dbg/retail_motion_test')
    args=ap.parse_args();p=Icd();p.uc.reg_write(UC_X86_REG_FPCW,0x027f)
    unit,mover,kind=HEAP,HEAP+0x1000,HEAP+0x2000
    nav,vtable,stub=HEAP+0x3000,HEAP+0x4000,HEAP+0x5000
    def put(a,v): p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    put(unit+8,mover);put(unit+0xb4,kind)
    put(mover,nav);put(nav,vtable);put(vtable+0x14,stub);put(vtable+0x34,stub)
    p.hooks[stub]=lambda uc,args:(0,0)
    # Synthetic units have no COB instance: discard TurnDirection notifications.
    # The captured sequence oracle executes the original notification routine.
    p.hooks[0x56c640]=lambda uc,args:(8,0)
    p.freeze_hooks();rng=random.Random(0x4d95f0);rows=[];expected=[]
    pitches=[(band*2048+delta)&65535 for band in range(-16,16) for delta in [-1,0,1,2047]]
    for i in range(16000):
        pitch=pitches[i%len(pitches)] if i<8192 else rng.randrange(65536)
        mode=i%8;heading=rng.randrange(65536)
        maximum=rng.randrange(1,4*65536);speed=rng.randrange(4*65536)
        brake=rng.randrange(0,4*65536)
        facing=((heading+32768)&65535) if i%31==0 else rng.randrange(65536) if i%2 else -1
        rate=rng.choice([0,1,500,1092,4096,32768,65535])
        road,water=rng.choice([32768,65536,78643,131072]),rng.choice([0,32768,65536])
        flags=rng.choice([0,0x800,0x1000,0x1800])
        p.uc.mem_write(kind+0x190,struct.pack('<H',rate))
        p.uc.mem_write(mover+0x34,struct.pack('<H',max(0,facing)))
        put(kind+0x166,brake);put(unit+0x12b,maximum);put(kind+0x162,maximum);put(mover+0x20,speed)
        put(kind+0x172,road);put(kind+0x16e,water)
        p.uc.mem_write(mover+0x36,struct.pack('<H',(mode<<8)|flags|(0x10 if facing>=0 else 0)))
        p.uc.mem_write(unit+0x80,struct.pack('<H',pitch))
        p.uc.mem_write(unit+0x7e,struct.pack('<H',heading))
        _,error=p.call(0x4d9ad0,(unit,),ecx=mover)
        if error: raise AssertionError((i,error))
        actual_speed=struct.unpack('<i',p.uc.mem_read(mover+0x20,4))[0]
        vx,vy,vz=struct.unpack('<3i',p.uc.mem_read(mover+8,12))
        if vy: raise AssertionError(('unexpected vertical velocity',vy))
        expected.append([actual_speed,vx,vz,struct.unpack('<H',p.uc.mem_read(unit+0x7e,2))[0]])
        rows.append(f'{speed} {maximum} {brake} {road} {water} {pitch} {mode} {heading} {flags} {facing} {rate}')
    result=subprocess.run([args.binary,'--ground-brake'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual=[list(map(int,line.split())) for line in result.stdout.splitlines()]
    if len(actual)!=len(expected): raise AssertionError('row count')
    for row,want,got in zip(rows,expected,actual):
        if want!=got: raise AssertionError((row,want,got))
    print(f'PASS: {len(rows)} native inactive-navigation brakes and World displacements')


if __name__=='__main__': main()
