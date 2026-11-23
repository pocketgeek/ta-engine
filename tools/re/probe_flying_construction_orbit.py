#!/usr/bin/env python3
"""Execute native flying-build retarget stages and report their orbit goal.

Runs the installed KINGDOMS.icd 41ef00 handler. The shared route radius setup
(4e4540) and navigator goal sink (4e40e0) are the only replaced native calls;
the RNG, handler, trig, dispatcher inputs, and goal-coordinate arithmetic run
from the binary. Stage 5 is entered with the timer event. If it requests stage
4, the handler is called again as 4d8450 would after a return-code-4 result.
"""
import argparse
import json
import math
import struct

from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBP,
                                UC_X86_REG_ECX, UC_X86_REG_EDI,
                                UC_X86_REG_ESP)


def run_case(owner_flags: int, seed: int) -> dict:
    p = Icd()
    unit, mission, site, game, kind, mover, owner, controller = (
        HEAP + n * 0x10000 for n in range(8))

    def put(address, *values):
        p.uc.mem_write(address, struct.pack('<' + 'I' * len(values),
                                           *(v & 0xffffffff for v in values)))

    def get(address):
        return struct.unpack('<I', p.uc.mem_read(address, 4))[0]

    def byte(address, value):
        p.uc.mem_write(address, bytes((value & 255,)))

    put(0x62d55c, game)
    put(0x62db84, game + 0x30000)
    put(0x65e108, 0)              # no external resource-refresh callback
    # Initialize only the native allocator's single-process bookkeeping. The
    # option/config parser is unrelated to orbit math; selecting its cached
    # false result takes the regular (HeapAlloc-backed) branch.
    byte(0x65e01c, 1)
    byte(0x65e02c, 0)
    byte(0x65e020, 1)
    byte(0x65e034, 0)
    put(0x629680, 0)              # force 5d8075 to its HeapAlloc fallback
    put(0x66e3d8, 1)              # synthetic non-null heap handle
    put(game + 0x19f30, 1000)       # unit's native position-update stamp
    put(game + 0x19f44, 1000)       # mission dispatcher tick
    put(game + 0x19e90, 4096, 4096)
    put(game + 0x175c4 + 0x126, kind)  # player 0 builder-type table entry
    put(0x64186c, seed)

    put(owner, 1)
    byte(owner + 0xea, 1)
    put(unit + 0x08, mover)
    put(unit + 0x68, 1000 << 16, 250 << 16, 1000 << 16)
    # Keep the native per-unit world-position refresh out of this focused
    # mission-stage probe. Its allocator/controller setup is unrelated to the
    # stage-5 orbit retarget; event kind 6 selects the stage-5 branch directly.
    put(unit + 0xa4, 999)
    byte(unit + 0x12a, 6)
    put(unit + 0xb4, kind)
    put(unit + 0xb8, owner)
    put(unit + 0x130, 0x01000000 | owner_flags)
    put(unit + 0xd0, 0)

    put(kind + 0x230, 100)          # FBI builddistance
    put(kind + 0x260, 0)
    byte(kind + 0x24b, 0)          # no idle order factory

    put(site + 0x68, 1120 << 16, 0, 1060 << 16)
    put(site + 0xb4, kind)
    put(site + 0x130, 0x01000000)
    put(site + 0x108, 0x3f000000)  # incomplete site
    put(mission + 0x0e, unit)
    put(mission + 0x16, site)
    put(mission + 0x22, 0, 0, 0)
    put(mission + 0x4e, 0)
    put(mission + 0x5a, 0)
    put(mission + 0x6a, 0)
    byte(mission + 4, 1)
    byte(mission + 5, 5)
    put(mission + 6, 1)            # timer event mask
    put(mission + 10, 0xffffffff)
    put(unit + 0x60, mission)

    rng = []
    goal_calls = []
    route_calls = []
    platform_calls = []
    heap_allocations = []
    orbit_math = []
    recent_pc = []
    stub_cs = HEAP + 0x3f0000
    stub_heap_alloc = HEAP + 0x3f0010
    heap_cursor = [HEAP + 0x80000]

    # The native 4eb9e0 -> 5ba690 allocation wrapper reaches these OS
    # services. Their one-argument stdcall ABI is kept; the probe is single
    # threaded, so the critical-section calls are no-ops. 5d3d12 only records
    # a process-exit callback and has no bearing on the constructed goal.
    put(0x5eb118, stub_cs)        # InitializeCriticalSection
    put(0x5eb108, stub_cs)        # EnterCriticalSection
    put(0x5eb110, stub_cs)        # LeaveCriticalSection
    put(0x5eb268, stub_heap_alloc)  # HeapAlloc

    def critical_section(uc, sp):
        platform_calls.append({'service': 'critical-section',
                               'object': hex(get(sp))})
        return 1, 0

    def heap_alloc(uc, sp):
        heap, flags, requested = get(sp), get(sp + 4), get(sp + 8)
        ptr = heap_cursor[0]
        size = (requested + 15) & ~15
        heap_cursor[0] += max(size, 16)
        uc.mem_write(ptr, bytes(size))
        heap_allocations.append({'heap': hex(heap), 'flags': flags,
                                 'requested': requested,
                                 'returned': hex(ptr)})
        return 3, ptr

    def register_exit_callback(_uc, _sp):
        platform_calls.append({'service': 'atexit-registration',
                               'result': 'ignored'})
        return 0, 0
    ret_sites = {
        0x41f662: 50,
        0x41f599: 0x2492,
        0x41f5a5: 0x2492,
        0x41f5b1: 8,
        0x41f5bf: 8,
    }

    # Observe native rand results at the instruction after each actual call.
    def observe_random(uc, address, size, data):
        bound = ret_sites[address]
        esp = uc.reg_read(UC_X86_REG_ESP)
        rng.append({'bound': bound, 'value': uc.reg_read(UC_X86_REG_EAX),
                    'seed_after': get(0x64186c)})

    for address in ret_sites:
        p.uc.hook_add(UC_HOOK_CODE, observe_random,
                      begin=address, end=address)

    def remember_pc(_uc, address, _size, _data):
        recent_pc.append(address)
        if len(recent_pc) > 32:
            del recent_pc[0]

    p.uc.hook_add(UC_HOOK_CODE, remember_pc,
                  begin=0x401000, end=0x5e9000)

    # 41ef00's shared route-update/radius setup. It mutates controller state in
    # retail; this is the same sink used by the existing transport probes.
    def route_radius(uc, sp):
        ecx = uc.reg_read(UC_X86_REG_ECX)
        route_calls.append({'owner': ecx, 'arg': get(sp)})
        return 1, 0

    # Capture installed native navigator point and preserve the controller
    # return expected by the following setter/binder calls.
    def install_goal(uc, sp):
        mission_arg, point = struct.unpack('<2I', uc.mem_read(sp, 8))
        return_address = get(uc.reg_read(UC_X86_REG_ESP))
        goal_object = uc.reg_read(UC_X86_REG_ECX)
        point_words = struct.unpack('<3i', uc.mem_read(point, 12))
        goal_calls.append({'return': hex(return_address),
                           'mission': mission_arg,
                           'object': hex(goal_object),
                           'point': list(point_words)})
        # Minimal result of the native 4e40e0 constructor. The constructor's
        # route/path setup is the sink being observed; preserve the fields that
        # the following native setters and mission binder consume.
        put(goal_object, 0x5f2974)
        p.uc.mem_write(goal_object + 8, struct.pack('<HHHH', 0x20, 0, 0,
                                                   0xffff))
        put(goal_object + 0x12, get(mission_arg + 0x0e))
        p.uc.mem_write(goal_object + 0x26, struct.pack('<3i', *point_words))
        return 2, goal_object

    def observe_orbit(uc, address, _size, _data):
        if address == 0x41f599:
            orbit_math.append({'stage': 'base-angle-after-direction',
                               'base_angle': uc.reg_read(UC_X86_REG_EDI),
                               'first_draw': uc.reg_read(UC_X86_REG_EAX)})
        elif address == 0x41f5a7:
            orbit_math.append({'stage': 'angle-after-two-draws',
                               'angle': uc.reg_read(UC_X86_REG_EDI)})
        elif address == 0x41f5b3:
            orbit_math.append({'stage': 'first-radius-draw',
                               'draw': uc.reg_read(UC_X86_REG_EDI)})
        elif address == 0x41f5bf:
            orbit_math.append({'stage': 'second-radius-draw',
                               'draw': uc.reg_read(UC_X86_REG_EAX)})
        if address == 0x41f5d8:
            ebp = uc.reg_read(UC_X86_REG_EBP)
            angle = struct.unpack('<i', uc.mem_read(ebp + 8, 4))[0]
            orbit_math.append({'stage': 'before-native-trig',
                               'angle_input': angle,
                               'radius_fixed': uc.reg_read(UC_X86_REG_EDI)})

    p.hooks.update({
        stub_cs: critical_section,
        stub_heap_alloc: heap_alloc,
        0x5d3d12: register_exit_callback,
        0x4e4540: route_radius,
        0x4e40e0: install_goal,
    })
    p.freeze_hooks()
    p.uc.hook_add(UC_HOOK_CODE, observe_orbit,
                  begin=0x41f599, end=0x41f5d8)

    # Call stage 5 with the timer event. Native result 4 means the dispatcher
    # re-enters this same handler immediately with stage 4; result 1 means it
    # advances to stage 6 and does not install an orbit point this tick.
    first_result, error = p.call(0x41ef00, (unit, mission, 1))
    if error:
        raise RuntimeError((error, [hex(pc) for pc in recent_pc]))
    stages = [{'input': 5, 'result': first_result,
               'stage_after': p.uc.mem_read(mission + 5, 1)[0]}]
    if first_result == 4:
        next_result, error = p.call(0x41ef00, (unit, mission, 0))
        if error:
            raise RuntimeError((error, [hex(pc) for pc in recent_pc]))
        stages.append({'input': 4, 'result': next_result,
                       'stage_after': p.uc.mem_read(mission + 5, 1)[0]})

    installed_goal = None
    if goal_calls:
        goal_object = int(goal_calls[0]['object'], 16)
        flags, min_radius, max_radius, heading = struct.unpack(
            '<4H', p.uc.mem_read(goal_object + 8, 8))
        point = list(struct.unpack('<3i', p.uc.mem_read(goal_object + 0x26, 12)))
        site_point = list(struct.unpack('<3i', p.uc.mem_read(site + 0x68, 12)))
        delta_x = (point[0] - site_point[0]) / 65536.0
        delta_z = (point[2] - site_point[2]) / 65536.0
        installed_goal = {
            'object': hex(goal_object), 'point': point,
            'heading': heading, 'flags': hex(flags),
            'min_radius': min_radius, 'max_radius': max_radius,
            'distance_from_site': math.hypot(delta_x, delta_z),
        }

    return {'owner_flags': hex(owner_flags), 'seed': seed, 'stages': stages,
            'rng': rng, 'route_radius_calls': route_calls,
            'navigator_goal_calls': goal_calls,
            'orbit_math': orbit_math,
            'installed_goal': installed_goal,
            'allocator': heap_allocations,
            'platform_calls': platform_calls,
            'mission_wait_mask': hex(get(mission + 6)),
            'mission_pending': hex(get(mission + 0x6a)),
            'mission_flags': hex(get(mission + 0x5a))}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--seed', type=lambda x: int(x, 0), default=1)
    args = ap.parse_args()
    rows = [run_case(0, args.seed), run_case(0x0c, args.seed)]
    print(json.dumps(rows, indent=2))


if __name__ == '__main__':
    main()
