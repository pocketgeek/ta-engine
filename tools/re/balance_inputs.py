"""Controlled movement-type inputs for standard/Crusades oracle comparisons.

Read source FBI/MOVEINFO assets independently of World. Select matching native
classes already loaded in the capture, checking their definitions first. This
changes type inputs, never native query outputs or World's grade results.
"""
from pathlib import Path
import re
import struct
import subprocess

from decode_save_state import decode


def properties(text):
    text = re.sub(r'/\*.*?\*/|//[^\r\n]*', '', text, flags=re.S)
    return {key.lower(): value.strip() for key,value in
            re.findall(r'([A-Za-z][A-Za-z0-9_]*)\s*=\s*([^;]*);', text)}


def unit_properties(text):
    """Read UNITINFO only; weapon sections also declare keys such as turnrate."""
    text=re.sub(r'/\*.*?\*/|//[^\r\n]*','',text,flags=re.S)
    start=re.search(r'\[\s*unitinfo\s*\]\s*\{',text,re.I)
    if not start: raise ValueError('source FBI lacks UNITINFO')
    depth=1;body=[]
    for char in text[start.end():]:
        if char=='{': depth+=1
        elif char=='}':
            depth-=1
            if depth==0: return properties(''.join(body))
        elif depth==1: body.append(char)
    raise ValueError('unterminated source UNITINFO')


def class_record(fields):
    number = lambda key, default: int(fields.get(key, default))
    dry, wet = number('maxslope',255)&255, number('maxwaterslope',255)&255
    soft_dry, soft_wet = number('badslope',dry//2)&255, number('badwaterslope',wet//2)&255
    dry = min(dry,wet)
    maximum, minimum = number('maxwaterdepth',10000), number('minwaterdepth',-10000)
    return struct.pack('<6h4B',number('footprintx',0),number('footprintz',0),
        maximum,minimum,number('badmaxwaterdepth',maximum),number('badminwaterdepth',minimum),
        dry,min(soft_dry,dry),wet,min(soft_wet,wet))


# Feature loader 493920: footprintx/z -> +0xb0/+0xb2 (4939cf/4939e4), height ->
# +0x138 (493a0e), sacredsite float -> +0x134 (494074) and the boolean keys
# below -> +0x13c bits (494088..4942de). Bit 0 marks a feature without an
# `object` model (493b21/493c04). Destructible features also receive 0x20000
# (4941a5..4941b3); 4945bb..494854 may later clear it through the
# featuredead/featureburnt chain. These are the record fields read by corpse
# placement/removal (495360/496380) and the raw grade's feature branch.
FEATURE_FLAG_BITS=(('animating',1,0),('animtrans',2,0),('shadtrans',3,0),('flamable',4,0),
    ('blocking',5,0),('reclaimable',6,0),('autoreclaimable',7,1),('indestructible',8,0),
    ('nodisplayinfo',9,0),('nodrawundergray',10,0),('resurrectable',11,0),
    ('animatable',12,0),('noshadow',13,0))
# Bits consumed by 495360/496380/512ee0 and the 5088f0 feature branch.
FEATURE_PLACEMENT_MASK=0x1|0x20|0x40|0x80|0x100|0x800|0x1000|0x20000


def feature_record_inputs(fields):
    """Source TDF -> (footprint, height, sacredsite, placement flag bits)."""
    number=lambda key,default=0: int(float(fields.get(key,default)))
    flags=0 if fields.get('object') else 1
    for key,bit,default in FEATURE_FLAG_BITS:
        flags|=(number(key,default)&1)<<bit
    if not flags&0x100: flags|=0x20000
    if fields.get('featuredead') or fields.get('featureburnt'):
        raise ValueError('synthesized feature records do not model replacement chains')
    return (number('footprintx'),number('footprintz'),number('height')&255,
            float(fields.get('sacredsite',0)),flags&FEATURE_PLACEMENT_MASK)


def synthesize_feature(process, name, fields):
    """Append a native feature-table record for a definition the capture never loaded.

    Retail resolves corpse names on demand through 494480 -> 493920, which parses
    a startup TDF database the capture did not retain. Clone a loaded record whose
    placement inputs equal the source definition, then rename it and set its
    decomposeTime. Only display pointers (model, palette, animation) remain the
    donor's; corpse placement and retirement do not read them for cell state.
    Returns the new index.
    """
    table,count=process.u32(process.game+0x19edc),process.u32(process.game+0x19ec0)
    wanted=feature_record_inputs(fields)
    read=lambda address,size: bytes(process.uc.mem_read(address,size))
    donor=None
    for index in range(count):
        record=table+index*320
        fx,fz=struct.unpack('<2h',read(record+0xb0,4))
        height=read(record+0x138,1)[0]
        sacred=struct.unpack('<f',read(record+0x134,4))[0]
        flags=struct.unpack('<I',read(record+0x13c,4))[0]&FEATURE_PLACEMENT_MASK
        chain=struct.unpack('<2H',read(record+0x12c,4))
        if (fx,fz,height,sacred,flags)==wanted and chain==(0xffff,0xffff):
            donor=index;break
    if donor is None:
        raise ValueError(f'no loaded feature record has the placement inputs of {name}: {wanted}')
    data=bytearray(read(table,count*320))
    clone=bytearray(data[donor*320:(donor+1)*320])
    clone[0:32]=name.encode('ascii')[:31].ljust(32,b'\0')
    # 494332..494353: decomposeTime * [5f0398] truncated to 16 bits; it seeds
    # the corpse lifetime, not cell state, but should not be the donor's.
    scale=struct.unpack('<d',read(0x5f0398,8))[0]
    struct.pack_into('<I',clone,0x130,int(float(fields.get('decomposetime',0))*scale)&0xffff)
    address=process.brk
    process.brk=(address+(count+1)*320+15)&~15
    process.put(address,bytes(data)+bytes(clone))
    process.allocations[address]=(count+1)*320
    process.uc.mem_write(process.game+0x19edc,struct.pack('<I',address))
    process.uc.mem_write(process.game+0x19ec0,struct.pack('<I',count+1))
    return count


def set_balance_inputs(process, capture, save_path, retail_root, crusades,
                       hpitool=Path('build-dbg/hpitool'), corpses=False, surface=False, motion=False,
                       synthesize_corpses=False):
    saved=decode(save_path.read_bytes())
    if saved['source_sha256'] != capture['source_sha256']:
        raise ValueError('balance input save does not belong to capture')
    cache={}
    def asset(path, optional=False):
        if path not in cache:
            found=subprocess.run([str(hpitool),'where',str(retail_root),path],
                                 text=True,capture_output=True)
            if found.returncode:
                if optional: return None
                raise ValueError(f'asset unavailable: {path}: {found.stderr}')
            location=found.stdout.strip().split(' -> ',1)[-1]
            if '!' not in location: raise ValueError(f'expected archive asset: {location}')
            archive,internal=location.split('!',1)
            cache[path]=subprocess.run([str(hpitool),'cat',str(retail_root/archive),internal],
                                      text=True,capture_output=True,check=True).stdout
        return cache[path]
    definitions={}
    for block in re.findall(r'\[[^]]+\]\s*\{([^{}]*)\}',asset('gamedata/moveinfo.tdf')):
        fields=properties(block)
        if 'name' in fields: definitions[fields['name'].lower()]=class_record(fields)
    classes={}
    for address in range(0x62dbf0,0x634670,0x354):
        pointer=process.u32(address)
        if not pointer: continue
        name=bytes(process.uc.mem_read(pointer,64)).split(b'\0')[0].decode('ascii').lower()
        record=bytes(process.uc.mem_read(address+4,16))
        if name not in definitions or record != definitions[name]:
            raise ValueError(f'captured MOVEINFO class differs from source assets: {name}')
        classes[name]=address
    saved_types={u['id']:u['type'].lower() for u in saved['units']}
    written=set()
    corpse_written=set()
    feature_names={}
    synthesized=[]
    if corpses:
        table=process.u32(process.game+0x19edc)
        for index in range(process.u32(process.game+0x19ec0)):
            name=bytes(process.uc.mem_read(table+index*320,32)).split(b'\0')[0].decode('ascii').lower()
            feature_names[name]=index
    selected={}
    for unit in process.frame['units']:
        name=saved_types[unit['id']]
        source=asset(f'unitscb/{name}.fbi',True) if crusades else None
        if source is None: source=asset(f'units/{name}.fbi')
        fields=unit_properties(source)
        movement=fields.get('movementclass','').lower()
        if corpses and unit['type_address'] not in corpse_written:
            footprint=tuple(int(fields.get(key,1)) for key in ('footprintx','footprintz'))
            if movement in definitions:
                inherited=struct.unpack_from('<2h',definitions[movement])
                footprint=tuple(parent if parent>0 else own for parent,own in zip(inherited,footprint))
            if footprint != tuple(unit['footprint']):
                raise ValueError(f'balance switch changes captured corpse anchor footprint: {name}')
            for key,offset in (('corpse',0x24e),('stone',0x250),('frozen',0x252)):
                feature=fields.get(key,'').lower()
                if feature and feature not in feature_names:
                    if not synthesize_corpses:
                        raise ValueError(f'controlled corpse feature is not loaded in capture: {feature}')
                    source=asset(f'features/corpses/{feature}.tdf',True)
                    if source is None: raise ValueError(f'corpse feature source unavailable: {feature}')
                    sections={match.group(1).lower():properties(match.group(2)) for match in
                              re.finditer(r'\[([^]]+)\]\s*\{([^{}]*)\}',source)}
                    if feature not in sections: raise ValueError(f'source lacks [{feature}]')
                    feature_names[feature]=synthesize_feature(process,feature,sections[feature])
                    synthesized.append(feature)
                process.uc.mem_write(unit['type_address']+offset,
                    struct.pack('<H',feature_names[feature] if feature else 0xffff))
            adjustment=tuple(int(fields.get(key,0)) for key in ('corpseadjustx','corpseadjustz'))
            process.uc.mem_write(unit['type_address']+0x25c,struct.pack('<2h',*adjustment))
            corpse_written.add(unit['type_address'])
        if movement not in classes:
            if unit.get('mover_address'): raise ValueError(f'unhandled movement class for {name}: {movement}')
            continue
        grid=classes[movement]
        record=definitions[movement]
        if tuple(unit['footprint']) != struct.unpack_from('<hh',record):
            raise ValueError(f'balance switch changes captured footprint: {name}')
        kind=unit['type_address']
        if kind not in written:
            if motion:
                process.uc.mem_write(kind+0x226,struct.pack('<h',int(fields.get('sightdistance',0))))
                for key,offset,default in (('maxvelocity',0x162,0),('brakerate',0x166,0.5),
                                           ('acceleration',0x16a,0.5)):
                    process.uc.mem_write(kind+offset,struct.pack('<i',int(float(fields.get(key,default))*65536)))
                for key,offset,default in (('turnrate',0x18e,500),('turninplacerate',0x190,0)):
                    process.uc.mem_write(kind+offset,struct.pack('<H',int(float(fields.get(key,default)))&65535))
            if surface:
                flags=process.u32(kind+0x260)&~(0x100000|0x80000|0x1000)
                for key,bit in (('upright',0x100000),('floater',0x80000),('canhover',0x1000)):
                    if int(fields.get(key,0)): flags|=bit
                process.uc.mem_write(kind+0x260,struct.pack('<I',flags))
                process.uc.mem_write(kind+0x248,bytes([int(fields.get('waterline',0))&255]))
                for key,offset in (('bankscale',0x176),('pitchscale',0x17a)):
                    default=0 if key=='pitchscale' and int(fields.get('canfly',0)) else 0.5
                    process.uc.mem_write(kind+offset,struct.pack('<i',int(float(fields.get(key,default))*65536)))
            process.uc.mem_write(kind+0x18a,struct.pack('<I',grid))
            process.uc.mem_write(kind+0x192,record[4:12])
            process.uc.mem_write(kind+0x23c,bytes((record[12],record[14])))
            road=int(float(fields.get('roadmultiplier',1.2))*65536)
            water=int(float(fields.get('watermultiplier',1))*65536)
            process.uc.mem_write(kind+0x172,struct.pack('<i',road))
            process.uc.mem_write(kind+0x16e,struct.pack('<i',water))
            if motion:
                maximum=int(float(fields.get('maxvelocity',0))*65536)
                best=max(road,water,65536)*maximum>>16
                scale=255 if best<=0 else max(1,min(255,(8*65536)//best))
                process.uc.mem_write(kind+0x249,bytes([scale]))
            written.add(kind)
        mover=unit.get('mover_address')
        if mover:
            process.uc.mem_write(mover+4,struct.pack('<I',grid))
            selected[unit['id']]=grid
    process.uc.mem_write(0x641144,bytes([int(crusades)]))
    process.synthesized_features=synthesized
    return selected
