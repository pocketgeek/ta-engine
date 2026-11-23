"""Decode AI ownership and schedules from a single captured boundary."""
import base64
import struct
import zlib


def initial_ai_state(frame,players):
    regions=frame.get('game_memory',[])
    if not regions: return None
    decoded={}
    def read(address,size):
        out=bytearray()
        while len(out)<size:
            cursor=address+len(out)
            region=next((r for r in regions if r['address']<=cursor<r['address']+r['size']),None)
            if region is None: raise ValueError(f'initial AI memory absent at {cursor:#x}')
            base=region['address']; extent=region['size']
            if not 0<extent<=32*1024*1024: raise ValueError('invalid AI memory extent')
            if base not in decoded:
                decoder=zlib.decompressobj()
                blob=decoder.decompress(base64.b64decode(region['zlib_base64'],validate=True),extent+1)
                if len(blob)!=extent or not decoder.eof or decoder.unused_data:
                    raise ValueError('invalid compressed AI memory')
                decoded[base]=blob
            offset=cursor-base;count=min(size-len(out),extent-offset)
            out.extend(decoded[base][offset:offset+count])
        return bytes(out)
    word=lambda a:struct.unpack('<I',read(a,4))[0]
    runtime=frame['runtime_state']
    game=next(r['address'] for r in runtime['world_buffers'] if r['name']=='game_fields')
    units={u['address']:u['id'] for u in runtime['units']}
    handlers={0x40b320:0,0x40e180:1,0x40ea30:2,0x40eb40:3,0x4101e0:4}
    def anchors():
        result=[]
        for global_address,sign in ((0x62dbcc,-1),(0x62dbdc,1)):
            begin,end,capacity=struct.unpack('<3I',read(global_address,12))
            if begin>end or end>capacity or (end-begin)%101 or capacity-begin>101*65536:
                raise ValueError('invalid AI anchor vector')
            for i in range((end-begin)//101):
                record=read(begin+i*101,5)
                if record[0]:result.append([sign*(i+1),*struct.unpack_from('<hh',record,1)])
        return result
    result=[]
    settings=word(word(0x62d558))
    for player in range(players):
        owner=game+0x2404+player*0x110
        ai=word(owner+0x80)
        enabled=(ai and word(owner)!=0 and read(owner+0xea,1)==b'\x02' and
                 read(owner+0xe3,1)!=b'\x00' and read(settings+0xb,1)!=b'\x00')
        if not enabled: result.append(None);continue
        if word(ai)!=owner: raise ValueError('AI manager belongs to another player')
        group_base=word(owner+0x84);seen=set();groups=[]
        for slot in range(100):
            pointer=word(ai+0x11+slot*4)
            if not pointer: continue
            data=word(pointer+8)
            if word(pointer+4)!=ai or data!=group_base+slot*196 or word(data+4)!=slot:
                raise ValueError('AI squad ownership/index disagrees')
            handler=word(word(pointer))
            if handler not in handlers: raise ValueError(f'unsupported AI squad handler {handler:#x}')
            begin,end,capacity=struct.unpack('<3I',read(data+0xb8,12))
            if begin>end or end>capacity or (end-begin)%4 or (capacity-begin)>80000:
                raise ValueError('invalid AI member vector')
            members=[]
            for address in struct.unpack(f'<{(end-begin)//4}I',read(begin,end-begin)):
                if address not in units or address in seen: raise ValueError('missing/duplicate AI member')
                seen.add(address);members.append(units[address])
            groups.append({'slot':slot,'kind':handlers[handler],'deadline':word(pointer+12),
                           'active':word(data+8),'dirty':word(data+0xb0),
                           'parameters':list(struct.unpack('<9i',read(data+12,36))), 'members':members})
        side=read(ai+4,1)[0]
        if side>=10: raise ValueError('invalid AI side')
        result.append({'countdown':struct.unpack('<i',read(ai+5,4))[0],
                       'initialized':int(read(ai+0x1a6,1)!=b'\x00'),
                       'scenario_deadline':word(word(game+0x175dc)+0xd55+side*12), 'groups':groups, 'anchors':anchors()})
    return result
