#!/usr/bin/env python3
"""Trace Araarch's native pre-boarding Move_Seek_Pickup approach on Lokken.

The default run constructs and dispatches actual code-30 orders, then follows
the native PathNavigator route/mover over full Lake Lokken TNT terrain until
the pickup circle. ``--near-circle-pair`` seeds the passenger at the verified
map-valid endpoint and dispatches both real pickup handlers through native
boarding. ``--continuous-pair`` dispatches both handlers while Araarch follows
that full-map route to boarding. Both paired modes use a zero-filled +0xc0
mount/pose-anchor table and stop before the attached passenger's display update.
Worker/player scheduling callbacks remain controlled.

Run from the repository root:
    PYTHONPATH=tools/re python3 tools/re/probe_lokken_passenger_approach_native.py
    PYTHONPATH=tools/re python3 tools/re/probe_lokken_passenger_approach_native.py \\
        --near-circle-pair --max-ticks 200
    PYTHONPATH=tools/re python3 tools/re/probe_lokken_passenger_approach_native.py \\
        --continuous-pair --max-ticks 9000
"""
import argparse
import hashlib
import math
import pickle
import re
import struct
from collections import Counter
from pathlib import Path
from probe_surface_pickup_native_map_route import run as run_route
from probe_surface_pickup_native_fullmap import install_native_entity_array
from probe_surface_pickup_native_mission import run_world
from check_surface_unload_map_grades import native_grade_reader
from check_surface_unload_map_release import cat
from balance_inputs import properties, unit_properties, class_record
from emuphase import GS, OBJ
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_ESI, UC_X86_REG_ESP

repo = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--retail-root', default='/home/pocket_geek/tak_data')
parser.add_argument('--hpitool', default=str(repo / 'build-o2/hpitool'))
parser.add_argument('--world-binary', default=str(repo / 'build-o2/transport_test'))
parser.add_argument('--max-ticks', type=int, default=5000)
parser.add_argument('--setup-only', action='store_true',
                    help='stop after the first and next-tick native passenger dispatches')
parser.add_argument('--near-circle-pair', action='store_true',
                    help='start at the previously verified map-valid passenger endpoint and dispatch both pickup handlers')
parser.add_argument('--continuous-pair', action='store_true',
                    help='dispatch both pickup handlers while Araarch follows its full-map route to boarding')
args = parser.parse_args()
paired_dispatch=args.near_circle_pair or args.continuous_pair
root=Path(args.retail_root).resolve()
hpitool=Path(args.hpitool).resolve()
state=run_route(root,hpitool,full_map=True,native_attachment=True)
phase,live,uc=state['phase'],state['live'],state['phase'].uc
get,byte=live.get,live.byte
def put(address,*values): uc.mem_write(address,struct.pack('<'+'I'*len(values),*(v&0xffffffff for v in values)))
pool,carrier,passenger=install_native_entity_array(state)

if paired_dispatch:
    # Desktop units have a per-entity +0xc0 mount/pose-anchor table consulted
    # by 0x4dd370 during ground motion. The headless entity-pool copy omits
    # that presentation service. Supply a zeroed, bounded table to keep its
    # reads in range; this is a fixture seam, not retail art/anchor parity.
    passenger_anchor_table=phase._alloc(0x400)
    uc.mem_write(passenger_anchor_table,bytes(0x400))
    put(passenger+0xC0,passenger_anchor_table)
    assert get(passenger+0xC0)==passenger_anchor_table
else:
    passenger_anchor_table=0

# Create a separate native Araarch PathNavigator and mover so its code-30
# order cannot mutate the carrier's existing pickup navigator.
nav=phase._alloc(0x180); mover=phase._alloc(0x80); nav_vtable=0x5F2A24
uc.mem_write(nav,bytes(0x180)); put(nav,nav_vtable,0,passenger)
uc.mem_write(mover,bytes(0x80)); put(mover,nav,0)
put(passenger+8,mover)

# Read the passenger's real GROUND2 movement profile and grade every Lake Lokken
# TNT cell through native 0x508cd0 in a standalone oracle instance.
moveinfo=cat(hpitool,root,'data.hpi','gamedata/moveinfo.tdf').decode('latin1')
araarch=cat(hpitool,root,'data.hpi','units/araarch.fbi').decode('latin1')
unit_fields=unit_properties(araarch)
movement=unit_fields['movementclass'].lower()
class_fields=next(properties(b) for b in re.findall(r'\[[^]]+\]\s*\{([^{}]*)\}',moveinfo,re.S)
                   if properties(b).get('name','').lower()==movement)
profile=class_record(class_fields); fx,fz=struct.unpack_from('<hh',profile)
assert (movement,fx,fz)==('ground2',2,2),(movement,fx,fz)
map_data=state['map_data']; width,height=state['width'],state['height']
grade=native_grade_reader(map_data,profile)
grade_hash=hashlib.sha256(profile+bytes(map_data[3])+
    struct.pack('<'+'H'*len(map_data[4]),*map_data[4])).hexdigest()
cache=Path('/tmp')/f'tak-lokken-ground2-{grade_hash}.pkl'
if cache.exists():
    grades=pickle.loads(cache.read_bytes())
    assert len(grades)==width*height,(len(grades),width*height)
else:
    print(f'grading {width*height} GROUND2 map cells through native 0x508cd0...',flush=True)
    grades=[grade(x,z) for z in range(height) for x in range(width)]
    cache.write_bytes(pickle.dumps(grades))
print(f'GROUND2 grade cache: {cache}',flush=True)
phase.set_grade_plane(grades)
uc.mem_write(phase.GRID+4,profile[:4]); uc.mem_write(phase.GRID+8,profile[4:])

# Give Araarch a separate type record matching its shipped movement class and
# movement inputs. Carrier's native type remains the Vertrans WATER4 record.
ptype=phase._alloc(0x400); uc.mem_write(ptype,bytes(0x400))
# Retail 0x507400 indexes a per-type footprint byte mask at +0x12a.
footprint_mask=phase._alloc(0x1000); put(ptype+0x12a,footprint_mask)
put(passenger+0xB4,ptype)
put(ptype+0x126,fx | (fz<<16)); put(ptype+0x18A,phase.GRID)
put(ptype+0x18E,int(float(unit_fields.get('turnrate',500))))
put(ptype+0x172,int(float(unit_fields.get('roadmultiplier',1.0))*65536))
put(ptype+0x16E,int(float(unit_fields.get('watermultiplier',1.0))*65536))
put(ptype+0x14A,1<<16); put(ptype+0x260,0); put(ptype+0x264,0x200)
for key,off,default in [('maxvelocity',0x162,0),('brakerate',0x166,0.5),('acceleration',0x16A,0.5)]:
    put(ptype+off,int(float(unit_fields.get(key,default))*65536))
put(ptype+0x18A,phase.GRID)
put(ptype+0x248,0); byte(ptype+0x24A,0); byte(ptype+0x249,6)

# Replace controlled unit IDs with the real entity pool for the handler's
# global lookup. Owner setup is restored for native mission queue insertion.
owner=live.owner; put(owner,1); byte(owner+0xEA,1)
# The carrier is still at its real pickup start; the passenger remains at the
# authored shore target. Install the retail code-30 passenger load order before moving
# the carrier and dispatch it through the original retail handler.
if args.near_circle_pair:
    # This is the exact shoreline endpoint reached in the full 6,286-tick
    # route above: it is inside the retail 284px circle and has a valid GROUND2
    # footprint. Seeding this verified endpoint keeps the complementary carrier
    # handoff test short; it does not claim a continuous walk from the spawn.
    near_x,near_y,near_z=3560.838104248047,52.0,1840.3680114746094
    put(passenger+0x68,int(near_x*65536)); put(passenger+0x6C,int(near_y*65536))
    put(passenger+0x70,int(near_z*65536))
else:
    put(passenger+0x68,240*16*65536); put(passenger+0x6C,state['sea']*65536); put(passenger+0x70,350*16*65536)
put(carrier+0x68,240*16*65536); put(carrier+0x6C,state['sea']*65536); put(carrier+0x70,120*16*65536)
put(0x62D55C,GS)
# Match the native mission-dispatch context used by the air/isolated probes:
# 0x4d8450 checks the game clock/active marker and the unit's COB VM.
put(GS+0x19F30,1); put(GS+0x174C8,101); put(GS+0x174CC,102)
empty_vm,empty_descriptor=phase._alloc(0x40),phase._alloc(0x40)
put(passenger+0xBC,empty_vm); put(empty_vm+0x0C,empty_descriptor)
put(empty_descriptor+4,0)
# Build retail's sorted mission registry before constructing Move_Seek_Pickup.
byte(0x62DB80,0)
put(0x62DB84,0,0,0)
for registration in (0x402740,0x4092E0,0x421850):
    _,error=phase.icd.call(registration)
    assert error is None,('native mission descriptor registration',hex(registration),error)
descriptors,descriptor_end=get(0x62DB84),get(0x62DB88)
assert (descriptor_end-descriptors)//25==76
registry_hooks=dict(phase.icd.hooks)
registry_hooks.pop(0x4D4BF0,None)
phase.icd.hooks=registry_hooks
ground_name=phase._alloc(4)
_,error=phase.icd.call(0x4D4BF0,(0x604C00,),ecx=ground_name)
assert error is None,error
ground_pickup_code=uc.mem_read(ground_name,1)[0]
assert ground_pickup_code>0,('retail Ground_Pickup was not registered',ground_pickup_code)
assert get(descriptors+ground_pickup_code*25+4)==0x408860
ground_row_name=get(descriptors+ground_pickup_code*25+0x15)
assert bytes(uc.mem_read(ground_row_name,64)).split(b'\0')[0].lower()==b'ground_pickup'
move_name=phase._alloc(4)
_,error=phase.icd.call(0x4D4BF0,(0x604CF8,),ecx=move_name)
assert error is None,error
move_pickup_code=uc.mem_read(move_name,1)[0]
assert move_pickup_code==30,('retail Move_Seek_Pickup code',move_pickup_code)
assert get(descriptors+move_pickup_code*25+4)==0x403430
old_placeholder=get(passenger+0x60)
assert old_placeholder, 'expected route fixture placeholder'
# run_route's carrier is held at the exact Lokken start but its queue head is
# a synthetic GROUND_PICKUP code-1 fixture order. Its +0xc4 reference chain is
# empty at this point; clear that head and pair real retail orders in both
# directions so 0x403430 validates descriptor 16 on the carrier.
old_carrier_order=get(carrier+0x60)
assert old_carrier_order and uc.mem_read(old_carrier_order+4,1)[0]==1
assert get(carrier+0xC4)==0 and get(passenger+0xC4)==0
# The route fixture already installed a native circle controller for its
# controlled code-1 carrier-order alias. Retire that controller through the
# native binding method before replacing its queue head with the real code-21
# GROUND_PICKUP order, so the new order starts with an empty navigator slot.
if get(old_carrier_order+0x6E):
    _,error=phase.icd.call(0x4D4D40,(0,),ecx=old_carrier_order)
    assert error is None,('detach route-fixture carrier controller',error)
    carrier_nav=get(get(carrier+8))
    assert get(old_carrier_order+0x6E)==0 and get(carrier_nav+4)==0
put(carrier+0x60,0)
put(passenger+0x60,0)
passenger_order=phase._alloc(0x80)
result,error=phase.icd.call(0x4D6C40,(move_pickup_code,carrier,0,0,0,0,0,0,0,0,0,0),ecx=passenger_order)
assert error is None,(result,error)
assert result==passenger_order
_,error=phase.icd.call(0x4D7750,(passenger,passenger_order)); assert error is None,error
assert get(passenger+0x60)==passenger_order
assert uc.mem_read(passenger_order+4,1)[0]==move_pickup_code
assert get(passenger_order+0x0E)==passenger
assert get(passenger_order+0x16)==carrier
assert get(carrier+0xC4)==passenger_order+0x12
carrier_order=phase._alloc(0x80)
result,error=phase.icd.call(0x4D6C40,(ground_pickup_code,passenger,0,0,0,0,0,0,0,0,0,0),ecx=carrier_order)
assert error is None,(result,error)
assert result==carrier_order
_,error=phase.icd.call(0x4D7750,(carrier,carrier_order)); assert error is None,error
assert get(carrier+0x60)==carrier_order
assert uc.mem_read(carrier_order+4,1)[0]==ground_pickup_code
assert get(carrier_order+0x0E)==carrier
assert get(carrier_order+0x16)==passenger
assert get(passenger+0xC4)==carrier_order+0x12
assert get(carrier+0xC4)==passenger_order+0x12
passenger_controller=phase._alloc(0x20)
start=(221,114) if args.near_circle_pair else (240-fx//2,350-fz//2)
phase.attach_live_request(passenger,mover,nav,passenger_controller,start,(fx,fz))
# attach_live_request reuses the search-handle argument as nav+4; for a live
# passenger order that slot must begin empty so retail's first SetController
# cannot emit a false removal event for this search fixture object.
put(nav+4,0)
assert get(nav+4)==0 and get(OBJ+0x68)==passenger_controller
# Route-search scheduling scans the owner's inclusive entity range. This probe
# exposes only the passenger to that native worker; an intentionally empty range
# causes its cursor at OBJ+0x115 to walk unboundedly past the pool.
put(owner+0x74,passenger,passenger)
put(OBJ+0x115,passenger)
phase.set_grade_plane(grades); uc.mem_write(phase.GRID+4,profile[:4]); uc.mem_write(phase.GRID+8,profile[4:])

# The route fixture used a controlled enqueue/lookup pair. Remove both for the
# Araarch request so retail 0x4e4f50 updates the real player counter, nav flags,
# and unit request bits; keep only the scheduler/feedback service seams.
worker_events=[];route_installs=[];native_handler_ticks=[];native_request_events=[]
feedback_events=[];arrival_trace=[]
arrival_counts=Counter();predicate_samples=[];detach_samples=[];event_set_samples=[];event_consumptions=[]
hooks=dict(phase.icd.hooks)
hooks.pop(0x4D6AD0,None)  # let retail retire the native code-30 order on arrival
def prepare(_uc,_sp): worker_events.append(('prepare',));return 2,0
def finish(_uc,_sp):
    worker_events.append(('finish',))
    return 1,0
def reqweight(_uc,sp):
    addr=struct.unpack('<I',uc.mem_read(sp,4))[0];put(addr,196608);return 1,addr
def visible(_uc,_sp):return 4,1
def nogate(_uc,_sp):return 0,0
def nospecial(_uc,_sp):return 2,0
def livegrade(_uc,sp):
    _who,x,_y,z=struct.unpack('<Iiii',uc.mem_read(sp,16))
    cx=((x>>19)-fx)//2; cz=((z>>19)-fz)//2
    if not (0<=cx<width and 0<=cz<height):return 4,0
    return 4,grades[cz*width+cx]
lookup_method=get(get(nav)+0x18)
controller_allocations=[passenger_controller]
def route_alloc(_uc,sp):
    size=struct.unpack('<I',uc.mem_read(sp,4))[0]
    if size==0x14 and controller_allocations:
        return 0,controller_allocations.pop(0)
    return 0,phase._alloc(size)
hooks.pop(0x4D4BF0,None)  # use retail's sorted name lookup, not a numeric fixture alias
hooks.pop(0x4E4F50,None)  # native request admission updates the player queue and nav bits
hooks.pop(0x4E2470,None)  # native event setter must receive circle-arrival 0x100
hooks.pop(lookup_method,None)  # use the native PathNavigator lookup implementation
hooks.update({0x4EB9E0:route_alloc,0x4E1EE0:prepare,
              0x4E2060:finish,0x413C80:visible,0x409FE0:nogate,
              0x4DB640:livegrade,0x50E600:nospecial,0x4161B0:reqweight})
phase.icd.hooks=hooks
# Existing specific Unicorn callbacks refer to the Icd object and see the new hook map.
# Observe native SetRoute at 0x4e4f05 (after route points are copied), not as a
# path-worker delivery callback. SetRoute calls 0x4e4f50 with a null argument
# to release its pending request; only non-null 0x4e4f50 calls admit work.
def observe(machine,address,_size,_data):
    if address==0x4E4F05 and machine.reg_read(UC_X86_REG_ESI)==nav:
        count=get(nav+0x10C)
        words=struct.unpack('<'+'h'*(count*2),machine.mem_read(nav+12,count*4)) if count else ()
        route_installs.append(list(zip(words[::2],words[1::2])))
    if address==0x4E4F50:
        sp=machine.reg_read(UC_X86_REG_ESP)
        ret,arg=struct.unpack('<II',machine.mem_read(sp,8))
        native_request_events.append((get(GS+0x19F44),arg,get(nav+0x114),
            get(0x634674+4*uc.mem_read(owner+0xEB,1)[0]),
            ret))
    if address==0x403430:
        native_handler_ticks.append(get(GS+0x19F44))
    if address==0x4E2470:
        sp=machine.reg_read(UC_X86_REG_ESP)
        ret,arg=struct.unpack('<II',machine.mem_read(sp,8))
        receiver=machine.reg_read(UC_X86_REG_ECX)
        event_order=get(receiver+4) if receiver else 0
        before=get(event_order+0x6A) if event_order else 0
        feedback_events.append((get(GS+0x19F44),arg,receiver,ret,event_order,before))
    if address==0x4E5175 and machine.reg_read(UC_X86_REG_ESI)==nav:
        controller=get(nav+4)
        event_order=get(controller+4) if controller else 0
        event_bits=get(event_order+0x6A) if event_order else 0
        record=(get(GS+0x19F44),controller,event_order,event_bits)
        event_set_samples.append(record)
        assert event_bits&0x100,('native 0x4e2470 did not set circle-arrival event',record)
uc.hook_add(UC_HOOK_CODE,observe)
assert 0x4E2470 not in phase.icd.hooks, '0x4e2470 substitution remains active'

# Install the native global search service and height-sector plane before the
# actual code-30 request reaches 0x416430.
search_service=phase._alloc(0x22B)
_,error=phase.icd.call(0x415F80,(),ecx=search_service)
assert error is None,('native global path-search constructor',error)
put(GS+0x19E70,search_service)
put(GS+0x19E88,width*16); put(GS+0x19E8C,height*16)
_,error=phase.icd.call(0x50E740)
assert error is None,('native map height-sector initializer',error)
sector_grid=get(GS+0x19F18); sector_stride=get(GS+0x19F1C)
assert sector_stride==(width+7)//8,('native sector width',sector_stride)
visibility=get(GS+0x19EF4)
uc.mem_write(visibility,struct.pack('<'+'H'*(width*height//4),*([0xFFFF]*(width*height//4))))
put(live.owner+0x8C,width//2,height//2); byte(GS+0x306F,0); put(GS+0x19EF8,state['sea'])
sector_x,sector_z=240//8,350//8
passenger_sector=sector_grid+(sector_z*sector_stride+sector_x)*10
_,error=phase.icd.call(0x506650,(passenger,passenger_sector))
assert error is None,('insert Araarch sector',error)
put(passenger+0x12B,int(float(unit_fields['maxvelocity'])*65536))
# Preserve mover+0 (navigator) and +4 (grade grid) installed by
# attach_live_request; +8 and +0x14 are zero in the native map-mover fixture.
put(mover+0x20,0); put(mover+0x30,0x7FFFFFFF); put(mover+0x14,0)
uc.mem_write(mover+0x36,struct.pack('<H',1))

# Native dispatcher constructs into the preallocated search handle, binds it
# through Araarch's real PathNavigator vtable, and asks the retail worker for a route.
tick=state['delivered_at']+1; put(GS+0x19F44,tick)
uc.mem_write(0x634674,bytes(40))  # discard the route fixture's worker budget
put(OBJ+0x165,0)
dispatch_sites=[]
def observe_dispatch(machine,address,_size,_data):
    if address in (0x4D8450,0x403430,0x4D4DA0,0x4E2500,0x4D4D40):
        dispatch_sites.append((address,get(passenger_order+0x6E),get(nav+4)))
for address in (0x4D8450,0x403430,0x4D4DA0,0x4E2500,0x4D4D40):
    uc.hook_add(UC_HOOK_CODE,observe_dispatch,begin=address,end=address)
_,error=phase.icd.call(0x4D8450,(passenger,))
if error: raise RuntimeError(error)
if args.setup_only:
    def setup_snapshot(label, current_tick):
        active=get(passenger+0x60)
        active_code=uc.mem_read(active+4,1)[0] if active else 0
        return (label,current_tick,hex(active),active_code,
            uc.mem_read(passenger_order+5,1)[0],hex(get(passenger_order+0x6A)),
            hex(get(passenger_order+0x6E)),hex(get(nav+4)),
            hex(get(carrier+0x60)),uc.mem_read(carrier_order+5,1)[0],
            hex(get(carrier_order+0x6A)),hex(get(carrier_order+0x6E)),
            hex(get(carrier+0xC4)),hex(get(passenger+0xC4)),
            hex(get(passenger_order+0x5A)),hex(get(carrier_order+0x5A)),
            tuple(feedback_events),tuple(dispatch_sites),
            tuple(native_request_events),hex(get(0x634674+4*uc.mem_read(owner+0xEB,1)[0])))
    print('setup after first dispatcher:',setup_snapshot('first',tick),flush=True)
    next_tick=tick+1;put(GS+0x19F44,next_tick)
    _,error=phase.icd.call(0x4D8450,(passenger,))
    if error: raise RuntimeError(('next-tick passenger dispatcher',next_tick,error))
    print('setup after next dispatcher:',setup_snapshot('next',next_tick),flush=True)
    raise SystemExit(0)
assert get(passenger_order+0x6e), ('native dispatcher did not create controller',dispatch_sites,
    hex(get(passenger+0x60)),hex(get(carrier+0x60)),hex(get(carrier+0xC4)),
    hex(get(passenger+0x130)),hex(get(passenger+0xBC)),
    'order_stage',uc.mem_read(passenger_order+5,1)[0],
    'order_events',hex(get(passenger_order+0x6A)),
    'feedback_events',feedback_events,'worker_events',worker_events,
    'nav_controller',hex(get(nav+4)))
assert get(nav+4)==get(passenger_order+0x6e),('controller not installed',hex(get(nav+4)),hex(get(passenger_order+0x6e)))
player_index=uc.mem_read(owner+0xEB,1)[0]
assert player_index<10,('owner player index',player_index)
native_queue_count=get(0x634674+player_index*4)
assert native_queue_count>0,('native 0x4e4f50 did not admit the request',native_queue_count)
assert any(arg for _,arg,*_ in native_request_events), 'native 0x4e4f50 admission was not observed'
assert native_handler_ticks, 'native retail code-30 handler was not executed'
dispatch_return=(get(nav+0x114),native_queue_count,tuple(native_request_events),
                 tuple(dispatch_sites),get(passenger_order+0x6A))

carrier_dispatch_sites=[]
def observe_carrier_dispatch(machine,address,_size,_data):
    if address in (0x4D8450,0x408860,0x4D4DA0,0x4D4D40,0x51B4F0,0x51B5A0):
        carrier_dispatch_sites.append((get(GS+0x19F44),address,
            uc.mem_read(carrier_order+5,1)[0],get(carrier_order+0x6A),
            get(carrier_order+0x6E),get(carrier+0xAC),get(passenger+0xA8),
            get(passenger+0xD0)))
uc.hook_add(UC_HOOK_CODE,observe_carrier_dispatch)
if paired_dispatch:
    _,error=phase.icd.call(0x4D8450,(carrier,))
    if error: raise RuntimeError(('native carrier GROUND_PICKUP initialization',tick,error))
    assert get(carrier+0x60)==carrier_order

# The native circle constructor used the preallocated controller already
# attached to the singleton route search object before 0x4e54e0 was called.
handle=get(nav+4)

for worker_tick in range(tick,tick+500):
    put(GS+0x19F44,worker_tick)
    _,error=phase.icd.call(0x416430,(1,),ecx=OBJ)
    if error: raise RuntimeError(('worker',worker_tick,error))
    native_queue_count=get(0x634674+player_index*4)
    if route_installs and native_queue_count==0:
        worker_route_install_tick=worker_tick
        break
else:
    raise AssertionError(('native Araarch route was not installed',worker_events[-30:],
                          native_queue_count,hex(get(nav+0x114))))
nav_pops=[]
def observe_waypoint_pop(machine,address,_size,_data):
    if address==0x4E50A0 and machine.reg_read(UC_X86_REG_ECX)==nav:
        count=get(nav+0x10C)
        words=struct.unpack('<'+'h'*(count*2),machine.mem_read(nav+12,count*4)) if count else ()
        pos=struct.unpack('<3i',machine.mem_read(passenger+0x68,12))
        points=tuple(zip(words[::2],words[1::2]))
        nav_pops.append((get(GS+0x19F44),count,points[:2],points[-2:],
                         (pos[0]/65536,pos[2]/65536),get(nav+0x110),get(nav+0x114)))
uc.hook_add(UC_HOOK_CODE,observe_waypoint_pop)
def observe_circle_arrival(machine,address,_size,_data):
    now=get(GS+0x19F44)
    controller=get(nav+4)
    pos=struct.unpack('<3i',machine.mem_read(passenger+0x68,12))
    px,pz=pos[0]/65536,pos[2]/65536
    distance=math.hypot(px-circle_x*16,pz-circle_z*16)
    if address==0x4E5150 and machine.reg_read(UC_X86_REG_ECX)==nav:
        arrival_counts['entry']+=1
        arrival_counts['entry-with-controller' if controller else 'entry-null-controller']+=1
        cvt=get(controller) if controller else 0
        if controller and (distance<=600 or len(predicate_samples)<5):
            arrival_trace.append(('arrival-entry',now,controller,(px,pz),distance,
                get(passenger+0x60),get(passenger+0x64),get(carrier+0x60),get(carrier+0x64),
                get(cvt+0x10),get(cvt+0x2C)))
    elif address==0x4E5164 and machine.reg_read(UC_X86_REG_ESI)==nav:
        result=machine.reg_read(UC_X86_REG_EAX)
        arrival_counts['predicate-true' if result else 'predicate-false']+=1
        predicate_samples.append((now,result,controller,(px,pz),distance,
            get(passenger+0x60),get(carrier+0x60)))
        if controller and (distance<=600 or result):
            arrival_trace.append(('circle-test-result',now,result,controller,(px,pz),distance))
    elif address==0x4E517D and machine.reg_read(UC_X86_REG_ESI)==nav:
        result=machine.reg_read(UC_X86_REG_EAX)
        arrival_counts['controller-state-truthy' if result else 'controller-state-false']+=1
        if controller and distance<=600:
            arrival_trace.append(('controller-state-result',now,result,controller,(px,pz),distance))
    elif address==0x4E5186 and machine.reg_read(UC_X86_REG_ECX)==nav:
        vtable=get(nav)
        target=get(vtable+4)
        esp=machine.reg_read(UC_X86_REG_ESP)
        event=('detach-callsite',now,controller,target,
            struct.unpack('<I',machine.mem_read(esp,4))[0],(px,pz),distance,
            get(passenger+0x60),get(carrier+0x60))
        detach_samples.append(event)
        arrival_trace.append(event)
    elif address==0x4E54E0 and machine.reg_read(UC_X86_REG_ECX)==nav:
        esp=machine.reg_read(UC_X86_REG_ESP)
        ret,arg=struct.unpack('<II',machine.mem_read(esp,8))
        if ret==0x4E5189:
            arrival_counts['native-detach-method-call']+=1
            arrival_trace.append(('native-detach-method-entry',now,ret,arg,
                get(nav+4),(px,pz),distance,get(passenger+0x60),get(carrier+0x60)))
for address in (0x4E5150,0x4E5164,0x4E517D,0x4E5186,0x4E54E0):
    uc.hook_add(UC_HOOK_CODE,observe_circle_arrival,begin=address,end=address)

start_position=struct.unpack('<3i',uc.mem_read(passenger+0x68,12))
last_position=start_position
last_move_tick=tick
start_x,start_z=start_position[0]/65536,start_position[2]/65536
max_displacement=0.0; max_displacement_tick=tick; total_distance=0.0
last_step_distance=0.0
circle_x,circle_z,circle_radius=struct.unpack('<hhI',uc.mem_read(handle+8,8))
terminal_reason='tick-limit'; plateau_ticks=1500; circle_event_tick=None
for i in range(1,args.max_ticks+1):
    now=tick+i
    put(GS+0x19F44,now); put(0x64186c,now)
    if paired_dispatch and get(carrier+0x60)==carrier_order:
        _,error=phase.icd.call(0x4D8450,(carrier,))
        if error: raise RuntimeError(('native Vertrans GROUND_PICKUP dispatcher',now,error))
    event_before=get(passenger_order+0x6A)
    order_before=(get(passenger+0x60),uc.mem_read(passenger_order+5,1)[0],event_before)
    _,error=phase.icd.call(0x4D8450,(passenger,))
    if error: raise RuntimeError(('Araarch native dispatcher',now,error))
    if (paired_dispatch and get(carrier+0xAC)==passenger and
            get(passenger+0xA8)==carrier):
        # The passenger is cargo now. Its ordinary ground mover is no longer
        # the next retail update; the display/mount pose updater owns it.
        terminal_reason='native-attachment'
        break
    if event_before&0x100:
        order_after=(get(passenger+0x60),uc.mem_read(passenger_order+5,1)[0],
                     get(passenger_order+0x6A))
        transitioned=(not (order_after[2]&0x100) or order_after[0]!=order_before[0]
                      or order_after[1]!=order_before[1])
        event_consumptions.append((now,order_before,order_after,transitioned))
        assert transitioned,('native dispatcher failed to consume/transition circle event',
                             now,order_before,order_after)
        terminal_reason='circle-event-consumed'
    if get(0x634674+player_index*4)>0:
        _,error=phase.icd.call(0x416430,(1,),ecx=OBJ)
        if error: raise RuntimeError(('native route worker',now,error))
    _,error=phase.icd.call(0x4DC800,(passenger,),ecx=mover)
    if error:
        if paired_dispatch:
            print('PAIRED_PICKUP_PARTIAL',{'tick':now,
                'passenger_order':(hex(get(passenger+0x60)),
                    uc.mem_read(passenger_order+5,1)[0],hex(get(passenger_order+0x6A)),
                    hex(get(passenger_order+0x6E))),
                'carrier_order':(hex(get(carrier+0x60)),
                    uc.mem_read(carrier_order+5,1)[0],hex(get(carrier_order+0x6A)),
                    hex(get(carrier_order+0x6E))),
                'carrier_dispatch_tail':carrier_dispatch_sites[-20:],
                'cargo':(hex(get(carrier+0xAC)),hex(get(passenger+0xA8))),
                'unit_events':hex(get(passenger+0xD0)),
                'anchor_table':hex(get(passenger+0xC0))},flush=True)
        raise RuntimeError(('native Araarch mover',now,error))
    _,error=phase.icd.call(0x51B2A0,(passenger,),ecx=mover)
    if error: raise RuntimeError(('native Araarch position commit',now,error))

    position=struct.unpack('<3i',uc.mem_read(passenger+0x68,12))
    if position!=last_position:
        last_x,last_z=last_position[0]/65536,last_position[2]/65536
        current_x,current_z=position[0]/65536,position[2]/65536
        last_step_distance=math.hypot(current_x-last_x,current_z-last_z)
        total_distance+=last_step_distance
        displacement=math.hypot(current_x-start_x,current_z-start_z)
        if displacement>max_displacement:
            max_displacement,max_displacement_tick=displacement,now
        last_position=position
        last_move_tick=now
    if get(passenger+0x60)!=passenger_order:
        terminal_reason='passenger-order-retired'
        break
    if get(passenger_order+0x6A)&0x100:
        if circle_event_tick is None:
            circle_event_tick=now
    if get(carrier+0xAC)==passenger and get(passenger+0xA8)==carrier:
        terminal_reason='native-attachment'
        break
    if now-last_move_tick>=plateau_ticks:
        terminal_reason='no-movement-plateau'
        break
    if i%1000==0:
        px,pz=position[0]/65536,position[2]/65536
        distance=math.hypot(px-circle_x*16,pz-circle_z*16)
        print(f'  progress tick={now}, x/z=({px:.1f},{pz:.1f}), '
              f'displacement={math.hypot(px-start_x,pz-start_z):.1f}px, '
              f'circle distance={distance:.1f}px, route_count={get(nav+0x10C)}, '
              f'route_installs={len(route_installs)}, pops={len(nav_pops)}, '
              f'queue_count={get(0x634674+4*player_index)}',flush=True)

position=struct.unpack('<3i',uc.mem_read(passenger+0x68,12))
world_x,world_z=position[0]/65536,position[2]/65536
circle_world_x,circle_world_z=circle_x*16,circle_z*16
circle_distance=math.hypot(world_x-circle_world_x,world_z-circle_world_z)
route_count=get(nav+0x10C)
route_words=struct.unpack('<'+'h'*(route_count*2),uc.mem_read(nav+12,route_count*4)) if route_count else ()
route_points=tuple(zip(route_words[::2],route_words[1::2]))
admission_tick=get(nav+0x110)
order_head=get(passenger+0x60)
assert order_head==passenger_order or terminal_reason in (
    'passenger-order-retired','native-attachment')
attached=(get(carrier+0xAC)==passenger and get(passenger+0xA8)==carrier)
assert (get(carrier+0xAC)==0 and get(passenger+0xA8)==0) or attached
if paired_dispatch and terminal_reason=='native-attachment':
    assert attached, 'native dispatcher retired pickup order without cargo links'
    assert get(carrier+0x60)==0, 'native carrier pickup order was not retired'
    assert get(passenger+0x60)!=passenger_order, 'passenger pickup order was not replaced'
    assert uc.mem_read(get(passenger+0x60)+4,1)[0]==11, (
        'retail did not replace Move_Seek_Pickup with BeCarried')
    assert any(row[1]==0x51B4F0 for row in carrier_dispatch_sites), carrier_dispatch_sites
    assert any(row[1]==0x51B5A0 for row in carrier_dispatch_sites), carrier_dispatch_sites
if args.continuous_pair:
    assert max_displacement>4000, max_displacement
    assert circle_event_tick is not None, 'native circle-arrival event was not consumed'
    assert any(sample[1] for sample in predicate_samples), predicate_samples[-20:]
    attachment_ticks={row[0] for row in carrier_dispatch_sites
                      if row[1] in (0x51B4F0,0x51B5A0)}
    assert attachment_ticks=={tick+i}, attachment_ticks
if order_head==passenger_order:
    assert get(carrier+0xC4)==passenger_order+0x12
elif terminal_reason=='native-attachment':
    # Retail replaced the pickup order with its carried order. Record the
    # resulting cross-reference state without assuming it is empty.
    pass
else:
    assert not get(carrier+0xC4)
if paired_dispatch:
    carried_order=get(passenger+0x60)
    assert terminal_reason=='native-attachment', terminal_reason
    print('PAIRED_PICKUP_RESULT',{'anchor_table':hex(passenger_anchor_table),
        'ticks':i,'terminal_reason':terminal_reason,
        'passenger_head':hex(get(passenger+0x60)),
        'passenger_head_code':uc.mem_read(carried_order+4,1)[0],
        'passenger_stage':uc.mem_read(passenger_order+5,1)[0],
        'passenger_events':hex(get(passenger_order+0x6A)),
        'passenger_controller':hex(get(passenger_order+0x6E)),
        'carrier_head':hex(get(carrier+0x60)),
        'carrier_stage':uc.mem_read(carrier_order+5,1)[0],
        'carrier_events':hex(get(carrier_order+0x6A)),
        'carrier_controller':hex(get(carrier_order+0x6E)),
        'cross_references':(hex(get(carrier+0xC4)),hex(get(passenger+0xC4))),
        'carrier_dispatch_tail':carrier_dispatch_sites[-30:],
        'cargo':(hex(get(carrier+0xAC)),hex(get(passenger+0xA8))),
        'passenger_unit_events':hex(get(passenger+0xD0))},flush=True)
    source='seeded at the verified endpoint' if args.near_circle_pair else 'moving on the full-map route'
    print(f'PASS: native GROUND_PICKUP code {ground_pickup_code} and '
          f'Move_Seek_Pickup code {move_pickup_code} attach after Araarch is '
          f'{source}, on tick {tick+i}; the passenger ground mover stops at '
          'the attachment boundary.')
assert route_installs,('native SetRoute was not observed',worker_events[-30:],
                  native_request_events,get(nav+0x114),get(0x634674+4*player_index))
if not nav_pops and terminal_reason!='native-attachment':
    raise AssertionError(('native route installed but no waypoint pop in bound',
        'max_ticks',args.max_ticks,'worker_route_install_tick',worker_route_install_tick,
        'route',route_installs[-1],'route_count',route_count,'nav_flags',hex(get(nav+0x114)),
        'admission_tick',admission_tick,'mover_state',tuple(struct.unpack('<8I',uc.mem_read(mover,0x20))),
        'position',position,'start',start_position,'last_move_tick',last_move_tick,
        'native_requests',native_request_events,'queue_count',get(0x634674+4*player_index)))

if paired_dispatch:
    world_tick=None
else:
    world_tick,_world_output=run_world(Path(args.world_binary).resolve(),root)
print(f'PASS: native Araarch Move_Seek_Pickup code {move_pickup_code} order '
      f'{passenger_order:#010x} installed through 0x4d6c40/0x4d7750/0x4d8450; '
      f'retail descriptor handler is 0x403430 and the reciprocal carrier reference is valid.')
print(f'  Native PathNavigator vtable {get(nav):#010x}: SetDestination slot '
      f'{get(get(nav)+4):#010x}; circle goal cell=({circle_x},{circle_z}), '
      f'radius={circle_radius}; native SetRoute installs={len(route_installs)}, '
      f'0x4e50a0 waypoint pops={len(nav_pops)}.')
print(f'  Dispatcher return (nav flags, native player queue count, '
      f'0x4e4f50 non-null admission samples, handler/dispatch sites, order events): '
      f'{dispatch_return}; after worker SetRoute count='
      f'{get(0x634674+4*player_index)}, nav flags={get(nav+0x114):#x}.')
arg_counts=Counter('admit' if arg else 'release' for _,arg,*_ in native_request_events)
print(f'  Native 0x4e4f50 call samples (tick, argument, flags, player count, '
      f'return address): first={native_request_events[:5]}, '
      f'last={native_request_events[-5:]}; kinds={dict(arg_counts)}.')
def order_snapshot(unit):
    out=[]
    for pointer in (get(unit+0x60),get(unit+0x64)):
        if not pointer: out.append(None); continue
        out.append({'ptr':hex(pointer),'code':uc.mem_read(pointer+4,1)[0],
            'vtable':hex(get(pointer)),'target':hex(get(pointer+0x16)),
            'controller':hex(get(pointer+0x6E)),'flags':hex(get(pointer+0x5A)),
            'events':hex(get(pointer+0x6A))})
    return out
print(f'  Circle/controller trace (last 20): {arrival_trace[-20:]}; '
      f'feedback events (last 20): {feedback_events[-20:]}')
print(f'  Native circle events set at 0x4e5175={event_set_samples[:5]}...'
      f'{event_set_samples[-5:]}; next-dispatch consumption={event_consumptions[:10]}...'
      f'{event_consumptions[-10:]}.')
circle_feedback=[e for e in feedback_events if e[1]==0x100]
true_predicates=[e for e in predicate_samples if e[1]]
near_predicates=[e for e in predicate_samples if e[4]<=600]
print(f'  Circle predicate stats: {dict(arrival_counts)}; '
      f'true samples={true_predicates[:5]}...{true_predicates[-5:]}; '
      f'near-circle samples={near_predicates[:5]}...{near_predicates[-5:]}; '
      f'0x100 feedback calls={circle_feedback[:8]}...{circle_feedback[-8:]}; '
      f'detach callsites={detach_samples[:10]}...{detach_samples[-10:]}.')
print(f'  Order heads: Araarch={order_snapshot(passenger)}, '
      f'Vertrans={order_snapshot(carrier)}.')
foot_x0=((position[0]>>19)-fx)//2; foot_z0=((position[2]>>19)-fz)//2
height_plane=state['map_data'][3]
nearby=[]
for dz in range(2):
    for dx in range(2):
        cx,cz=foot_x0+dx,foot_z0+dz
        if 0<=cx<width and 0<=cz<height:
            nearby.append((cx,cz,grades[cz*width+cx],height_plane[cz*width+cx]))
print(f'  Terminal Araarch GROUND2 footprint origin=({foot_x0},{foot_z0}); '
      f'2x2 cells (x,z,grade,height)={nearby}; sea level={state["sea"]}.')
print(f'  Terminal native queues/ref/controller: Araarch +60/+64='
      f'{get(passenger+0x60):#010x}/{get(passenger+0x64):#010x}, '
      f'Vertrans +60/+64={get(carrier+0x60):#010x}/{get(carrier+0x64):#010x}, '
      f'nav+4={get(nav+4):#010x}, Araarch order controller='
      f'{get(passenger_order+0x6e):#010x}, events={get(passenger_order+0x6a):#x}.')
print(f'  Waypoint-pop samples (tick, count, route head/tail, mover x/z, '
      f'admission timestamp, flags): first={nav_pops[:3]}, last={nav_pops[-5:]}')
print(f'  Full-map physical trace: {args.max_ticks if terminal_reason=="tick-limit" else i} '
      f'ticks from {start_position[0]/65536:.1f},{start_position[2]/65536:.1f} to '
      f'{world_x:.1f},{world_z:.1f}; last moved at tick {last_move_tick}; '
      f'last step={last_step_distance:.2f}px; max displacement={max_displacement:.1f}px '
      f'at tick {max_displacement_tick}; total planar travel={total_distance:.1f}px; '
      f'circle distance={circle_distance:.1f}px; event first set at tick '
      f'{circle_event_tick}; termination={terminal_reason}.')
print(f'  Terminal navigator: count={route_count}, admission timestamp={admission_tick}, '
      f'remaining route points={route_points}; order head={order_head:#010x}, '
      f'order events={get(passenger_order+0x6A):#x}, nav flags={get(nav+0x114):#x}, '
      f'controller={get(nav+4):#010x}, '
      f'carrier cargo={get(carrier+0xAC):#010x}, passenger parent={get(passenger+0xA8):#010x}.')
if world_tick is not None:
    print(f'  World control: the same shipped-map load-order roundtrip boards at tick '
          f'{world_tick}; that command reports no passenger position trace, so it is '
          'a terminal-state control, not a route comparison.')
print('  Controlled seams: worker callbacks (0x4e1ee0, 0x4e2060), '
      'route allocation/weight, visibility and special-body gates, '
      'movement-grade callback (0x4db640 backed by native 0x508cd0 grades), '
      'plus inherited mover-side services. Native 0x4e4f50 admission, its '
      'PathNavigator lookup slot, code-30 registry/handler, dispatcher, '
      'SetDestination, route worker, waypoint pop, mover, and position commit '
      'execute from KINGDOMS.icd. Native 0x4e2470 mission-event updates also '
      'execute from KINGDOMS.icd. No retail GUI is launched.')
