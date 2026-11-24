#!/usr/bin/env python3
# Usage: ~/venvs/ta-re/bin/python tools/re/cob_signal_emu.py  (needs ~/ta_install/TotalA.exe)
"""Run TotalA.exe's COB thread start (0x4b0a70) and per-thread runner (0x4b0da0)
under unicorn on a hand-built script object to check SIGNAL semantics."""
import sys,struct
sys.path.insert(0,'/home/pocket_geek/TA/tools/re')
from pe import Bin
from unicorn import *
from unicorn.x86_const import *
b=Bin('/home/pocket_geek/ta_install/TotalA.exe')
PUSHC=0x10021001; SIGNAL=0x10067000; SETMASK=0x10068000; WAITTURN=0x10011000; RET=0x10065000
H=0x20000000
O=H+0x1000; F=H+0x3000; CODE=H+0x4000; NAMES=H+0x5000; STARTS=H+0x5800; ANIM=H+0x6000
S0=H+0x9000; S1=H+0x9100; STK=H+0x80000; RETADDR=H+0xff000
def mk(sig):
    return [PUSHC,sig,SIGNAL,PUSHC,4,SETMASK,WAITTURN,0,1,PUSHC,1,RET]
def setup(scripts):
    mu=Uc(UC_ARCH_X86,UC_MODE_32)
    size=(len(b.img)+0xfff)&~0xfff
    mu.mem_map(b.base,size); mu.mem_write(b.base,bytes(b.img))
    mu.mem_map(H,0x100000); mu.mem_write(RETADDR,b'\xf4')
    code=[]; starts=[]; names=[]
    no=NAMES
    for name,body in scripts:
        starts.append(len(code)); code+=body
        mu.mem_write(no,name.encode()+b'\0'); names.append(no); no+=0x40
    mu.mem_write(CODE,struct.pack('<%dI'%len(code),*code))
    mu.mem_write(STARTS,struct.pack('<%dI'%len(starts),*starts))
    mu.mem_write(STARTS+0x100,struct.pack('<%dI'%len(names),*names))
    w=lambda a,v: mu.mem_write(a,struct.pack('<I',v&0xffffffff))
    w(F+4,len(scripts)); w(F+0x18,STARTS); w(F+0x1c,STARTS+0x100); w(F+0x24,CODE)
    w(O+4,1); w(O+8,F); w(O+0x14,ANIM); w(O+0x53c,0)
    for S in (S0,S1): w(S+4,0x4fd6f0); w(S+8,0)   # callback object = S+4, ready = S+8
    return mu,w
def call(mu,fn,ecx,args):
    sp=STK-0x100
    mu.mem_write(sp,struct.pack('<I',RETADDR)+b''.join(struct.pack('<I',a&0xffffffff) for a in args))
    mu.reg_write(UC_X86_REG_ESP,sp); mu.reg_write(UC_X86_REG_ECX,ecx)
    mu.emu_start(fn,RETADDR,count=200000)
    return mu.reg_read(UC_X86_REG_EAX)
def start(mu,name,cb,h=0,p=0):
    np=H+0x7000; mu.mem_write(np,name.encode()+b'\0')
    return call(mu,0x4b0a70,O,[np,cb,0,2,h,p,0,0])
def vmpass(mu):
    for i in range(8): call(mu,0x4b0da0,O,[i,1])
def dump(mu,tag):
    r=lambda a: struct.unpack('<I',mu.mem_read(a,4))[0]
    th=[]
    for i in range(8):
        t=O+0x1c+i*0xa4
        st=r(t)
        if st: th.append('t%d:st=%x pc=%d mask=%d cb=%x'%(i,st,r(t+4),r(t+0x1c),r(t+0x20)))
    print(tag,'| ready0=%d ready1=%d | live=%d |'%(r(S0+8),r(S1+8),r(O+0x53c)),'; '.join(th))
scripts=[('AimPrimary',mk(4)),('AimSecondary',mk(4))]
print('Case 1: AimPrimary then AimSecondary started in one weapon update (same tick)')
mu,w=setup(scripts)
start(mu,'AimPrimary',S0+4); start(mu,'AimSecondary',S1+4); dump(mu,' after starts')
vmpass(mu); dump(mu,' after pass 1')
vmpass(mu); dump(mu,' after pass 2 (turns done)')
print('Case 2: AimPrimary started tick 1, AimSecondary tick 2 (primary still waiting for its turn)')
mu,w=setup(scripts)
start(mu,'AimPrimary',S0+4); vmpass(mu); dump(mu,' tick1')
w(ANIM+0x2c,1)   # piece 0 axis 1 still turning (wait-turn test reads anim[piece*19+axis]+0x28)
start(mu,'AimSecondary',S1+4); vmpass(mu); dump(mu,' tick2')
w(ANIM+0x2c,0); vmpass(mu); dump(mu,' tick3 (turn done)')
print('Case 3: fresh, not yet run thread (default mask 1) vs SIGNAL 4 and SIGNAL 1')
for sig in (4,1):
    mu,w=setup([('Victim',[PUSHC,1,RET]),('Killer',[PUSHC,sig,SIGNAL,PUSHC,0,RET])])
    start(mu,'Killer',0); start(mu,'Victim',S0+4); dump(mu,'  sig=%d before'%sig)
    vmpass(mu); dump(mu,'  sig=%d after '%sig)
