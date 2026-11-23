"""Substitution dispatch without retail assets."""
import struct
import unittest
from unicorn import Uc,UC_ARCH_X86,UC_MODE_32,UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP,UC_X86_REG_EAX
from emu import Icd


class BoundedHooksTest(unittest.TestCase):
    def test_frozen_hooks_preserve_call_result_and_stack(self):
        p=Icd.__new__(Icd); p.uc=Uc(UC_ARCH_X86,UC_MODE_32)
        p.uc.mem_map(0x1000,0x3000); calls=[]
        def substitute(uc,args):
            calls.append(struct.unpack('<I',uc.mem_read(args,4))[0])
            return 1,42
        p.hooks={0x1100:substitute}
        p._global_code_hook=p.uc.hook_add(UC_HOOK_CODE,p._code)
        # push 7; call 0x1100; inc eax; nop
        p.uc.mem_write(0x1000,bytes.fromhex('6a07e8f90000004090'))
        p.uc.mem_write(0x1100,b'\xcc')
        p.uc.reg_write(UC_X86_REG_ESP,0x3800)
        p.freeze_hooks()
        with self.assertRaises(TypeError): p.hooks[0x1200]=substitute
        p.uc.emu_start(0x1000,0x1009)
        self.assertEqual(calls,[7])
        self.assertEqual(p.uc.reg_read(UC_X86_REG_EAX),43)
        self.assertEqual(p.uc.reg_read(UC_X86_REG_ESP),0x3800)


if __name__=='__main__': unittest.main()
