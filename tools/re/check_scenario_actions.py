#!/usr/bin/env python3
"""Check native CRT action targeting/amounts and creation placement, without GUI.
Reads the user's KINGDOMS.icd via emu.py; distributes no executable data.
"""
import struct
from emu import Icd, HEAP, STACK
from unicorn.x86_const import UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EBP


def main():
    checks = 0
    for opcode in (8, 9, 10, 11, 15):
        for selector in (0, 17, 99):
            m = Icd()
            game, script, action, regions, units = [HEAP + n for n in (0, 0x30000, 0x31000, 0x32000, 0x34000)]
            def put(a, fmt, *v): m.uc.mem_write(a, struct.pack(fmt, *v))
            def get(a): return struct.unpack('<I', m.uc.mem_read(a, 4))[0]
            put(0x62d55c, '<I', game)
            put(script, '<I', 0)
            put(script + 12, '<I', regions)
            put(action, '<II', opcode, 23)
            put(action + 0x4d, '<Hii', selector, 0, 1)
            put(regions + 0x100, '<4i', 2, 3, 4, 5)
            put(regions + 0x210, '<4i', 10, 20, 15, 27)
            # Three matching-region slots; middle slot different type, third outside.
            for p in range(2):
                row = game + 0x2404 + p * 0x110
                put(row, '<I', 1)
                put(row + 0xea, '<4B', 1, p, 1, 0)
                put(row + 0x74, '<II', units + p * 0x1000, units + p * 0x1000 + 2 * 0x138)
                put(row + 0x50, '<I', HEAP + 0x38000)
                for i, (kind, x, z) in enumerate(((17, 2, 3), (18, 4, 5), (17, 5, 5))):
                    u = units + p * 0x1000 + i * 0x138
                    put(u, '<H', kind)
                    put(u + 0x74, '<hh', x, z)
                    put(u + 0x130, '<I', 0x1000000)
            events = []
            def killed(uc, a): events.append(('destroy', get(a))); return 2, 0
            def transferred(uc, a): events.append(('transfer', get(a))); return 3, 0
            def health(uc, a):
                events.append(('hp', get(a + 4), get(a + 8), get(a + 12)))
                return 5, 0
            def order(uc, a):
                destination = get(a + 16)
                events.append(('move', get(a + 8), struct.unpack('<iii', m.uc.mem_read(destination, 12))))
                put(get(a), '<B', 0)  # observe native eligibility and destination before mission allocation
                return 6, get(a)
            m.hooks.update({0x512610: killed, 0x514da0: transferred, 0x51a140: health, 0x4de530: order})
            _, err = m.call(0x4cad50, (action, action + 0x100), ecx=script)
            assert not err, (opcode, selector, err)
            expected = 2 if selector == 0 else 1 if selector == 17 else 0
            assert len(events) == expected, (opcode, selector, events)
            for event in events:
                if opcode in (10, 11): assert event[2:] == (23, opcode + 2), event
                if opcode == 15: assert event[2][::2] == (12 << 20, 23 << 20), event
                if opcode == 9: assert event[1] >= units + 0x1000, event
                else: assert event[1] < units + 0x1000, event
            checks += 1
            put(action + 0x4f, '<i', -1)
            events.clear()
            _, err = m.call(0x4cad50, (action, action + 0x100), ecx=script)
            assert not err and not events, (opcode, 'missing region', err, events)
            checks += 1
    # Execute the native HP arithmetic block itself; amount is raw HP, no percentage.
    m = Icd()
    u, kind, packet = HEAP, HEAP + 0x1000, HEAP + 0x2000
    for amount in (0, 1, 23, 150, 32767, 32768, 65535):
        for hp in (0, 1, 80, 190, 200):
            m.uc.mem_write(u + 0x10c, struct.pack('<h', hp))
            m.uc.mem_write(u + 0x108, struct.pack('<f', .75))
            m.uc.mem_write(u + 0xb4, struct.pack('<I', kind))
            m.uc.mem_write(kind + 0x1be, struct.pack('<I', 200))
            m.uc.mem_write(packet + 5, struct.pack('<H', amount))
            m.uc.reg_write(UC_X86_REG_ESI, u)
            m.uc.reg_write(UC_X86_REG_EDI, packet)
            m.uc.emu_start(0x51a2f8, 0x51a320)
            assert struct.unpack('<h', m.uc.mem_read(u + 0x10c, 2))[0] == min(200, hp + amount)
            assert struct.unpack('<f', m.uc.mem_read(u + 0x108, 4))[0] == .75
            checks += 1
    for amount in (0,1,23,32767,32768,65535):
        for hp in (0,1,200):
            m.uc.mem_write(u+0x10c,struct.pack('<h',hp))
            m.uc.mem_write(packet+5,struct.pack('<H',amount))
            m.uc.reg_write(UC_X86_REG_ESI,u)
            m.uc.reg_write(UC_X86_REG_EDI,packet)
            m.uc.emu_start(0x51a3c2,0x51a3d9)
            expected=((hp-amount+32768)&65535)-32768
            assert struct.unpack('<h',m.uc.mem_read(u+0x10c,2))[0]==expected
            checks+=1
    # Native damage recomputes unfinished construction progress from remaining HP.
    m.uc.reg_write(UC_X86_REG_EBP,STACK+0x1000)
    for hp in (0,23,100):
        m.uc.mem_write(u+0x10c,struct.pack('<h',hp))
        m.uc.reg_write(UC_X86_REG_ESI,u)
        m.uc.emu_start(0x51a3e0,0x51a40e)
        progress=struct.unpack('<f',m.uc.mem_read(u+0x108,4))[0]
        assert abs(progress-(1-hp/200))<1e-7,(hp,progress)
        checks+=1
    # Creation chooses first free cell, including footprint-center offset.
    for occupied in (0, 1, 4):
        m = Icd()
        game, region, kinds, cells, live, out = [HEAP+n for n in (0,0x30000,0x31000,0x40000,0x50000,0x60000)]
        def put(a, fmt, *v): m.uc.mem_write(a, struct.pack(fmt,*v))
        put(0x62d55c,'<I',game)
        put(game+0x175c4,'<I',kinds)
        put(game+0x19e98,'<II',8,8)
        put(game+0x19f04,'<I',cells)
        put(game+0x14e84,'<II',live,live+0x138)
        put(live+0x138+0x130,'<I',0x1000000)
        put(region+0x100,'<4i',2,3,3,4)
        put(kinds+0x2a4+0x126,'<hh',1,1)
        for x,z in ((2,3),(3,3),(2,4),(3,4))[:occupied]:
            put(cells+(z*8+x)*14,'<H',1)
        m.hooks[0x5085c0]=lambda uc,a:(2,0)
        _,err=m.call(0x4cc3c0,(out,region,1))
        assert not err,err
        x,_,z=struct.unpack('<iii',m.uc.mem_read(out,12))
        want=((2,3),(3,3),(2,3))[(0,1,4).index(occupied)]
        assert (x,z)==tuple((c*16+8)*65536 for c in want),(occupied,x,z,want)
        checks+=1
    # Populate the native PRIMARY cell grid first: flying mode 2 does not block
    # scenario creation, while grounded and unfinished units do; yards honor state.
    for mode, structural, opened, unfinished, expected_x in (
            (1,False,False,False,56),(2,False,False,False,40),
            (1,False,False,True,56),(1,True,False,False,56),
            (1,True,True,False,40)):
        m=Icd()
        game, region, kinds, cells, live, out, yard = [HEAP+n for n in
            (0,0x30000,0x31000,0x40000,0x50000,0x60000,0x61000)]
        def put(a,fmt,*v):m.uc.mem_write(a,struct.pack(fmt,*v))
        put(0x62d55c,'<I',game);put(game+0x175c4,'<I',kinds)
        put(game+0x19e98,'<II',8,8);put(game+0x19f04,'<I',cells)
        put(game+0x14e84,'<II',live,live+0x138)
        unit=live+0x138;kind=kinds+0x2a4
        put(unit+2,'<H',1);put(unit+0x74,'<4h',2,3,1,1)
        put(unit+0x130,'<I',0x1000000|mode|(0x2000000 if structural else 0))
        put(unit+0xb4,'<I',kind);put(unit+0x108,'<f',.5 if unfinished else 0)
        put(unit+0x12f,'<B',4 if opened else 0)
        put(kind+0x126,'<hh',1,1);put(kind+0x12a,'<I',yard);put(yard,'<B',4)
        put(region+0x100,'<4i',2,3,3,4)
        for address in (0x506650,0x50ed60,0x4e1e20):m.hooks[address]=lambda uc,a:(2,0)
        m.hooks[0x5085c0]=lambda uc,a:(2,0)
        _,err=m.call(0x5066f0,(unit,));assert not err,err
        _,err=m.call(0x4cc3c0,(out,region,1));assert not err,err
        x,_,z=struct.unpack('<iii',m.uc.mem_read(out,12))
        assert (x,z)==(expected_x*65536,56*65536),(mode,structural,opened,unfinished,x,z)
        checks+=1
    # Completely off-map regions still use the asymmetrically clipped midpoint.
    for bounds,want in (((80,80,90,90),(44*16+8,44*16+8)),
                        ((-20,-20,-10,-10),(-5*16+8,-5*16+8))):
        put(region+0x100,'<4i',*bounds)
        _,err=m.call(0x4cc3c0,(out,region,1));assert not err,err
        x,_,z=struct.unpack('<iii',m.uc.mem_read(out,12))
        assert (x,z)==tuple(c*65536 for c in want),(bounds,x,z,want)
        checks+=1
    # Local transfer copies HP exactly; it does not heal a badly hurt victim.
    m=Icd();old,new=HEAP,HEAP+0x1000
    for hp in (1,23,190):
        m.uc.mem_write(old+0x10c,struct.pack('<h',hp))
        m.uc.reg_write(UC_X86_REG_EDI,old)
        m.uc.reg_write(UC_X86_REG_ESI,new)
        m.uc.emu_start(0x514f8a,0x514fa7)
        assert struct.unpack('<h',m.uc.mem_read(new+0x10c,2))[0]==hp
        checks+=1
    print(f'PASS: {checks} native CRT action targeting/amount cases')


if __name__ == '__main__': main()
