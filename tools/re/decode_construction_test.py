import base64
import struct
import unittest
import zlib
from decode_construction import initial_construction


class ConstructionRestoreTest(unittest.TestCase):
    def fixture(self, mutate=lambda b,m: None):
        base=0x10000; blob=bytearray(0x2000); mission=bytearray(0x70)
        def word(offset,value): struct.pack_into('<I',blob,offset,value)
        word(0xb4,base+0x400);word(0xc0,base+0x800);blob[0x114]=8
        word(0x200+0xb4,base+0x400);word(0x200+0x10c,50)
        struct.pack_into('<f',blob,0x200+0x108,0.5)
        struct.pack_into('<f',blob,0x400+0x216,0.01)
        struct.pack_into('<f',blob,0x400+0x20e,100)
        struct.pack_into('<f',blob,0x400+0x21a,10)
        word(0x400+0x1be,100)
        word(0x800+0x174,base+0xa00)
        struct.pack_into('<6I',blob,0xa08,base+0xb00,0,5,base,65536,65536)
        word(0xb00,base+0xb00)
        mission[5]=3
        struct.pack_into('<I',mission,0xe,base)
        struct.pack_into('<I',mission,0x16,base+0x200)
        struct.pack_into('<I',mission,0x52,1)
        mutate(blob,mission)
        return {'runtime_state':{'units':[{'id':1,'address':base,'primary':base+0x1000},
                                         {'id':2,'address':base+0x200,'primary':0}],
                                 'missions':[{'address':base+0x1000,'handler':0x405560,
                                              'fields_hex':mission.hex()}]},
                'game_memory':[{'address':base,'size':len(blob),
                                'zlib_base64':base64.b64encode(zlib.compress(blob)).decode()}]}

    def test_initial_job_site_and_empty_emitter(self):
        state=initial_construction(self.fixture())
        self.assertEqual(state['sites'][0]['remaining'],0.5)
        self.assertEqual(state['sites'][0]['hp'],50)
        self.assertEqual(state['builders'][0]['target'],2)
        self.assertTrue(state['builders'][0]['working'])
        self.assertEqual(state['emitters'][0]['particles'],[])
        self.assertIsNone(initial_construction({}))

    def test_flying_work_keeps_distinct_stage_and_flags(self):
        frame=self.fixture(lambda b,m:(m.__setitem__(5,5),struct.pack_into('<I',b,0x130,0xc)))
        frame['runtime_state']['missions'][0]['handler']=0x41ef00
        state=initial_construction(frame)
        self.assertEqual(state['builders'],[])
        job=state['flying_builders'][0]
        self.assertEqual((job['target'],job['stage'],job['owner_flags']),(2,5,0xc))

    def test_flying_work_rejects_missing_site(self):
        frame=self.fixture(lambda b,m:(m.__setitem__(5,6),struct.pack_into('<I',m,0x16,0x90000)))
        frame['runtime_state']['missions'][0]['handler']=0x41ef00
        with self.assertRaisesRegex(ValueError,'owner/target'):initial_construction(frame)

    def waiting_fixture(self):
        frame=self.fixture()
        mission=bytearray(0x70);mission[5]=2
        struct.pack_into('<I',mission,0xe,0x10200)
        struct.pack_into('<I',mission,0x16,0x10000)
        struct.pack_into('<II',mission,6,0x10000001,42)
        frame['runtime_state']['units'][1]['primary']=0x11070
        frame['runtime_state']['missions'].append({'address':0x11070,'handler':0x402220,'fields_hex':mission.hex()})
        return frame

    def test_get_built_waiting_state(self):
        state=initial_construction(self.waiting_fixture())['waiting'][0]
        self.assertEqual((state['id'],state['builder'],state['stage'],state['wait'],state['deadline']),
                         (2,1,2,0x10000001,42))

    def test_get_built_wrong_builder_rejected(self):
        frame=self.waiting_fixture();entry=frame['runtime_state']['missions'][-1]
        data=bytearray.fromhex(entry['fields_hex']);struct.pack_into('<I',data,0x16,0x90000)
        entry['fields_hex']=data.hex()
        with self.assertRaisesRegex(ValueError,'GetBuilt owner/builder'):initial_construction(frame)

    def test_get_built_invalid_stage_rejected(self):
        frame=self.waiting_fixture();entry=frame['runtime_state']['missions'][-1]
        data=bytearray.fromhex(entry['fields_hex']);data[5]=3;entry['fields_hex']=data.hex()
        with self.assertRaisesRegex(ValueError,'GetBuilt waiting'):initial_construction(frame)

    def test_missing_target_rejected(self):
        frame=self.fixture(lambda b,m:struct.pack_into('<I',m,0x16,0x90000))
        with self.assertRaisesRegex(ValueError,'owner/target'): initial_construction(frame)

    def test_completed_target_rejected(self):
        frame=self.fixture(lambda b,m:struct.pack_into('<f',b,0x308,0))
        with self.assertRaisesRegex(ValueError,'complete'): initial_construction(frame)

    def test_nonfinite_progress_rejected(self):
        frame=self.fixture(lambda b,m:struct.pack_into('<f',b,0x308,float('nan')))
        with self.assertRaisesRegex(ValueError,'remainder'): initial_construction(frame)

    def test_particle_count_disagreement_rejected(self):
        frame=self.fixture(lambda b,m:struct.pack_into('<I',b,0xa0c,1))
        with self.assertRaisesRegex(ValueError,'count disagrees'): initial_construction(frame)

    def test_particle_cycle_rejected(self):
        def mutate(blob,mission):
            struct.pack_into('<I',blob,0xa0c,2)
            struct.pack_into('<I',blob,0xb00,0x10c00)
            struct.pack_into('<I',blob,0xc00,0x10c00)
            struct.pack_into('<I',blob,0xc08,0x10000)
        with self.assertRaisesRegex(ValueError,'particle list'): initial_construction(self.fixture(mutate))


if __name__=='__main__': unittest.main()
