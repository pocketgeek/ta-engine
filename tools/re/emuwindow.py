"""Bounded recorded Wine window queries, with explicit per-API contracts."""
import struct


class CapturedWindowLoop:
    """Observed main-thread message inputs; no Wine callbacks or queue simulation."""
    SPECS={0x1156c:('thread_state',1),0x1261c:('foreground',1),
           0x11d2c:('peek',5),0x1123c:('get',4),0x12e0c:('translate',2)}

    def __init__(self,process,capture,base):
        self.process=process; self.index=0; self.base=base
        self.events=[e for e in capture.get('syscall_calls',[])
                     if e['stub_return']-12-base in self.SPECS]
        for rva,(name,words) in self.SPECS.items():
            entry=base+rva; code=bytes(process.uc.mem_read(entry,15))
            if code[0]!=0xb8 or code[5]!=0xba or code[10:13]!=b'\xff\xd2\xc2' or struct.unpack_from('<H',code,13)[0]!=words*4:
                raise ValueError('window loop syscall stub differs')
            process.icd.hooks[entry]=lambda uc,argv,entry=entry:self.query(uc,argv,entry)

    def query(self,uc,argv,entry):
        if self.index>=len(self.events): raise RuntimeError('recorded window loop inputs exhausted')
        e=self.events[self.index]; p=self.process
        if e['stub_return']!=entry+12 or p.u32(argv-4)!=e['return_address'] or p.u32(p.game+0x19f44)!=e['tick']:
            raise RuntimeError(f'window loop order differs at {self.index}')
        name,words=self.SPECS[entry-self.base]
        args=list(struct.unpack(f'<{words}I',uc.mem_read(argv,words*4)))
        if name in ('thread_state','foreground'):
            if args!=e['arguments']: raise RuntimeError('window loop scalar arguments differ')
        else:
            if args[1:]!=e['arguments'][1:] or bool(args[0])!=bool(e['arguments'][0]):
                raise RuntimeError('message query arguments differ')
            if name=='translate' or (name=='peek' and e['result']) or (name=='get' and e['result']!=0xffffffff):
                msg=bytes.fromhex(e['pointer_samples']['0'])[:28]
                if len(msg)!=28: raise RuntimeError('message structure was not captured')
                kind=struct.unpack_from('<I',msg,4)[0]
                # These messages have immediate scalar payloads. WM_TIMER's
                # callback address is consumed by the real dispatcher later;
                # unsupported callback dependencies must stop there.
                if kind not in (0x12,0xf,0x1c,0x112,0x113,0x200):
                    raise RuntimeError(f'unsupported recorded message payload {kind:#x}')
                if name=='translate':
                    if bytes(uc.mem_read(args[0],28))!=msg: raise RuntimeError('translated message differs')
                else: uc.mem_write(args[0],msg)
        self.index+=1
        return words,e['result']


class CapturedWindowQueries:
    def __init__(self,process,capture,dpi_entry,rect_entry):
        self.process=process
        self.events=[e for e in capture.get('syscall_calls',[])
                     if e['stub_return'] in (dpi_entry+12,rect_entry+12)]
        self.index=0
        self.dpi_entry=dpi_entry
        for entry,words in ((dpi_entry,1),(rect_entry,3)):
            code=bytes(process.uc.mem_read(entry,15))
            if code[0]!=0xb8 or code[5]!=0xba or code[10:13]!=b'\xff\xd2\xc2' or struct.unpack_from('<H',code,13)[0]!=words*4:
                raise ValueError('window query syscall stub differs')
            process.icd.hooks[entry]=lambda uc,argv,entry=entry,words=words:self.query(uc,argv,entry,words)

    def query(self,uc,argv,entry,words):
        if self.index>=len(self.events): raise RuntimeError('recorded window queries exhausted')
        e=self.events[self.index]; p=self.process
        if (e['stub_return']!=entry+12 or p.u32(argv-4)!=e['return_address']
                or p.u32(p.game+0x19f44)!=e['tick']):
            raise RuntimeError(f'window query order differs at {self.index}')
        args=list(struct.unpack(f'<{words}I',uc.mem_read(argv,words*4)))
        if entry==self.dpi_entry:
            if args!=e['arguments']: raise RuntimeError('process DPI handle differs')
        else:
            if args[0]!=e['arguments'][0] or args[2]!=e['arguments'][2] or args[2] not in (13,14):
                raise RuntimeError('window rectangle handle/operation differs')
            # user32 GetWindowRect/GetClientRect pass {RECT*, UINT dpi}.
            sample=bytes.fromhex(e['pointer_samples']['1'])
            old_rect,old_dpi=struct.unpack_from('<II',sample)
            rect,dpi=struct.unpack('<II',uc.mem_read(args[1],8))
            if dpi!=old_dpi or bool(rect)!=bool(old_rect):
                raise RuntimeError('window rectangle DPI/output differs')
            if e['result']:
                offset=old_rect-e['arguments'][1]
                if not rect or not 0<=offset<=len(sample)-16:
                    raise RuntimeError('window rectangle output was not captured')
                uc.mem_write(rect,sample[offset:offset+16])
        self.index+=1
        return words,e['result']
