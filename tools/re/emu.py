#!/usr/bin/env python3
"""Emulate individual KINGDOMS.icd routines to check our port against the real
thing. Observation only -- nothing from the binary is copied into the engine,
and nothing from it is committed: this reads YOUR OWN retail install from the
gitignored assets/ directory at run time.

    pip install unicorn
    python3 tools/re/emusearch.py

What is in this file is addresses and struct offsets, which are ordinary
reverse-engineering notes -- the same ones written up in
docs/retail-engine.md.

Loads the PE's sections at their virtual addresses, sets up a stack, and calls a
function with the __thiscall/stdcall conventions the binary uses.
"""
import struct
import time
from unicorn import *
from unicorn.x86_const import *

ICD = "/home/pocket_geek/TAK/assets/game/KINGDOMS.icd"
SECTIONS = [  # (vma, size, file offset)
    (0x401000, 0x1e9912, 0x1000),     # .text
    (0x5eb000, 0x018862, 0x1eb000),   # .rdata
    (0x604000, 0x027000, 0x204000),   # .data
]
STACK = 0x70000000
STACK_SZ = 0x100000
HEAP = 0x71000000
HEAP_SZ = 0x400000


RETURN_MAGIC = 0x6FFFF000


class EmulationIncomplete(RuntimeError):
    """A native call ended before reaching its return address."""


def page(a):
    return a & ~0xFFF


class Icd:
    def __init__(self):
        self.data = open(ICD, "rb").read()
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        for vma, size, off in SECTIONS:
            base = page(vma)
            end = (vma + size + 0xFFF) & ~0xFFF
            self.uc.mem_map(base, end - base)
            self.uc.mem_write(vma, self.data[off:off + size])
        # .data's raw size stops short of the image's bss, and globals the game
        # keeps there (0x62d55c, the game-state pointer) sit past it. Map the
        # whole span up to .rsrc so those resolve.
        self.uc.mem_map(0x62b000, 0x670000 - 0x62b000)
        self.uc.mem_map(STACK, STACK_SZ)
        self.uc.mem_map(HEAP, HEAP_SZ)
        self.hooks = {}          # address -> python callable(uc) -> return value in eax
        self._global_code_hook = self.uc.hook_add(UC_HOOK_CODE, self._code)

    def freeze_hooks(self):
        """Finalize substitutions and avoid a Python callback on every instruction."""
        from types import MappingProxyType
        if self._global_code_hook is None:
            raise RuntimeError('substitution hooks already finalized')
        for address in self.hooks:
            self.uc.hook_add(UC_HOOK_CODE,self._code,begin=address,end=address)
        self.uc.hook_del(self._global_code_hook)
        self._global_code_hook=None
        self.hooks=MappingProxyType(dict(self.hooks))

    def _code(self, uc, addr, size, _):
        fn = self.hooks.get(addr)
        if fn is None:
            return
        # emulate a call's epilogue: take the return address and args off the
        # stack ourselves, then jump back
        esp = uc.reg_read(UC_X86_REG_ESP)
        ret = struct.unpack("<I", uc.mem_read(esp, 4))[0]
        nargs, val = fn(uc, esp + 4)
        uc.reg_write(UC_X86_REG_EAX, val & 0xFFFFFFFF)
        uc.reg_write(UC_X86_REG_ESP, esp + 4 + nargs * 4)
        uc.reg_write(UC_X86_REG_EIP, ret)

    # Scripts that deliberately end a native call early -- an emu_stop hook
    # inside the routine, or a deliberately bounded run -- set this on their
    # instance (or pass allow_early_stop=True) to keep the old behaviour of
    # returning whatever state the routine reached.
    allow_early_stop = False

    def call(self, addr, args=(), ecx=None, timeout=5_000_000, allow_early_stop=None):
        """stdcall: args pushed right to left. ecx set for __thiscall.

        `timeout` is unicorn's wall-clock budget in microseconds (0 = none).
        Unicorn ends a run that exhausts it, or one an emu_stop hook ends,
        exactly as if it had finished, so a routine that did not reach its
        return address raises EmulationIncomplete unless the caller opted into
        early stops."""
        uc = self.uc
        sp = STACK + STACK_SZ - 0x1000
        for a in reversed(args):
            sp -= 4
            uc.mem_write(sp, struct.pack("<i", a))
        magic = RETURN_MAGIC
        sp -= 4
        uc.mem_write(sp, struct.pack("<I", magic))
        uc.reg_write(UC_X86_REG_ESP, sp)
        if ecx is not None:
            uc.reg_write(UC_X86_REG_ECX, ecx)
        started = time.monotonic()
        try:
            uc.emu_start(addr, magic, timeout=timeout)
        except UcError as e:
            return None, "UC error %s at eip=%#x" % (e, uc.reg_read(UC_X86_REG_EIP))
        if allow_early_stop is None:
            allow_early_stop = self.allow_early_stop
        eip = uc.reg_read(UC_X86_REG_EIP)
        if eip != magic and not allow_early_stop:
            elapsed = time.monotonic() - started
            reason = ("exhausted its %.1f s wall-clock budget" % (timeout / 1e6)
                      if timeout and elapsed * 1e6 >= timeout * 0.9 else "was stopped early")
            raise EmulationIncomplete("call %#x %s at eip=%#x after %.2f s without returning"
                                      % (addr, reason, eip, elapsed))
        return uc.reg_read(UC_X86_REG_EAX), None

if __name__ == "__main__":
    icd = Icd()
    print("loaded; checking 0x415040 (delta -> 8-way direction) against our port\n")
    print("   dx    dz | icd | ours")
    # our port's rule, transcribed from src/sim/pathsearch.cpp
    def ours(dx, dz):
        if dx == 0 and dz == 0: return 0
        if abs(dx) > 2 * abs(dz): return 2 if dx < 0 else 6
        if abs(dz) > 2 * abs(dx): return 0 if dz <= 0 else 4
        if dx < 0: return 1 if dz <= 0 else 3
        return 7 if dz <= 0 else 5
    bad = 0
    tests = []
    for dx in (-40, -16, -5, -1, 0, 1, 5, 16, 40):
        for dz in (-40, -16, -5, -1, 0, 1, 5, 16, 40):
            tests.append((dx, dz))
    for dx, dz in tests:
        got, err = icd.call(0x415040, args=(dx, dz, -1))
        if err:
            print("  %5d %5d | %s" % (dx, dz, err))
            bad += 1
            break
        mine = ours(dx, dz)
        flag = "" if got == mine else "   <-- MISMATCH"
        if got != mine:
            bad += 1
        if got != mine or (dx, dz) in ((-40, 0), (40, 0), (0, -40), (0, 40), (40, 40), (-5, -16)):
            print("  %5d %5d |  %d  |  %d%s" % (dx, dz, got, mine, flag))
    print("\n%d/%d disagree" % (bad, len(tests)))
