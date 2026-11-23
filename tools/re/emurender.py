"""Replay recorded external clock inputs; never synthesize time or RNG calls."""
import struct


def restore_thread_segment(process, teb):
    """Point x86 FS at the captured TEB using a diagnostic GDT descriptor."""
    from emu import STACK, STACK_SZ
    from unicorn.x86_const import (UC_X86_REG_GDTR, UC_X86_REG_FS,
                                  UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS)
    if process.u32(teb+0x18) != teb:
        raise ValueError('captured TEB self pointer differs')
    # Reserved emulator stack space above the harness's highest call frame.
    gdt = STACK+STACK_SZ-256
    descriptor = struct.pack('<HHBBBB',0xffff,teb & 0xffff,
                             (teb>>16)&255,0x93,0xcf,(teb>>24)&255)
    code=struct.pack('<HHBBBB',0xffff,0,0,0x9b,0xcf,0)
    data=struct.pack('<HHBBBB',0xffff,0,0,0x93,0xcf,0)
    process.uc.mem_write(gdt,bytes(8)+code+data+descriptor)
    process.uc.reg_write(UC_X86_REG_GDTR,(0,gdt,31,0))
    # Explicit flat 32-bit stack/code segments are required when enabling the
    # GDT; otherwise Unicorn may retain a 16-bit default stack segment.
    for reg,selector in ((UC_X86_REG_CS,8),(UC_X86_REG_DS,16),(UC_X86_REG_ES,16),
                         (UC_X86_REG_SS,16),(UC_X86_REG_FS,24)):
        process.uc.reg_write(reg,selector)


class CapturedClock:
    def __init__(self, process, capture):
        self.process = process
        self.events = capture.get('performance_calls', [])
        self.index = 0
        end = capture.get('performance_counter_return')
        if not end:
            raise ValueError('capture has no identified performance-counter return')
        entry = end - 12
        code = bytes(process.uc.mem_read(entry, 15))
        if code[0] != 0xb8 or code[5] != 0xba or code[10:] != b'\xff\xd2\xc2\x08\x00':
            raise ValueError('captured performance-counter syscall stub differs')
        process.icd.hooks[entry] = self.query

    def query(self, uc, argv):
        if self.index >= len(self.events):
            raise RuntimeError('recorded performance-counter inputs exhausted')
        event = self.events[self.index]
        p = self.process
        caller = p.u32(argv-4)
        tick = p.u32(p.game+0x19f44)
        counter, frequency = p.u32(argv), p.u32(argv+4)
        if caller != event['return_address'] or tick != event['tick']:
            raise RuntimeError(f'clock input order differs at {self.index}: '
                               f'caller={caller:#x}, tick={tick}, expected={event}')
        if bool(counter) != bool(event['counter_address']) or bool(frequency) != bool(event['frequency_address']):
            raise RuntimeError(f'clock output arguments differ at {self.index}')
        if event['status'] == 0:
            uc.mem_write(counter, struct.pack('<q', event['counter']))
            if frequency:
                uc.mem_write(frequency, struct.pack('<q', event['frequency']))
        self.index += 1
        return 2, event['status']


class CapturedCursor:
    """Replay only NtUserGetCursorPos's defined BOOL and POINT output."""
    def __init__(self,process,capture,entry):
        self.process=process
        self.events=[e for e in capture.get('syscall_calls',[]) if e['stub_return']==entry+12]
        self.index=0
        code=bytes(process.uc.mem_read(entry,15))
        if code[0]!=0xb8 or code[5]!=0xba or code[10:]!=b'\xff\xd2\xc2\x04\x00':
            raise ValueError('captured cursor syscall stub differs')
        process.icd.hooks[entry]=self.query

    def query(self,uc,argv):
        if self.index>=len(self.events):
            raise RuntimeError('recorded cursor inputs exhausted')
        event=self.events[self.index]; p=self.process
        if p.u32(argv-4)!=event['return_address'] or p.u32(p.game+0x19f44)!=event['tick']:
            raise RuntimeError(f'cursor input order differs at {self.index}')
        output=p.u32(argv)
        if len(event['arguments'])!=1 or bool(output)!=bool(event['arguments'][0]):
            raise RuntimeError('cursor output argument differs')
        if event['result']:
            data=bytes.fromhex(event['pointer_samples'].get('0',''))
            if not output or len(data)<8:
                raise RuntimeError('successful cursor input lacks its POINT output')
            # Additional sampled bytes may be caller stack or game state;
            # they are not API outputs and must never be injected.
            uc.mem_write(output,data[:8])
        self.index+=1
        return 1,event['result']


class CapturedMonitor:
    """Replay Wine NtUserCallTwoParam monitor queries (codes 4 and 2)."""
    def __init__(self,process,capture,entry):
        self.process=process
        self.events=[e for e in capture.get('syscall_calls',[]) if e['stub_return']==entry+12]
        self.index=0
        code=bytes(process.uc.mem_read(entry,15))
        if code[0]!=0xb8 or code[5]!=0xba or code[10:]!=b'\xff\xd2\xc2\x0c\x00':
            raise ValueError('captured monitor syscall stub differs')
        process.icd.hooks[entry]=self.query

    def query(self,uc,argv):
        if self.index>=len(self.events): raise RuntimeError('recorded monitor inputs exhausted')
        event=self.events[self.index]; p=self.process
        if p.u32(argv-4)!=event['return_address'] or p.u32(p.game+0x19f44)!=event['tick']:
            raise RuntimeError(f'monitor input order differs at {self.index}')
        a,b,kind=struct.unpack('<III',uc.mem_read(argv,12))
        expected=event['arguments']
        if len(expected)!=3 or kind!=expected[2]: raise RuntimeError('monitor operation differs')
        if kind==4:  # MonitorFromRect, also used by MonitorFromPoint.
            sample=bytes.fromhex(event['pointer_samples'].get('0',''))
            if b!=expected[1] or len(sample)<16 or bytes(uc.mem_read(a,16))!=sample[:16]:
                raise RuntimeError('monitor rectangle input differs')
        elif kind==2:  # GetMonitorInfoW.
            sample=bytes.fromhex(event['pointer_samples'].get('1',''))
            if a!=expected[0] or not b or len(sample)<4 or p.u32(b)!=struct.unpack_from('<I',sample)[0]:
                raise RuntimeError('monitor information input differs')
            if event['result']:
                size=p.u32(b)
                if size not in (40,104) or len(sample)<size:
                    raise RuntimeError('unsupported monitor information output size')
                end=40
                if size==104:
                    for end in range(42,105,2):
                        if sample[end-2:end]==b'\0\0': break
                    else: raise RuntimeError('unterminated monitor device name')
                # cbSize is caller input; name-buffer padding is not output.
                uc.mem_write(b+4,sample[4:end])
        else:
            raise RuntimeError(f'unsupported NtUserCallTwoParam operation {kind}')
        self.index+=1
        return 3,event['result']


class CapturedDisplaySettings:
    """Replay NtUserEnumDisplaySettings with validated monitor-name provenance."""
    def __init__(self,process,capture,entry,monitor_entry):
        self.process=process; self.index=0; self.events=[]
        monitor=None
        for event in capture.get('syscall_calls',[]):
            if event['stub_return']==monitor_entry+12 and event['arguments'][2]==2:
                monitor=event
            if event['stub_return']==entry+12: self.events.append((event,monitor))
        code=bytes(process.uc.mem_read(entry,15))
        if code[0]!=0xb8 or code[5]!=0xba or code[10:]!=b'\xff\xd2\xc2\x10\x00':
            raise ValueError('captured display settings syscall stub differs')
        process.icd.hooks[entry]=self.query

    def query(self,uc,argv):
        if self.index>=len(self.events): raise RuntimeError('recorded display settings inputs exhausted')
        event,monitor=self.events[self.index]; p=self.process
        if p.u32(argv-4)!=event['return_address'] or p.u32(p.game+0x19f44)!=event['tick']:
            raise RuntimeError(f'display settings input order differs at {self.index}')
        name,mode,output,flags=struct.unpack('<4I',uc.mem_read(argv,16))
        want=event['arguments']
        if len(want)!=4 or mode!=want[1] or flags!=want[3] or bool(name)!=bool(want[0]):
            raise RuntimeError('display settings arguments differ')
        if name:
            desc=bytes.fromhex(event['pointer_samples'].get('0',''))
            if len(desc)<8: raise RuntimeError('display name descriptor is missing')
            length,maximum,buffer=struct.unpack_from('<HHI',desc)
            actual_length,actual_maximum,actual_buffer=struct.unpack('<HHI',uc.mem_read(name,8))
            if (length,maximum)!=(actual_length,actual_maximum):
                raise RuntimeError('display name lengths differ')
            # In this call path the device name is the immediately preceding
            # GetMonitorInfoW output. Require the recorded pointer alias;
            # don't guess a name or copy unrelated snapshot memory.
            if not monitor or not monitor['result'] or buffer!=monitor['arguments'][1]+40:
                raise RuntimeError('display name lacks a captured monitor-output source')
            sample=bytes.fromhex(monitor['pointer_samples'].get('1',''))
            if length>64 or len(sample)<40+length or bytes(uc.mem_read(actual_buffer,length))!=sample[40:40+length]:
                raise RuntimeError('display name differs')
        sample=bytes.fromhex(event['pointer_samples'].get('2',''))
        if not output or len(sample)<72: raise RuntimeError('display settings output is missing')
        size,extra=struct.unpack_from('<HH',sample,68)
        actual_size,actual_extra=struct.unpack('<HH',uc.mem_read(output+68,4))
        if actual_size<188 or actual_extra or size!=188 or extra:
            raise RuntimeError(f'unsupported display settings structure: input={actual_size}/{actual_extra}, output={size}/{extra}')
        if event['result']:
            if len(sample)<size: raise RuntimeError('truncated display settings output')
            uc.mem_write(output,sample[:size])
        self.index+=1
        return 4,event['result']


class CapturedFormatProperties:
    """Replay Vulkan format capabilities, including the explicit flags3 chain."""
    def __init__(self,process,capture,entry):
        self.process=process; self.index=0
        # Verified Wine wrapper: its Unix bridge returns at entry+3f.
        self.events=[e for e in capture.get('unix_calls',[]) if e['return_address']==entry+0x3f]
        code=bytes(process.uc.mem_read(entry,0x3f))
        if code[:3]!=b'\x55\x89\xe5' or code[-6:-4]!=b'\xff\x15':
            raise ValueError('captured Vulkan format wrapper differs')
        process.icd.hooks[entry]=self.query

    def query(self,uc,argv):
        if self.index>=len(self.events): raise RuntimeError('recorded format capability inputs exhausted')
        event=self.events[self.index]; p=self.process
        if p.u32(p.game+0x19f44)!=event['tick']:
            raise RuntimeError(f'format capability input tick differs at {self.index}')
        device,format_id,output=struct.unpack('<III',uc.mem_read(argv,12))
        raw=bytes.fromhex(event['parameter_bytes'])
        if len(raw)<12 or event['number']!=0x220 or event['result']!=0:
            raise RuntimeError('unsupported format capability bridge result')
        want_device,want_format,captured_output=struct.unpack_from('<III',raw)
        if (device,format_id)!=(want_device,want_format):
            raise RuntimeError('format capability query arguments differ')
        writes=[]; visited=set()
        for depth in range(8):
            if not output or not captured_output or captured_output in visited:
                raise RuntimeError('format capability extension chain differs')
            visited.add(captured_output)
            sample=bytes.fromhex(event['pointer_samples'].get(str(captured_output),''))
            if len(sample)<8: raise RuntimeError('format capability output was not captured')
            kind,next_captured=struct.unpack_from('<II',sample)
            actual_kind,next_output=struct.unpack('<II',uc.mem_read(output,8))
            if kind!=actual_kind or bool(next_captured)!=bool(next_output):
                raise RuntimeError('format capability structure differs')
            size=20 if depth==0 and kind==1000059002 else 32 if depth>0 and kind==1000360000 else 0
            if not size or len(sample)<size: raise RuntimeError('unsupported format capability extension')
            writes.append((output+8,sample[8:size]))
            if not next_output: break
            output,captured_output=next_output,next_captured
        else: raise RuntimeError('format capability extension limit reached')
        # Validate the entire chain before changing any outputs; preserve
        # caller-provided sType/pNext, including relocated extension pointers.
        for address,data in writes: uc.mem_write(address,data)
        self.index+=1
        return 3,0


class CapturedMapView:
    """Replay observed map outputs; aliases and pre-call in/out values are unaudited."""
    def __init__(self,process,capture,entry):
        self.process=process; self.index=0
        if capture.get('map_view_return')!=entry+12:
            raise ValueError('capture lacks identified map-view return')
        self.events=[e for e in capture.get('syscall_calls',[]) if e['stub_return']==entry+12]
        code=bytes(process.uc.mem_read(entry,15))
        if code[0]!=0xb8 or code[5]!=0xba or code[10:]!=b'\xff\xd2\xc2\x28\x00':
            raise ValueError('captured map-view syscall stub differs')
        process.icd.hooks[entry]=self.query

    def query(self,uc,argv):
        import base64
        import zlib
        if self.index>=len(self.events): raise RuntimeError('recorded map-view inputs exhausted')
        event=self.events[self.index]; p=self.process
        if p.u32(argv-4)!=event['return_address'] or p.u32(p.game+0x19f44)!=event['tick']:
            raise RuntimeError(f'map-view input order differs at {self.index}')
        args=list(struct.unpack('<10I',uc.mem_read(argv,40))); expected=event['arguments']
        if any(args[i]!=expected[i] for i in (0,1,3,4,7,8,9)):
            raise RuntimeError('map-view scalar arguments differ')
        if any(bool(args[i])!=bool(expected[i]) for i in (2,5,6)):
            raise RuntimeError('map-view output pointers differ')
        if not event['result'] & 0x80000000:
            view=event.get('mapped_view')
            if not view: raise RuntimeError('successful map-view lacks captured bytes')
            address,size=view['address'],view['size']
            if not 0<size<=32*1024*1024 or address<0x10000 or address+size>0x100000000 or (address|size)&4095:
                raise RuntimeError('invalid recorded map-view range')
            for lo,hi in ((0x60000000,0x64000000),(0x70000000,0x70100000),(0x71000000,0x71400000)):
                if address<hi and address+size>lo: raise RuntimeError('map-view overlaps emulator reserved space')
            decoder=zlib.decompressobj()
            raw=decoder.decompress(base64.b64decode(view['zlib_base64'],validate=True),size+1)
            if len(raw)!=size or not decoder.eof or decoder.unused_data:
                raise RuntimeError('map-view decompressed size differs')
            offset=bytes.fromhex(view['section_offset']) if args[5] else None
            if offset is not None and len(offset)!=8: raise RuntimeError('map-view section offset differs')
            p.ensure(address,size)
            uc.mem_write(address,raw)
            uc.mem_write(args[2],struct.pack('<I',address))
            uc.mem_write(args[6],struct.pack('<I',size))
            if offset is not None: uc.mem_write(args[5],offset)
        self.index+=1
        return 10,event['result']


class CapturedUnmap:
    """Diagnostic NtUnmapViewOfSection result; retain inert offline pages.

    This does not reconstruct address-space lifetime or shared-view aliasing.
    It allows investigation past deallocation but prohibits a native-state
    parity claim. New mappings must still be implemented separately.
    """
    def __init__(self,process,capture,entry):
        self.process=process; self.index=0
        self.events=[e for e in capture.get('syscall_calls',[]) if e['stub_return']==entry+12]
        code=bytes(process.uc.mem_read(entry,15))
        if code[0]!=0xb8 or code[5]!=0xba or code[10:]!=b'\xff\xd2\xc2\x08\x00':
            raise ValueError('captured unmap syscall stub differs')
        process.icd.hooks[entry]=self.query

    def query(self,uc,argv):
        if self.index>=len(self.events): raise RuntimeError('recorded unmap inputs exhausted')
        event=self.events[self.index]; p=self.process
        if p.u32(argv-4)!=event['return_address'] or p.u32(p.game+0x19f44)!=event['tick']:
            raise RuntimeError(f'unmap input order differs at {self.index}')
        if list(struct.unpack('<II',uc.mem_read(argv,8)))!=event['arguments']:
            raise RuntimeError('unmap arguments differ')
        self.index+=1
        return 2,event['result']
