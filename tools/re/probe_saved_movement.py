#!/usr/bin/env python3
"""Run an explicitly partial movement restore against a captured retail sequence.

This diagnoses importer/simulation gaps. It cannot certify save or game parity.
The output directory must be new; retail-derived artifacts stay local.
"""
import argparse
import base64
from collections import Counter
import json
from pathlib import Path
import struct
import subprocess
import zlib

from decode_save_state import decode
from decode_ai_state import initial_ai_state
from decode_player_cache import initial_build_caches, initial_build_planner
from decode_construction import initial_construction
from decode_features import initial_feature_presence
from captured_memory import initial_memory_reader


def initial_crusades(frame):
    # 5174e0 selects unitsCB versus units at 4bf540 during type loading.
    if not frame.get('game_memory'):
        return False  # Legacy partial captures did not preserve this global.
    return bool(initial_memory_reader(frame)(0x641144,1)[0])


def initial_exploration(frame):
    """Restore known terrain and each unit's sight cache from the initial boundary."""
    read=initial_memory_reader(frame)
    runtime=frame['runtime_state']
    game=next(b['address'] for b in runtime['world_buffers'] if b['name']=='game_fields')
    width,height=struct.unpack('<2I',read(game+0x19e98,8))
    viewer=read(game+0x306f,1)[0]
    if not width or not height or width%2 or height%2 or width*height>64*1024*1024 or viewer>31:
        raise ValueError('invalid captured exploration geometry/context')
    width//=2; height//=2
    pointer=struct.unpack('<I',read(game+0x19ef4,4))[0]
    masks=struct.unpack('<'+'H'*(width*height),read(pointer,width*height*2))
    addresses={unit['id']:unit['address'] for unit in runtime['units']}
    sight=[]
    for unit in frame['units']:
        fields=struct.unpack('<hhihBB',read(addresses[unit['id']]+0x98,12))
        sight.append((unit['id'],*fields))
    return width,height,viewer,masks,sight


def initial_wind(frame):
    """Read one capture boundary, never later frames or another capture's seed."""
    if 'crt_seed' not in frame:
        return None
    game = next(b for b in frame['runtime_state']['world_buffers'] if b['name']=='game_fields')
    address = game['address']
    for region in [game, *frame.get('game_memory', [])]:
        if region['address'] <= address and region['address']+region['size'] >= address+0x19f74:
            data=zlib.decompress(base64.b64decode(region['zlib_base64']))
            offset=address-region['address']
            minimum,maximum=struct.unpack_from('<ii',data,offset+0x19ec4)
            deadline=struct.unpack_from('<I',data,offset+0x19f58)[0]
            x,_,z,speed,heading,flags=struct.unpack_from('<4iHH',data,offset+0x19f60)
            return [minimum,maximum,deadline,x,z,speed,heading,flags,frame['crt_seed']]
    return None


def initial_player_caches(frame,players):
    """Use only initial captured globals/objects, never later RNG call owners."""
    regions=frame.get('game_memory',[])
    decoded={}
    def read(address,size):
        out=bytearray()
        while len(out)<size:
            cursor=address+len(out)
            region=next((r for r in regions if r['address']<=cursor<r['address']+r['size']),None)
            if region is None: return None
            base=region['address']
            if base not in decoded:
                decoded[base]=zlib.decompress(base64.b64decode(region['zlib_base64']))
                if len(decoded[base])!=region['size']: raise ValueError('truncated captured game memory')
            offset=cursor-base;count=min(size-len(out),region['size']-offset)
            out.extend(decoded[base][offset:offset+count])
        return bytes(out)
    table=read(0x62a33c,players*4)
    if table is None: return None
    game=next(b['address'] for b in frame['runtime_state']['world_buffers'] if b['name']=='game_fields')
    result=[]
    for player,pointer in enumerate(struct.unpack(f'<{players}I',table)):
        if not pointer:
            result.append([0,0]);continue
        manager=read(pointer,0x105)
        owner=read(game+0x2404+player*0x110,0xf0)
        if manager is None or owner is None: return None
        if struct.unpack_from('<I',manager)[0]!=game+0x2404+player*0x110 or manager[4]!=player:
            raise ValueError('captured player cache owner disagrees with global table')
        enabled=owner[0xea] in (1,2,3) and owner[0xeb]!=10
        result.append([int(enabled),struct.unpack_from('<I',manager,0x101)[0]])
    return result


def movement_goal(unit, order):
    if order['controller_kind'] != 4 or 'controller_runtime_fields' not in order:
        raise ValueError('ground movement probe requires the decoded cell-goal controller')
    # 0x4e2820 converts the controller's origin cell and the unit's footprint
    # into its navigation point. This differs from the original click (+22).
    cx, cz = struct.unpack('<hh', struct.pack('<I', order['controller_runtime_fields']['08']))
    fx, fz = unit['footprint_size']
    return (cx * 16 + fx * 8) * 65536, (cz * 16 + fz * 8) * 65536



def captured_mission(runtime, identity, handler):
    unit = next((u for u in runtime.get('units', []) if u['id'] == identity), None)
    if unit is None:
        return None
    mission = next((m for m in runtime.get('missions', []) if m['address'] == unit['primary']), None)
    if mission is None or mission['handler'] != handler:
        return None
    fields = bytes.fromhex(mission['fields_hex'])
    if len(fields) < 0x6e:
        raise ValueError('truncated captured mission')
    return unit, fields


def captured_type_name(frame, table, index):
    """Resolve queued types too: compact prefixes contain only existing units."""
    address = table + 676 * index
    for record in frame['runtime_state']['type_prefixes']:
        if record['address'] == address:
            return bytes.fromhex(record['fields_hex'])[32:64].split(b'\0', 1)[0].decode('ascii')
    for region in frame.get('game_memory', []):
        offset = address + 32 - region['address']
        if 0 <= offset and offset + 32 <= region['size']:
            data = zlib.decompress(base64.b64decode(region['zlib_base64']))
            return data[offset:offset+32].split(b'\0', 1)[0].decode('ascii')
    raise ValueError(f'captured type {index} at {address:#x} is unavailable')


def ground_mission_state(runtime, identity):
    """Restore ordinary ground stages, including attack-response/return state."""
    record = captured_mission(runtime, identity, 0x402b00)
    if record is None:
        return None
    unit, fields = record
    word = lambda offset: struct.unpack_from('<I', fields, offset)[0]
    if fields[5] > 3:
        return None
    return [fields[5], word(6), word(10), word(0x6a), word(0x5a), word(0x4e), unit['events'],
            struct.unpack_from('<i',fields,0x52)[0],
            *struct.unpack_from('<2h',fields,0x2e),
            struct.unpack_from('<h',fields,0x24)[0],struct.unpack_from('<h',fields,0x2c)[0]]


def initial_sector_links(runtime):
    """Restore cached sector references from the initial entity pool."""
    game=next(b for b in runtime['world_buffers'] if b['name']=='game_fields')
    fields=zlib.decompress(base64.b64decode(game['zlib_base64']))
    base,width=struct.unpack_from('<II',fields,0x19f18)
    pool=runtime['entity_pool']
    data=zlib.decompress(base64.b64decode(pool['zlib_base64']))
    result=[]
    for unit in runtime['units']:
        offset=unit['address']-pool['address']
        if offset<0 or offset+0xa8>len(data): raise ValueError('sector owner outside captured entity pool')
        address=struct.unpack_from('<I',data,offset+0xa4)[0]
        if not address: continue
        index,remainder=divmod(address-base,10)
        if remainder or index<0 or not width: raise ValueError('invalid captured sector reference')
        result.append((unit['id'],index%width,index//width))
    return result


def factory_unit_values(runtime):
    """Initial unit-side COB flags; they are not part of the saved VM blob."""
    pool=runtime['entity_pool']
    data=zlib.decompress(base64.b64decode(pool['zlib_base64']))
    if len(data)!=pool['size']:
        raise ValueError('truncated captured entity pool')
    values={}
    for unit in runtime['units']:
        offset=unit['address']-pool['address']
        if offset<0 or offset+0x130>len(data):
            raise ValueError('script owner outside captured entity pool')
        flags=data[offset+0x12f]
        values[unit['id']]=[data[offset+0x114]&1,flags&1,(flags>>2)&1,(flags>>3)&1,
                            struct.unpack_from('<I',data,offset+0x100)[0]]
    return values


def standby_mission_state(runtime, identity):
    record = captured_mission(runtime, identity, 0x407770)
    if record is None:
        return None
    unit, fields = record
    word = lambda offset: struct.unpack_from('<I', fields, offset)[0]
    if fields[5] > 1:
        return None
    return [fields[5], word(6), word(10), word(0x6a), word(0x5a), 0, unit['events']]


def vtol_standby_state(runtime, identity, actual):
    record=captured_mission(runtime,identity,0x417350)
    if record is None or record[1][5]>2: return None
    unit,fields=record
    word=lambda offset:struct.unpack_from('<I',fields,offset)[0]
    return [fields[5],word(6),word(10),word(0x6a),word(0x5a),actual['flags']&3,
            unit['events'],actual['position_raw'][1]]


def builder_approach(runtime, unit, actual):
    record = captured_mission(runtime, unit['id'], 0x405560)
    if record is None or record[1][5] != 1:
        return None
    order = unit['orders'][0]
    if order['controller_kind'] != 6 or 'controller_runtime_fields' not in order:
        return None
    fields = record[1]
    type_index = struct.unpack_from('<I', fields, 0x4e)[0]
    game = next(b for b in runtime['world_buffers'] if b['name'] == 'game_fields')
    game_bytes = zlib.decompress(base64.b64decode(game['zlib_base64']))
    table = struct.unpack_from('<I', game_bytes, 0x175c4)[0]
    target = next((t for t in runtime['type_prefixes'] if t['address'] == table+676*type_index), None)
    if target is None:
        return None
    name = bytes.fromhex(target['fields_hex'])[32:64].split(b'\0',1)[0].decode('ascii')
    bounds = [struct.unpack('<i',struct.pack('<I',order['controller_runtime_fields'][key]))[0]
              for key in ('08','0c','10','14')]
    a,b,c,d = bounds
    x,z = actual['cell_origin']
    cx,cz = min(max(x,a),b), min(max(z,c),d)
    if a<x<b and c<z<d:
        distances = (x-a,b-x,d-z,z-c)
        side = distances.index(min(distances))
        cx,cz = ((a,z),(b,z),(x,d),(x,c))[side]
    fx,fz = unit['footprint_size']
    return {'type': name, 'bounds': bounds,
            'goal': [(cx*16+fx*8)*65536,(cz*16+fz*8)*65536],
            'site': [struct.unpack_from('<i',fields,offset)[0] for offset in (0x22,0x2a)],
            'wait': struct.unpack_from('<I',fields,6)[0],
            'pending': struct.unpack_from('<I',fields,0x6a)[0]}


def queued_builder_orders(runtime, unit):
    """Restore queued stage-zero builds behind an approach or active work."""
    game=next(b for b in runtime['world_buffers'] if b['name']=='game_fields')
    fields=zlib.decompress(base64.b64decode(game['zlib_base64']))
    table=struct.unpack_from('<I',fields,0x175c4)[0]
    names={t['address']:bytes.fromhex(t['fields_hex'])[32:64].split(b'\0',1)[0].decode('ascii')
           for t in runtime['type_prefixes']}
    result=[]
    for order in unit['orders'][1:]:
        state=order['runtime_fields']
        name=names.get(table+676*state['4e'])
        if order['type']!='MobileBuild' or state['05']!=0 or not name:
            break # preserve order: never skip an unsupported predecessor
        coords=[struct.unpack('<i',struct.pack('<I',state[key]))[0] for key in ('22','2a')]
        result.append((unit['id'],name,*coords))
    return result


def compare_frames(retail, port, *, flight_height=False):
    """Return the first actual difference; never silently align different ticks."""
    if len(retail) != len(port):
        return {'field': 'frame_count', 'retail': len(retail), 'port': len(port)}
    for expected, observed in zip(retail, port):
        tick = expected['tick']
        if observed['tick'] != tick:
            return {'field': 'tick', 'retail': tick, 'port': observed['tick']}
        units = {u['id']: u for u in observed['units']}
        expected_ids = {u['id'] for u in expected['units']}
        if units.keys() != expected_ids:
            return {'tick': tick, 'field': 'unit_ids', 'retail_only': sorted(expected_ids - units.keys()),
                    'port_only': sorted(units.keys() - expected_ids)}
        for unit in expected['units']:
            actual = units[unit['id']]
            fields = {'x_raw': unit['position_raw'][0], 'z_raw': unit['position_raw'][2],
                      'heading': unit['heading'], 'speed_raw': unit['speed_raw'] or 0,
                      'base_speed_raw': unit['base_speed_raw']}
            if flight_height:
                fields['y_raw']=unit['position_raw'][1]
            for field, value in fields.items():
                if actual[field] != value:
                    return {'tick': tick, 'id': unit['id'], 'field': field,
                            'retail': value, 'port': actual[field]}
    return None


def compare_rng(retail, port):
    for index, (expected, observed) in enumerate(zip(retail, port)):
        for field in ('tick', 'bound', 'seed_before'):
            if expected[field] != observed[field]:
                return {'index': index, 'field': field, 'retail': expected, 'port': observed}
    if len(retail) != len(port):
        return {'index': min(len(retail), len(port)), 'field': 'call_count',
                'retail': len(retail), 'port': len(port)}
    return None


def compare_selected_units(retail, port, identities, *, flight_height=False):
    """An explicitly scoped diagnostic; the full comparison remains authoritative."""
    selected = set(identities)
    if not selected:
        raise ValueError('selected movement comparison requires units')
    def frames(source):
        return [{**frame, 'units': [unit for unit in frame['units'] if unit['id'] in selected]}
                for frame in source]
    for frame in (*retail, *port):
        missing = selected - {unit['id'] for unit in frame['units']}
        if missing:
            return {'tick': frame['tick'], 'field': 'missing_selected_units', 'ids': sorted(missing)}
    return compare_frames(frames(retail), frames(port),flight_height=flight_height)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('save', type=Path)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--retail-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    saved = decode(args.save.read_bytes())
    capture = json.loads(args.capture.read_text())
    if capture['status'] != 'captured' or capture['source_sha256'] != saved['source_sha256']:
        raise ValueError('capture must be complete and belong to this exact save')
    frames = capture['frames']
    if not frames or frames[0]['tick'] != saved['tick']:
        raise ValueError('capture does not start on the saved tick')
    if any(b['tick'] != a['tick'] + 1 for a, b in zip(frames, frames[1:])):
        raise ValueError('capture has a tick gap')
    live = {u['id']: u for u in frames[0]['units']}
    if live.keys() != {u['id'] for u in saved['units']}:
        raise ValueError('capture/save unit identities disagree')
    args.output.mkdir(parents=True, exist_ok=False)
    players = 1 + max(u['player'] for u in saved['units'])
    crusades = initial_crusades(frames[0])
    lines = [f'TAK_MOVEMENT_PROBE 37 {saved["tick"]} {frames[0]["rng_before"]} '
             f'{len(live)} {len(frames)-1} {players} {json.dumps(saved["map"])} {int(crusades)}']
    wind=initial_wind(frames[0])
    lines.append('0' if wind is None else '1 '+' '.join(map(str,wind)))
    runtime=frames[0]['runtime_state']
    game=next(b for b in runtime['world_buffers'] if b['name']=='game_fields')
    game_bytes=zlib.decompress(base64.b64decode(game['zlib_base64']))
    entity_base=struct.unpack_from('<I',game_bytes,0x14e84)[0]
    path_bytes=bytes.fromhex(frames[0]['path_state']['fields_hex'])
    lines.append(str(path_bytes[0x114]))
    for player in range(players):
        first,last=struct.unpack_from('<II',game_bytes,0x2478+player*0x110)
        cursor=struct.unpack_from('<I',path_bytes,0x115+player*4)[0]
        wrap=struct.unpack_from('<i',path_bytes,0x169+player*4)[0]
        lines.append(f'{(first-entity_base)//312} {(last-first)//312+1} {(cursor-first)//312} {wrap} '
                     f'{int(bool(game_bytes[0x24e7+player*0x110]&1))}')
    feature_presence=initial_feature_presence(frames[0])
    lines.append(str(len(feature_presence)) if feature_presence is not None else '-1')
    for identity,name in feature_presence or []:lines.append(f'{identity} {json.dumps(name)}')
    read_initial = initial_memory_reader(frames[0]) if frames[0].get("game_memory") else None
    unsupported = Counter()
    restored_missions = []
    restored_standby = []
    restored_vtol_standby=[]
    restored_builders = []
    restored_initial_builds = []
    queued_builds=[]
    for unit in saved['units']:
        actual = live[unit['id']]
        orders = unit['orders']
        moving = bool(orders and orders[0]['type'] == 'Move_Ground')
        builder = builder_approach(frames[0].get('runtime_state', {}), unit, actual)
        gx = gz = 0
        if moving:
            gx, gz = movement_goal(unit, orders[0])
        elif builder:
            gx,gz = builder['goal']
        for order in orders:
            if order['type'] not in ('Move_Ground', 'Standby'):
                unsupported[order['type']] += 1
        x, _, z = actual['position_raw']
        fx, fz = actual['footprint']
        building = int(any(o['type'] == 'GetBuilt' for o in orders))
        lines.append(f'{unit["id"]} {unit["player"]} {json.dumps(unit["type"])} '
                     f'{x} {z} {actual["heading"]} {actual["speed_raw"] or 0} '
                     f'{actual["base_speed_raw"]} {fx} {fz} {int(moving or bool(builder))} {gx} {gz} {building}')
        route = actual.get('route_world')
        lines.append(f'{actual.get("movement_flags", 0)} {actual.get("route_tick", 0)} '
                     f'{actual.get("route_flags", 0)} {len(route) if route is not None else -1}')
        if route:
            lines.extend(f'{x} {z}' for x, z in route)
        lines.append(str(actual.get('refusal_deadline') or 0)) # mover +30, local scan deadline
        mission = ground_mission_state(frames[0].get('runtime_state', {}), unit['id']) if moving else None
        standby = standby_mission_state(frames[0].get('runtime_state', {}), unit['id']) if not moving else None
        vtol=vtol_standby_state(runtime,unit['id'],actual) if not moving else None
        if mission is not None:
            lines.append('1 ' + ' '.join(map(str, [*mission,(actual['flags']>>16)&3,(actual['flags']>>18)&3])))
        elif standby is not None:
            lines.append('2 ' + ' '.join(map(str, standby)))
            restored_standby.append(unit['id'])
        elif vtol is not None:
            lines.append('3 '+' '.join(map(str,vtol)))
            restored_vtol_standby.append(unit['id'])
            unsupported['VTOL_Standby']-=1
        else:
            lines.append('0')
        if builder:
            lines.append('1 '+json.dumps(builder['type'])+' '+ ' '.join(map(str,
                [*builder['site'],*builder['bounds'],builder['wait'],builder['pending']])))
            restored_builders.append(unit['id'])
            queue=queued_builder_orders(runtime,unit)
            queued_builds.extend(queue)
            unsupported['MobileBuild']-=1+len(queue)
        else:
            lines.append('0')
            active=captured_mission(runtime,unit['id'],0x405560)
            if active and active[1][5]==0:
                state=active[1]
                wait,deadline=struct.unpack_from('<II',state,6)
                pending=struct.unpack_from('<I',state,0x6a)[0]
                repeat=struct.unpack_from('<i',state,0x52)[0]
                if wait or deadline!=0xffffffff or pending or repeat!=1:
                    raise ValueError('unsupported initial ground-build scheduling state')
                index=struct.unpack_from('<I',state,0x4e)[0]
                table=struct.unpack_from('<I',game_bytes,0x175c4)[0]
                name=captured_type_name(frames[0],table,index)
                x,z=(struct.unpack_from('<i',state,o)[0] for o in (0x22,0x2a))
                queued_builds.append((unit['id'],name,x,z))
                restored_initial_builds.append(unit['id'])
            if active and active[1][5]==3:
                queue=queued_builder_orders(runtime,unit)
                queued_builds.extend(queue)
                unsupported['MobileBuild']-=len(queue)
        if mission is not None:
            restored_missions.append(unit['id'])
        elif moving:
            unsupported['Move_Ground stage or missing runtime state'] += 1
        mover=bytes.fromhex(actual.get('mover_hex',''))
        grade_tick=struct.unpack_from('<I',mover,0x28)[0] if len(mover)>=0x2c else saved['tick']
        if read_initial is not None:
            # Landed flyers occupy the same map cells as ground movers. Their
            # controller stamp is present in memory even without mover_hex.
            pointer=struct.unpack('<I',read_initial(entity_base+unit['id']*312+8,4))[0]
            if pointer:
                grade_tick=struct.unpack('<I',read_initial(pointer+0x28,4))[0]
        lines.append(f'{grade_tick} {actual.get("route_outcome",0)} {actual["flags"] & 3}')
    planes=[]
    for grid in runtime['grids']:
        owners=[identity for identity,actual in live.items()
                if len(bytes.fromhex(actual.get('mover_hex','')))>=8 and
                struct.unpack_from('<I',bytes.fromhex(actual['mover_hex']),4)[0]==grid['address']]
        if not owners:
            continue
        fields=bytes.fromhex(grid['fields_hex'])
        recent,stale=struct.unpack_from('<II',fields,0x34c)
        planes.append(f'{min(owners)} {recent} {stale} '+
                      zlib.decompress(base64.b64decode(grid['zlib_base64'])).hex())
    lines.append(str(len(planes)))
    lines.extend(planes)
    lines.append(str(len(queued_builds)))
    lines.extend(f'{identity} {json.dumps(name)} {x} {z}' for identity,name,x,z in queued_builds)
    flights=[]
    for unit in saved['units']:
        orders=unit['orders']
        if not orders or any(o['type']!='VTOL_Patrol' for o in orders):
            continue
        controller=orders[0].get('flight_controller')
        captured=captured_mission(runtime,unit['id'],0x419db0)
        if not captured or not controller or any(o.get('flight_controller', {}).get('flags', 0) & ~0x70 for o in orders):
            continue
        movement=unit['movement']['runtime_fields']
        fields=[unit['id'],live[unit['id']]['position_raw'][1],
                *(movement[k] for k in ('08','0c','10')),captured[0]['events'],len(orders)]
        records=[' '.join(map(str,fields))]
        for order in orders:
            state=order['runtime_fields']
            records.append(' '.join(str(state[k]) for k in ('22','2a','05','06','0a','6a','5a')))
            c=order.get('flight_controller')
            records.append('1 '+' '.join(map(str,[*c['point'],c['flags'],c['radius'],c['heading']]))
                           if c else '0')
        flights.append((unit['id'],records))
        unsupported['VTOL_Patrol']-=len(orders)
    lines.append(str(len(flights)))
    for _,records in flights: lines.extend(records)
    guards=[]
    for unit in saved['units']:
        record=captured_mission(runtime,unit['id'],0x401bb0)
        if not record: continue
        owner,fields=record
        state=[fields[5],*(struct.unpack_from('<I',fields,o)[0] for o in (6,10,0x6a,0x5a)),owner['events']]
        guards.append((unit['id'],state))
        unsupported['Guard_NoMove']-=1
    lines.append(str(len(guards)))
    lines.extend(' '.join(map(str,[identity,*state])) for identity,state in guards)
    scripts=[]
    factory_orders=[]
    factory_values=factory_unit_values(runtime)
    table=struct.unpack_from('<I',game_bytes,0x175c4)[0]
    for unit in saved['units']:
        state=unit.get('script_state')
        if state is None: continue
        header=[state['signature']]
        for thread in state['threads']:
            header.extend(thread[f'{offset:02x}'] for offset in range(0,0xa4,4))
        header.append(state['runtime_a60'])
        blob=struct.pack(f'<{len(header)}I',*header)
        blob+=bytes.fromhex(state['static_piece_data_hex'])
        target=''
        record=captured_mission(runtime,unit['id'],0x401c20)
        if record and record[1][5]==1:
            type_index=struct.unpack_from('<I',record[1],0x4e)[0]
            target=captured_type_name(capture['frames'][0],table,type_index)
            factory_orders.append(unit['id'])
        values=' '.join(map(str,factory_values[unit['id']]))
        scripts.append(f'{unit["id"]} {blob.hex()} {json.dumps(target)} {values}')
    lines.append(str(len(scripts))); lines.extend(scripts)
    player_caches=initial_player_caches(frames[0],players)
    lines.append(str(len(player_caches)) if player_caches is not None else '0')
    if player_caches is not None:
        lines.extend(' '.join(map(str,state)) for state in player_caches)
    ai_states=initial_ai_state(frames[0],players)
    lines.append(str(len(ai_states)) if ai_states is not None else '0')
    if ai_states is not None:
        for state in ai_states:
            if state is None: lines.append('0');continue
            lines.append(f'1 {state["countdown"]} {state["initialized"]} {state["scenario_deadline"]} {len(state["groups"])}')
            for group in state['groups']:
                lines.append(' '.join(map(str,[group['slot'],group['kind'],group['deadline'],group['active'],group['dirty'],
                                               *group['parameters'],len(group['members']),*group['members']])))
            lines.append(str(len(state['anchors'])))
            lines.extend(' '.join(map(str,anchor)) for anchor in state['anchors'])
    build_caches=initial_build_caches(frames[0],players)
    lines.append(str(len(build_caches)) if build_caches is not None else '0')
    for state in build_caches or []:
        if state is None: lines.append('0');continue
        lines.append(' '.join(map(str,[1,state['mana'],state['storage'],state['income'],state['usage'],
                                      *sum(state['samples'],[]),len(state['entries'])])))
        for entry in state['entries']:
            lines.append(json.dumps(entry['name'])+' '+' '.join(map(str,[entry[key] for key in
                ('flags','secondary_flags','weapon','cost','desired','count','completed','priority','income','storage',
                 'inverse_time','worker_time','max_hp','emitter_radius','emitter_height','emitter_capacity')])))
    construction=initial_construction(frames[0])
    lines.append(str(len(build_caches)) if build_caches is not None else '0')
    for state in build_caches or []:
        lines.append('0' if state is None else '1 '+' '.join(map(str,[state[k] for k in
            ('allocation','capacity_override','total_produced','excess')])))
    lines.append(str(len(construction['sites'])) if construction else '0')
    for site in construction['sites'] if construction else []:
        lines.append(' '.join(map(str,[site[k] for k in
            ('id','remaining','hp','events','flags','inverse_time','cost','max_hp')])))
    lines.append(str(len(construction['builders'])) if construction else '0')
    for builder in construction['builders'] if construction else []:
        if builder['repeat']!=1: raise ValueError('unsupported repeated active construction')
        lines.append(' '.join(map(str,[builder['id'],builder['target'],builder['worker_time'],int(builder['working']),
            builder['activity_deadline'],builder['wait'],builder['deadline'],builder['pending']])))
        unsupported['MobileBuild']-=1
    lines.append(str(len(construction['emitters'])) if construction else '0')
    for emitter in construction['emitters'] if construction else []:
        lines.append(' '.join(map(str,[emitter['id'],emitter['capacity'],emitter['radius'],emitter['height'],
                                      len(emitter['particles']),*sum(emitter['particles'],[])])))
    lines.append(str(len(construction['waiting'])) if construction else '0')
    for waiting in construction['waiting'] if construction else []:
        lines.append(' '.join(map(str,[waiting[k] for k in ('id','builder','stage','wait','deadline','pending')])))
        unsupported['GetBuilt']-=1
    planner=initial_build_planner(frames[0],players)
    lines.append(str(len(planner['owners'])) if planner else '0')
    for owner in planner['owners'] if planner else []:
        if owner is None: lines.append('0');continue
        lines.append(f'1 {int(owner["limited"])} {owner["population"]} {len(planner["types"])}')
        for index,kind in enumerate(planner['types']):
            lines.append(json.dumps(kind['faction'])+' '+' '.join(map(str,
                [owner['weights'][index],int(kind['special']),len(kind['choices']),*kind['choices']])))
    sectors=initial_sector_links(runtime)
    lines.append(str(len(sectors)))
    lines.extend(' '.join(map(str,row)) for row in sectors)
    flying=construction['flying_builders'] if construction else []
    lines.append(str(len(flying)))
    for job in flying:
        unit=next(u for u in saved['units'] if u['id']==job['id'])
        controller=unit['orders'][0].get('flight_controller')
        if not controller or job['repeat']!=1:
            raise ValueError('unsupported flying construction controller/repeat')
        actual=live[unit['id']]
        movement=unit['movement']['runtime_fields']
        lines.append(' '.join(str(int(job[k]) if k=='working' else job[k]) for k in
            ('id','target','worker_time','working','activity_deadline','wait','deadline','pending','stage','owner_flags','flags','events')))
        lines.append(' '.join(str(job[k]) for k in ('slow_speed','fast_speed','build_distance')))
        lines.append(' '.join(map(str,[actual['position_raw'][1],*(movement[k] for k in ('08','0c','10')),
            *controller['point'],controller['flags'],controller['radius'],controller['heading']])))
        unsupported['VTOL_MobileBuild']-=1
    read_height=initial_memory_reader(frames[0])
    lines.append(str(len(frames[0]['units'])))
    for unit in frames[0]['units']:
        mover=unit.get('mover_address',0)
        address=next(u['address'] for u in runtime['units'] if u['id']==unit['id'])
        stamp=struct.unpack('<I',read_height(mover+0x2c,4))[0] if mover else 0
        phase=struct.unpack('<H',read_height(address+0x82,2))[0]
        pitch=struct.unpack('<H',read_height(address+0x80,2))[0]
        roll=struct.unpack('<H',read_height(address+0x7c,2))[0]
        lines.append(f'{unit["id"]} {unit["position_raw"][1]} {stamp} {phase} {pitch} {roll}')
    width,height,viewer,explored,sight=initial_exploration(frames[0])
    lines.append(f'{width} {height} {viewer}')
    lines.append(' '.join(map(str,explored)))
    lines.append(str(len(sight)))
    lines.extend(' '.join(map(str,row)) for row in sight)
    input_path = args.output / 'input.txt'
    input_path.write_text('\n'.join(lines) + '\n')
    trace_path = args.output / 'port.jsonl'
    with (args.output / 'runner.log').open('x') as log:
        subprocess.run([str(args.runner.resolve()), str(args.retail_root), str(input_path), str(trace_path)],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    events = [json.loads(line) for line in trace_path.read_text().splitlines()]
    port_frames = [e for e in events if e['kind'] == 'frame']
    port_rng = [e for e in events if e['kind'] == 'rng']
    port_crt=[{k:e[k] for k in ('tick','return_address','seed_before')} for e in events if e['kind']=='crt']
    retail_crt=capture.get('crt_calls',[])
    crt_mismatch=next(({'index':i,'retail':a,'port':b} for i,(a,b) in enumerate(zip(retail_crt,port_crt)) if a!=b),None)
    if not crt_mismatch and len(retail_crt)!=len(port_crt):
        crt_mismatch={'index':min(len(retail_crt),len(port_crt)),'retail_count':len(retail_crt),'port_count':len(port_crt)}
    report = {'complete_state': False, 'source_sha256': saved['source_sha256'],
              'restored_navigation_exploration': {'width':width,'height':height,'viewer':viewer,'units':len(sight)},
              'restored_wind': wind is not None,
              'restored_player_cache_clocks': player_caches,
              'restored_build_caches': build_caches is not None,
              'restored_construction_jobs': [b['id'] for b in construction['builders']] if construction else [],
              'restored_flying_construction_jobs': [b['id'] for b in flying],
              'first_flying_construction_motion_mismatch': compare_selected_units(
                  frames,port_frames,[b['id'] for b in flying],flight_height=True) if flying else None,
              'first_crt_mismatch':crt_mismatch if retail_crt else None,
              'restored_ai_players': [i for i,s in enumerate(ai_states or []) if s is not None],
              'restored_guards': [identity for identity,_ in guards],
              'restored_queued_builds': len(queued_builds),
              'restored_factory_script_orders': factory_orders,
              'restored_flight_patrols': [identity for identity,_ in flights],
              'restored_ground_missions': restored_missions,
              'restored_standby_missions': restored_standby,
              'restored_vtol_standby_missions': restored_vtol_standby,
              'restored_builder_approaches': restored_builders,
              'restored_initial_ground_builds': restored_initial_builds,
              'limitations': events[0]['limitations'], 'unsupported_orders': dict(unsupported),
              'initial_mismatch': compare_frames(frames[:1], port_frames[:1]),
              'first_motion_mismatch': compare_frames(frames, port_frames),
              'first_restored_flight_mismatch': compare_selected_units(frames,port_frames,[i for i,_ in flights],flight_height=True) if flights else None,
              'first_restored_ground_mismatch': compare_selected_units(frames, port_frames, restored_missions)
                  if restored_missions else None,
              'first_restored_builder_mismatch': compare_selected_units(frames, port_frames, restored_builders)
                  if restored_builders else None,
              'first_rng_mismatch': compare_rng(capture['rng_calls'], port_rng),
              'port_rng_calls': len(port_rng), 'retail_rng_calls': len(capture['rng_calls'])}
    (args.output / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
