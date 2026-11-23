"""Decode construction jobs and emitters from the initial boundary only."""
import math
import struct
from captured_memory import initial_memory_reader


def initial_construction(frame):
    if not frame.get('game_memory'):
        return None
    read = initial_memory_reader(frame)
    word = lambda a: struct.unpack('<I', read(a,4))[0]
    real = lambda a: struct.unpack('<f', read(a,4))[0]
    runtime = frame['runtime_state']
    units = {u['address']: u for u in runtime['units']}
    if len(units) != len(runtime['units']):
        raise ValueError('duplicate construction unit address')
    result = {'sites': [], 'builders': [], 'flying_builders': [], 'emitters': [], 'waiting': []}
    for address, unit in units.items():
        remaining = real(address+0x108)
        if not math.isfinite(remaining) or not 0 <= remaining <= 1:
            raise ValueError('invalid construction remainder')
        if remaining:
            kind = word(address+0xb4)
            inverse, cost = real(kind+0x216), real(kind+0x20e)
            if not all(math.isfinite(v) and 0 <= v <= 1e9 for v in (inverse,cost)):
                raise ValueError('invalid construction type')
            result['sites'].append({'id': unit['id'], 'remaining': remaining,
                'hp': word(address+0x10c)&65535, 'events': word(address+0xd0),
                'flags': word(address+0x130), 'inverse_time': inverse,
                'cost': cost, 'max_hp': word(kind+0x1be)})
        primary = unit.get('primary',0)
        mission = next((m for m in runtime['missions'] if m['address']==primary),None)
        if mission and mission['handler']==0x402220:
            data=bytes.fromhex(mission['fields_hex'])
            if len(data)<0x6e or data[5]>2 or not remaining:
                raise ValueError('invalid GetBuilt waiting mission')
            owner,builder=struct.unpack_from('<I',data,0xe)[0],struct.unpack_from('<I',data,0x16)[0]
            if owner!=address or (builder and builder not in units):
                raise ValueError('invalid GetBuilt owner/builder')
            result['waiting'].append({'id':unit['id'],'builder':units[builder]['id'] if builder else 0,
                'stage':data[5],'wait':struct.unpack_from('<I',data,6)[0],
                'deadline':struct.unpack_from('<I',data,0xa)[0],
                'pending':struct.unpack_from('<I',data,0x6a)[0]})
        if mission and mission['handler'] in (0x405560,0x41ef00):
            data = bytes.fromhex(mission['fields_hex'])
            if len(data)<0x6e:
                raise ValueError('truncated construction mission')
            flying=mission['handler']==0x41ef00
            if (not flying and data[5]==3) or (flying and data[5] in (5,6)):
                owner,target = struct.unpack_from('<I',data,0xe)[0],struct.unpack_from('<I',data,0x16)[0]
                if owner != address or target not in units:
                    raise ValueError('invalid construction mission owner/target')
                worker = real(word(address+0xb4)+0x21a)
                if not math.isfinite(worker) or not 0 <= worker <= 1e9:
                    raise ValueError('invalid construction worker')
                job={'id':unit['id'],'target':units[target]['id'],
                    'worker_time':worker,'wait':struct.unpack_from('<I',data,6)[0],
                    'deadline':struct.unpack_from('<I',data,0xa)[0],
                    'pending':struct.unpack_from('<I',data,0x6a)[0],
                    'repeat':struct.unpack_from('<i',data,0x52)[0],
                    'activity_deadline':word(address+0xcc),
                    'working':bool(read(address+0x114,1)[0]&8)}
                if flying:
                    job.update(stage=data[5],owner_flags=word(address+0x130),
                               flags=struct.unpack_from('<I',data,0x5a)[0],events=unit.get('events',0))
                    kind=word(address+0xb4)
                    job['slow_speed'],job['fast_speed']=struct.unpack('<2i',read(kind+0x182,8))
                    job['build_distance']=struct.unpack('<H',read(kind+0x230,2))[0]
                result['flying_builders' if flying else 'builders'].append(job)
        visual = word(address+0xc0)
        emitter = word(visual+0x174) if visual else 0
        if not emitter:
            continue
        head,count,capacity,owner,radius,height = struct.unpack('<6I',read(emitter+8,24))
        if owner != address or not 0 <= count <= capacity <= 100000:
            raise ValueError('invalid construction emitter owner/capacity')
        particles=[];seen=set();node=word(head)
        while node != head:
            if node in seen or len(particles)>=count:
                raise ValueError('invalid construction particle list')
            seen.add(node)
            particle_owner = word(node+8)
            if particle_owner != address:
                raise ValueError('invalid construction particle owner')
            particles.append(list(struct.unpack('<5i',read(node+12,20))))
            node=word(node)
        if len(particles)!=count:
            raise ValueError('construction particle count disagrees')
        result['emitters'].append({'id':unit['id'],'capacity':capacity,
            'radius':struct.unpack('<i',struct.pack('<I',radius))[0],
            'height':struct.unpack('<i',struct.pack('<I',height))[0],
            'particles':particles})
    sites = {s['id'] for s in result['sites']}
    if any(b['target'] not in sites for b in result['builders']+result['flying_builders']):
        raise ValueError('active construction target is complete')
    return result
