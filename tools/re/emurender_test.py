"""External-input replay tests with synthetic memory, no retail installation."""
import struct
import unittest
from types import SimpleNamespace
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_ESP
from emu import STACK, STACK_SZ
from emurender import (CapturedClock, CapturedCursor, CapturedMonitor,
                       CapturedDisplaySettings, CapturedFormatProperties,
                       CapturedUnmap, CapturedMapView, restore_thread_segment)


class RenderInputTest(unittest.TestCase):
    def test_map_view_restores_bounded_bytes_and_relocates_outputs(self):
        import base64
        import zlib
        self.uc.mem_write(0x10000d,b'\x28\x00')
        args=[12,0xffffffff,0x100600,0,0,0x100608,0x100604,1,0,4]
        self.uc.mem_write(0x100504,struct.pack('<10I',*args))
        expected=list(args)
        expected[2]=0x200600; expected[5]=0x200608; expected[6]=0x200604
        raw=bytes(range(256))*16
        view={'address':0x200000,'size':4096,'section_offset':'00000b0000000000',
              'zlib_base64':base64.b64encode(zlib.compress(raw)).decode()}
        event={'stub_return':0x10000c,'return_address':0x1234,'tick':7,
               'result':0,'arguments':expected,'mapped_view':view}
        mapper=CapturedMapView(self.p,{'map_view_return':0x10000c,'syscall_calls':[event]},0x100000)
        self.p.ensure=lambda a,n:self.uc.mem_map(a,n)
        self.put(0x100520,2)  # InheritDisposition mismatch must not write outputs.
        with self.assertRaisesRegex(RuntimeError,'scalar'):
            mapper.query(self.uc,0x100504)
        self.assertEqual(mapper.index,0)
        self.put(0x100520,1)
        self.assertEqual(mapper.query(self.uc,0x100504),(10,0))
        self.assertEqual(bytes(self.uc.mem_read(0x200000,4096)),raw)
        self.assertEqual(self.p.u32(0x100600),0x200000)
        self.assertEqual(self.p.u32(0x100604),4096)
        self.assertEqual(bytes(self.uc.mem_read(0x100608,8)),bytes.fromhex(view['section_offset']))

    def setUp(self):
        self.uc=Uc(UC_ARCH_X86,UC_MODE_32)
        self.uc.mem_map(0x100000,4096)
        self.uc.mem_map(STACK,STACK_SZ)
        self.p=SimpleNamespace(uc=self.uc,icd=SimpleNamespace(hooks={}),game=0x100100)
        self.uc.mem_map(0x11a000,4096)
        self.p.u32=lambda a:struct.unpack('<I',self.uc.mem_read(a,4))[0]
        self.put(self.p.game+0x19f44,7)
        self.uc.mem_write(0x100000,b'\xb8\x31\0\0\0\xba\x64\xe1\xef\x7b\xff\xd2\xc2\x08\x00')
        self.put(0x100500,0x1234); self.put(0x100504,0x100600); self.put(0x100508,0)
        self.event={'return_address':0x1234,'tick':7,'counter_address':1,
                    'frequency_address':0,'counter':1234567890123,'status':0}

    def put(self,address,value):
        self.uc.mem_write(address,struct.pack('<I',value))

    def clock(self):
        return CapturedClock(self.p,{'performance_counter_return':0x10000c,
                                    'performance_calls':[self.event]})

    def test_counter_relocates_output_and_consumes_once(self):
        clock=self.clock()
        self.assertEqual(clock.query(self.uc,0x100504),(2,0))
        self.assertEqual(struct.unpack('<q',self.uc.mem_read(0x100600,8))[0],self.event['counter'])
        with self.assertRaisesRegex(RuntimeError,'exhausted'):
            clock.query(self.uc,0x100504)

    def test_counter_rejects_wrong_order_without_consumption(self):
        clock=self.clock()
        self.put(self.p.game+0x19f44,8)
        with self.assertRaisesRegex(RuntimeError,'order differs'):
            clock.query(self.uc,0x100504)
        self.assertEqual(clock.index,0)
        self.assertEqual(bytes(self.uc.mem_read(0x100600,8)),bytes(8))

    def test_fs_reads_the_captured_teb(self):
        teb=0x100800
        self.put(teb+0x18,teb)
        restore_thread_segment(self.p,teb)
        self.uc.reg_write(UC_X86_REG_ESP,STACK+STACK_SZ-4096)
        self.uc.reg_write(UC_X86_REG_EAX,42)
        self.uc.mem_write(0x100020,b'\x50\x59\x64\xa1\x18\0\0\0')
        self.uc.emu_start(0x100020,0x100028)
        self.assertEqual(self.uc.reg_read(UC_X86_REG_EAX),teb)
        self.assertEqual(self.uc.reg_read(UC_X86_REG_ECX),42)
        self.assertEqual(self.uc.reg_read(UC_X86_REG_ESP),STACK+STACK_SZ-4096)

    def test_cursor_copies_only_defined_output(self):
        self.uc.mem_write(0x10000d,b'\x04')
        event={'stub_return':0x10000c,'return_address':0x1234,'tick':7,
               'arguments':[0x200000],'result':1,
               'pointer_samples':{'0':(struct.pack('<ii',-200,400)+b'UNRELATED').hex()}}
        cursor=CapturedCursor(self.p,{'syscall_calls':[event]},0x100000)
        self.assertEqual(cursor.query(self.uc,0x100504),(1,1))
        self.assertEqual(bytes(self.uc.mem_read(0x100600,16)),struct.pack('<ii',-200,400)+bytes(8))
        with self.assertRaisesRegex(RuntimeError,'exhausted'): cursor.query(self.uc,0x100504)

    def test_cursor_failure_has_no_output_and_wrong_tick_is_rejected(self):
        self.uc.mem_write(0x10000d,b'\x04')
        event={'stub_return':0x10000c,'return_address':0x1234,'tick':8,
               'arguments':[1],'result':0,'pointer_samples':{}}
        cursor=CapturedCursor(self.p,{'syscall_calls':[event]},0x100000)
        with self.assertRaisesRegex(RuntimeError,'order differs'): cursor.query(self.uc,0x100504)
        self.assertEqual(cursor.index,0)
        self.put(self.p.game+0x19f44,8)
        self.assertEqual(cursor.query(self.uc,0x100504),(1,0))
        self.assertEqual(bytes(self.uc.mem_read(0x100600,8)),bytes(8))

    def test_monitor_info_preserves_unwritten_name_padding(self):
        self.uc.mem_write(0x10000d,b'\x0c')
        sample=struct.pack('<I',104)+bytes(range(36))+b'D\0\0\0'+b'X'*60
        event={'stub_return':0x10000c,'return_address':0x1234,'tick':7,
               'arguments':[1,0x200000,2],'result':1,'pointer_samples':{'1':sample.hex()}}
        monitor=CapturedMonitor(self.p,{'syscall_calls':[event]},0x100000)
        self.put(0x100504,1); self.put(0x100508,0x100600); self.put(0x10050c,2)
        self.put(0x100600,104)
        self.assertEqual(monitor.query(self.uc,0x100504),(3,1))
        self.assertEqual(bytes(self.uc.mem_read(0x100600,104)),sample[:44]+bytes(60))

    def test_display_settings_requires_recorded_name_source_and_bounds_output(self):
        self.uc.mem_write(0x10000d,b'\x10')
        monitor={'stub_return':0x10010c,'return_address':1,'tick':7,
                 'arguments':[1,0x200000,2],'result':1,
                 'pointer_samples':{'1':(struct.pack('<I',104)+bytes(36)+b'D\0\0\0'+bytes(60)).hex()}}
        mode=bytearray(188); struct.pack_into('<HH',mode,68,188,0)
        event={'stub_return':0x10000c,'return_address':0x1234,'tick':7,
               'arguments':[0x300000,0xffffffff,0x400000,0],'result':1,
               'pointer_samples':{'0':struct.pack('<HHI',2,4,0x200028).hex(),
                                  '2':(mode+b'UNRELATED').hex()}}
        display=CapturedDisplaySettings(self.p,{'syscall_calls':[monitor,event]},0x100000,0x100100)
        self.put(0x100504,0x100700); self.put(0x100508,0xffffffff)
        self.put(0x10050c,0x100600); self.put(0x100510,0)
        self.uc.mem_write(0x100700,struct.pack('<HHI',2,4,0x100800))
        self.uc.mem_write(0x100800,b'D\0\0\0'); self.put(0x100644,220)
        self.assertEqual(display.query(self.uc,0x100504),(4,1))
        self.assertEqual(bytes(self.uc.mem_read(0x100600,200)),mode+bytes(12))
        event['pointer_samples']['0']=struct.pack('<HHI',2,4,0x200030).hex()
        display=CapturedDisplaySettings(self.p,{'syscall_calls':[monitor,event]},0x100000,0x100100)
        with self.assertRaisesRegex(RuntimeError,'monitor-output source'):
            display.query(self.uc,0x100504)

    def test_format_capabilities_relocate_extensions_and_reject_unknown_chain(self):
        code=bytearray(0x3f); code[:3]=b'\x55\x89\xe5'; code[-6:-4]=b'\xff\x15'
        self.uc.mem_write(0x100000,bytes(code))
        event={'return_address':0x10003f,'number':0x220,'result':0,'tick':7,
               'parameter_bytes':struct.pack('<III',123,44,0x200000).hex(),
               'pointer_samples':{str(0x200000):struct.pack('<5I',1000059002,0x300000,1,2,3).hex(),
                                  str(0x300000):struct.pack('<II3Q',1000360000,0,1<<40,5,6).hex()}}
        self.put(0x100504,123); self.put(0x100508,44); self.put(0x10050c,0x100600)
        self.put(0x100600,1000059002); self.put(0x100604,0x100700)
        self.put(0x100700,1000360000); self.put(0x100704,0)
        formats=CapturedFormatProperties(self.p,{'unix_calls':[event]},0x100000)
        self.assertEqual(formats.query(self.uc,0x100504),(3,0))
        self.assertEqual(self.p.u32(0x100604),0x100700)
        self.assertEqual(bytes(self.uc.mem_read(0x100608,12)),struct.pack('<3I',1,2,3))
        self.assertEqual(bytes(self.uc.mem_read(0x100708,24)),struct.pack('<3Q',1<<40,5,6))
        self.put(0x100608,99); self.put(0x100700,123)
        formats=CapturedFormatProperties(self.p,{'unix_calls':[event]},0x100000)
        with self.assertRaisesRegex(RuntimeError,'structure differs'): formats.query(self.uc,0x100504)
        self.assertEqual(self.p.u32(0x100608),99)
        self.assertEqual(formats.index,0)

    def test_unmap_diagnostic_checks_exact_arguments(self):
        event={'stub_return':0x10000c,'return_address':0x1234,'tick':7,
               'arguments':[0xffffffff,0x200000],'result':0}
        unmap=CapturedUnmap(self.p,{'syscall_calls':[event]},0x100000)
        self.put(0x100504,0xffffffff); self.put(0x100508,0x300000)
        with self.assertRaisesRegex(RuntimeError,'arguments differ'): unmap.query(self.uc,0x100504)
        self.assertEqual(unmap.index,0)
        self.put(0x100508,0x200000)
        self.assertEqual(unmap.query(self.uc,0x100504),(2,0))


if __name__=='__main__': unittest.main()
