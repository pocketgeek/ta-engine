"""Synthetic archived data exercises VFS/TDF balance comparison; no game assets."""
import csv
import io
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
AUDIT = ROOT / 'build/crusades_balance_audit'
HPI = ROOT / 'build/hpitool'

@unittest.skipUnless(AUDIT.exists() and HPI.exists(), 'Build crusades_balance_audit and hpitool first')
class BalanceAuditTests(unittest.TestCase):
    def run_audit(self, files, *options):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'input'
            source.mkdir()
            for path, text in files.items():
                p = source / path
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(text.replace(";", ";\n"))
            data = root / 'data'
            data.mkdir()
            subprocess.run([str(HPI), 'pack', str(source), str(data/'data.hpi')],
                           check=True, capture_output=True)
            return subprocess.run([str(AUDIT), str(data), *options], text=True, capture_output=True)

    def test_nested_diff_and_prose_redaction(self):
        result = self.run_audit({
            'units/example.fbi': '[UNITINFO]{ObjectName=example;Name=Secret old name;MaxDamage=10;}[WEAPON1]{[DAMAGE]{Default=5;Fort=2;}}',
            'unitscb/example.fbi': '[UNITINFO]{ObjectName=example;Name=Secret new name;MaxDamage=20;}[WEAPON1]{[DAMAGE]{Default=7;}}',
        })
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('Secret', result.stdout)
        rows = {r['field']: r for r in csv.DictReader(io.StringIO(result.stdout), delimiter='\t')}
        self.assertEqual(rows['unitinfo#1/maxdamage']['crusades'], '20')
        self.assertEqual(rows['weapon1#1/damage#1/fort']['crusades'], '<absent>')
        self.assertEqual(rows['weapon1#1/damage#1/default']['standard'], '5')
        self.assertNotIn('unitinfo#1/objectname', rows)

    def test_roster_removal_and_menus_ignored(self):
        # Retail never reads canbuildcb/, and a unit without a unitscb FBI does not
        # exist in Crusades: the audit reports the removal and no menu rows.
        result = self.run_audit({
            'units/baseonly.fbi': '[UNITINFO]{ObjectName=baseonly;MaxDamage=10;}',
            'units/kept.fbi': '[UNITINFO]{ObjectName=kept;MaxDamage=10;}',
            'unitscb/kept.fbi': '[UNITINFO]{ObjectName=kept;MaxDamage=10;}',
            'canbuild/builder/old.tdf': '',
            'canbuildcb/builder/new.tdf': '',
            'canbuild/other/retained.tdf': '[MENU]{Priority=1;}',
        })
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = list(csv.DictReader(io.StringIO(result.stdout), delimiter='\t'))
        self.assertEqual({(r['path'],r['kind']) for r in rows}, {
            ('units/baseonly.fbi','removed-definition')})

    def test_repeated_sections_remain_distinct(self):
        result = self.run_audit({
            'units/test.fbi': '[EFFECT]{Value=1;}[EFFECT]{Value=2;}',
            'unitscb/test.fbi': '[EFFECT]{Value=3;}[EFFECT]{Value=2;}',
        })
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = list(csv.DictReader(io.StringIO(result.stdout), delimiter='\t'))
        self.assertEqual(len(rows),1)
        self.assertEqual(rows[0]['field'],'effect#1/value')

    def test_manifest_records_decoded_bytes_and_archive_source(self):
        text = '[UNITINFO]{MaxDamage=10;}'
        result = self.run_audit({'units/example.fbi': text}, '--manifest')
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = list(csv.DictReader(io.StringIO(result.stdout), delimiter='\t'))
        archive = next(r for r in rows if r['kind'] == 'archive')
        member = next(r for r in rows if r['kind'] == 'member')
        self.assertEqual(archive['path'], 'data.hpi')
        self.assertEqual(member['path'].lower(), 'units/example.fbi')
        self.assertTrue(member['source'].startswith('data.hpi'))
        encoded = text.replace(';', ';\n').encode()
        self.assertEqual(int(member['size']), len(encoded))
        self.assertEqual(member['sha256'], hashlib.sha256(encoded).hexdigest())

    def test_absent_overlay_fails(self):
        result = self.run_audit({'units/test.fbi': '[UNITINFO]{MaxDamage=1;}'})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('No Crusades definitions', result.stderr)

if __name__ == '__main__':
    unittest.main()
