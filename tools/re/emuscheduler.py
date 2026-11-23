#!/usr/bin/env python3
"""Execute retail's scheduler over synthetic unit slots and controlled searches.

Only search phases and navigator lookup are stubbed. Budget division, slot
iteration, admission, phase dispatch, suspension and accounting execute retail
instructions. This checks scheduling, not path geometry or full-game parity.
"""
import struct

from emu import Icd, HEAP
from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ECX, UC_X86_REG_EIP


OBJ, GS, CONFIG, VT, LOOKUP = (HEAP + n for n in (0, 0x1000, 0x30000, 0x31000, 0x31100))
ENTITIES, MOVERS, NAVS = (HEAP + n for n in (0x40000, 0x80000, 0xc0000))
STRIDE = 0x138


class Scheduler:
    def __init__(self, requests, slots=8, budget=12000, priorities=()):
        self.icd = Icd()
        self.uc = self.icd.uc
        self.requests = dict(requests)  # (player, slot) -> cost-search pops needed
        self.slots = slots
        self.events = []
        self.tick = 0
        self.pops = 0
        self.write(0x62d55c, GS)
        self.write(0x62d558, CONFIG)
        self.write(CONFIG + 8, CONFIG + 0x100)
        self.write(CONFIG + 0x10c, slots)
        self.uc.mem_write(GS + 0x3068, b'\x01\x00')
        self.write(OBJ + 0x225, budget)
        self.write(VT + 0x18, LOOKUP)
        self.uc.mem_write(LOOKUP, b'\xc3')
        for player in range(10):
            record = GS + 0x2404 + player * 0x110
            enabled = any(p == player for p, _ in requests)
            self.write(record, int(enabled))
            self.uc.mem_write(record + 0xea, bytes((1, player)))
            self.uc.mem_write(record + 0xe3, bytes((int(player in priorities),)))
            first = self.entity(player, 0)
            last = self.entity(player, slots - 1)
            self.write(record + 0x74, first)
            self.write(record + 0x78, last)
            self.write(OBJ + 0x115 + player * 4, first)
            for slot in range(slots):
                index = player * slots + slot
                entity = self.entity(player, slot)
                mover, nav = MOVERS + index * 16, NAVS + index * 16
                self.write(entity + 0x130, 0x1000000)
                self.write(entity + 8, mover)
                self.write(mover, nav)
                self.write(mover + 4, 1)
                self.write(nav, VT)
                self.write(nav + 4, index)
        self.icd.hooks[LOOKUP] = self.lookup
        self.icd.hooks[0x415170] = self.initialize
        self.icd.hooks[0x415b10] = self.contour
        self.icd.hooks[0x4142c0] = self.pop
        self.icd.hooks[0x414450] = lambda uc, args: (1, 0)
        self.icd.hooks[0x4e2060] = self.complete

    def write(self, address, value):
        self.uc.mem_write(address, struct.pack('<I', value & 0xffffffff))

    def read(self, offset, signed=False):
        return struct.unpack('<i' if signed else '<I', self.uc.mem_read(OBJ + offset, 4))[0]

    def entity(self, player, slot):
        return ENTITIES + (player * self.slots + slot) * STRIDE

    def active(self):
        entity = self.read(0x58)
        return divmod((entity - ENTITIES) // STRIDE, self.slots) if entity else None

    def event(self, name):
        self.events.append((self.tick, name, self.active()))

    def lookup(self, uc, args):
        nav = uc.reg_read(UC_X86_REG_ECX)
        key = divmod((nav - NAVS) // 16, self.slots)
        return 0, nav if key in self.requests else 0

    def initialize(self, uc, args):
        self.event('init')
        self.pops = 0
        return 1, 0

    def contour(self, uc, args):
        self.event('contour')
        self.write(OBJ + 0x14, 1)
        self.write(OBJ + 0x18, 0)
        self.write(OBJ + 0x191, 0)
        self.write(OBJ + 0xec, 1000000)
        self.write(OBJ + 0x48, 30)
        return 0, 0

    def pop(self, uc, args):
        self.pops += 1
        return 0, int(self.pops >= self.requests[self.active()])

    def complete(self, uc, args):
        self.event('done')
        del self.requests[self.active()]
        return 1, 0

    def step(self, divisor=1):
        self.tick += 1
        for p in range(10):
            self.write(0x634674 + p*4, sum(player == p for player, _ in self.requests))
        _, error = self.icd.call(0x416430, (divisor,), ecx=OBJ)
        if error or self.uc.reg_read(UC_X86_REG_EIP) != 0x6ffff000:
            raise RuntimeError(error or 'scheduler did not return within execution limit')
        return {'active': self.active(), 'phase': self.read(0x5c),
                'remaining': self.read(0x165, True),
                'players': [self.read(0x13d + p*4, True) for p in range(10)]}


def main():
    # Start at first slot, increment before testing. Empty slots also cost 7.
    schedule = Scheduler({(0, 0): 1, (0, 2): 1, (1, 1): 1})
    schedule.step()
    done = [key for _, event, key in schedule.events if event == 'done']
    assert done == [(1, 1), (0, 2), (0, 0)], done
    # One unfinished search consumes the GLOBAL budget even when its owner's
    # share is exhausted. It is resumed before admitting any other player.
    schedule = Scheduler({(0, 1): 1, (1, 1): 2000}, budget=12000)
    first = schedule.step()
    assert first['active'] == (1, 1) and first['players'][1] < 0, first
    assert schedule.events == [(1, 'init', (1, 1)), (1, 'contour', (1, 1))]
    schedule.step()
    assert [(tick, key) for tick, event, key in schedule.events if event == 'done'] == [
        (2, (1, 1)), (2, (0, 1))], schedule.events
    # The 500 initialization charge is indivisible: small budgets overshoot,
    # preserve phase1 and defer the contour until the next tick.
    schedule = Scheduler({(0, 1): 1}, budget=100)
    first = schedule.step()
    assert first['phase'] == 1 and first['remaining'] == -407, first
    schedule.step()
    assert schedule.events[-1] == (2, 'done', (0, 1)), schedule.events
    # Snapshot budgets at the loop entry: pending COUNTS don't weight a player.
    for counts, priority, expected in (((1, 7), (), (6000, 6000)),
                                       ((7, 1), (1,), (2000, 10000))):
        requests = {(p, s): 1 for p, count in enumerate(counts) for s in range(count)}
        schedule = Scheduler(requests, priorities=priority)
        budgets = []
        def observe(uc, address, size, data):
            budgets.append(tuple(schedule.read(0x13d + p*4) for p in range(2)))
        schedule.uc.hook_add(UC_HOOK_CODE, observe, begin=0x4165d8, end=0x4165d8)
        schedule.step()
        assert budgets == [expected], budgets
    # Exhaust the heap after every contour. The scheduler retries three times,
    # then queries a fixed 9x9 diagnostic neighborhood and clears the route.
    # It does not change the destination or seed a relocated search.
    schedule = Scheduler({(0, 1): 1})
    def exhausted_contour(uc, args):
        schedule.event('contour')
        schedule.write(OBJ + 0x14, 0)
        schedule.write(OBJ + 0x18, 0)
        schedule.write(OBJ + 0x48, 30)
        return 0, 0
    schedule.icd.hooks[0x415b10] = exhausted_contour
    schedule.write(VT + 0x14, LOOKUP + 16)
    schedule.uc.mem_write(LOOKUP + 16, b'\xc3')
    schedule.icd.hooks[LOOKUP + 16] = lambda uc, args: (2, 0)
    schedule.write(OBJ + 0x68, NAVS)
    queries, routes = [], []
    def grade(uc, args):
        queries.append(struct.unpack('<iii', uc.mem_read(args, 12)))
        return 3, 6
    def clear_route(uc, args):
        routes.append(struct.unpack('<II', uc.mem_read(args, 8)))
        return 2, 0
    schedule.icd.hooks[0x4139d0] = grade
    schedule.icd.hooks[0x4e4ea0] = clear_route
    schedule.step()
    assert schedule.read(0x1ad) == 3 and not schedule.requests
    assert [event for _, event, _ in schedule.events] == ['init', 'contour'] * 4 + ['done']
    assert queries == [(x, z, 0) for x in range(-4, 5) for z in range(-4, 5)]
    assert routes == [(0, 0)]
    assert schedule.read(0x30) == 0  # original start remains unchanged
    print('PASS: retail slot order, singleton resume, budget borrowing, init overshoot, player weights and exhausted retries')


if __name__ == '__main__':
    main()
