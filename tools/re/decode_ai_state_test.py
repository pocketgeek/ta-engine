import base64
import struct
import unittest
import zlib
from decode_ai_state import initial_ai_state


class AiRestoreTest(unittest.TestCase):
    def fixture(self,change=lambda blobs:None):
        game,manager,groups,squad=0x100000,0x200000,0x300000,0x400000
        blobs={game:bytearray(0x18000),manager:bytearray(0x200),groups:bytearray(400),
               squad:bytearray(32),0x500000:bytearray(16),0x600000:bytearray(4),
               0x620000:bytearray(0x1000),0x62d558:bytearray(4),0x630000:bytearray(16),
               0x62dbcc:bytearray(28)}
        def put(base,offset,value):struct.pack_into('<I',blobs[base],offset,value)
        put(game,0x2404,1);put(game,0x2484,manager);put(game,0x2488,groups)
        blobs[game][0x2404+0xea]=2;blobs[game][0x2404+0xe3]=1
        put(game,0x175dc,0x620000)
        put(manager,0,game+0x2404);put(manager,5,29);put(manager,0x15,squad)
        put(squad,0,0x500000);put(0x500000,0,0x40b320)
        put(squad,4,manager);put(squad,8,groups+196);put(squad,12,10084)
        put(groups,196+4,1);put(groups,196+12,7)
        for off,value in [(0xb8,0x600000),(0xbc,0x600004),(0xc0,0x600004)]:put(groups,196+off,value)
        put(0x600000,0,0x700000)
        put(0x62d558,0,0x630000);put(0x630000,0,0x630004);blobs[0x630000][15]=1
        change(blobs)
        return {'runtime_state':{'world_buffers':[{'name':'game_fields','address':game}],
                                 'units':[{'id':123,'address':0x700000}]},
                'game_memory':[{'address':a,'size':len(b),
                                'zlib_base64':base64.b64encode(zlib.compress(b)).decode()} for a,b in blobs.items()]}

    def test_initial_schedule_and_members(self):
        state=initial_ai_state(self.fixture(),1)[0]
        self.assertEqual((state['countdown'],state['initialized'],state['scenario_deadline']),(29,0,0))
        self.assertEqual(state['groups'][0]['members'],[123])
        self.assertEqual(state['groups'][0]['parameters'][0],7)
        self.assertEqual(state['groups'][0]['deadline'],10084)

    def test_anchor_vectors_and_signed_references(self):
        def change(blobs):
            for offset,address in ((0,0x800000),(16,0x810000)):
                struct.pack_into('<3I',blobs[0x62dbcc],offset,address,address+202,address+202)
                blobs[address]=bytearray(202)
                struct.pack_into('<Bhh',blobs[address],101,1,123,-45)
        state=initial_ai_state(self.fixture(change),1)[0]
        self.assertEqual(state['anchors'],[[-2,123,-45],[2,123,-45]])

    def test_invalid_anchor_vector_rejected(self):
        frame=self.fixture(lambda b:struct.pack_into('<3I',b[0x62dbcc],0,0x800000,0x800001,0x800002))
        with self.assertRaisesRegex(ValueError,'anchor vector'):initial_ai_state(frame,1)

    def test_owner_rejected(self):
        frame=self.fixture(lambda b:struct.pack_into('<I',b[0x400000],4,0x200004))
        with self.assertRaisesRegex(ValueError,'ownership'):initial_ai_state(frame,1)

    def test_invalid_vector_rejected(self):
        frame=self.fixture(lambda b:struct.pack_into('<I',b[0x300000],196+0xbc,0x600005))
        with self.assertRaisesRegex(ValueError,'vector'):initial_ai_state(frame,1)

    def test_missing_member_and_unknown_handler_rejected(self):
        for address,value,pattern in [(0x600000,0x700100,'member'),(0x500000,0xdeadbeef,'handler')]:
            frame=self.fixture(lambda b:struct.pack_into('<I',b[address],0,value))
            with self.assertRaisesRegex(ValueError,pattern):initial_ai_state(frame,1)

    def test_human_or_missing_capture_does_not_invent_ai(self):
        frame=self.fixture(lambda b:b[0x100000].__setitem__(0x2404+0xea,1))
        self.assertEqual(initial_ai_state(frame,1),[None])
        self.assertIsNone(initial_ai_state({},1))


if __name__=='__main__':unittest.main()
