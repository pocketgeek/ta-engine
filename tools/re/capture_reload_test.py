"""Bounded runtime capture checks using synthetic memory only."""
import base64
import struct
import unittest
import zlib

from capture_reload import (runtime_state, game_memory_ranges, game_memory_state,
                            captured_crt_thread, native_memory_ranges, native_memory_state,
                            device_memory_ranges, device_memory_state, captured_performance_counter,
                            captured_syscall_return, captured_unix_return, captured_unix_arguments, captured_mapped_view,
                            captured_user_callback, captured_syscall_arguments)
from livesample import STRIDE


class Memory:
    def __init__(self):
        self.data = bytearray(0x640000)
        self.game, self.entities = 0x1000, 0x30000
        self.unit = self.entities + STRIDE
        self.put(self.game + 0x14e84, self.entities)
        self.put(self.game + 0x14e88, self.unit)
        self.put(self.game + 0x19e98, 8)
        self.put(self.game + 0x19e9c, 8)
        self.put(self.game + 0x19ef4, 0x80000)
        self.put(self.game + 0x19f04, 0x81000)
        self.put(self.game + 0x19edc, 0x82000)
        self.put(self.game + 0x19ec0, 2)
        self.put(0x62d558, 0x40000)
        self.put(0x40008, 0x40100)
        self.put(0x4010c, 2)
        self.put(0x62db84, 0x50000)
        self.put(0x50004, 0x402c00)
        self.put(self.unit + 0x60, 0x60000)
        self.put(0x60000 + 0x66, 0x61000)
        self.put(0x60000 + 0x6e, 0x62000)
        self.put(0x62000, 0x5f290c)
        self.put(self.unit + 8, 0x70000)
        self.put(0x70000, 0x71000)
        self.put(0x71000, 0x5f2a24)
        self.put(0x70004, 0x72000)
        self.put(0x72340, 8)
        self.put(0x72344, 9)  # partial last packed row
        self.put(0x72348, 0x73000)
        self.data[0x73000:0x73040] = bytes(range(64))

    def put(self, address, value):
        struct.pack_into('<I', self.data, address, value)

    def read(self, address, size):
        if address < 0 or address + size > len(self.data):
            raise RuntimeError('out of bounds')
        return bytes(self.data[address:address+size])

    def u32(self, address):
        return struct.unpack('<I', self.read(address, 4))[0]


class RuntimeCaptureTest(unittest.TestCase):
    def test_user_callback_entry_return_and_bounds(self):
        mem=Memory(); stack=0x100
        struct.pack_into('<4I',mem.data,stack,0x1234,4,0x30000,44)
        event=captured_user_callback(mem,stack)
        self.assertEqual((event['number'],event['phase'],event['size']),(4,'entry',44))
        self.assertEqual(bytes.fromhex(event['payload_hex']),mem.read(0x30000,44))
        struct.pack_into('<III',mem.data,stack+4,0x30000,4,0)
        event=captured_user_callback(mem,stack,True)
        self.assertEqual((event['status'],event['phase'],event['return_address']),(0,'return',0x1234))
        mem.put(stack+8,65537)
        with self.assertRaisesRegex(ValueError,'limit'):
            captured_user_callback(mem,stack,True)

    def test_mapped_view_bytes_and_limits(self):
        mem=Memory()
        event={'result':0,'arguments':[1,0xffffffff,0x200,0,0,0x208,0x204,1,0,4]}
        mem.put(0x200,0x90000); mem.put(0x204,4096)
        mem.data[0x90000:0x91000]=bytes(range(256))*16
        view=captured_mapped_view(mem,event)
        self.assertEqual(zlib.decompress(base64.b64decode(view['zlib_base64'])),mem.read(0x90000,4096))
        self.assertEqual(view['section_offset'],'0000000000000000')
        with self.assertRaisesRegex(ValueError,'limit'):
            captured_mapped_view(mem,event,4095)
        mem.put(0x200,0x60000000)
        with self.assertRaisesRegex(ValueError,'reserved'):
            captured_mapped_view(mem,event)
        event['result']=0xc0000001
        self.assertIsNone(captured_mapped_view(mem,event))

    def test_unix_return_samples_bounded_extension_chain(self):
        mem=Memory(); frame,stack,params=0x100,0x200,0x10000
        mem.put(frame+8,0x7968d9ef); mem.put(frame+12,stack+16)
        struct.pack_into('<4I',mem.data,stack,0x1234,1,0x220,params)
        struct.pack_into('<3I',mem.data,params,0,44,0x20000)
        struct.pack_into('<5I',mem.data,0x20000,1000059002,0x30000,1,2,3)
        struct.pack_into('<II3Q',mem.data,0x30000,1000360000,0x30000,4,5,6)
        event=captured_unix_return(mem,frame,0)
        self.assertEqual(event['number'],0x220)
        self.assertEqual(event['handle'],0x100001234)
        self.assertEqual(set(event['pointer_samples']),{str(0x20000),str(0x30000)})
        self.assertEqual(len(bytes.fromhex(event['parameter_bytes'])),256)

    def test_actual_unix_return_uses_entry_arguments_after_stack_reuse(self):
        mem=Memory(); stack,params=0x200,0x10000
        entry=(0x1234,1,397,params)
        struct.pack_into('<4I',mem.data,stack,0x1234,1,397,0x796866f1)
        mem.data[params:params+4]=b'out!'
        event=captured_unix_arguments(mem,stack,0x796866f1,0,entry)
        self.assertEqual(event['parameters'],params)
        self.assertEqual(event['handle'],0x100001234)
        self.assertTrue(event['parameter_bytes'].startswith(b'out!'.hex()))

    def test_shared_syscall_exit_records_outputs_and_validates_frame(self):
        mem=Memory(); frame,stack,entry=0x100,0x200,0x300
        mem.data[entry:entry+15]=bytes.fromhex('b8ea130000ba84d04d7bffd2c20400')
        mem.put(frame+8,entry+12); mem.put(frame+12,stack); mem.put(frame+0x1c,0x13ea)
        mem.put(stack,0x48c65b); mem.put(stack+4,0x1fff8)
        struct.pack_into('<ii',mem.data,0x1fff8,-400,1200)
        event=captured_syscall_return(mem,frame,1)
        self.assertEqual(event['arguments'],[0x1fff8])
        self.assertEqual(event['return_address'],0x48c65b)
        self.assertEqual(event['pointer_samples'],{'0':struct.pack('<ii',-400,1200).hex()})
        before=captured_syscall_arguments(mem,entry+12,stack,0x13ea)
        self.assertNotIn('result',before)
        mem.put(0x1fff8,42)
        after=captured_syscall_return(mem,frame,1)
        self.assertNotEqual(before['pointer_samples'],after['pointer_samples'])
        self.assertEqual(before['arguments'],after['arguments'])
        mem.put(stack+4,0xf0000000)
        self.assertEqual(captured_syscall_return(mem,frame,0)['pointer_samples'],{})
        mem.put(frame+0x1c,123)
        with self.assertRaisesRegex(ValueError,'numbers differ'):
            captured_syscall_return(mem,frame,1)

    def test_unix_return_captures_nested_image_format_array(self):
        mem=Memory(); frame,stack,params=0x100,0x200,0x10000
        mem.put(frame+8,0x79683bbb); mem.put(frame+12,stack+16)
        struct.pack_into('<4I',mem.data,stack,0x1234,0,342,params)
        struct.pack_into('<4I',mem.data,params,0,0x20000,0,0x50000)
        struct.pack_into('<II',mem.data,0x20000,14,0x30000)
        struct.pack_into('<4I',mem.data,0x30000,1000147000,0,2,0x40000)
        struct.pack_into('<2I',mem.data,0x40000,44,50)
        event=captured_unix_return(mem,frame,0)
        self.assertEqual(event['pointer_samples'][str(0x40000)],struct.pack('<2I',44,50).hex())
        mem.put(0x30008,257)
        with self.assertRaisesRegex(ValueError,'array exceeds'):
            captured_unix_return(mem,frame,0)

    def test_completed_performance_counter(self):
        mem=Memory()
        mem.put(0x100,0x12345678); mem.put(0x104,0x200); mem.put(0x108,0x208)
        struct.pack_into('<qq',mem.data,0x200,1234567890123,10000000)
        event=captured_performance_counter(mem,0x100,0)
        self.assertEqual(event['counter'],1234567890123)
        self.assertEqual(event['frequency'],10000000)
        mem.put(0x108,0)
        self.assertNotIn('frequency',captured_performance_counter(mem,0x100,0))
        mem.put(0x104,0)
        self.assertNotIn('counter',captured_performance_counter(mem,0x100,0xc0000001))
        with self.assertRaisesRegex(ValueError,'no output pointer'):
            captured_performance_counter(mem,0x100,0)

    def test_present_mode_count_query_allows_null_output_array(self):
        mem=Memory(); frame,stack,params=0x100,0x200,0x10000
        mem.put(frame+8,0x7968eac0); mem.put(frame+12,stack+16)
        struct.pack_into('<4I',mem.data,stack,0x1234,0,569,params)
        mem.put(params,0x20000)
        struct.pack_into('<4I',mem.data,0x20000,1000274002,0,4,0)
        event=captured_unix_return(mem,frame,0)
        self.assertIn(str(0x20000),event['pointer_samples'])
        mem.put(0x2000c,0x30000)
        struct.pack_into('<4I',mem.data,0x30000,0,1,2,3)
        event=captured_unix_return(mem,frame,0)
        self.assertEqual(event['pointer_samples'][str(0x30000)],struct.pack('<4I',0,1,2,3).hex())

    def test_device_ranges_and_read_failure(self):
        maps = ('ca000000-ca400000 rw-s 0 00:00 0 /dev/nvidia0\n'
                'cb000000-cb001000 rw-p 0 00:00 0 /dev/nvidia0\n'
                'cc000000-cc001000 ---s 0 00:00 0 /dev/nvidia0\n'
                'cd000000-cd001000 rw-s 0 00:00 0 /other\n')
        records = device_memory_ranges(maps)
        self.assertEqual(len(records),4)
        self.assertEqual(sum(r['size'] for r in records),4*1024*1024)
        with self.assertRaisesRegex(RuntimeError,'out of bounds'):
            device_memory_state(Memory(),maps)
        with self.assertRaisesRegex(ValueError,'overlaps'):
            device_memory_ranges('70000000-70001000 rw-s 0 00:00 0 /dev/nvidia0')
        with self.assertRaisesRegex(ValueError,'384 MiB'):
            device_memory_ranges('80000000-a0000000 rw-s 0 00:00 0 /dev/nvidia0')

    def test_wine_shared_texture_buffer_roundtrip(self):
        maps = ('00010000-00012000 rwxs 00020000 00:01 61189 /memfd:wine-mapping (deleted)\n'
                '00020000-00021000 rw-p 0 00:01 1 /memfd:wine-mapping (deleted)\n'
                '00030000-00031000 rw-s 0 00:01 2 /memfd:unrelated (deleted)\n')
        mem = Memory()
        mem.put(0x10000, 0x12345678)
        records = device_memory_state(mem, maps)
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]['address'], 0x10000)
        self.assertEqual(zlib.decompress(base64.b64decode(records[0]['zlib_base64'])),
                         mem.read(0x10000, 0x2000))

    def test_initial_queue_and_partial_packed_grid(self):
        mem = Memory()
        result = runtime_state(mem, mem.game, [{'id': 1}], True)
        self.assertEqual([x['address'] for x in result['missions']], [0x60000, 0x61000])
        self.assertEqual(result['scan_slot_limit'], 2)
        self.assertEqual(result['entity_pool']['size'], 2 * STRIDE)
        self.assertEqual({x['name']: x['size'] for x in result['world_buffers']},
                         {'game_fields': 0x19f74, 'coarse_visibility': 32,
                          'map_cells': 896, 'occupancy': 640})
        self.assertTrue(result['controllers'][0]['known_size'])
        plane = result['grids'][0]
        self.assertEqual(zlib.decompress(base64.b64decode(plane['zlib_base64'])), bytes(range(64)))

    def test_followup_only_active_head_and_events(self):
        mem = Memory()
        mem.put(mem.unit + 0xd0, 0x2700)
        result = runtime_state(mem, mem.game, [{'id': 1}], False)
        self.assertEqual(len(result['missions']), 1)
        self.assertEqual(result['units'][0]['events'], 0x2700)
        self.assertFalse(result['grids'])

    def test_cycle_rejected(self):
        mem = Memory()
        mem.put(0x61000 + 0x66, 0x60000)
        with self.assertRaisesRegex(RuntimeError, 'cyclic'):
            runtime_state(mem, mem.game, [{'id': 1}], True)

    def test_oversize_plane_rejected_before_read(self):
        mem = Memory()
        mem.put(0x72340, 0xffffffff)
        with self.assertRaisesRegex(RuntimeError, 'bounded navigation'):
            runtime_state(mem, mem.game, [{'id': 1}], True)

    def test_unknown_controller_is_explicit(self):
        mem = Memory()
        mem.put(0x62000, 0x123456)
        controller = runtime_state(mem, mem.game, [{'id': 1}], True)['controllers'][0]
        self.assertFalse(controller['known_size'])
        self.assertIsNone(controller['fields_hex'])


class GameMemoryTests(unittest.TestCase):
    def test_native_images_include_writable_globals_and_paths_with_spaces(self):
        maps = '\n'.join((
            '797b0000-79d8c000 r-xp 0 00:00 1 /Proton Latest/d3d9.dll',
            '79d8c000-79d93000 rwxp 0 00:00 1 /Proton Latest/d3d9.dll',
            '79d93000-79ee3000 r--p 0 00:00 1 /Proton Latest/d3d9.dll',
            '00400000-00500000 r-xp 0 00:00 1 /game.exe',
            '80000000-80001000 rw-p 0 00:00 0',
            '81000000-81001000 rw-s 0 00:00 1 /shared.dll',
            '82000000-82001000 ---p 0 00:00 1 /reserved.dll',
            '100000000-100001000 r-xp 0 00:00 1 /64bit.dll'))
        records = native_memory_ranges(maps)
        self.assertTrue(any(r['address'] <= 0x79d90bd0 < r['address']+r['size']
                            and 'w' in r['permissions'] for r in records))
        self.assertEqual(sum(r['size'] for r in records), 0x79ee3000-0x797b0000)
        self.assertTrue(all(r['size'] <= 1024*1024 and r['module'].endswith('/d3d9.dll') for r in records))

    def test_native_reserved_emulator_space_rejected(self):
        for address in (0x60000000, 0x70000000, 0x71000000):
            with self.assertRaisesRegex(ValueError, 'overlaps'):
                native_memory_ranges(f'{address:x}-{address+4096:x} r-xp 0 00:00 1 /module.dll')
        with self.assertRaisesRegex(ValueError, '256 MiB'):
            native_memory_ranges('80000000-a0000000 r-xp 0 00:00 1 /large.dll')

    def test_native_capture_reads_globals_at_capture_time(self):
        class NativeMemory:
            value = 7
            def read(self, address, size): return bytes([self.value])*size
        mem = NativeMemory()
        maps = '79d90000-79d91000 rw-p 0 00:00 1 /d3d9.dll'
        before, = native_memory_state(mem, maps)
        mem.value = 9
        after, = native_memory_state(mem, maps)
        self.assertEqual(zlib.decompress(base64.b64decode(before['zlib_base64'])), bytes([7])*4096)
        self.assertEqual(zlib.decompress(base64.b64decode(after['zlib_base64'])), bytes([9])*4096)

    def test_crt_thread_selected_by_stack_and_tls_owner(self):
        mem = Memory()
        teb, thread = 0x90000, 0xa0000
        mem.put(0x629210, 23)
        for offset, value in ((4, 0x110000), (8, 0x100000), (0x18, teb),
                              (0x24, 356), (0xe10 + 23 * 4, thread)):
            mem.put(teb + offset, value)
        mem.put(thread, 356)
        mem.put(thread + 0x14, 0xb209a60f)
        records = game_memory_state(mem, '00090000-00091000 rw-p 00000000 00:00 0')
        found = captured_crt_thread(mem, records, 0x108000)
        self.assertEqual(found['address'], thread)
        self.assertEqual(found['seed'], 0xb209a60f)
        with self.assertRaisesRegex(RuntimeError, 'found 0'):
            captured_crt_thread(mem, records, 0x120000)
        mem.put(thread, 357)
        with self.assertRaisesRegex(RuntimeError, 'found 0'):
            captured_crt_thread(mem, records, 0x108000)

    def test_private_readable_low_ranges_only(self):
        maps = '\n'.join((
            '00000000-00020000 rw-p 00000000 00:00 0',
            '00100000-00301000 r--p 00000000 00:00 0',
            '00400000-00401000 rw-s 00000000 00:00 0',
            '00500000-00501000 ---p 00000000 00:00 0',
            '5ffff000-70000000 rw-p 00000000 00:00 0',
            'f0000000-f0010000 r--p 00000000 00:00 0'))
        self.assertEqual(game_memory_ranges(maps), [
            (0x10000, 0x10000), (0x100000, 0x100000),
            (0x200000, 0x100000), (0x300000, 0x1000), (0x5ffff000, 0x1000)])

    def test_empty_and_oversized_maps_rejected(self):
        for maps in ('', '00100000-50000000 rw-p 00000000 00:00 0'):
            with self.assertRaises(ValueError):
                game_memory_ranges(maps)

    def test_compressed_record_preserves_bytes(self):
        mem = Memory()
        record, = game_memory_state(mem, '00030000-00031000 rw-p 00000000 00:00 0')
        self.assertEqual(record['address'], 0x30000)
        self.assertEqual(record['size'], 4096)
        self.assertEqual(zlib.decompress(base64.b64decode(record['zlib_base64'])), mem.read(0x30000, 4096))


if __name__ == '__main__':
    unittest.main()
