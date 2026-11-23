"""Where the flight ports leave retail on purpose, for the checks that feed them the whole 32-bit range.

A position is 16.16 in 32 bits. Retail adds and subtracts positions in 32-bit
registers, where a sum past the end of the range wraps, and it reads a
whole-pixel distance as a signed 16-bit word, which turns negative at
32768 px. No retail map is wide enough for either to happen. The ports stop at
the end of the range instead (satAdd32 / satSub32, src/sim/fixed.h) and read
the distance whole: docs/retail-engine.md, "Nothing bounds a position".

A check lets a fixture differ only where retail itself left the range in that
very call. Watch sees it happen: it follows the handler while it builds the
mission's points and notes each sum that wrapped and each distance word that
read negative. Every other fixture must match exactly. Where the sum that
wrapped is the built point's own, with nothing derived from it, the row may
differ in that point's columns alone, by a coordinate the port stopped at an
end of the range (stopped).
"""
import struct

import unicorn.x86_const as x86
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_OP_REG
from unicorn import UC_HOOK_CODE

TOP, BOTTOM = 2**31 - 1, -2**31

# The helpers that add and subtract positions for every flight mission: 51b340(a, b), the direction of a from b
# (it takes the differences), and 421890, the sum of two vectors.
HELPERS = ((0x51b340, 0x51b364), (0x421890, 0x4218c3))


def fits(*raw):
    """Every raw value (before any 32-bit wrap) lies inside the range."""
    return all(BOTTOM <= v <= TOP for v in raw)


def stopped(got, want, columns):
    """The port's row is the native one except in `columns` (the x and z of a built point), where the port
    stopped at an end of the range. Retail's wrapped word there has the other sign: a sum or difference of two
    32-bit words that passes the top wraps to a negative one, and one that passes the bottom to 0 or more."""
    return got != want and len(got) == len(want) and all(
        g == w or i in columns and (g == TOP and w < 0 or g == BOTTOM and w >= 0)
        for i, (g, w) in enumerate(zip(got, want)))


class Watch:
    """Where retail left the range in a native call.

    `spans` are the first and last address of the code to follow: the mission's handler, the facing test it
    calls and HELPERS. There a 32-bit `add` or `sub` of two words held in registers or memory whose true result
    does not fit 32 bits has wrapped. Only a sum of positions, or of a position and a length, is large enough
    for that: the other such sums are of draws, counts and angles. (A sum with a constant is a pointer, an
    angle or the stack, and is not followed.) `words` are the addresses of the `movsx` that reads a whole-pixel
    distance as a signed 16-bit word. `kept` are the sums the port wraps as retail does (VTOL_Evade's line),
    which are no reason to differ.

    Make it before the first native call: unicorn keeps the code it has already translated, without the hook.
    """

    def __init__(self, icd, spans, words=(), kept=()):
        self.uc = icd.uc
        self.words, self.kept = frozenset(words), frozenset(kept)
        self.decoder = Cs(CS_ARCH_X86, CS_MODE_32)
        self.decoder.detail = True
        self.tests = {}        # address -> None, or the test of the instruction there
        self.seen = set()      # the addresses where retail left the range since left() was last asked
        for first, last in spans:
            self.uc.hook_add(UC_HOOK_CODE, self._step, begin=first, end=last)

    def left(self):
        """The addresses where the call(s) since the last question left the range: empty when none did."""
        seen, self.seen = frozenset(self.seen), set()
        return seen

    def _step(self, uc, address, size, _):
        if address not in self.tests:
            self.tests[address] = self._test(address, size)
        test = self.tests[address]
        if test and test():
            self.seen.add(address)

    def _test(self, address, size):
        insn = next(self.decoder.disasm(bytes(self.uc.mem_read(address, size)), address))
        if address in self.words:
            if insn.mnemonic != 'movsx' or insn.operands[1].size != 2:
                raise AssertionError('%#x is not a movsx of a word: %s %s' % (address, insn.mnemonic, insn.op_str))
            word = self._operand(insn, insn.operands[1])
            return lambda: word() >= 0x8000
        if insn.mnemonic not in ('add', 'sub') or insn.operands[0].size != 4 or address in self.kept or \
                any(op.type == X86_OP_IMM for op in insn.operands):
            return None
        a, b = (self._operand(insn, op) for op in insn.operands)
        sign = 1 if insn.mnemonic == 'add' else -1
        signed = lambda v: (v + 2**31) % 2**32 - 2**31
        return lambda: not fits(signed(a()) + sign * signed(b()))

    def _operand(self, insn, op):
        """A reader of a register or memory operand's value, asked before the instruction runs."""
        uc = self.uc
        register = lambda r: getattr(x86, 'UC_X86_REG_' + insn.reg_name(r).upper()) if r else None
        if op.type == X86_OP_REG:
            reg = register(op.reg)
            return lambda: uc.reg_read(reg)
        if op.type != X86_OP_MEM or op.size not in (2, 4):
            raise AssertionError('%#x: unexpected operand in %s %s' % (insn.address, insn.mnemonic, insn.op_str))
        base, index, scale, disp = register(op.mem.base), register(op.mem.index), op.mem.scale, op.mem.disp
        fmt, size = ('<H', 2) if op.size == 2 else ('<I', 4)

        def read():
            at = disp + (uc.reg_read(base) if base else 0) + (uc.reg_read(index) * scale if index else 0)
            return struct.unpack(fmt, uc.mem_read(at & 0xffffffff, size))[0]
        return read
