from paths import ROOT
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
import unittest
from patches import (ULTRAWIDE_PATCHES, ultrawide_patch, ultrawide_writes, compile_patches,
                     EBOOT_BASE, SCENE_WIDTH, SCENE_HEIGHT, UI_WIDTH, UI_HEIGHT)
from test_patches import immediate


class UltrawideTests(unittest.TestCase):
    def test_complete_native_patch_with_bounded_texture_pool(self):
        for size, name in ULTRAWIDE_PATCHES.items():
            with self.subTest(size=size):
                width, height = map(int, size.split('x'))
                self.assertEqual(ultrawide_patch({'ultrawide_res': size}), name)
                xml = ROOT / 'patches/Bloodborne.xml'
                writes = ultrawide_writes(xml, name, '01.09', [(0, 0x6000000)])
                full = compile_patches(xml, [name], '01.09', [(0, 0x6000000)])
                heaps = {0x04B36DDA - EBOOT_BASE, 0x04B36DE2 - EBOOT_BASE}
                expected = [w for w in full if w[0] not in heaps]
                if width * height > 1920 * 1080:
                    expected.append((0x04B36DE0 - EBOOT_BASE, struct.pack('<I', 1536 * 1024 * 1024)))
                # Above 4K the first heap is the community patch's 3952 MiB (#93); below
                # it stays retail.
                if width * height > 3840 * 2160:
                    expected.append((0x04B36DD8 - EBOOT_BASE, struct.pack('<I', 3952 * 1024 * 1024)))
                self.assertEqual(writes, expected)
                addr = 0x04B36DDA - EBOOT_BASE
                self.assertEqual(any(a < addr + 2 and a + len(d) > addr for a, d in writes),
                                 width * height > 3840 * 2160)
                # Regression: removing these MOV/NOPs while keeping the fallback
                # globals silently allocated 640x360 and broke the entire UI.
                for address, expected in ((SCENE_WIDTH, width), (SCENE_HEIGHT, height),
                                          (UI_WIDTH, width), (UI_HEIGHT, height)):
                    self.assertEqual(immediate(writes, address)[1], expected)
                camera = dict(writes)[0x0183A35D - EBOOT_BASE]
                self.assertAlmostEqual(struct.unpack('<f', camera)[0], width / height, places=5)

    def test_off_and_invalid_resolution(self):
        self.assertEqual(ultrawide_patch({}), '')
        self.assertEqual(ultrawide_patch({'ultrawide_res': ''}), '')
        with self.assertRaises(ValueError):
            ultrawide_patch({'ultrawide_res': '3000x1000'})

    def test_invalid_selection_cli_reports_one_line_without_traceback(self):
        with tempfile.TemporaryDirectory() as folder:
            settings = Path(folder) / 'bbport.ini'
            settings.write_text('ultrawide_res=3000x1000\n')
            result = subprocess.run([sys.executable, '-B', str(ROOT / 'scripts/patches.py'),
                                     '--print-ultrawide', '--settings', str(settings)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(result.stdout, '')
            self.assertEqual(len(result.stderr.splitlines()), 1)
            self.assertIn('Unsupported ultrawide resolution: 3000x1000', result.stderr)
            self.assertIn('remove ultrawide_res', result.stderr)
            self.assertNotIn('Traceback', result.stderr)
