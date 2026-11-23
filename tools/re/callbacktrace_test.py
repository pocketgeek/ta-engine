import copy
import unittest
from callbacktrace import native_call_tree, summary


class CallbackTreeTest(unittest.TestCase):
    def fixture(self):
        entry={'kind':'syscall','stub_return':0x1000,'number':2,'return_address':0x2000,
               'stack':0x3000,'arguments':[4,5],'tick':10}
        child=dict(entry,number=3,stub_return=0x1004)
        return {'native_entry_sites':[1,2],'native_entries':[entry,child],
                'syscall_calls':[dict(child,result=0),dict(entry,result=1)],
                'user_callbacks':[{'phase':'entry','number':4,'tick':10},
                                  {'phase':'return','status':0,'tick':10}],
                'native_order':[{'kind':k,'index':i} for k,i in
                    [('entry',0),('callback',0),('entry',1),('syscall',0),('callback',1),('syscall',1)]]}

    def test_explicit_nested_parent(self):
        roots=native_call_tree(self.fixture())
        self.assertEqual(summary(roots),{'roots':1,'calls':{'syscall':2,'callback':1},
                                       'max_depth':3,'callback_parents':{'syscall':1}})
        self.assertEqual(roots[0]['children'][0]['children'][0]['return']['index'],0)

    def test_rejects_return_only_and_misnested_or_missing_events(self):
        c=self.fixture(); del c['native_entry_sites']
        with self.assertRaisesRegex(ValueError,'entry records'): native_call_tree(c)
        c=self.fixture(); c['native_order'][3],c['native_order'][4]=c['native_order'][4],c['native_order'][3]
        with self.assertRaisesRegex(ValueError,'nesting'): native_call_tree(c)
        c=self.fixture(); c['native_order'].pop()
        with self.assertRaisesRegex(ValueError,'remain open'): native_call_tree(c)
        c=self.fixture(); c['syscall_calls'][0]['arguments']=[999]
        with self.assertRaisesRegex(ValueError,'arguments'): native_call_tree(c)
        c=self.fixture(); c['syscall_calls'].append(copy.deepcopy(c['syscall_calls'][0]))
        with self.assertRaisesRegex(ValueError,'missing from'): native_call_tree(c)

    def test_explicit_dispatcher_restart_preserves_one_call(self):
        c=self.fixture()
        c['native_entries'].append(dict(c['native_entries'][0],phase='restart',restarts_entry=0))
        c['native_order'].insert(1,{'kind':'entry','index':2})
        roots=native_call_tree(c)
        self.assertEqual(summary(roots)['calls']['syscall'],2)
        self.assertEqual(roots[0]['restarts'],[{'kind':'entry','index':2}])
        c['native_entries'][2]['arguments']=[6]
        with self.assertRaisesRegex(ValueError,'restart arguments'): native_call_tree(c)


if __name__=='__main__': unittest.main()
