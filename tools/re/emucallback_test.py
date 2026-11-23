"""Callback execution and nonlocal return without retail assets."""
import struct
from types import SimpleNamespace
import unittest
from unicorn import Uc,UC_ARCH_X86,UC_MODE_32
from unicorn.x86_const import UC_X86_REG_ESP,UC_X86_REG_EBX,UC_X86_REG_EAX
from emucallback import CapturedDispatchCallbacks


class CallbackReplayTest(unittest.TestCase):
    def setup_fixture(self):
        uc=Uc(UC_ARCH_X86,UC_MODE_32);uc.mem_map(0x1000,0x30000)
        p=SimpleNamespace(uc=uc,game=0x10000)
        p.u32=lambda a:struct.unpack('<I',uc.mem_read(a,4))[0]
        uc.mem_write(p.game+0x19f44,struct.pack('<I',10))
        entry=dict(kind='syscall',stub_return=0x110c,number=1,return_address=0x1200,
                   stack=0x9000,arguments=[0xa000],tick=10,pointer_samples={'0':bytes(28).hex()})
        callback=dict(phase='entry',number=4,stack=0x8f00,address=0x8f20,size=44,
                      payload_hex=(struct.pack('<I',42)+bytes(40)).hex(),tick=10)
        returned=dict(phase='return',status=0,size=4,payload_hex=struct.pack('<I',42).hex(),tick=10)
        capture=dict(native_entry_sites=[1,2],native_entries=[entry],syscall_calls=[dict(entry,result=123)],
                     user_callbacks=[callback,returned],callback_entry=0x2000,callback_return=0x3000,
                     native_order=[{'kind':k,'index':i} for k,i in
                                   [('entry',0),('callback',0),('callback',1),('syscall',0)]])
        uc.mem_write(0x9000,struct.pack('<II',0x1200,0xa000))
        uc.reg_write(UC_X86_REG_ESP,0x9000);uc.reg_write(UC_X86_REG_EBX,17)
        # A real callback changes memory and EBX, then makes its nonlocal return.
        code=bytes.fromhex('c70500b0000063000000bb e7030000 6a00 6a04 68208f0000 b800300000 ffd0 cc')
        uc.mem_write(0x2000,code);uc.mem_write(0x1100,b'\xcc');uc.mem_write(0x3000,b'\xcc')
        return p,capture

    def test_executes_callback_and_restores_parent_registers(self):
        p,c=self.setup_fixture(); replay=CapturedDispatchCallbacks(p,c,0x1100)
        p.uc.emu_start(0x1100,0x1200)
        self.assertEqual(p.u32(0xb000),99)
        self.assertEqual(p.uc.reg_read(UC_X86_REG_EBX),17)
        self.assertEqual(p.uc.reg_read(UC_X86_REG_EAX),123)
        self.assertEqual(p.uc.reg_read(UC_X86_REG_ESP),0x9008)
        self.assertEqual((replay.index,replay.callback_count),(1,1))
        self.assertIsNone(replay.active)

    def test_rejects_changed_message_before_executing_callback(self):
        p,c=self.setup_fixture(); replay=CapturedDispatchCallbacks(p,c,0x1100)
        p.uc.mem_write(0xa004,b'\x01')
        with self.assertRaisesRegex(RuntimeError,'MSG input differs'):
            p.uc.emu_start(0x1100,0x1200)
        self.assertEqual(p.u32(0xb000),0)
        self.assertEqual(replay.callback_count,0)


if __name__=='__main__':unittest.main()
