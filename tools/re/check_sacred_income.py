#!/usr/bin/env python3
"""Compare sacred-site footprint coverage and income with the original scan."""
import argparse
import random
import struct
import subprocess
from emu import Icd, HEAP, STACK, STACK_SZ
from unicorn.x86_const import UC_X86_REG_FPCW, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_ESP


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', default='build-dbg/retail_construction_test')
    args = ap.parse_args(); p = Icd(); p.uc.reg_write(UC_X86_REG_FPCW, 0x027f)
    game, unit, kind, features, cells = [HEAP+n*0x20000 for n in range(5)]
    frame = STACK+STACK_SZ-0x2000
    put = lambda a, v: p.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    real = lambda a, v: p.uc.mem_write(a, struct.pack('<f', v))
    word = lambda a: struct.unpack('<I', p.uc.mem_read(a, 4))[0]
    sites = []
    def query(uc, sp):
        x,z = struct.unpack('<2i', uc.mem_read(sp,8))
        for i in reversed(range(len(sites))):
            sx,sz,fx,fz,power = sites[i]
            if sx <= x < sx+fx and sz <= z < sz+fz: return 2, cells+i*16
        return 2, 0
    p.hooks[0x50e600] = query;p.freeze_hooks()
    put(0x62d55c,game);put(unit+0xb4,kind);put(kind+0x264,0x80000000)
    put(game+0x19edc,features)
    rng = random.Random(0x51d678); rows=[]; expected=[]
    for case in range(1024):
        x,z = rng.randrange(-2,8),rng.randrange(-2,8)
        fx,fz = rng.randrange(1,8),rng.randrange(1,8)
        income,prior = rng.choice([1.,10.,15.]),rng.random()*100
        prior = struct.unpack('<f',struct.pack('<f',prior))[0]
        sites[:] = [[rng.randrange(8),rng.randrange(8),rng.randrange(1,5),rng.randrange(1,5),
                     rng.choice([0.,1.,1.5,2.])] for _ in range(rng.randrange(1,8))]
        for i,(sx,sz,fw,fh,power) in enumerate(sites):
            p.uc.mem_write(features+320*i+0xb0,struct.pack('<2h',fw,fh))
            real(features+320*i+0x134,power)
            p.uc.mem_write(cells+16*i+8,struct.pack('<H',i))
        put(game+0x19ec0,len(sites))
        p.uc.mem_write(unit+0x74,struct.pack('<4h',x,z,fx,fz))
        real(kind+0x20a,income);real(frame-8,prior)
        for reg,value in ((UC_X86_REG_EBP,frame),(UC_X86_REG_ESP,frame-256),
                          (UC_X86_REG_ESI,unit),(UC_X86_REG_EDI,0)):
            p.uc.reg_write(reg,value)
        p.uc.emu_start(0x51d678,0x51d7c4,timeout=5_000_000)
        expected.append(word(frame-8))
        rows.append(' '.join(map(str,[x,z,fx,fz,income,prior,len(sites),*sum(sites,[])])))
    proc=subprocess.run([args.binary,'--sacred'],input='\n'.join(rows)+'\n',text=True,capture_output=True,check=True)
    actual=list(map(int,proc.stdout.split()))
    if actual!=expected:
        for row,want,got in zip(rows,expected,actual):
            if want!=got: raise AssertionError((row,want,got))
        raise AssertionError('row count')
    print(f'PASS: {len(rows)} sacred-site footprints, overlap order, coverage and income')


if __name__=='__main__':main()
