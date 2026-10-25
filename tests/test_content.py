from paths import ROOT
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import content_profile

EXE=ROOT/'out/content-test'

class ProfileTests(unittest.TestCase):
    def test_sfo_parameters_and_explicit_trial_profile(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'sce_sys').mkdir();(p/'sce_sys/param.sfo').write_bytes(b'fixture')
            with patch.object(content_profile,'sfo',return_value={'USER_DEFINED_PARAM_1':13,'USER_DEFINED_PARAM_4':0xffffffff}):
                content_profile.prepare(p,p,'trial')
            self.assertEqual(struct.unpack('<8s5I',(p/'content.bin').read_bytes()),(b'BBCONT01',1,13,0,0,0xffffffff))

    def test_invalid_sfo_parameter_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'sce_sys').mkdir();(p/'sce_sys/param.sfo').write_bytes(b'fixture')
            with patch.object(content_profile,'sfo',return_value={'USER_DEFINED_PARAM_1':'13'}):
                with self.assertRaisesRegex(ValueError,'invalid user-defined'):
                    content_profile.prepare(p,p)

@unittest.skipUnless(EXE.exists(),'run bash build_macos.sh --test first')
class ContentTests(unittest.TestCase):
    def run_case(self,*args):
        return subprocess.run([str(EXE.resolve()),*args],capture_output=True,text=True,timeout=5)

    def test_lifecycle_and_output_boundaries(self):
        r=self.run_case();self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_missing_profile_does_not_report_loaded(self):
        r=self.run_case('--missing');self.assertEqual(r.returncode,21)
        self.assertIn('needs --content-profile',r.stderr)

    def test_other_modules_are_counted_independently(self):
        r=self.run_case('--unknown');self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_wide_ids_stop_before_lookup_or_mutation(self):
        names={'load':'sceSysmoduleLoadModule', 'query':'sceSysmoduleIsLoaded',
               'unload':'sceSysmoduleUnloadModule'}
        for operation,name in names.items():
            ids=[0x100b4,0xffff00b4,0x10000,0xffff0000,0xffffffff]
            # Only LoadModule has a proven early error for 0x80000000.
            if operation!='load': ids.append(0x80000000)
            for state in ('loaded','unloaded'):
                for module_id in ids:
                    with self.subTest(operation=operation,id=hex(module_id),state=state):
                        r=self.run_case('--wide',operation,hex(module_id),state)
                        self.assertEqual(r.returncode,21,r.stdout+r.stderr)
                        self.assertIn(name,r.stderr)
                        self.assertIn(f'0x{module_id:08x}',r.stderr)
                        self.assertIn('outside the supported host namespace',r.stderr)
                        self.assertIn('PASS: system-module records preserved',r.stdout)
