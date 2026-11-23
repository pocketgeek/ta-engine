"""Synthetic compression fixtures; no retail data required."""
import struct
import unittest

from inspect_save import directory, lzss, sections
from decode_save_state import order_record, unit_record, script_record
from capture_save_baseline import compare


class SaveTest(unittest.TestCase):
    def test_script_restore_clears_transient_pointers(self):
        words=list(range((0xa48+4*4+2*0x6c)//4))
        record=struct.pack(f'<{len(words)}I',*words)
        state=script_record(record,4,2)
        self.assertEqual(len(state['threads']),16)
        for index,thread in enumerate(state['threads']):
            self.assertEqual(thread['04'],2+41*index)
            self.assertEqual(thread['20'],0)
            self.assertEqual(thread['a0'],41+41*index)
        self.assertEqual(state['runtime_a60'],0xa44//4)
        self.assertEqual(state['statics'],list(range(0xa48//4,0xa48//4+4)))
        self.assertEqual(state['pieces'][1]['68'],len(words)-1)
        for blob,statics,pieces in ((record[:-1],4,2),(record,3,2),(record,4,3),
                                    (record,-1,2),(record,None,2),(bytes(0xa44),None,None)):
            with self.subTest(statics=statics,pieces=pieces),self.assertRaises(ValueError):
                script_record(blob,statics,pieces)

    # Two literals then a four-byte overlapping reference to window index 1,
    # followed by a terminator and the padding byte present in retail chunks.
    payload = b'\x0cAB\x12\x00\x00\x00\x00'

    def test_overlapping_reference(self):
        self.assertEqual(lzss(self.payload, 6), b'ABABAB')

    def test_section_checksum(self):
        chunk = b'SQSH' + struct.pack('<BBBIII', 2, 1, 0, len(self.payload), 6, sum(self.payload)) + self.payload
        self.assertEqual(list(sections(b'HAPIBANK' + chunk)), [(8, b'ABABAB')])
        with self.assertRaisesRegex(ValueError, 'checksum'):
            list(sections(b'HAPIBANK' + chunk[:-1] + b'\x01'))

    def test_invalid_streams(self):
        for data, expected in ((b'\x00', 1), (b'\x01\x12', 4),
                               (self.payload, 5), (self.payload, 7), (b'', 0)):
            with self.subTest(data=data, expected=expected), self.assertRaises(ValueError):
                lzss(data, expected)

    def bank_fixture(self):
        names = b'Node\0Count\0Ratio\0Map\0Arena\0Blob\0'
        fields = struct.pack('<IiIdII', 5, -7, 11, 1.25, 17, 21)
        payload_offset = len(fields) + 32
        blobs = struct.pack('<8I', 27, 34, 34 + 32 + payload_offset, 3,
                            0xffffffff, 9, 34 + 32 + payload_offset + 3, 2)
        body = fields + blobs + b'abcde'
        node = struct.pack('<8I', 32 + len(body), 0, 1, 1, 1, 2, 0, 0) + body
        end = 34 + len(node)
        header = b'HAPIBANK' + struct.pack('<III', 0, end, 34) + bytes(14)
        return header + node + b'X', {end: names}

    def test_directory_associations(self):
        data, chunks = self.bank_fixture()
        node, = directory(data, chunks)
        self.assertEqual(node['integers'], [{'name': 'Count', 'value': -7}])
        self.assertEqual(node['doubles'], [{'name': 'Ratio', 'value': 1.25}])
        self.assertEqual(node['strings'], [{'name': 'Map', 'value': 'Arena'}])
        self.assertEqual([b['name'] for b in node['blobs']], ['Blob', None])
        self.assertEqual([b['size'] for b in node['blobs']], [3, 2])

    def test_directory_rejects_invalid_references(self):
        data, chunks = self.bank_fixture()
        # Bad name, inflated field count, and a physical/virtual blob offset mixup.
        for offset, value in ((38, 1), (42, 9999), (102, 1)):
            damaged = bytearray(data)
            struct.pack_into('<I', damaged, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                directory(damaged, chunks)

    def test_unit_record_packed_fields(self):
        # Sentinel bytes expose accidental aligned reads in this packed format.
        record = bytearray([0xa5] * 235)
        record[:32] = b'hunter\0' + bytes(25)
        record[0x20] = 3
        struct.pack_into('<HIIiii', record, 0x21, 421, 2, 1, -65537, 64 * 65536, 900 * 65536)
        struct.pack_into('<H', record, 0x39, 50000)
        struct.pack_into('<hhhh', record, 0x83, -1, 56, 2, 3)
        struct.pack_into('<i', record, 0xa3, 117964)
        unit = unit_record(record)
        self.assertEqual((unit['id'], unit['player'], unit['order_count']), (421, 3, 2))
        self.assertEqual(unit['position_raw'], [-65537, 64 * 65536, 900 * 65536])
        self.assertEqual(unit['heading'], 50000)
        self.assertEqual(unit['footprint_origin'], [-1, 56])
        self.assertEqual(unit['footprint_size'], [2, 3])
        self.assertEqual(unit['base_speed_raw'], 117964)
        with self.assertRaises(ValueError):
            unit_record(record[:-1])

    def test_order_record_packed_fields(self):
        record = bytes(range(82))
        order = order_record(record)
        self.assertEqual(order['owner_id'], 256)
        self.assertEqual(order['target_id'], 770)
        self.assertEqual(order['saved_type_byte'], 8)
        self.assertNotIn('04', order['runtime_fields'])
        self.assertEqual(order['runtime_fields']['05'], 9)
        self.assertEqual(order['runtime_fields']['22'], 0x15141312)
        self.assertEqual(order['runtime_fields']['5a'], 0x4d4c4b4a)
        self.assertEqual(order['runtime_fields']['6a'], 0x51504f4e)
        with self.assertRaises(ValueError):
            order_record(record[:-1])

    def test_baseline_comparison_never_aligns_different_ticks(self):
        saved = {'tick': 100, 'units': []}
        self.assertEqual(compare(saved, {'tick': 100, 'units': []}), [])
        self.assertEqual(compare(saved, {'tick': 101, 'units': []}),
                         [{'field': 'tick', 'saved': 100, 'live': 101}])
        self.assertEqual(compare(saved, {'tick': 100, 'units': [{'id': 7}]}),
                         [{'id': 7, 'field': 'presence', 'saved': False, 'live': True}])


if __name__ == '__main__':
    unittest.main()
