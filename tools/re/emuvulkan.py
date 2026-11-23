"""Recorded Vulkan image API contracts for CPU-side replay investigation.

No GPU work is submitted. Resource handles are external recorded outputs;
worker concurrency, shared mappings and presentation remain outside this model.
Structure layouts below use the i386 Vulkan ABI from vulkan_core.h.
"""
import struct


def captured_bytes(event,address,size):
    regions=[(event['parameters'],bytes.fromhex(event['parameter_bytes']))]
    regions.extend((int(a),bytes.fromhex(data)) for a,data in event['pointer_samples'].items())
    for base,data in regions:
        offset=address-base
        if 0<=offset and offset+size<=len(data): return data[offset:offset+size]
    raise RuntimeError(f'Vulkan pointee was not captured: {address:#x}, {size} bytes')


class CapturedVulkanImages:
    # Unix function number: (PE entry RVA, bridge-return offset, API words).
    SPECS={548:(0x1dc50,0x50,3),342:(0x13b60,0x5b,4),
           503:(0x1bb50,0x50,3),17:(0xacd0,0x57,7),397:(0x165a0,0x51,4),
           398:(0x16650,0x51,4),681:(0x235a0,0x60,6),650:(0x22200,0x3f,3),
           395:(0x16440,0x51,4),416:(0x172c0,0x51,4),420:(0x17580,0x51,4),
           569:(0x1ea70,0x50,3),572:(0x1ec80,0x5d,5),573:(0x1ed40,0x5d,5),
           610:(0x207a0,0x5d,5),339:(0x13930,0x5b,4),364:(0x14df0,0x5b,4),
           367:(0x15010,0x5b,4),4:(0xa3b0,0x5d,10)}

    def __init__(self,process,capture,base):
        self.p=process; self.index=0
        self.events=[e for e in capture.get('unix_calls',[]) if e['number'] in self.SPECS]
        for number,(rva,offset,words) in self.SPECS.items():
            entry=base+rva
            for e in self.events:
                if e['number']==number and e['return_address']!=entry+offset:
                    raise ValueError('captured Vulkan image wrapper address differs')
            process.icd.hooks[entry]=lambda uc,a,n=number,w=words:self.query(uc,a,n,w)

    def input_chain(self,event,actual,expected,allowed):
        seen=set()
        for _ in range(8):
            if not actual or not expected or expected in seen:
                raise RuntimeError('Vulkan input extension chain differs')
            seen.add(expected)
            kind,next_expected=struct.unpack('<II',captured_bytes(event,expected,8))
            actual_kind,next_actual=struct.unpack('<II',self.p.uc.mem_read(actual,8))
            if kind!=actual_kind or kind not in allowed or bool(next_actual)!=bool(next_expected):
                raise RuntimeError('unsupported or mismatched Vulkan input structure')
            size=allowed[kind]
            want=captured_bytes(event,expected,size)
            got=bytes(self.p.uc.mem_read(actual,size))
            if kind in (1000147000,1000275002):  # Format or present-mode list.
                count,formats=struct.unpack_from('<II',want,8)
                actual_count,actual_formats=struct.unpack_from('<II',got,8)
                if count!=actual_count or count>256 or (count and bytes(self.p.uc.mem_read(actual_formats,count*4))!=captured_bytes(event,formats,count*4)):
                    raise RuntimeError('Vulkan view-format list differs' if kind==1000147000 else 'Vulkan present-mode list differs')
            elif kind==1000001000:  # VkSwapchainCreateInfoKHR, i386 alignment.
                if got[8:12]!=want[8:12] or got[16:60]!=want[16:60] or got[64:88]!=want[64:88]:
                    raise RuntimeError('Vulkan swapchain creation fields differ')
                count=struct.unpack_from('<I',want,56)[0]
                if count>256: raise RuntimeError('Vulkan queue-family limit reached')
                if count:
                    ptr=struct.unpack_from('<I',want,60)[0]
                    actual_ptr=struct.unpack_from('<I',got,60)[0]
                    if bytes(self.p.uc.mem_read(actual_ptr,count*4))!=captured_bytes(event,ptr,count*4):
                        raise RuntimeError('Vulkan queue-family list differs')
            elif kind==14:  # VkImageCreateInfo: optional queue-family array.
                if got[8:60]!=want[8:60] or got[64:68]!=want[64:68]:
                    raise RuntimeError('Vulkan image creation fields differ')
                count=struct.unpack_from('<I',want,56)[0]
                if count>256: raise RuntimeError('Vulkan queue-family limit reached')
                if count:
                    ptr=struct.unpack_from('<I',want,60)[0]
                    actual_ptr=struct.unpack_from('<I',got,60)[0]
                    if bytes(self.p.uc.mem_read(actual_ptr,count*4))!=captured_bytes(event,ptr,count*4):
                        raise RuntimeError('Vulkan queue-family list differs')
            elif got[8:]!=want[8:]:
                raise RuntimeError('Vulkan input fields differ')
            if not next_actual: return
            actual,expected=next_actual,next_expected
        raise RuntimeError('Vulkan input extension limit reached')

    def output_chain(self,event,actual,expected,allowed):
        writes=[]; seen=set()
        for _ in range(8):
            if not actual or not expected or expected in seen:
                raise RuntimeError('Vulkan output extension chain differs')
            seen.add(expected)
            kind,next_expected=struct.unpack('<II',captured_bytes(event,expected,8))
            actual_kind,next_actual=struct.unpack('<II',self.p.uc.mem_read(actual,8))
            if kind!=actual_kind or kind not in allowed or bool(next_actual)!=bool(next_expected):
                raise RuntimeError('unsupported or mismatched Vulkan output structure')
            end=allowed[kind]
            if kind==1000274002:  # VkSurfacePresentModeCompatibilityEXT/KHR.
                count,pointer=struct.unpack('<II',captured_bytes(event,expected+8,8))
                capacity,destination=struct.unpack('<II',self.p.uc.mem_read(actual+8,8))
                if bool(pointer)!=bool(destination) or count>64 or (pointer and capacity<count):
                    raise RuntimeError('Vulkan present-mode output capacity differs')
                writes.append((actual+8,struct.pack('<I',count)))
                if pointer: writes.append((destination,captured_bytes(event,pointer,count*4)))
            else:
                writes.append((actual+8,captured_bytes(event,expected+8,end-8)))
            if not next_actual: return writes
            actual,expected=next_actual,next_expected
        raise RuntimeError('Vulkan output extension limit reached')

    def query(self,uc,argv,number,words):
        if self.index>=len(self.events): raise RuntimeError('recorded Vulkan image inputs exhausted')
        event=self.events[self.index]; p=self.p
        if event['number']!=number or event['tick']!=p.u32(p.game+0x19f44):
            raise RuntimeError(f'Vulkan image call order differs at {self.index}')
        if event['result']!=0: raise RuntimeError('unsupported Wine Vulkan bridge failure')
        raw=bytes.fromhex(event['parameter_bytes'])
        args=struct.unpack(f'<{words}I',uc.mem_read(argv,words*4))
        want=struct.unpack_from('<'+str(len(raw)//4)+'I',raw)
        if args[0]!=want[0]: raise RuntimeError('Vulkan device differs')
        writes=[]; result=0
        if number==548:
            self.input_chain(event,args[1],want[1],{1000059004:28})
            result=want[3]
            if result==0: writes=self.output_chain(event,args[2],want[2],{1000059003:40})
        elif number in (339,342,364,367):
            if args[2] or want[2]: raise RuntimeError('custom Vulkan allocator is unsupported')
            structures={339:{8:12},342:{14:68,1000147000:16},364:{9:12},
                        367:{1000001000:88,1000275002:16,1000147000:16}}
            self.input_chain(event,args[1],want[1],structures[number])
            result=want[4]
            if result==0:
                if not args[3]: raise RuntimeError('missing Vulkan image output')
                writes=[(args[3],captured_bytes(event,want[3],8))]
        elif number==503:
            self.input_chain(event,args[1],want[1],{1000146001:16})
            writes=self.output_chain(event,args[2],want[2],{1000146003:28,1000127000:16})
        elif number==17:
            if tuple(args[1:])!=tuple(want[2:8]): raise RuntimeError('Vulkan image binding arguments differ')
            result=want[8]
        elif number in (395,397,398,416,420):
            if args[3] or want[4]: raise RuntimeError('custom Vulkan allocator is unsupported')
            if tuple(args[1:])!=tuple(want[2:5]): raise RuntimeError('Vulkan image destruction arguments differ')
        elif number in (650,681):
            count=args[1]
            if count!=want[1] or not 1<=count<=32:
                raise RuntimeError('Vulkan fence count differs or exceeds captured bound')
            if bytes(uc.mem_read(args[2],count*8))!=captured_bytes(event,want[2],count*8):
                raise RuntimeError('Vulkan fence handles differ')
            if number==681 and tuple(args[3:])!=tuple(want[3:6]):
                raise RuntimeError('Vulkan fence wait mode or timeout differs')
            result=want[words]
        elif number==569:
            self.input_chain(event,args[1],want[1],{1000119000:16,1000274000:12})
            result=want[3]
            if result==0:
                writes=self.output_chain(event,args[2],want[2],{1000119001:60,1000274002:16})
        elif number in (572,573,610):
            if tuple(args[1:3])!=tuple(want[2:4]) or bool(args[4])!=bool(want[5]):
                raise RuntimeError('Vulkan enumeration handle or output mode differs')
            result=want[6]
            if result in (0,5):
                count=struct.unpack('<I',captured_bytes(event,want[4],4))[0]
                stride=4 if number==573 else 8
                if count*stride>256 or (args[4] and p.u32(args[3])<count):
                    raise RuntimeError('Vulkan enumeration exceeds captured output capacity')
                writes=[(args[3],struct.pack('<I',count))]
                if args[4]: writes.append((args[4],captured_bytes(event,want[5],count*stride)))
        elif number==4:
            if tuple(args[1:9])!=tuple(want[2:10]):
                raise RuntimeError('Vulkan acquire-image handles or timeout differ')
            result=want[11]
            if result in (0,1000001003):
                writes=[(args[9],captured_bytes(event,want[10],4))]
        for address,data in writes: uc.mem_write(address,data)
        self.index+=1
        return words,result
