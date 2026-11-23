import copy
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import crusades_inventory as ci


class InventoryTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / 'source'
        self.root.mkdir()

    def put(self, name, data=b'test'):
        p = self.root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        return p

    def test_hash_and_no_content(self):
        self.put('Folder/é.fbi', b'private original content')
        inv = ci.inventory(self.root, 'fixture')
        e = inv['entries'][0]
        self.assertEqual(e['sha256'], hashlib.sha256(b'private original content').hexdigest())
        self.assertEqual(e['size'], 24)
        self.assertEqual(e['parser_candidates'], ['tdftool'])
        self.assertNotIn('private original content', str(inv))
        self.assertEqual(inv, ci.inventory(self.root, 'fixture'))

    def test_compare(self):
        self.put('same'); self.put('change'); self.put('remove'); self.put('Case')
        a = ci.inventory(self.root, 'before')
        (self.root/'remove').unlink(); (self.root/'Case').rename(self.root/'case')
        self.put('change', b'xxxx'); self.put('added')
        r = ci.compare(a, ci.inventory(self.root, 'after'))
        self.assertEqual([e['path'] for e in r['added']], ['added'])
        self.assertEqual([e['path'] for e in r['removed']], ['remove'])
        self.assertEqual([e['after']['path'] for e in r['changed']], ['change'])
        self.assertEqual(len(r['spelling_changes']), 1)
        self.assertEqual(r['unchanged'], 2)

    def test_case_collision(self):
        self.put('Name'); self.put('name')
        if len(list(self.root.iterdir())) < 2: self.skipTest('case-insensitive filesystem')
        with self.assertRaisesRegex(ValueError, 'collision'): ci.inventory(self.root, 'bad')

    def test_symlink(self):
        self.put('real')
        try: (self.root/'link').symlink_to(self.root/'real')
        except OSError: self.skipTest('symlinks unavailable')
        inv = ci.inventory(self.root, 'symlink')
        self.assertEqual(len(inv['entries']), 1)
        self.assertEqual(inv['skipped'][0]['path'], 'link')

    def test_scope_and_filters(self):
        self.put('TEST.FBI'); self.put('other.txt')
        a = ci.inventory(self.root, 'all')
        b = ci.inventory(self.root, 'subset', ['*.fbi'])
        self.assertEqual(len(b['entries']), 1)
        self.assertFalse(ci.compare(a, b)['same_scope'])

    def test_reject_duplicate_manifest(self):
        self.put('file')
        a = ci.inventory(self.root, 'fixture')
        a['entries'].append(copy.deepcopy(a['entries'][0]))
        with self.assertRaisesRegex(ValueError, 'duplicate'): ci.compare(a, a)

    def test_reject_bad_hash(self):
        self.put('file'); a = ci.inventory(self.root, 'fixture')
        a['entries'][0]['sha256'] = 'bad'
        with self.assertRaises(ValueError): ci.compare(a, a)

    def test_hpi_listing_validation(self):
        with patch.object(ci.subprocess, 'run') as run:
            run.return_value.stdout = b'         [folder]\n        3 folder/a b.tdf\n1 files, 3 bytes\n'
            self.assertEqual(ci.archive_entries('tool', 'file'), [('folder/a b.tdf', 3)])
            run.return_value.stdout = b'        3 a\n2 files, 3 bytes\n'
            with self.assertRaises(ValueError): ci.archive_entries('tool', 'file')

    def test_output_cannot_modify_source(self):
        self.put('file')
        r = subprocess.run([sys.executable, str(Path(ci.__file__)), 'inventory', str(self.root),
                            '--label', 'fixture', '--output', str(self.root/'out.json')], capture_output=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertFalse((self.root/'out.json').exists())

    def test_archive_identity_is_separate(self):
        self.put('file'); a = ci.inventory(self.root, 'fixture')
        entry = copy.deepcopy(a['entries'][0])
        entry['archive'] = 'first.hpi'; a['entries'].append(entry)
        entry = copy.deepcopy(entry); entry['archive'] = 'second.hpi'; a['entries'].append(entry)
        self.assertEqual(ci.compare(a, a)['unchanged'], 3)

    def test_compare_cannot_overwrite_input(self):
        self.put('file'); manifest = Path(self.tmp.name)/'inventory.json'
        ci.write_json(manifest, ci.inventory(self.root, 'fixture'))
        before = manifest.read_bytes()
        r = subprocess.run([sys.executable, str(Path(ci.__file__)), 'compare', str(manifest),
                            str(manifest), '--output', str(manifest)], capture_output=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(manifest.read_bytes(), before)

    def test_missing_source_rejected(self):
        with self.assertRaises(ValueError): ci.inventory(self.root/'missing', 'fixture')

    def test_real_hpi(self):
        tool = Path(__file__).resolve().parents[2]/'build/hpitool'
        if not tool.exists(): self.skipTest('build/hpitool unavailable')
        source = Path(self.tmp.name)/'pack'; source.mkdir()
        (source/'test.tdf').write_bytes(b'[TEST]{value=42;}')
        subprocess.run([str(tool), 'pack', str(source), str(self.root/'test.hpi')], check=True, capture_output=True)
        inv = ci.inventory(self.root, 'synthetic', members=['*.hpi'], tool=tool)
        self.assertEqual(len(inv['entries']), 2)
        member = next(e for e in inv['entries'] if e['archive'])
        self.assertEqual(member['archive'], 'test.hpi')
        self.assertEqual(member['sha256'], hashlib.sha256(b'[TEST]{value=42;}').hexdigest())


if __name__ == '__main__': unittest.main()
