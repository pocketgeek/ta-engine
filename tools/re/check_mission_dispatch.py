#!/usr/bin/env python3
"""Compare the C++ primary order dispatcher with executable 4d8450.

Uses controlled handlers and queue-lifetime callbacks on both sides; retail
executes the event filtering, timer expiry, state transitions, loop limit and
RNG delay. This does not validate concrete handlers or World integration.
"""
import argparse
import random
import struct
import subprocess

from emu import Icd, HEAP

GS, UNIT, OWNER, TYPE, DEFS, HANDLER, MISSIONS = (HEAP+n for n in
    (0, 0x20000, 0x21000, 0x22000, 0x23000, 0x24000, 0x25000))


def retail(tick, seed, events, live, states, actions):
    icd = Icd(); uc = icd.uc
    def put(a, v): uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    def get(a): return struct.unpack('<I', uc.mem_read(a, 4))[0]
    def byte(a, v): uc.mem_write(a, bytes((v & 255,)))
    def address(i): return MISSIONS + i * 0x100
    queue = list(range(len(states))); trace = []; calls = 0
    put(0x62d55c, GS); put(GS + 0x19f44, tick); put(0x64186c, seed)
    put(UNIT + 0x130, 0x1000000 if live else 0); put(UNIT + 0xd0, events)
    put(UNIT + 0xb8, OWNER); put(OWNER, 1); byte(OWNER + 0xea, 1)
    put(UNIT + 0xb4, TYPE); byte(TYPE + 0x24b, 0)  # no idle order factory
    put(0x62db84, DEFS); put(DEFS + 25 + 4, HANDLER)
    for i, (stage, mask, deadline, pending, flags) in enumerate(states):
        a = address(i); byte(a+4, 1); byte(a+5, stage)
        for offset, value in ((6,mask),(10,deadline),(14,UNIT),(0x6a,pending),(0x5a,flags)):
            put(a+offset, value)
    def relink():
        put(UNIT+0x60, address(queue[0]) if queue else 0)
        for j,i in enumerate(queue):
            put(address(i)+0x66, address(queue[j+1]) if j+1<len(queue) else 0)
    relink()
    def handle(uc, args):
        nonlocal calls
        a = get(args+4); i = (a-MISSIONS)//0x100
        trace.append(f'H {i} {uc.mem_read(a+5,1)[0]} {get(args+8)} {get(a+0x6a)} {get(UNIT+0xd0)}')
        result, stage, mask, pending, event, disable = actions[min(calls, len(actions)-1)]
        calls += 1
        if stage >= 0: byte(a+5, stage)
        put(a+6, mask); put(a+0x6a, get(a+0x6a)|pending)
        put(UNIT+0xd0, get(UNIT+0xd0)|event)
        if disable: put(UNIT+0x130, 0)
        return 3, result
    def remove(uc, args):
        i = (get(args+4)-MISSIONS)//0x100
        trace.append(f'R {i}'); queue.remove(i); relink(); return 2, 0
    def rotate(uc, args):
        i = (get(args+4)-MISSIONS)//0x100
        trace.append(f'Q {i}'); queue.remove(i); queue.append(i); relink(); return 2, 0
    def clear(uc, args):
        trace.append('C'); queue.clear(); relink(); return 2, 0
    icd.hooks[HANDLER] = handle
    icd.hooks[0x4d6ad0] = remove; icd.hooks[0x4d6b30] = rotate; icd.hooks[0x4d6a50] = clear
    _, error = icd.call(0x4d8450, (UNIT,))
    if error: raise AssertionError(error)
    trace.append('F ' + ' '.join(map(str, [get(0x64186c),get(UNIT+0xd0),int(get(UNIT+0x130)!=0),len(queue),*queue])))
    for i in range(len(states)):
        a=address(i)
        trace.append('S ' + ' '.join(map(str, [uc.mem_read(a+5,1)[0], get(a+6), get(a+10), get(a+0x6a), get(a+0x5a)])))
    return trace


def check(binary, fixture):
    tick,seed,events,live,states,actions=fixture
    data=' '.join(map(str,[tick,seed,events,live,len(states),len(actions)]))+'\n'
    data+=''.join(' '.join(map(str,s))+'\n' for s in states+actions)
    actual=subprocess.run([binary,'--oracle'],input=data,text=True,capture_output=True,check=True).stdout.splitlines()
    expected=retail(*fixture)
    if actual!=expected:
        first=next((i for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),min(len(actual),len(expected)))
        raise AssertionError({'input':data,'line':first,'port':actual[first:first+1],'retail':expected[first:first+1]})
    return sum(line.startswith('H ') for line in expected)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary',default='./build-dbg/retail_mission_test')
    args=parser.parse_args()
    state=[2,0x2701,10076,0x3000,0]
    fixtures=[(10070,1941872796,0,1,[state],[[0,-1,0,0,0,0],[1,-1,0,0,0,0],[1,-1,0x2701,0,0,1]])]
    # Every return code, byte overflow, queue removal/rotation, timer boundary,
    # unsigned wrap, no-head/disabled cases and the infinite-handler guard.
    for result in range(12):
        for count in (1,3):
            fixtures.append((50,123,0,1,[[255,0,0xffffffff,0,0] for _ in range(count)],
                             [[result,-1,0,0,0,0]]))
    for tick in (0,49,50,51,0xffffffff):
        for mask in (0,1,0x2000,0x2701):
            for pending in (0,1,0x1000,0x2000,0x3000):
                fixtures.append((tick,1,0x400,1,[[2,mask,50,pending,0]],[[3,-1,0x2700,0,0,0]]))
    fixtures.extend([(0,1,0,1,[],[[7,-1,0,0,0,0]]),(0,1,0,0,[state],[[7,-1,0,0,0,0]])])
    rng=random.Random(0x4d8450)
    for _ in range(200):
        tick=rng.choice((100,0xffffffff,0xfffffff0))
        states=[[rng.randrange(256),rng.choice((0,1,0x2701,0xffffffff)),
                 rng.choice((tick,(tick+5)&0xffffffff,0xffffffff)),rng.getrandbits(16),rng.getrandbits(24)]
                for _ in range(rng.randint(1,4))]
        actions=[[rng.randrange(11),rng.choice((-1,0,1,2,255)),rng.choice((0,1,0x2701)),
                  rng.getrandbits(16),rng.getrandbits(16),int(rng.randrange(10)==0)]
                 for _ in range(rng.randint(1,6))]
        fixtures.append((tick,rng.randrange(1,0x7fffffff),rng.getrandbits(16),1,states,actions))
    calls=sum(check(args.binary,f) for f in fixtures)
    print(f'PASS: {len(fixtures)} executable dispatcher fixtures, {calls} handler calls, all queue/state/RNG results')


if __name__=='__main__': main()
