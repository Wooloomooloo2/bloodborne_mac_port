# SPDX-License-Identifier: GPL-2.0-or-later
from paths import ROOT
import os
import subprocess
import sys
import tempfile
import unittest


class ContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="bb-mutex_ownership-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.exe = os.path.join(cls.temp.name, "contract-test")
        flags = ["-arch", "x86_64"] if sys.platform == "darwin" else []
        subprocess.run([os.environ.get("CC", "clang"), *flags, "-std=c11", "-O2",
                        "-Wall", "-Wextra", "-Werror", "-pthread", "-Isrc",
                        "tests/test_mutex_ownership.c", "src/runtime_mutex.c", "src/runtime_host.c",
                        "-o", cls.exe], cwd=ROOT, check=True, capture_output=True, text=True)

    def test_contracts(self):
        for mode in ['']:
            with self.subTest(mode=mode):
                result = subprocess.run([self.exe, *([mode] if mode else [])], cwd=ROOT,
                                        capture_output=True, text=True, timeout=15)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("PASS:", result.stdout)
