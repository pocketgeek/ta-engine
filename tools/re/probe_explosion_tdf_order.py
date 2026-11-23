#!/usr/bin/env python3
"""Execute native TDF parsing with allocator sinks only; check class order."""
import sys,struct
from emu import Icd,HEAP
p=Icd(); bump=HEAP+0x100000; sizes={}
def words(a,n):return struct.unpack('<'+'I'*n,p.uc.mem_read(a,n*4))
def alloc(n):
 global bump
 a=bump;bump+=(n+15)&~15;sizes[a]=n;p.uc.mem_write(a,bytes(n));return a
def malloc(uc,sp):return 0,alloc(words(sp,1)[0])
def named(uc,sp):return 0,alloc(words(sp,2)[1])
def realloc(uc,sp):
 old,tag,n=words(sp,3);a=alloc(n)
 if old:p.uc.mem_write(a,bytes(p.uc.mem_read(old,min(n,sizes[old]))))
 return 0,a
def strdup(uc,sp):
 a=words(sp,1)[0];s=bytearray()
 while True:
  b=p.uc.mem_read(a+len(s),1)[0];s.append(b)
  if not b:break
 out=alloc(len(s));p.uc.mem_write(out,bytes(s));return 0,out
p.hooks.update({0x5ba480:malloc,0x4eb9e0:malloc,0x5ba3d0:named,0x5ba4d0:realloc,0x5ba640:strdup,0x5ba5d0:lambda uc,sp:(0,0),0x4eba00:lambda uc,sp:(0,0)})
p.freeze_hooks();cursor=HEAP;text=HEAP+0x10000
import random
rng=random.Random(0x5427f0)
checks=0
for case in range(64):
 names=[f'class-{i:02d}' for i in range(1+case%31)];rng.shuffle(names)
 source=('// synthetic class order\n'+''.join('['+name+']{[0]{gaf=x;anim=y;}}' for name in names)).encode()
 bump=HEAP+0x100000;sizes.clear();p.uc.mem_write(cursor,bytes(16));p.uc.mem_write(text,source+b'\0')
 value,error=p.call(0x5427f0,(text,len(source),0,0),ecx=cursor)
 assert not error,error
 root=words(cursor,1)[0];a,end=words(root+8,2)
 actual=[]
 for i in range((end-a)//4):
  node=words(a+i*4,1)[0];name=words(node,1)[0]
  actual.append(bytes(p.uc.mem_read(name,40)).split(b'\0')[0].decode())
 assert actual==names,(case,actual,names)
 checks+=len(names)
print(f'PASS: {checks} classes through full native TDF parsing retain authored order with allocator sinks only')
if len(sys.argv)>1:
 source=open(sys.argv[1],'rb').read();assert len(source)<0x80000
 bump=HEAP+0x100000;sizes.clear();p.uc.mem_write(cursor,bytes(16));p.uc.mem_write(text,source+b'\0')
 _,error=p.call(0x5427f0,(text,len(source),0,0),ecx=cursor);assert not error,error
 root=words(cursor,1)[0];a,end=words(root+8,2);names=[]
 for i in range((end-a)//4):
  node=words(a+i*4,1)[0];name=words(node,1)[0]
  names.append(bytes(p.uc.mem_read(name,128)).split(b'\0')[0].decode())
 print('Native parsed class count:',len(names))
 print('Native classes 9/10:',names[9:11])
