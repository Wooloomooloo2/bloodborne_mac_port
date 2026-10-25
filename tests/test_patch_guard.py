"""Exercise the production hook metadata and complete patch-set scan without a game."""
from paths import ROOT
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


@unittest.skipUnless(sys.platform == 'darwin', 'macOS x86-64 runtime verifier')
class RuntimePatchGuardTests(unittest.TestCase):
    def test_protected_bytes_and_complete_scan(self):
        with tempfile.TemporaryDirectory() as directory:
            exe=Path(directory)/'patch-guard-test'
            command=['/usr/bin/clang','-arch','x86_64','-mmacosx-version-min=15.0',
                     '-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Isrc',
                     'tests/test_runtime_patch_guard.c','src/runtime_hooks.c','-o',str(exe)]
            subprocess.run(command,cwd=ROOT,check=True,capture_output=True,text=True)
            result=subprocess.run(['/usr/bin/arch','-x86_64',str(exe)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
