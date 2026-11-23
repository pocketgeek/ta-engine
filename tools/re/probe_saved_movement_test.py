"""Comparison contract: subpixel differences and timeline gaps must stay visible."""
import copy
import unittest
import struct
import base64
import zlib

from probe_saved_movement import (compare_frames, compare_rng, movement_goal,
                                 ground_mission_state, standby_mission_state, vtol_standby_state,
                                 captured_type_name, factory_unit_values, initial_player_caches,
                                 compare_selected_units, initial_wind, initial_sector_links, initial_crusades, initial_exploration)


class ComparisonTest(unittest.TestCase):
    def test_exploration_restore_keeps_owner_masks_viewer_and_sight_cache(self):
        game,visibility,unit=0x100000,0x200000,0x300000
        fields=bytearray(0x19f00);body=bytearray(312)
        struct.pack_into('<2I',fields,0x19e98,4,6)
        struct.pack_into('<I',fields,0x19ef4,visibility);fields[0x306f]=3
        cache=(-3,4,123456,-1,2,1)
        struct.pack_into('<hhihBB',body,0x98,*cache)
        masks=(0,1,2,8,0x8000,0xffff)
        def region(address,data):
            return {'address':address,'size':len(data),
                    'zlib_base64':base64.b64encode(zlib.compress(data)).decode()}
        frame={'units':[{'id':7}],
            'runtime_state':{'world_buffers':[{'name':'game_fields','address':game}],
                             'units':[{'id':7,'address':unit}]},
            'game_memory':[region(game,fields),region(visibility,struct.pack('<6H',*masks)),region(unit,body)]}
        self.assertEqual(initial_exploration(frame),(2,3,3,masks,[(7,*cache)]))
        frame['game_memory'].pop(1)
        with self.assertRaises(ValueError): initial_exploration(frame)

    def test_roster_selection_preserves_standard_and_crusades(self):
        for value in (0,1):
            frame={'game_memory':[{'address':0x641144,'size':1,
                'zlib_base64':base64.b64encode(zlib.compress(bytes([value]))).decode()}]}
            self.assertEqual(initial_crusades(frame),bool(value))
        self.assertFalse(initial_crusades({}))

    def test_sector_restore_uses_cached_link_and_rejects_invalid_addresses(self):
        fields=bytearray(0x19f20);pool=bytearray(312)
        struct.pack_into('<II',fields,0x19f18,0x200000,80)
        struct.pack_into('<I',pool,0xa4,0x200000+(54*80+64)*10)
        def region(address,data):
            return {'address':address,'size':len(data),
                    'zlib_base64':base64.b64encode(zlib.compress(data)).decode()}
        runtime={'world_buffers':[dict(region(0x100000,fields),name='game_fields')],
                 'entity_pool':region(0x300000,pool),'units':[{'id':821,'address':0x300000}]}
        self.assertEqual(initial_sector_links(runtime),[(821,64,54)])
        struct.pack_into('<I',pool,0xa4,0x200001)
        runtime['entity_pool']=region(0x300000,pool)
        with self.assertRaises(ValueError): initial_sector_links(runtime)
        runtime['units'][0]['address']-=1
        with self.assertRaises(ValueError): initial_sector_links(runtime)

    def test_player_cache_restore_uses_initial_global_table_and_validates_owner(self):
        game,manager=0x100000,0x200000
        fields=bytearray(0x2600)
        fields[0x2404+0xea]=1;fields[0x2404+0xeb]=0
        state=bytearray(0x105)
        struct.pack_into('<I',state,0,game+0x2404)
        struct.pack_into('<I',state,0x101,10069)
        def region(address,data):
            return {'address':address,'size':len(data),
                    'zlib_base64':base64.b64encode(zlib.compress(data)).decode()}
        frame={'runtime_state':{'world_buffers':[{'name':'game_fields','address':game}]},
               'game_memory':[region(0x62a33c,struct.pack('<2I',manager,0)),region(game,fields),
                              region(manager,state[:0x100]),region(manager+0x100,state[0x100:])]}
        self.assertEqual(initial_player_caches(frame,2),[[1,10069],[0,0]])
        frame['game_memory'].pop()
        self.assertIsNone(initial_player_caches(frame,2))
        frame['game_memory'].append(region(manager+0x100,state[0x100:]))
        state[4]=1
        frame['game_memory'][2]=region(manager,state[:0x100])
        with self.assertRaises(ValueError): initial_player_caches(frame,2)
        self.assertIsNone(initial_player_caches({'runtime_state':frame['runtime_state']},2))

    def test_factory_handshake_flags_are_separate_from_script_threads(self):
        data=bytearray(0x270)
        data[0x114]=0xfe
        data[0x12f]=0x0d
        struct.pack_into('<I',data,0x100,5)
        data[0x244]=1
        data[0x25f]=2
        pool={'address':4096,'size':len(data),
              'zlib_base64':base64.b64encode(zlib.compress(data)).decode()}
        runtime={'entity_pool':pool,'units':[{'id':540,'address':4096},
                                           {'id':541,'address':4096+0x130}]}
        self.assertEqual(factory_unit_values(runtime),{540:[0,1,1,1,5],541:[1,0,0,0,0]})
        runtime['units'][0]['address']-=1
        with self.assertRaises(ValueError): factory_unit_values(runtime)
        runtime['units'][0]['address']+=1
        pool['size']+=1
        with self.assertRaises(ValueError): factory_unit_values(runtime)

    def test_vtol_restore_preserves_ground_mode_and_subpixel_altitude(self):
        fields=bytearray(0x6e)
        fields[5]=2
        for offset,value in ((6,0x21),(10,10073),(0x6a,4),(0x5a,0x80000)):
            struct.pack_into('<I',fields,offset,value)
        mission={'address':100,'handler':0x417350,'fields_hex':fields.hex()}
        runtime={'units':[{'id':308,'primary':100,'events':8}],'missions':[mission]}
        actual={'flags':0x80000002,'position_raw':[1,4196444,3]}
        self.assertEqual(vtol_standby_state(runtime,308,actual),
                         [2,0x21,10073,4,0x80000,2,8,4196444])
        fields[5]=3
        mission['fields_hex']=fields.hex()
        self.assertIsNone(vtol_standby_state(runtime,308,actual))
        self.assertIsNone(vtol_standby_state(runtime,309,actual))

    def test_queued_type_can_be_absent_from_existing_unit_prefixes(self):
        table,index=4096,31
        data=bytearray(676*(index+1))
        offset=676*index+32
        data[offset:offset+8]=b'VERPULT\0'
        region={'address':table,'size':len(data),
                'zlib_base64':base64.b64encode(zlib.compress(data)).decode()}
        frame={'runtime_state':{'type_prefixes':[]},'game_memory':[region]}
        self.assertEqual(captured_type_name(frame,table,index),'VERPULT')
        with self.assertRaises(ValueError):
            captured_type_name(frame,table,index+1)
        del frame['game_memory']
        with self.assertRaises(ValueError):
            captured_type_name(frame,table,index)

    def test_wind_requires_the_same_initial_boundary_and_crt_seed(self):
        data=bytearray(0x19f74)
        struct.pack_into('<ii',data,0x19ec4,25,5000)
        struct.pack_into('<I',data,0x19f58,99)
        struct.pack_into('<4iHH',data,0x19f60,-10,0,20,30,65535,2)
        region={'name':'game_fields','address':4096,'size':len(data),
                'zlib_base64':base64.b64encode(zlib.compress(data)).decode()}
        frame={'runtime_state':{'world_buffers':[region]},'crt_seed':123}
        self.assertEqual(initial_wind(frame),[25,5000,99,-10,20,30,65535,2,123])
        del frame['crt_seed']
        self.assertIsNone(initial_wind(frame))
        frame['crt_seed']=123
        region['size']=0x19f48
        self.assertIsNone(initial_wind(frame))

    def test_standby_restore_rejects_unknown_stages(self):
        fields=bytearray(0x6e)
        fields[5]=1
        for offset,value in ((6,0x21),(10,10070),(0x6a,4),(0x5a,0x80000)):
            struct.pack_into('<I',fields,offset,value)
        mission={'address':100,'handler':0x407770,'fields_hex':fields.hex()}
        runtime={'units':[{'id':49,'primary':100,'events':8}],'missions':[mission]}
        self.assertEqual(standby_mission_state(runtime,49),[1,0x21,10070,4,0x80000,0,8])
        fields[5]=2
        mission['fields_hex']=fields.hex()
        self.assertIsNone(standby_mission_state(runtime,49))

    def test_captured_mission_fields_and_unsupported_modes(self):
        fields = bytearray(0x6e)
        fields[5] = 2
        values = {6: 0x2701, 10: 10076, 0x6a: 0x2000, 0x5a: 0x8000000, 0x4e: 64}
        for offset, value in values.items():
            struct.pack_into('<I', fields, offset, value)
        mission = {'address': 100, 'handler': 0x402b00, 'fields_hex': fields.hex()}
        runtime = {'units': [{'id': 53, 'primary': 100, 'events': 7}], 'missions': [mission]}
        self.assertEqual(ground_mission_state(runtime, 53), [2, 0x2701, 10076, 0x2000, 0x8000000, 64, 7,0,0,0,0,0])
        struct.pack_into('<i', fields, 0x52, -1)
        struct.pack_into('<2h',fields,0x2e,-123,32767)
        struct.pack_into('<h',fields,0x24,-32768)
        struct.pack_into('<h',fields,0x2c,99)
        fields[5]=3
        mission['fields_hex'] = fields.hex()
        self.assertEqual(ground_mission_state(runtime,53),[3,0x2701,10076,0x2000,0x8000000,64,7,-1,-123,32767,-32768,99])
        fields[5]=4
        mission['fields_hex']=fields.hex()
        self.assertIsNone(ground_mission_state(runtime, 53))
        self.assertIsNone(ground_mission_state(runtime, 40))
        mission['fields_hex'] = '00'
        with self.assertRaises(ValueError):
            ground_mission_state(runtime, 53)

    def setUp(self):
        self.retail = [{'tick': 42, 'units': [{'id': 17, 'position_raw': [123456789, 0, -1],
                                             'heading': 65535, 'speed_raw': 123, 'base_speed_raw': 456}]}]
        self.port = [{'tick': 42, 'units': [{'id': 17, 'x_raw': 123456789, 'z_raw': -1,
                                           'heading': 65535, 'speed_raw': 123, 'base_speed_raw': 456}]}]

    def test_raw_subpixel_difference(self):
        self.assertIsNone(compare_frames(self.retail, self.port))
        self.port[0]['units'][0]['x_raw'] += 1
        self.assertEqual(compare_frames(self.retail, self.port),
                         {'tick': 42, 'id': 17, 'field': 'x_raw', 'retail': 123456789, 'port': 123456790})

    def test_flight_comparison_includes_raw_altitude(self):
        self.port[0]['units'][0]['y_raw'] = 0
        self.assertIsNone(compare_selected_units(self.retail, self.port, [17], flight_height=True))
        self.port[0]['units'][0]['y_raw'] = 1
        self.assertEqual(compare_selected_units(self.retail, self.port, [17], flight_height=True),
                         {'tick': 42, 'id': 17, 'field': 'y_raw', 'retail': 0, 'port': 1})
        self.assertIsNone(compare_frames(self.retail, self.port))

    def test_scoped_diagnostic_keeps_timeline_and_presence_checks(self):
        self.assertIsNone(compare_selected_units(self.retail, self.port, [17]))
        self.port[0]['tick'] += 1
        self.assertEqual(compare_selected_units(self.retail, self.port, [17])['field'], 'tick')
        self.port[0]['tick'] -= 1
        self.port[0]['units'].clear()
        self.assertEqual(compare_selected_units(self.retail, self.port, [17])['ids'], [17])
        with self.assertRaises(ValueError):
            compare_selected_units(self.retail, self.port, [])

    def test_tick_and_presence_are_not_silently_aligned(self):
        self.port[0]['tick'] += 1
        self.assertEqual(compare_frames(self.retail, self.port)['field'], 'tick')
        self.port[0]['tick'] -= 1
        self.port[0]['units'] = []
        self.assertEqual(compare_frames(self.retail, self.port)['retail_only'], [17])
        self.assertEqual(compare_frames(self.retail, [])['field'], 'frame_count')

    def test_rng_consumption_and_order(self):
        calls = [{'tick': 42, 'bound': 5, 'seed_before': 100},
                 {'tick': 42, 'bound': 7, 'seed_before': 200}]
        self.assertIsNone(compare_rng(calls, copy.deepcopy(calls)))
        self.assertEqual(compare_rng(calls, list(reversed(calls)))['index'], 0)
        self.assertEqual(compare_rng(calls, calls[:1])['field'], 'call_count')

    def test_navigation_goal_uses_footprint_origin_not_click(self):
        unit = {'footprint_size': [2, 2]}
        order = {'controller_kind': 4, 'controller_runtime_fields': {'08': (36 << 16) | 135}}
        self.assertEqual(movement_goal(unit, order), (2176 * 65536, 592 * 65536))
        order['controller_runtime_fields']['08'] = 0xffffffff
        self.assertEqual(movement_goal(unit, order), (0, 0))
        order['controller_kind'] = 2
        with self.assertRaises(ValueError):
            movement_goal(unit, order)


if __name__ == '__main__':
    unittest.main()
