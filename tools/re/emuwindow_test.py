"""Window query contracts using synthetic memory only."""
import struct
import unittest
from types import SimpleNamespace
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from emuwindow import CapturedWindowQueries, CapturedWindowLoop


class WindowTest(unittest.TestCase):
    def test_message_payload_and_translate_validation(self):
        uc=Uc(UC_ARCH_X86,UC_MODE_32); uc.mem_map(0x100000,0x20000)
        def put(a,v): uc.mem_write(a,struct.pack('<I',v))
        p=SimpleNamespace(uc=uc,game=0x100000,icd=SimpleNamespace(hooks={}),
                          u32=lambda a:struct.unpack('<I',uc.mem_read(a,4))[0])
        put(p.game+0x19f44,20)
        for rva,(_,words) in CapturedWindowLoop.SPECS.items():
            uc.mem_write(0x100000+rva,b'\xb8\x01\0\0\0\xba\x02\0\0\0\xff\xd2\xc2'+struct.pack('<H',words*4))
        msg=struct.pack('<7I',5,0x200,0,0x12340056,100,86,4660)
        events=[{'stub_return':0x111d38,'return_address':0x1234,'tick':20,
                 'arguments':[0x200000,0,0,0,0],'result':1,'pointer_samples':{'0':msg.hex()}},
                {'stub_return':0x112e18,'return_address':0x1234,'tick':20,
                 'arguments':[0x200000,0],'result':0,'pointer_samples':{'0':msg.hex()}}]
        q=CapturedWindowLoop(p,{'syscall_calls':events},0x100000)
        put(0x100500,0x1234)
        uc.mem_write(0x100504,struct.pack('<5I',0x100600,0,0,0,0))
        uc.mem_write(0x100600,bytes([77])*32)
        self.assertEqual(q.query(uc,0x100504,0x111d2c),(5,1))
        self.assertEqual(bytes(uc.mem_read(0x100600,32)),msg+bytes([77])*4)
        put(0x100604,0x201)
        with self.assertRaisesRegex(RuntimeError,'translated message'):
            q.query(uc,0x100504,0x112e0c)
        self.assertEqual(q.index,1)
        put(0x100604,0x200)
        self.assertEqual(q.query(uc,0x100504,0x112e0c),(2,0))

    def test_order_dpi_and_relocated_rectangle(self):
        uc=Uc(UC_ARCH_X86,UC_MODE_32); uc.mem_map(0x100000,0x20000)
        def put(a,v): uc.mem_write(a,struct.pack('<I',v))
        p=SimpleNamespace(uc=uc,game=0x100000,icd=SimpleNamespace(hooks={}),
                          u32=lambda a:struct.unpack('<I',uc.mem_read(a,4))[0])
        put(p.game+0x19f44,20)
        for a,n in ((0x100100,4),(0x100120,12)):
            uc.mem_write(a,b'\xb8\x01\0\0\0\xba\x02\0\0\0\xff\xd2\xc2'+struct.pack('<H',n))
        rect=struct.pack('<4i',-10,30,800,900)
        sample=struct.pack('<II',0x200010,96)+bytes(8)+rect+bytes([255])*32
        events=[{'stub_return':0x10010c,'return_address':0x1234,'tick':20,
                 'arguments':[0xffffffff],'result':0x6011},
                {'stub_return':0x10012c,'return_address':0x1234,'tick':20,
                 'arguments':[5,0x200000,13],'result':1,'pointer_samples':{'1':sample.hex()}}]
        q=CapturedWindowQueries(p,{'syscall_calls':events},0x100100,0x100120)
        put(0x100500,0x1234); put(0x100504,0xffffffff)
        self.assertEqual(q.query(uc,0x100504,0x100100,1),(1,0x6011))
        uc.mem_write(0x100504,struct.pack('<III',5,0x100600,13))
        uc.mem_write(0x100600,struct.pack('<II',0x100700,120))
        with self.assertRaisesRegex(RuntimeError,'DPI'):
            q.query(uc,0x100504,0x100120,3)
        self.assertEqual(q.index,1)
        put(0x100604,96)
        uc.mem_write(0x100700,bytes([77])*32)
        self.assertEqual(q.query(uc,0x100504,0x100120,3),(3,1))
        self.assertEqual(bytes(uc.mem_read(0x100700,32)),rect+bytes([77])*16)


if __name__=='__main__': unittest.main()
