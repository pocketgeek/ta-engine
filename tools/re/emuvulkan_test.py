"""Synthetic Vulkan replay contracts; no retail assets or GPU required."""
import struct
import unittest
from types import SimpleNamespace
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from emuvulkan import CapturedVulkanImages


class VulkanImageTest(unittest.TestCase):
    def setUp(self):
        self.uc=Uc(UC_ARCH_X86,UC_MODE_32)
        self.uc.mem_map(0x100000,0x20000)
        self.p=SimpleNamespace(uc=self.uc,icd=SimpleNamespace(hooks={}),game=0x100000)
        self.p.u32=lambda a:struct.unpack('<I',self.uc.mem_read(a,4))[0]
        self.write(0x119f44,7)

    def write(self,address,*values):
        self.uc.mem_write(address,struct.pack('<'+str(len(values))+'I',*values))

    def event(self,number,params,samples):
        rva,offset,_=CapturedVulkanImages.SPECS[number]
        return {'number':number,'return_address':0x200000+rva+offset,'tick':7,'result':0,
                'parameters':0x300000,'parameter_bytes':struct.pack('<'+str(len(params))+'I',*params).hex(),
                'pointer_samples':{str(a):data.hex() for a,data in samples.items()}}

    def test_image_format_properties_preserve_header(self):
        info=struct.pack('<7I',1000059004,0,44,1,0,7,8)
        props=struct.pack('<8IQ',1000059003,0,32768,32768,1,16,2048,15,1<<40)
        event=self.event(548,[123,0x400000,0x500000,0],{0x400000:info,0x500000:props})
        replay=CapturedVulkanImages(self.p,{'unix_calls':[event]},0x200000)
        self.uc.mem_write(0x101000,info); self.write(0x102000,1000059003,0)
        self.write(0x100500,123,0x101000,0x102000)
        self.assertEqual(replay.query(self.uc,0x100500,548,3),(3,0))
        self.assertEqual(bytes(self.uc.mem_read(0x102000,40)),props)
        with self.assertRaisesRegex(RuntimeError,'exhausted'): replay.query(self.uc,0x100500,548,3)

    def test_create_image_validates_nested_format_list(self):
        info=bytearray(68); struct.pack_into('<II',info,0,14,0x410000)
        ext=struct.pack('<4I',1000147000,0,2,0x420000)
        formats=struct.pack('<2I',44,50); handle=struct.pack('<Q',0xffffffff12345678)
        event=self.event(342,[123,0x400000,0,0x500000,0],
                         {0x400000:bytes(info),0x410000:ext,0x420000:formats,0x500000:handle})
        replay=CapturedVulkanImages(self.p,{'unix_calls':[event]},0x200000)
        struct.pack_into('<I',info,4,0x101100); self.uc.mem_write(0x101000,bytes(info))
        self.write(0x101100,1000147000,0,2,0x101200); self.write(0x101200,44,99)
        self.write(0x100500,123,0x101000,0,0x102000)
        with self.assertRaisesRegex(RuntimeError,'view-format list differs'): replay.query(self.uc,0x100500,342,4)
        self.assertEqual(replay.index,0); self.assertEqual(self.p.u32(0x102000),0)
        self.write(0x101200,44,50)
        self.assertEqual(replay.query(self.uc,0x100500,342,4),(4,0))
        self.assertEqual(bytes(self.uc.mem_read(0x102000,8)),handle)

    def test_memory_requirements_skip_padding_and_preserve_chain(self):
        info=struct.pack('<IIQ',1000146001,0,123456)
        result=struct.pack('<IIQQII',1000146003,0x510000,65536,1024,3,0xdeadbeef)
        ext=struct.pack('<4I',1000127000,0,1,0)
        event=self.event(503,[123,0x400000,0x500000],{0x400000:info,0x500000:result,0x510000:ext})
        replay=CapturedVulkanImages(self.p,{'unix_calls':[event]},0x200000)
        self.uc.mem_write(0x101000,info); self.write(0x102000,1000146003,0x102100)
        self.write(0x102100,1000127000,0); self.write(0x100500,123,0x101000,0x102000)
        replay.query(self.uc,0x100500,503,3)
        self.assertEqual(self.p.u32(0x102004),0x102100)
        self.assertEqual(self.p.u32(0x10201c),0)
        self.assertEqual(bytes(self.uc.mem_read(0x102008,20)),result[8:28])
        self.assertEqual(bytes(self.uc.mem_read(0x102108,8)),ext[8:16])

    def test_destroy_view_checks_full_handle_and_rejects_allocator(self):
        event=self.event(398,[123,0,0x12345678,0x87654321,0],{})
        replay=CapturedVulkanImages(self.p,{'unix_calls':[event]},0x200000)
        self.write(0x100500,123,0x12345678,0x87654320,0)
        with self.assertRaisesRegex(RuntimeError,'destruction arguments differ'):
            replay.query(self.uc,0x100500,398,4)
        self.assertEqual(replay.index,0)
        self.write(0x100500,123,0x12345678,0x87654321,0x101000)
        with self.assertRaisesRegex(RuntimeError,'custom Vulkan allocator'):
            replay.query(self.uc,0x100500,398,4)
        self.write(0x100500,123,0x12345678,0x87654321,0)
        self.assertEqual(replay.query(self.uc,0x100500,398,4),(4,0))

    def test_fence_wait_checks_handles_timeout_and_returns_recorded_result(self):
        handle=struct.pack('<Q',0x1234567887654321)
        event=self.event(681,[123,1,0x400000,1,0xffffffff,0xffffffff,2],{0x400000:handle})
        replay=CapturedVulkanImages(self.p,{'unix_calls':[event]},0x200000)
        self.uc.mem_write(0x101000,handle)
        self.write(0x100500,123,1,0x101000,1,0,0)
        with self.assertRaisesRegex(RuntimeError,'timeout differs'):
            replay.query(self.uc,0x100500,681,6)
        self.assertEqual(replay.index,0)
        self.write(0x100500,123,1,0x101000,1,0xffffffff,0xffffffff)
        self.assertEqual(replay.query(self.uc,0x100500,681,6),(6,2))

    def test_surface_array_relocation_capacity_and_output_bounds(self):
        formats=struct.pack('<4I',44,0,50,0)
        event=self.event(572,[123,0,456,0,0x400000,0x500000,0],
                         {0x400000:struct.pack('<I',2),0x500000:formats})
        replay=CapturedVulkanImages(self.p,{'unix_calls':[event]},0x200000)
        self.write(0x100500,123,456,0,0x101000,0x102000)
        self.write(0x101000,1); self.write(0x102010,0xdeadbeef)
        with self.assertRaisesRegex(RuntimeError,'output capacity'):
            replay.query(self.uc,0x100500,572,5)
        self.assertEqual(self.p.u32(0x101000),1)
        self.assertEqual(replay.index,0)
        self.write(0x101000,2)
        self.assertEqual(replay.query(self.uc,0x100500,572,5),(5,0))
        self.assertEqual(bytes(self.uc.mem_read(0x102000,16)),formats)
        self.assertEqual(self.p.u32(0x102010),0xdeadbeef)

    def test_swapchain_checks_present_modes_and_ignores_abi_padding(self):
        info=bytearray(88); struct.pack_into('<4I',info,0,1000001000,0x410000,4,0xdeadbeef)
        modes=struct.pack('<4I',1000275002,0,2,0x420000)
        handle=struct.pack('<Q',0x1234567887654321)
        event=self.event(367,[123,0x400000,0,0x500000,0],
            {0x400000:bytes(info),0x410000:modes,0x420000:struct.pack('<2I',2,0),0x500000:handle})
        replay=CapturedVulkanImages(self.p,{'unix_calls':[event]},0x200000)
        struct.pack_into('<I',info,4,0x101100); struct.pack_into('<I',info,12,0)
        self.uc.mem_write(0x101000,bytes(info));self.write(0x101100,1000275002,0,2,0x101200)
        self.write(0x101200,2,1);self.write(0x100500,123,0x101000,0,0x102000)
        with self.assertRaisesRegex(RuntimeError,'present-mode list differs'):
            replay.query(self.uc,0x100500,367,4)
        self.write(0x101200,2,0)
        self.assertEqual(replay.query(self.uc,0x100500,367,4),(4,0))
        self.assertEqual(bytes(self.uc.mem_read(0x102000,8)),handle)


if __name__=='__main__': unittest.main()
