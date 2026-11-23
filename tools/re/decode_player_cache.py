"""Restore build-priority inputs from one initial memory boundary only."""
import math
import struct
from captured_memory import initial_memory_reader



def construction_emitter_profile(bounds, structure, movement_code, quality, current, target):
    """Initial visual constructor inputs; quality changes require a new profile."""
    half_x,half_z=(bounds[3]-bounds[0])//2,(bounds[5]-bounds[2])//2
    if not 0 <= half_x <= 0x4000000 or not 0 <= half_z <= 0x4000000 or not 0 <= bounds[4]-bounds[1] <= 0x7fffffff:
        raise ValueError('invalid construction model bounds')
    if not 0 <= current < 0x7fffffff or not 0 <= target <= 0x7fffffff:
        raise ValueError('invalid construction quality counters')
    if structure: radius=min(half_x,half_z)
    else:
        square=half_x*half_x+half_z*half_z
        radius=math.isqrt(square)
        radius+=square-radius*radius>radius
    capacity=(radius>>16)//(4 if movement_code==1 else 1)
    if quality and current+1<target:
        capacity=max(1,capacity//min(6,(target-current)//2))
    return radius,bounds[4]-bounds[1],capacity


def initial_build_caches(frame, players):
    regions = frame.get('game_memory', [])
    if not regions:
        return None
    read = initial_memory_reader(frame)
    word = lambda a: struct.unpack('<I', read(a, 4))[0]
    runtime = frame['runtime_state']
    game = next(r['address'] for r in runtime['world_buffers'] if r['name'] == 'game_fields')
    count, table = word(game+0x175b8), word(game+0x175c4)
    if not 1 <= count <= 65536:
        raise ValueError('invalid build catalogue size')
    types = []
    seen = set()
    quality=read(0x61a01c,1)[0]
    quality_count,quality_target=word(0x61a024),word(0x61a02c)
    if quality_count>0x7fffffff or quality_target>0x7fffffff:
        raise ValueError('invalid construction quality counters')
    for index in range(1, count):
        data = read(table+676*index, 676)
        name = data[32:64].split(b'\0', 1)[0].decode('ascii').lower()
        cost = struct.unpack_from('<f', data, 0x20e)[0]
        storage, income = struct.unpack_from('<2f', data, 0x206)
        if not all(math.isfinite(v) and 0 <= v <= 1e9 for v in (storage, income)):
            raise ValueError('invalid catalogue economy')
        if not name or name in seen or not math.isfinite(cost) or not 0 <= cost <= 1e9:
            raise ValueError('invalid build catalogue entry')
        seen.add(name)
        bounds=struct.unpack_from('<6i',data,0x13a)
        # bmcode zero is the structure constructor classification. The radius
        # oracle takes the resulting entity flag separately from movement code.
        radius,height,capacity=construction_emitter_profile(
            bounds,data[0x24a]==0,data[0x24a],quality,quality_count,quality_target)
        inverse=struct.unpack_from('<f',data,0x216)[0]
        worker=struct.unpack_from('<f',data,0x21a)[0]
        max_hp=struct.unpack_from('<I',data,0x1be)[0]
        if not all(math.isfinite(v) and 0 <= v <= 1e9 for v in (inverse,worker)) or max_hp>32767:
            raise ValueError('invalid catalogue construction')
        types.append({'name': name, 'cost': cost, 'income': income, 'storage': storage,
                      'inverse_time':inverse,'worker_time':worker,'max_hp':max_hp,
                      'emitter_radius':radius,'emitter_height':height,'emitter_capacity':capacity,
                      'flags': struct.unpack_from('<I', data, 0x260)[0],
                      'secondary_flags': struct.unpack_from('<I', data, 0x264)[0],
                      'weapon': struct.unpack_from('<h', data, 0x194)[0]})
    # Counts are checked against unit state, not accepted as timeless inputs.
    actual = [[0]*count for _ in range(players)]
    complete = [[0]*count for _ in range(players)]
    for unit in runtime['units']:
        data = read(unit['address'], 312)
        owner = struct.unpack_from('<I', data, 0xb8)[0]
        player, remainder = divmod(owner-game-0x2404, 0x110)
        type_address = struct.unpack_from('<I', data, 0xb4)[0]
        index, type_remainder = divmod(type_address-table, 676)
        remaining = struct.unpack_from('<f', data, 0x108)[0]
        if remainder or not 0 <= player < players or type_remainder or not 1 <= index < count:
            raise ValueError('invalid cached unit owner/type')
        if not math.isfinite(remaining) or not 0 <= remaining <= 1:
            raise ValueError('invalid captured construction remainder')
        actual[player][index] += 1
        complete[player][index] += remaining == 0
    result = []
    for player in range(players):
        manager = word(0x62a33c+4*player)
        if not manager:
            result.append(None)
            continue
        owner = game+0x2404+0x110*player
        if word(manager) != owner or read(manager+4, 1)[0] != player:
            raise ValueError('invalid build-cache owner')
        resource = read(word(owner+0x10c), 0x190)
        mana, storage = struct.unpack_from('<2f', resource)
        allocation = struct.unpack_from('<f',resource,8)[0]
        capacity_override = struct.unpack_from('<f',resource,0x14)[0]
        total_produced, excess = struct.unpack_from('<2d',resource,0x18)
        if not all(math.isfinite(v) for v in (allocation,capacity_override,total_produced,excess)) or allocation<0:
            raise ValueError('invalid resource accounting')
        income, usage = struct.unpack_from('<2f', resource, 12)
        samples = [list(struct.unpack_from('<2f', resource, 0x2c+12*i)) for i in range(30)]
        if not all(math.isfinite(v) and v >= 0 for v in [mana, storage, income, usage, *sum(samples, [])]):
            raise ValueError('invalid build-cache resource history')
        counts = struct.unpack(f'<{count}h', read(word(manager+0x85), count*2))
        completed = struct.unpack(f'<{count}h', read(word(manager+0x95), count*2))
        desired = struct.unpack(f'<{count}i', read(word(manager+0xe5), count*4))
        priorities = read(word(manager+0x69), count)
        if list(counts) != actual[player] or list(completed) != complete[player]:
            raise ValueError('build-cache counts disagree with current unit construction state')
        entries = [dict(t, count=counts[i], completed=completed[i], desired=desired[i], priority=priorities[i])
                   for i, t in enumerate(types, 1)]
        result.append({'mana': mana, 'storage': storage, 'income': income, 'usage': usage,
                       'allocation':allocation,'capacity_override':capacity_override,
                       'total_produced':total_produced,'excess':excess,
                       'samples': samples, 'entries': entries})
    return result


def initial_build_planner(frame, players):
    """Initial ordered menus and preference weights used by occupied AI bases."""
    if not frame.get('game_memory'):
        return None
    read = initial_memory_reader(frame)
    word = lambda a: struct.unpack('<I', read(a, 4))[0]
    game = next(r['address'] for r in frame['runtime_state']['world_buffers']
                if r['name'] == 'game_fields')
    count, table = word(game+0x175b8), word(game+0x175c4)
    if not 1 <= count <= 65536:
        raise ValueError('invalid planner catalogue size')
    types = []
    for index in range(1, count):
        address = table+676*index
        size, menu = word(address+0x12e), word(address+0x132)
        if size > 65535 or (size and not menu):
            raise ValueError('invalid planner build menu')
        choices = list(struct.unpack(f'<{size}H', read(menu, size*2))) if size else []
        if any(not 1 <= choice < count for choice in choices):
            raise ValueError('planner build menu type outside catalogue')
        faction_address = word(address+0x8a)
        faction = bytearray()
        for offset in range(256):
            value = read(faction_address+offset, 1)[0]
            if not value:
                break
            faction.append(value)
        else:
            raise ValueError('unterminated planner faction')
        descriptor = word(address+0x12a)
        types.append({'choices': choices, 'faction': faction.decode('ascii'),
                      'special': bool(descriptor and read(descriptor, 1)[0] & 128)})
    owners = []
    for player in range(players):
        manager = word(0x62a33c+4*player)
        if not manager:
            owners.append(None)
            continue
        if word(manager) != game+0x2404+player*0x110 or read(manager+4, 1)[0] != player:
            raise ValueError('invalid planner owner')
        owners.append({'population': word(manager+0x7d),
                       'limited': bool(read(game+0x24e7+player*0x110, 1)[0]),
                       'weights': list(read(word(manager+0xc5)+1, count-1))})
    return {'types': types, 'owners': owners}
