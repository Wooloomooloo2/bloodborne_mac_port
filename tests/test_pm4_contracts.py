# SPDX-License-Identifier: GPL-2.0-or-later
from paths import ROOT
import os
import subprocess
import sys
import tempfile
import unittest


class BoundaryTests(unittest.TestCase):
    def test_production_boundary(self):
        with tempfile.TemporaryDirectory(prefix="bb-pm4_contracts-") as directory:
            for arch in (["arm64", "x86_64"] if sys.platform == "darwin" else [None]):
                with self.subTest(arch=arch):
                    exe = os.path.join(directory, arch or "test")
                    flags = ["-arch", arch] if arch else []
                    subprocess.run([os.environ.get("CXX", "clang++"), *flags, "-std=c++20",
                                    "-O2", "-Wall", "-Wextra", "-Werror", "-Igpu/shim",
                                    "tests/test_pm4_contracts.cpp", "-o", exe], cwd=ROOT, check=True,
                                   capture_output=True, text=True)
                    result = subprocess.run([exe], cwd=ROOT, capture_output=True, text=True, timeout=15)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("PASS:", result.stdout)
