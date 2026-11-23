#!/usr/bin/env python3
"""Observe retail debris construction with synthetic model data.

Only allocation and optional blood-emitter boundaries are sinks. Native geometry setup,
visibility mutation and bounding-center calculation execute. Includes child-subtree copying and allocation failure. The optional
script-driven blood effect and update/render phases are excluded.
"""
import random
import struct
from emu import Icd, HEAP

p = Icd()
manager, descriptor, unit, view, model, vertices, output, settings, options, unit_type, type_model = [HEAP+i*0x10000 for i in range(11)]
def put(a, *values):
    p.uc.mem_write(a, struct.pack('<'+'I'*len(values), *(v & 0xffffffff for v in values)))
def get(a, n=1):
    return struct.unpack('<'+'I'*n, p.uc.mem_read(a, n*4))
allocations=[]
slot_available=True
allocation_ok=True
query_result=-1
blood_calls=[]
blood_object=HEAP+0x100000
def query_blood(uc, sp):
    name,result,count,arg1,arg2=get(sp,5)
    assert bytes(uc.mem_read(name,11))==b'QueryBlood\0'
    assert (count,arg1,arg2)==(0,0,0)
    put(result,query_result)
    blood_calls.append(('query',))
    return 5,0
def blood_allocate(uc,sp):
    assert get(sp)==(44,)
    blood_calls.append(('allocate',))
    return 0,blood_object  # cdecl: caller removes the size argument
def blood_construct(uc,sp):
    assert get(sp,3)==(100,unit_type+0x26c,3)
    blood_calls.append(('construct',))
    return 3,blood_object
def blood_start(uc,sp):
    count,position=get(sp,2)
    assert count==100 and get(position,3)==get(unit+0x68,3)
    blood_calls.append(('start',))
    return 2,0
def allocate(uc, sp):
    destination, size = get(sp, 2)
    allocations.append(size)
    if allocation_ok: put(destination, output)
    return 2, int(allocation_ok)
p.hooks.update({0x4d4610:allocate,
                0x56c720:query_blood,0x4eb9e0:blood_allocate,
                0x4f1f80:blood_construct,0x4f2130:blood_start})
p.freeze_hooks()
slots=manager+0x2b74
# Exercise the real first-free search, including a hole at every possible index.
for free in range(101):
    entries=[output]*100
    if free<100:entries[free]=0
    put(slots,*entries)
    result,error=p.call(0x4923b0,ecx=manager)
    assert not error,error
    assert result==(free if free<100 else 0xffffffff),(free,result)
    assert get(slots,100)==tuple(entries)
put(slots,*([0]*100))
result,error=p.call(0x4923b0,ecx=manager)
assert not error and result==0
print('PASS: native debris admission selects the first free slot across all 100 slots; full pool returns failure without mutation')

def create():
    put(slots,*([0 if slot_available else output]*100))
    result,error=p.call(0x492910,(descriptor,),ecx=manager)
    if not error:
        expected=([output]+[0]*99) if slot_available and allocation_ok else ([0]*100 if slot_available else [output]*100)
        assert get(slots,100)==tuple(expected)
    return result,error
put(unit+0xc0, view); put(unit+0xb4, unit_type)
put(unit_type+0x8a, type_model)
put(0x62d558, settings); put(settings+0x18, options)
p.uc.mem_write(options+0x11, b'\0')
rng=random.Random(0x492910)
for case in range(1024):
    piece=case%7
    source=view+0x1cc+piece*56
    count=1+case%19
    points=[tuple(rng.randrange(-1000000,1000000) for _ in range(3)) for _ in range(count)]
    # Keep authored and transformed arrays equal here to isolate copying/centering.
    put(vertices, *(v for point in points for v in point))
    offsets=tuple(rng.randrange(-65536,65536) for _ in range(3))
    put(model+4,count);put(model+0x10,*offsets);put(model+0x24,vertices)
    moves=tuple(rng.randrange(-65536,65536) for _ in range(3))
    angles=tuple(rng.randrange(65536) for _ in range(3))
    p.uc.mem_write(source,bytes(56))
    put(source,model,*(offsets[a]+moves[a] for a in range(3)))
    p.uc.mem_write(source+0x10,struct.pack('<3H',*angles))
    put(source+0x24,vertices)
    p.uc.mem_write(source+0x2a,b'\x01\x00')
    body=tuple(rng.randrange(-10000000,10000000) for _ in range(3))
    put(unit+0x68,*body)
    p.uc.mem_write(descriptor,bytes(80));put(descriptor,unit,piece)
    # Cover all low debris flags while blood is disabled. Smoke/fire descriptor
    # bits do not independently allocate attached emitters in this constructor.
    flags=case%64
    put(descriptor+0x20,900,0,flags)
    p.uc.mem_write(output,bytes(4096));allocations.clear()
    _,error=create()
    assert not error,(case,error)
    assert allocations == [540+56+count*12], (case,allocations)
    assert get(output+0x2c,3)==tuple(v&0xffffffff for v in body)
    assert get(output+0x44)==(output+0x21c,)
    assert get(output+0x50)==(1,)
    assert get(output+0x28)==(flags,)
    assert get(output+0x1d0)==(0,)
    assert get(output+0x21c)==(model,)
    assert get(output+0x220,3)==tuple(v&0xffffffff for v in moves)
    assert bytes(p.uc.mem_read(output+0x21c+0x10,6))==struct.pack('<3H',*angles)
    assert int.from_bytes(p.uc.mem_read(output+0x21c+0x2a,2),'little')&1==1
    assert int.from_bytes(p.uc.mem_read(source+0x2a,2),'little')&1==0
    # Native seeds bounds from the first transformed vertex, then visits authored vertices.
    center=tuple(int((min(v[a] for v in points)+max(v[a] for v in points))/2) for a in range(3))
    assert get(output+0x38,3)==tuple(v&0xffffffff for v in center),(case,get(output+0x38,3),center)
    assert get(output+0x21c+0x18,3)==tuple(v&0xffffffff for v in center)
print('PASS: 1024 native single-piece debris creations preserve model/body, allocate geometry, hide the source and remove authored offsets; all low flag combinations retain no attached emitter with blood disabled')

# Exercise the complete native constructor with a child and a child sibling.
# The root's sibling must remain on the original unit even with flag 0x40.
for with_children in (False, True):
    nodes = [view+0x1cc+i*56 for i in range(4)]
    models = [model+i*0x100 for i in range(4)]
    arrays = [vertices+i*0x100 for i in range(4)]
    for i, (node, mesh, array) in enumerate(zip(nodes, models, arrays)):
        p.uc.mem_write(node, bytes(56));p.uc.mem_write(mesh, bytes(64))
        put(mesh+4,1);put(mesh+0x24,array);put(array,i*100,i*200,i*300)
        put(node,mesh);put(node+0x24,array)
        p.uc.mem_write(node+0x2a,b'\x01\x00')
    put(nodes[0]+0x30,nodes[1]);put(nodes[1]+0x2c,nodes[2])
    put(nodes[0]+0x2c,nodes[3])
    p.uc.mem_write(descriptor,bytes(80));put(descriptor,unit,0)
    put(descriptor+0x20,900,0,0x40 if with_children else 0)
    p.uc.mem_write(output,bytes(4096));allocations.clear()
    _,error=create()
    assert not error,error
    total=3 if with_children else 1
    assert allocations==[540+total*68],allocations
    assert get(output+0x50)==(total,)
    copied=output+0x21c
    assert get(copied+0x2c)==(0,)
    assert get(copied+0x34)==(0,)
    assert get(copied+0x30)==((copied+56) if with_children else 0,)
    for i,node in enumerate(nodes):
        visible=int.from_bytes(p.uc.mem_read(node+0x2a,2),'little')&1
        assert visible==int(i>=total),(with_children,i,visible)
    if with_children:
        assert get(copied+56+0x2c)==(copied+112,)
        assert get(copied+56+0x34)==(copied,)
        assert get(copied+112+0x34)==(copied+56,)
        for i in range(3):
            assert get(copied+i*56)==(models[i],)
            assert int.from_bytes(p.uc.mem_read(copied+i*56+0x2a,2),'little')&1
print('PASS: native child-subtree construction includes child siblings only with flag 0x40, preserves copied visibility and leaves root siblings attached')

for slot_available,allocation_ok in ((False,True),(True,False)):
    for node in nodes:
        p.uc.mem_write(node+0x2a,b'\x01\x00')
    allocations.clear()
    _,error=create()
    assert not error,error
    assert bool(allocations)==slot_available
    assert all(int.from_bytes(p.uc.mem_read(node+0x2a,2),'little')&1 for node in nodes)
print('PASS: exhausted debris slots and failed allocation leave source visibility unchanged')

slot_available=allocation_ok=True
for enabled in (False,True):
    for query_result in (-1,0,1,37):
        blood_calls.clear()
        p.uc.mem_write(options+0x11,bytes([enabled]))
        p.uc.mem_write(output,bytes(4096))
        _,error=create()
        assert not error,error
        expected=[('query',)] if enabled else []
        attached=enabled and query_result!=-1
        if attached:expected += [('allocate',),('construct',),('start',)]
        assert blood_calls==expected,(enabled,query_result,blood_calls)
        assert get(output+0x1d0)==(blood_object if attached else 0,)
print('PASS: debris QueryBlood gate accepts zero and every non-minus-one output; creates capacity-100, kind-3 blood emitter at body origin only when enabled')
