#!/usr/bin/env python3
"""Compare hover attack ring destinations and RNG draws with the user-owned retail binary."""
import sys,struct,random,subprocess

from emu import Icd,HEAP,STACK,STACK_SZ
from unicorn.x86_const import *
p=Icd();u=HEAP;m=HEAP+4096;frame=STACK+STACK_SZ-0x2000
put=lambda a,v:p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
state=[0,0]
def rand(uc,args):
 n=struct.unpack('<I',uc.mem_read(args,4))[0];state[0]=(state[0]*1664525+1013904223)&0xffffffff;state[1]+=1;return 1,state[0]%n
p.hooks[0x535cc0]=rand;p.freeze_hooks();p.uc.reg_write(UC_X86_REG_FPCW,0x27f)
rng=random.Random(175);rows=[];expected=[]
for i in range(4096):
 x,z,tx,tz=[rng.randrange(100,2000)*65536 for _ in range(4)];distance=rng.choice([0,75,150,200,250]);seed=rng.randrange(2**32)
 if i%3==0:x=tx+(distance+rng.randrange(-17,18))*65536;z=tz
 for a,v in [(u+0x68,x),(u+0x70,z),(m+0x22,tx),(m+0x26,0),(m+0x2a,tz),(frame+12,distance)]:put(a,v)
 p.uc.reg_write(UC_X86_REG_EDI,u);p.uc.reg_write(UC_X86_REG_ESI,m);p.uc.reg_write(UC_X86_REG_EBP,frame);p.uc.reg_write(UC_X86_REG_ESP,frame-0x100)
 state[:]=[seed,0];p.uc.emu_start(0x41de5d,0x41df67,count=100000)
 a=struct.unpack('<iii',p.uc.mem_read(frame-0x14,12));expected.append([a[0],a[2],*state]);rows.append(f'{x} {z} {tx} {tz} {distance} {seed}')
r=subprocess.run([sys.argv[1] if len(sys.argv)>1 else 'build-dbg/retail_motion_test','--hover-attack'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
actual=[list(map(int,s.split())) for s in r.stdout.splitlines()]
for i,(want,got) in enumerate(zip(expected,actual)):
 if want!=got:raise AssertionError((i,rows[i],want,got))
assert len(actual)==len(expected)
print('PASS',len(actual),'retail hover attack points and RNG states')
