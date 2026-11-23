"""Recovery must reject invalid records before launching a debugger."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from probe_guard import recover_manifest, validate_manifest


class GuardTests(unittest.TestCase):
    def record(self):
        return dict(pid=123,start_time='456',resume=True,state='armed',
                    probes=[dict(address=0x1000,bytes='55'+'90'*15)],
                    hardware={'123':{str(i):0 for i in (0,1,2,3,6,7)}})

    def test_invalid_records_never_attach(self):
        cases=[]
        for key,value in [('pid',-1),('start_time',''),('resume','yes'),
                          ('state','unknown'),('probes',[]),('hardware',{}),
                          ('hardware',{'123':{'7':0}})]:
            m=self.record(); m[key]=value; cases.append(m)
        for key,value in [('address',0xffffffff),('bytes',''),('bytes','cc'+'90'*15)]:
            m=self.record(); m['probes'][0][key]=value; cases.append(m)
        m=self.record(); p=copy.deepcopy(m['probes'][0]); p['address']+=1
        m['probes'].append(p); cases.append(m)
        with tempfile.TemporaryDirectory() as directory, patch('probe_guard.subprocess.run') as run:
            path=Path(directory)/'manifest.json'
            for m in cases:
                with self.subTest(record=m):
                    path.write_text(json.dumps(m))
                    with self.assertRaises(ValueError):
                        recover_manifest(path,Path(directory)/'log')
            run.assert_not_called()

    def test_reused_pid_never_attaches(self):
        with tempfile.TemporaryDirectory() as directory, patch('probe_guard.subprocess.run') as run:
            path=Path(directory)/'manifest.json'; path.write_text(json.dumps(self.record()))
            with patch('probe_guard.process_start_time',return_value='457'):
                with self.assertRaisesRegex(RuntimeError,'identity changed'):
                    recover_manifest(path,Path(directory)/'log')
            run.assert_not_called()

    def test_valid_record(self):
        self.assertEqual(validate_manifest(self.record()),self.record())


if __name__=='__main__': unittest.main()
