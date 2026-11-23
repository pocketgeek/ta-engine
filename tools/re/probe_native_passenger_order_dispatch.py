#!/usr/bin/env python3
"""Execute one real retail Move_Seek_Pickup order through native dispatch.

The probe creates code-1 GROUND_PICKUP and code-2 Move_Seek_Pickup orders with
the executable's 0x4d6c40 constructor, inserts both with 0x4d7750, then calls
the real 0x4d8450 dispatcher for the passenger. Retail's 0x403430 handler,
0x4d4da0 approach setup, 0x4e2500 circle controller constructor, and
0x4d4d40 mission binding execute from KINGDOMS.icd.

The fixture controls mission memory allocation, the malloc/free boundary,
mission-name lookup, passenger eligibility, deterministic RNG, and the
navigator's SetController vtable slot. It checks controller construction and
ownership, not route search, terrain, or passenger movement. No retail GUI is
launched.

Run from the repository root:
    PYTHONPATH=tools/re python3 tools/re/probe_native_passenger_order_dispatch.py
"""
import struct

from emu import HEAP, Icd
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_ESP


p = Icd()
uc = p.uc
BASE = HEAP + 0x10000
passenger, carrier = BASE, BASE + 0x1000
owner = BASE + 0x2000
passenger_kind, carrier_kind = BASE + 0x3000, BASE + 0x4000
passenger_order, carrier_order = BASE + 0x5000, BASE + 0x5100
nav, mover = BASE + 0x6000, BASE + 0x6100
game, definitions = BASE + 0x7000, BASE + 0x8000
navigator_vtable = BASE + 0x9000
allocator_cursor = [BASE + 0x10000]

allocations = []
name_lookups = []
eligibility_checks = []
approach_calls = []
controller_bindings = []
native_entries = []


def put(address, *values):
    uc.mem_write(address, struct.pack(
        '<' + 'I' * len(values), *(value & 0xffffffff for value in values)))


def get(address):
    return struct.unpack('<I', uc.mem_read(address, 4))[0]


def byte(address, value):
    uc.mem_write(address, bytes((value & 0xff,)))


def word(address, value):
    uc.mem_write(address, struct.pack('<H', value & 0xffff))


def alloc(_uc, stack_args):
    size = get(stack_args)
    address = (allocator_cursor[0] + 15) & ~15
    allocator_cursor[0] = address + max(size, 16)
    uc.mem_write(address, bytes(max(size, 16)))
    allocations.append((size, address))
    return 0, address  # retail 0x4eb9e0 is cdecl; caller removes its argument


def free(_uc, _stack_args):
    return 0, 0  # cdecl free sink


def mission_name(_uc, stack_args):
    destination = uc.reg_read(UC_X86_REG_ECX)
    key = get(stack_args)
    kind = {0x604c00: 1, 0x604de4: 2}.get(key)
    assert kind is not None, hex(key)
    byte(destination, kind)
    name_lookups.append((key, kind))
    return 1, destination


def eligible_passenger(_uc, stack_args):
    unit = get(stack_args)
    eligibility_checks.append(unit)
    return 1, int(unit == passenger)


def install_controller(_uc, stack_args):
    controller = get(stack_args)
    unit = uc.reg_read(UC_X86_REG_ECX)
    assert unit == nav, (hex(unit), hex(nav))
    put(nav + 4, controller)
    controller_bindings.append((unit, controller))
    return 1, 0


p.hooks.update({
    0x4eb9e0: alloc,
    0x4eba00: free,
    0x4d4bf0: mission_name,
    0x519f50: eligible_passenger,
    0x535cc0: lambda _uc, _sp: (1, 0),  # deterministic Random(n) -> 0
    0x401000: install_controller,       # navigator SetController boundary
})
p.freeze_hooks()


def observe_entry(address, label):
    def observe(_uc, current, _size, _data):
        native_entries.append(label)
        if current == 0x4d4da0:
            sp = uc.reg_read(UC_X86_REG_ESP)
            approach_calls.append((
                uc.reg_read(UC_X86_REG_ECX), get(sp + 4), get(sp + 8)))
    return uc.hook_add(UC_HOOK_CODE, observe, begin=address, end=address)


# The callbacks below only observe function entry. The executable handles the
# queue, descriptor dispatch and nested passenger/controller logic itself.
entry_hooks = [
    observe_entry(0x4d8450, 'dispatcher'),
    observe_entry(0x403430, 'Move_Seek_Pickup'),
    observe_entry(0x4d4da0, 'ground-approach'),
    observe_entry(0x4e2500, 'circle-controller-constructor'),
    observe_entry(0x4d4d40, 'mission-controller-binding'),
]

put(0x62d55c, game)
put(0x62db84, definitions)
put(game + 0x19f44, 100)

# Descriptor rows have 25-byte stride. Code 1 names GROUND_PICKUP; code 2 is
# Move_Seek_Pickup. Both use the observed 0x200 reference-tracking flag.
put(definitions + 25 + 4, 0x408860)
put(definitions + 25 + 0x11, 0x200)
put(definitions + 50 + 4, 0x403430)
put(definitions + 50 + 0x11, 0x200)

put(owner, 1)
byte(owner + 0xea, 1)
for unit, kind, position in (
        (passenger, passenger_kind, (100, 10, 100)),
        (carrier, carrier_kind, (105, 10, 100))):
    put(unit + 0xb4, kind)
    put(unit + 0xb8, owner)
    put(unit + 0x130, 0x01000000)  # live
    put(unit + 0x68, *(coordinate << 16 for coordinate in position))
    put(unit + 0x78, 1 | (1 << 16))  # one-cell footprint
    put(unit + 8, mover)
    byte(kind + 0x24b, 0)  # don't manufacture an idle mission
    put(kind + 0x260, 0)  # ground mover class
    word(kind + 0x23e, 150)  # carrier pickup circle

put(mover, nav)
put(nav, navigator_vtable)
put(nav + 4, 0)
put(nav + 8, passenger)
put(navigator_vtable + 4, 0x401000)


def construct_and_insert(unit, kind, target, order):
    # The real order allocator normally surrounds this constructor. Supply its
    # object memory explicitly while executing the complete retail constructor.
    result, error = p.call(
        0x4d6c40, (kind, target, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), ecx=order)
    assert error is None, error
    assert result == order
    assert get(order) == 0x5f2814
    _, error = p.call(0x4d7750, (unit, order))
    assert error is None, error
    assert get(unit + 0x60) == order
    assert get(order + 0x0e) == unit
    assert get(order + 0x66) == 0


# Native reciprocal target references are installed by the mission reference
# constructor and the actual queue insertion. The carrier has a recognized
# code-1 pickup order targeting this passenger.
construct_and_insert(passenger, 2, carrier, passenger_order)
assert get(passenger_order + 0x5a) == 0x200
assert get(passenger_order + 0x16) == carrier
assert get(carrier + 0xc4) == passenger_order + 0x12

construct_and_insert(carrier, 1, passenger, carrier_order)
assert get(carrier_order + 0x16) == passenger
assert get(passenger + 0xc4) == carrier_order + 0x12

# One retail dispatcher call reaches 0x403430 and its real ground
# approach/controller constructors. Only environmental/UI boundaries above
# are substituted.
_, error = p.call(0x4d8450, (passenger,))
assert error is None, error

controller = get(passenger_order + 0x6e)
assert controller != 0
assert get(controller) == 0x5f28d8
assert get(controller + 4) == passenger_order
assert struct.unpack('<hh', uc.mem_read(controller + 8, 4)) == (6, 6)
assert get(controller + 0x0c) == 134
assert get(controller + 0x10) == 70
assert allocations == [(20, controller)]
assert get(nav + 4) == controller
assert controller_bindings == [(nav, controller)]
assert approach_calls == [(passenger_order, passenger_order + 0x22, 134)]
assert bytes(uc.mem_read(passenger_order + 0x22, 12)) == bytes(
    uc.mem_read(carrier + 0x68, 12))
assert native_entries == [
    'dispatcher', 'Move_Seek_Pickup', 'ground-approach',
    'circle-controller-constructor', 'mission-controller-binding']
assert eligibility_checks == [passenger]
assert name_lookups == [(0x604c00, 1)]
assert get(passenger + 0xd0) == 0
assert get(passenger_order + 6) == 0x789
assert get(passenger_order + 0x0a) == 130

print('PASS: native code-2 passenger order dispatches once and constructs its passenger-owned retail circle controller')
print({
    'passenger_order': hex(passenger_order),
    'passenger_order_vtable': hex(get(passenger_order)),
    'flags': hex(get(passenger_order + 0x5a)),
    'target_carrier': hex(get(passenger_order + 0x16)),
    'carrier_reference_head': hex(get(carrier + 0xc4)),
    'controller': hex(controller),
    'controller_vtable': hex(get(controller)),
    'controller_mission': hex(get(controller + 4)),
    'circle_origin': struct.unpack('<hh', uc.mem_read(controller + 8, 4)),
    'circle_radius': get(controller + 0x0c),
    'controller_bound_to_passenger_nav': hex(get(nav + 4)),
    'post_dispatch_event_mask': hex(get(passenger_order + 6)),
    'post_dispatch_deadline': get(passenger_order + 0x0a),
    'controlled_boundaries': [
        'mission object memory', 'malloc/free', 'mission-name lookup',
        'passenger eligibility', 'deterministic RNG', 'navigator SetController'],
})
