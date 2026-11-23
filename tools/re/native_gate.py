"""Native gate script/yard fixture for composed movement comparisons.

Executes the original active-bit setter, COB VM/animation, yard validation,
restamping and cache-refresh dispatch. Rendering, audio and simple unit queries
are host boundaries. Assets are read from the caller's local retail install.
"""
import struct
import subprocess

from emu import HEAP, Icd
from balance_inputs import unit_properties
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EIP, UC_X86_REG_ESP


def asset(root, name):
    location=subprocess.run(['build/hpitool','where',str(root),name],text=True,
                            capture_output=True,check=True).stdout.strip().split(' -> ',1)[1]
    archive,path=location.split('!',1)
    return subprocess.run(['build/hpitool','cat',str(root/archive),path],
                          capture_output=True,check=True).stdout


class NativeGate:
    def __init__(self, root, name, *, icd=None, game_fields=None, freeze=True):
        self.p=icd if icd is not None else Icd()
        self.game,self.cells,self.pool,self.kind,self.yard,self.vm,self.desc,self.code,self.entries,self.statics,self.pieces,self.vtable,self.player=(
            HEAP+n*0x10000 for n in (0,*range(2,14)))
        self.gate=self.pool+312
        if game_fields is not None:
            if len(game_fields)>0x20000: raise ValueError('game fields exceed reserved fixture block')
            self.p.uc.mem_write(self.game,game_fields)
        fields=unit_properties(asset(root,f'units/{name}.fbi').decode())
        data=asset(root,f'scripts/{name}.cob')
        h=struct.unpack_from('<10I',data)
        _,ns,self.np,nc,self.nv,_,index,names,_,off=h
        self.names=[]
        for n in range(ns):
            address=struct.unpack_from('<I',data,names+4*n)[0]
            self.names.append(data[address:].split(b'\0',1)[0].decode().lower())
        self.fx,self.fz=int(fields['footprintx']),int(fields['footprintz'])
        self.x=(512-(self.fx-1)*8)//16
        self.z=(512-(self.fz-1)*8)//16
        self.yard_text=''.join(fields['yardmap'].split())
        if len(self.yard_text)!=self.fx*self.fz: raise ValueError('yard dimensions differ')
        self.put(0x62d55c,self.game)
        for offset,value in ((0x19e98,64),(0x19e9c,64),(0x19f04,self.cells),
                             (0x14e84,self.pool),(0x14e88,self.gate)):
            self.put(self.game+offset,value)
        cells=bytearray(64*64*14)
        for n in range(64*64): struct.pack_into('<H',cells,n*14+8,0xffff)
        self.p.uc.mem_write(self.cells,bytes(cells))
        self.p.uc.mem_write(self.gate+2,struct.pack('<H',1))
        self.p.uc.mem_write(self.gate+0x74,struct.pack('<4h',self.x,self.z,self.fx,self.fz))
        self.put(self.gate+0xb4,self.kind);self.put(self.gate+0xb8,self.player)
        self.put(self.gate+0xbc,self.vm);self.put(self.gate+0x130,0x3800001)
        # A new unit's +0x114 is 0 (511990); only activatewhenbuilt (4c033e,
        # default 0, check_change_owner.py) activates it at creation.
        self.p.uc.mem_write(self.gate+0x114,bytes([int(fields.get('activatewhenbuilt','0'))&1]))
        self.put(self.kind+0x264,0x40000000);self.put(self.kind+0x12a,self.yard)
        codes={'.':0,'o':0x2f,'c':0x2d,'C':0x35,'y':0x29,'Y':0x31,'w':0x37}
        self.p.uc.mem_write(self.yard,bytes(codes[c] for c in self.yard_text))
        self.put(self.vm,self.vtable);self.put(self.vm+4,30);self.put(self.vm+0xc,self.desc)
        self.put(self.vm+0x14,self.statics);self.put(self.vm+0x18,self.pieces)
        for offset,value in ((4,ns),(8,self.np),(0x10,self.nv),(0x18,self.entries),(0x24,self.code),
                             (0x2c,HEAP+0xe0000)):
            self.put(self.desc+offset,value)
        self.p.uc.mem_write(self.code,data[off:off+nc*4])
        self.p.uc.mem_write(self.entries,data[index:index+ns*4])
        self.pose=[[0]*6 for _ in range(self.np)]
        self.ready=self.bugger=False
        def write_pose(base):
            def invoke(uc,args):
                self.pose[self.get(args)][base+self.get(args+4)]=self.get(args+8)
                return 3,0
            return invoke
        def read_pose(base):
            return lambda uc,args:(2,self.pose[self.get(args)][base+self.get(args+4)])
        def query(uc,args):
            key=self.get(args)
            values={1:self.active,2:int(fields.get('standingmoveorder','1')),
                    3:int(fields.get('standingfireorder','2')),4:100,5:self.ready,
                    8:0,17:0,18:self.opened,19:self.bugger,32:0}
            if key not in values: raise ValueError(f'unsupported gate query {key}')
            return 5,int(values[key])
        callbacks={0:write_pose(0),4:write_pose(3),8:lambda uc,a:(2,0),12:lambda uc,a:(2,0),
                   16:lambda uc,a:(2,0),20:lambda uc,a:(2,0),24:read_pose(0),28:read_pose(3),
                   44:lambda uc,a:(2,0),48:lambda uc,a:(2,0),52:lambda uc,a:(2,0),
                   56:lambda uc,a:(2,0),84:query}
        for n,(offset,callback) in enumerate(callbacks.items()):
            address=0x56a000+n*16
            self.put(self.vtable+offset,address);self.p.hooks[address]=callback
        def lookup(uc,args):
            pointer=self.get(args)
            name=bytes(uc.mem_read(pointer,128)).split(b'\0',1)[0].decode().lower()
            return 1,self.names.index(name) if name in self.names else -1
        self.p.hooks[0x56c4a0]=lookup
        setter=0x56a800
        self.put(self.vtable+80,setter)
        def set_value(uc,address,size,unused):
            sp=uc.reg_read(UC_X86_REG_ESP)
            key,value=struct.unpack('<2I',uc.mem_read(sp+4,8))
            if key==18:
                self.put(sp+4,self.gate)
                uc.reg_write(UC_X86_REG_EIP,0x507c70)
                return
            if key==5: self.ready=bool(value&1)
            elif key==19: self.bugger=bool(value)
            else: raise ValueError(f'unsupported gate write {key}')
            uc.reg_write(UC_X86_REG_EIP,self.get(sp))
            uc.reg_write(UC_X86_REG_ESP,sp+12)
        self.p.uc.hook_add(UC_HOOK_CODE,set_value,begin=setter,end=setter)
        if freeze: self.p.freeze_hooks()
        self.put(0x64186c,1)
        self.call(0x5062d0,(self.gate,))
        self.call(0x56c540,(self.names.index('create'),),self.vm)

    def put(self,address,value): self.p.uc.mem_write(address,struct.pack('<I',value&0xffffffff))
    def get(self,address): return struct.unpack('<I',self.p.uc.mem_read(address,4))[0]
    def call(self,address,args=(),this=None):
        result,error=self.p.call(address,args,ecx=this)
        if error or self.p.uc.reg_read(UC_X86_REG_EIP)!=0x6ffff000:
            raise RuntimeError((hex(address),error))
        return result
    @property
    def active(self): return bytes(self.p.uc.mem_read(self.gate+0x114,1))[0]&1
    @property
    def opened(self): return int(bool(bytes(self.p.uc.mem_read(self.gate+0x12f,1))[0]&4))
    def step(self,active=-1):
        if active>=0: self.call(0x51e4d0,(1,active),self.gate)
        self.call(0x56c870,(1,),self.vm)
    def state(self):
        result=[self.active,self.opened,self.get(self.vm+0xa60),self.get(0x64186c)]
        result += [self.get(self.statics+4*n) for n in range(self.nv)]
        result += list(struct.unpack('<656I',self.p.uc.mem_read(self.vm+0x20,16*0xa4)))
        for n in range(self.np):
            result += list(struct.unpack('<19I',self.p.uc.mem_read(self.pieces+n*76,76)))+self.pose[n]
        for z in range(self.z,self.z+self.fz):
            for x in range(self.x,self.x+self.fx):
                result.append(struct.unpack('<H',self.p.uc.mem_read(self.cells+(z*64+x)*14,2))[0])
        return result
