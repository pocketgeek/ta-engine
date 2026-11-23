import unittest

from balance_inputs import feature_record_inputs, properties, unit_properties


class UnitPropertiesTest(unittest.TestCase):
    def test_weapon_fields_do_not_replace_motion_inputs(self):
        fields=unit_properties('''[UNITINFO] { turnrate=2300; brakerate=10; }
                                  [WEAPON2] { turnrate=180; range=400; }''')
        self.assertEqual(fields,{'turnrate':'2300','brakerate':'10'})

    def test_nested_fields_and_comments_are_not_unit_inputs(self):
        fields=unit_properties('''// [UNITINFO] { turnrate=0; }
            [UnitInfo] { turnrate=500; [unused] { turnrate=12; }
                         maxvelocity=1.5; }''')
        self.assertEqual(fields,{'turnrate':'500','maxvelocity':'1.5'})

    def test_missing_or_unterminated_section_is_rejected(self):
        for source in ('[WEAPON1] { turnrate=5; }','[UNITINFO] { turnrate=5;'):
            with self.assertRaises(ValueError): unit_properties(source)


class FeatureRecordInputsTest(unittest.TestCase):
    def test_nonblocking_model_corpse(self):
        fields=properties('''animatable=1; blocking=0; footprintx=2; footprintz=2;
            height=0; object=tarzom_dead; reclaimable=1; resurrectable=1;''')
        # Model feature (bit 0 clear), reclaimable, default autoreclaimable,
        # resurrectable, animatable; destructible -> 0x20000; not blocking.
        self.assertEqual(feature_record_inputs(fields),(2,2,0,0.0,0x40|0x80|0x800|0x1000|0x20000))

    def test_sprite_blocking_indestructible(self):
        fields=properties('blocking=1; indestructible=1; autoreclaimable=0; footprintx=3; footprintz=1; height=20;')
        self.assertEqual(feature_record_inputs(fields),(3,1,20,0.0,0x1|0x20|0x100))

    def test_replacement_chain_is_not_synthesized(self):
        with self.assertRaises(ValueError):
            feature_record_inputs(properties('object=x; featuredead=y_dead;'))


if __name__=='__main__': unittest.main()
