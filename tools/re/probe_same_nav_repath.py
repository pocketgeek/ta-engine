#!/usr/bin/env python3
"""Exercise two native route deliveries on one moving navigator.

This is a disposable, synthetic open-grid lifecycle probe. It keeps one unit,
mover, navigator, and circle controller alive; submits one goal, moves the unit,
changes that controller's goal, submits again through 0x4e54e0, and lets the
retail singleton scheduler deliver the replacement through 0x4e4ea0.
"""
import struct

from emuphase import Phase, OBJ, GS, TYPE
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_ESI, UC_X86_REG_ESP


def main():
    width = height = 64
    phase = Phase(width, height)
    unit = phase.unit(8, 8)
    assert phase.construct() is None
    phase.plant_request(unit, (8, 8), (40, 8))
    mover = struct.unpack('<I', phase.uc.mem_read(unit + 8, 4))[0]
    nav = phase.NAV
    controller = phase.HANDLE
    mission = controller + 0x1000

    def put(address, value):
        phase.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))

    def read(address):
        return struct.unpack('<I', phase.uc.mem_read(address, 4))[0]

    put(mover + 0x30, 0)
    phase.uc.mem_write(unit + 0x68, struct.pack('<iii', 8 * 16 * 65536, 20 * 65536,
                                                8 * 16 * 65536))
    phase.uc.mem_write(unit + 0x74, struct.pack('<hh', 8, 8))
    phase.uc.mem_write(unit + 0x78, struct.pack('<hh', 1, 1))
    phase.uc.mem_write(unit + 0x7e, struct.pack('<H', 0))
    phase.uc.mem_write(mover + 0x20, struct.pack('<i', 0))
    phase.uc.mem_write(mover + 0x36, struct.pack('<H', 1))
    phase.uc.mem_write(TYPE + 0x126, struct.pack('<hh', 1, 1))
    clearance = phase._alloc(0x4000)
    put(TYPE + 0x12a, clearance)
    phase.uc.mem_write(TYPE + 0x162, struct.pack('<i', 2 * 65536))
    phase.uc.mem_write(TYPE + 0x166, struct.pack('<i', 1 * 65536))
    phase.uc.mem_write(TYPE + 0x16a, struct.pack('<i', 1 * 65536))
    phase.uc.mem_write(TYPE + 0x182, struct.pack('<i', 2 * 65536))
    phase.uc.mem_write(TYPE + 0x186, struct.pack('<i', 4 * 65536))
    phase.uc.mem_write(unit + 0x12b, struct.pack('<i', 2 * 65536))
    phase.uc.mem_write(TYPE + 0x18e, struct.pack('<H', 500))
    phase.uc.mem_write(TYPE + 0x18a, struct.pack('<I', phase.GRID))
    phase.uc.mem_write(TYPE + 0x172, struct.pack('<i', 65536))
    phase.uc.mem_write(TYPE + 0x16e, struct.pack('<i', 65536))
    phase.uc.mem_write(TYPE + 0x249, b'\x04')
    phase.uc.mem_write(phase.GRID + 4, struct.pack('<hh', 1, 1))
    phase.uc.mem_write(phase.GRID + 8, struct.pack('<4h4B', 100, -10000, 100, -10000,
                                                   32, 32, 32, 32))
    cells = bytearray(width * height * 14)
    for index in range(width * height):
        cells[index * 14 + 4:index * 14 + 7] = bytes((20, 20, 20))
        cells[index * 14 + 8:index * 14 + 10] = b'\xff\xff'
    phase.uc.mem_write(phase.cells_addr, bytes(cells))
    phase.uc.mem_write(GS + 0x19ef8, b'\x14')
    sector_stride = (width + 7) // 8
    sector_rows = (height + 7) // 8
    sector_records = bytearray(sector_stride * sector_rows * 10)
    for index in range(sector_stride * sector_rows):
        sector_records[index * 10 + 1] = 20
    sector_grid = phase._alloc(len(sector_records))
    phase.uc.mem_write(sector_grid, bytes(sector_records))
    put(GS + 0x19f18, sector_grid)
    put(GS + 0x19f1c, sector_stride)
    phase.icd.hooks[0x51ad20] = lambda _uc, _args: (1, 0)
    phase.icd.hooks[0x56c640] = lambda _uc, _args: (8, 0)

    # Configure one actual scheduler-visible entity slot and the singleton.
    config = GS + 0x600000
    put(0x62d558, config)
    put(config, GS + 0x700000)
    put(config + 8, config + 0x100)
    put(config + 0x10c, 4)
    phase.uc.mem_write(GS + 0x3068, b'\x01\x00')
    owner = GS + 0x2404
    put(owner, 1)
    phase.uc.mem_write(owner + 0xea, b'\x01\x00')
    pool_first = unit - 0x138
    put(owner + 0x74, pool_first)
    put(owner + 0x78, pool_first + 3 * 0x138)
    put(OBJ + 0x115, pool_first)
    put(OBJ + 0x225, 12000)
    put(GS + 0x19e70, OBJ)
    put(unit + 0x60, mission)
    put(mission + 0x0e, unit)
    put(mission + 0x6e, controller)

    # The grid is synthetic and open, but query and path phases are retail code.
    grades = [6] * (width * height)
    def grade(_uc, args):
        x, z, _direction = struct.unpack('<iii', phase.uc.mem_read(args, 12))
        return 3, grades[z * width + x] if 0 <= x < width and 0 <= z < height else 0

    pending = [False]
    current_tick = [0]
    requests = []
    deliveries = []
    delivery_ticks = []
    delivered_inputs = []
    def lookup(_uc, _args):
        return 0, nav if pending[0] else 0
    def enqueue(_uc, args):
        requests.append((current_tick[0], hex(read(args - 4)),
                         struct.unpack('<I', phase.uc.mem_read(args, 4))[0]))
        pending[0] = True
        return 1, 0
    def finish(_uc, _args):
        pending[0] = False
        return 1, 0
    def delivered(uc, address, _size, _data):
        if uc.reg_read(UC_X86_REG_ECX) == nav:
            sp = uc.reg_read(UC_X86_REG_ESP)
            route_pointer, count = struct.unpack('<II', uc.mem_read(sp + 4, 8))
            words = struct.unpack('<' + 'h' * (count * 2),
                                  uc.mem_read(route_pointer, count * 4)) if count else ()
            delivered_inputs.append(list(zip(words[::2], words[1::2])))
            delivery_ticks.append(current_tick[0])
    def copied_to_nav(uc, address, _size, _data):
        if uc.reg_read(UC_X86_REG_ESI) == nav:
            count = read(nav + 0x10c)
            words = struct.unpack('<' + 'h' * (count * 2),
                                  uc.mem_read(nav + 12, count * 4)) if count else ()
            deliveries.append(list(zip(words[::2], words[1::2])))

    vt = read(nav)
    lookup_address = read(vt + 0x18)
    phase.icd.hooks[lookup_address] = lookup
    phase.icd.hooks[0x4e4f50] = enqueue
    phase.icd.hooks[0x4e1ee0] = lambda _uc, _args: (2, 0)
    phase.icd.hooks[0x4e2470] = lambda _uc, _args: (1, 0)
    phase.icd.hooks[0x4e2060] = finish
    phase.icd.hooks[0x4139d0] = grade
    phase.uc.hook_add(UC_HOOK_CODE, delivered, begin=0x4e4ea0, end=0x4e4ea0)
    phase.uc.hook_add(UC_HOOK_CODE, copied_to_nav, begin=0x4e4f05, end=0x4e4f05)

    def set_goal(goal_x, goal_z):
        # Preserve controller identity: rebuild its native circle for the new
        # target, then ask the same navigator to replace/revalidate its route.
        _, error = phase.icd.call(0x4e2500,
            (mission, goal_x * 16 * 65536, goal_z * 16 * 65536, 24),
            ecx=controller)
        assert error is None, ('0x4e2500', error)
        _, error = phase.icd.call(0x4e54e0, (controller,), ecx=nav)
        assert error is None, ('0x4e54e0', error)

    def tick(game_tick):
        current_tick[0] = game_tick
        put(GS + 0x19f44, game_tick)
        put(0x634674, int(pending[0]))
        _, error = phase.icd.call(0x4dc800, (unit,), ecx=mover)
        assert error is None, ('0x4dc800', game_tick, error)
        _, error = phase.icd.call(0x51b2a0, (unit,), ecx=mover)
        assert error is None, ('0x51b2a0', game_tick, error)
        _, error = phase.icd.call(0x416430, (1,), ecx=OBJ)
        assert error is None, ('0x416430', game_tick, error)

    current_tick[0] = 0
    set_goal(40, 8)
    assert pending[0] and requests
    for game_tick in range(1, 25):
        tick(game_tick)
    assert len(deliveries) == 1, ('initial route delivery', deliveries)
    first_route = deliveries[0]
    first_count = read(nav + 0x10c)
    assert first_route and first_count == len(first_route), (first_route, first_count)
    start_after_first = struct.unpack('<iii', phase.uc.mem_read(unit + 0x68, 12))
    assert (start_after_first[0], start_after_first[2]) != (8 * 16 * 65536,
                                                              8 * 16 * 65536), \
        ('carrier did not move before replacement', start_after_first)

    # This is the live replan: update the same controller's destination, submit
    # through native setDestination, and keep advancing its same mover/nav.
    request_count_before_replan = len(requests)
    current_tick[0] = 24
    set_goal(40, 40)
    assert pending[0] and len(requests) > request_count_before_replan, (pending, requests)
    old_nav_pointer = read(mover)
    old_controller_pointer = read(nav + 4)
    for game_tick in range(25, 100):
        tick(game_tick)
        if len(deliveries) == 2 and not pending[0]:
            break
    assert len(deliveries) == 2, ('replacement route was not delivered', deliveries,
                                  read(OBJ + 0x5c), pending[0])
    second_route = deliveries[1]
    assert second_route != first_route, (first_route, second_route)
    assert read(mover) == old_nav_pointer == nav, hex(read(mover))
    assert read(nav + 4) == old_controller_pointer == controller, hex(read(nav + 4))
    assert read(nav + 0x10c) == len(second_route), (read(nav + 0x10c), second_route)
    goal_center = (40 * 16, 40 * 16)
    end = second_route[-1]
    assert (end[0] - goal_center[0]) ** 2 + (end[1] - goal_center[1]) ** 2 <= 24 ** 2, second_route
    assert delivered_inputs[-1] == second_route, (delivered_inputs, second_route)

    replacement_position = struct.unpack('<iii', phase.uc.mem_read(unit + 0x68, 12))
    for game_tick in range(game_tick + 1, game_tick + 9):
        tick(game_tick)
    position_after = struct.unpack('<iii', phase.uc.mem_read(unit + 0x68, 12))
    assert (position_after[0], position_after[2]) != (replacement_position[0], replacement_position[2]), \
        ('carrier stopped after replacement delivery', replacement_position, position_after)
    print('PASS: one unit/mover/nav/controller completed two native route requests')
    print(f'  requests={len(requests)}, deliveries={len(deliveries)}')
    print(f'  request events (tick, caller, player): {requests}')
    print(f'  delivery ticks: {delivery_ticks}; post-replacement ticks={game_tick}')
    print(f'  0x4e4ea0 input routes: {delivered_inputs}')
    print(f'  first route: {first_route}')
    print(f'  replacement route: {second_route}')
    print(f'  same nav/controller retained; replacement-position={(replacement_position[0] // 65536, replacement_position[2] // 65536)}; final position={(position_after[0] // 65536, position_after[2] // 65536)}')


if __name__ == '__main__':
    main()
