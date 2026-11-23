#!/usr/bin/env python3
"""Compare campaign condition predicates/events with original retail routines.
Loads only the user's ignored KINGDOMS.icd; no game GUI or assets are copied.
"""
import argparse,random,struct,subprocess
from emu import Icd,HEAP

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary',default='build-o2/campaign_conditions_test')
    args=ap.parse_args();p=Icd()
    game=HEAP;condition=HEAP+0x30000;unit=HEAP+0x31000;survivor=HEAP+0x32000
    definition=HEAP+0x33000;owner=HEAP+0x34000;side=HEAP+0x35000;buckets=HEAP+0x36000
    def put(a,v):p.uc.mem_write(a,struct.pack('<I',v&0xffffffff))
    def get(a):return struct.unpack('<I',p.uc.mem_read(a,4))[0]
    comparison=[True]
    p.hooks[0x5d5bb0]=lambda uc,sp:(0,0 if comparison[0] else 1)
    p.hooks[0x50a720]=lambda uc,sp:(4,0)
    p.hooks[0x515ac0]=lambda uc,sp:(1,5)
    p.hooks[0x4eb9e0]=lambda uc,sp:(0,HEAP+0x3a000)
    p.freeze_hooks();put(0x62d55c,game)
    put(unit+0xb4,definition);put(unit+0xb8,owner);put(owner+0x50,side)
    p.uc.mem_write(side+0x85,b'\0');p.uc.mem_write(definition+0x20,b'target\0')
    put(game+0x19f1c,16);put(game+0x19f20,16);put(game+0x19f18,buckets)
    for i in range(256):put(buckets+i*10+6,unit)
    rng=random.Random(0x523670);rows=[];expected=[]
    def call(address,args=(),ecx=condition):
        _,error=p.call(address,args,ecx=ecx)
        if error:raise RuntimeError((hex(address),error))
    # Execute the actual condition manager's virtual dispatch for every 4-bit
    # completion mask: victory requires all, defeat requires any.
    manager=HEAP+0x38000;objects=HEAP+0x39000
    put(game+0x24e8,1)
    for mask in range(16):
        for i in range(4):
            obj=objects+i*32;put(obj,0x5f34b8);put(obj+4,(mask>>i)&1)
            put(manager+i*4,obj);put(manager+0x44+i*4,obj)
        put(manager+0x40,4);put(manager+0x84,4)
        for address,want in ((0x5230b0,int(mask==15)),(0x523120,int(mask!=0))):
            actual,error=p.call(address,(),ecx=manager)
            if error or actual!=want:raise AssertionError((hex(address),mask,actual,want,error))
    # The parser tail skips fallback DestroyAllUnits only in mission mode1;
    # the defeat fallback is unconditional. Enter with its original frame.
    stub=HEAP+0x3b000;scenario=HEAP+0x3c000
    prefix=bytes.fromhex('55 89 e5 53 56 57 89 cf 31 db')
    p.uc.mem_write(stub,prefix+b'\xe9'+struct.pack('<i',0x522f32-(stub+len(prefix)+5)))
    put(game+0x175dc,scenario)
    for mode in (0,1):
        p.uc.mem_write(manager,b'\0'*0x90);put(scenario,mode)
        _,error=p.call(stub,(0,),ecx=manager)
        assert not error and get(manager+0x40)==1-mode and get(manager+0x84)==1, ('parser defaults',mode,error)
    put(manager+0x40,0)
    actual,error=p.call(0x5230b0,(),ecx=manager)
    assert not error and actual==0, ('empty victory',actual,error)
    put(game+0x2478,survivor);put(game+0x247c,survivor)
    for live in (0,1):
        put(manager+0x84,0);put(survivor+0x130,0x1000020 if live else 0)
        actual,error=p.call(0x523120,(),ecx=manager)
        assert not error and actual==1-live and get(manager+0x84)==1, ('default defeat',live,actual,error)
    for n in range(2048):
        p.uc.mem_write(condition,b'\0'*128)
        axis=rng.randrange(2);cell=rng.randrange(-100,100);line=cell+rng.randrange(-5,6)
        put(unit+0x130,0x1000000);p.uc.mem_write(unit+0x74,struct.pack('<hh',cell,cell))
        put(condition+0x30,line)
        call(0x524100 if axis else 0x524000,(unit,),condition+12)
        rows.append(f'A {cell} {line}');expected.append([get(condition+4)])
    for n in range(1024):
        p.uc.mem_write(condition,b'\0'*128)
        put(condition+0xc,0x5f33c4)
        cx=rng.randrange(128,768)*65536;cz=rng.randrange(128,768)*65536;r=rng.randrange(1,64)*65536
        x=cx+rng.randrange(-64,65)*65536+rng.randrange(65536)
        z=cz+rng.randrange(-64,65)*65536+rng.randrange(65536)
        put(condition+0x30,cx);put(condition+0x38,cz);put(condition+0x3c,r)
        put(unit+0x68,x);put(unit+0x70,z);put(unit+0x130,0x1000020)
        put(unit+0x108,0);put(unit+0x104,0);put(unit+0xa8,0);p.uc.mem_write(unit+0xfd,b'\0')
        call(0x523e70)
        rows.append(f'R {x} {z} {cx} {cz} {r}');expected.append([get(condition+4)])
    # The unit predicate of MoveUnitToRadius (523eb0, its scan callback, any
    # type) and of AllUnitsKilled (5243f0, which clears the survivor flag the
    # caller set): retail's selection predicate, every bit varied.
    carrier=HEAP+0x37000
    for n in range(2048):
        which=rng.randrange(2);own=rng.randrange(2);bits=[rng.randrange(2) for _ in range(7)]
        live,dying,selectable,finished,transferring,carried,holder=bits
        p.uc.mem_write(condition,b'\0'*128)
        put(unit+0x130,(0x1000000 if live else 0)|(0x1000 if dying else 0)|(0x20 if selectable else 0)|rng.choice((0,0x4000000)))
        p.uc.mem_write(unit+0x108,struct.pack('<f',0.0 if finished else rng.choice((0.25,30.0))))
        put(unit+0x104,150 if transferring else 0);put(unit+0xa8,carrier if carried else 0)
        put(carrier+0x130,0x1000020|(0x4000000 if holder else 0));p.uc.mem_write(unit+0xfd,bytes([own]))
        if which:
            put(condition,1);actual,error=p.call(0x5243f0,(unit,),ecx=condition+8)
            if error:raise RuntimeError(error)
            got=get(condition)
        else:
            call(0x523eb0,(unit,),condition+0xc);got=get(condition+4)
        rows.append('P %d %d '%(which,own)+' '.join(map(str,bits)));expected.append([got])
    kinds={1:0x523670,9:0x524310,5:0x523d60,12:0x524540,
           3:0x523870,4:0x523c00,11:0x524660}
    for n in range(4096):
        kind=rng.choice(list(kinds));own=rng.randrange(3);match=rng.randrange(2)
        commander=rng.randrange(2);mobile=rng.randrange(2);present=rng.randrange(2);met=rng.randrange(2)
        remaining=rng.randrange(-2,5)
        p.uc.mem_write(condition,b'\0'*128);put(condition+4,met);put(condition+8,1);put(condition+0x2c,remaining)
        p.uc.mem_write(unit+0xfd,bytes([own]));put(unit+8,owner if mobile else 0)
        comparison[0]=commander if kind in (1,9) else match
        p.uc.mem_write(survivor,b'\0'*0x138);put(survivor+0x130,0x1000000 if present else 0)
        put(survivor+8,owner);p.uc.mem_write(survivor,struct.pack('<H',5))
        put(game+0x2588,survivor);put(game+0x258c,survivor)
        put(game+0x2478,survivor);put(game+0x247c,survivor)
        put(condition+0xc,0x5f3470 if kind==3 else 0x5f340c)
        call(kinds[kind],(unit,))
        value=get(condition+0x2c);value=value if value<2**31 else value-2**32
        rows.append(f'D {kind} {own} {match} {commander} {mobile} {present} {met} {remaining}')
        expected.append([get(condition+4),value])
    result=subprocess.run([args.binary,'--native'],input='\n'.join(rows)+'\n',capture_output=True,text=True,check=True)
    actual=[list(map(int,line.split())) for line in result.stdout.splitlines()]
    if actual!=expected:
        raise AssertionError(next(((rows[i],a,b) for i,(a,b) in enumerate(zip(actual,expected)) if a!=b),(len(actual),len(expected))))
    print('PASS: 9253 native campaign cases: manager AND/OR, axis tolerance, fixed radius, the radius and AllUnitsKilled unit predicate, commander/type death events and survivor filters')
if __name__=='__main__':main()
