"""Read named feature presence from the initial captured map, never later frames."""
import struct
from captured_memory import initial_memory_reader


def initial_feature_presence(frame):
    if not frame.get('game_memory'):return None
    read=initial_memory_reader(frame)
    game=next(b['address'] for b in frame['runtime_state']['world_buffers'] if b['name']=='game_fields')
    word=lambda address:struct.unpack('<I',read(address,4))[0]
    count,table=word(game+0x19ec0),word(game+0x19edc)
    width,height=frame['map_cells']
    if not 0<count<65530 or not 1<=width<=4096 or not 1<=height<=4096:
        raise ValueError('invalid initial feature catalogue/map dimensions')
    names=[]
    for i in range(count):
        raw=read(table+i*320,32)
        if b'\0' not in raw:raise ValueError('unterminated feature name')
        name=raw.split(b'\0')[0].decode('ascii').lower()
        if not name:raise ValueError('empty feature name')
        names.append(name)
    cells=read(word(game+0x19f04),width*height*14)
    result=[]
    for i in range(width*height):
        feature=struct.unpack_from('<H',cells,i*14+8)[0]
        if feature>=65530:continue # empty cells and footprint/terrain markers
        if feature>=count:raise ValueError('feature index outside initial catalogue')
        result.append((i,names[feature]))
    return result
