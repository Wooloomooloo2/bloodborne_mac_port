from paths import ROOT
import subprocess
import sys
import tempfile
import unittest

@unittest.skipUnless(sys.platform=='darwin','macOS x86-64 runtime fixture')
class SemaphoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory=tempfile.TemporaryDirectory(prefix='bb-sema-test-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.exe=cls.directory.name+'/sema-test'
        command=[
            '/usr/bin/clang','-arch','x86_64','-mmacosx-version-min=15.0',
            '-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-pthread',
            '-DBB_SEMA_STANDALONE','-Isrc','tests/test_sema.c','tests/sema_fixture.c',
            '-o',cls.exe,
        ]
        subprocess.run(command,cwd=ROOT,check=True,capture_output=True,text=True)

    def run_case(self,*args):
        return subprocess.run(['/usr/bin/arch','-x86_64',self.exe,*args],capture_output=True,text=True,timeout=10)

    def test_counts_errors_timeout_and_stale_ids(self):
        r=self.run_case()
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_fifo_wakeup_and_token_reservation(self):
        r=self.run_case('--concurrency')
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_cancel_and_delete_wake_all_waiters(self):
        r=self.run_case('--cancel-delete')
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_priority_wait_blocks_until_signal(self):
        r=self.run_case('--priority')
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_eight_byte_unaligned_publication_and_full_handle_admission(self):
        r=self.run_case('--wire')
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_options_error_precedes_other_argument_access(self):
        r=self.run_case('--options')
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)

    def test_positive_int32_ids_exhaust_without_reuse(self):
        r=self.run_case('--id-limit')
        self.assertEqual(r.returncode,0,r.stdout+r.stderr)
