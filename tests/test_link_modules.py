"""Owned fixtures for linked module TLS displacement metadata."""
from paths import ROOT
import copy
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

import link_libc
import link_modules
import test_link_libc
from test_probe import package


class ModuleTlsTests(unittest.TestCase):
    def test_one_contract_covers_main_and_both_module_rewrites(self):
        main, libc = test_link_libc.LinkTests().fixture()
        main['ph'].append(dict(type=1, vaddr=0, filesz=4096, flags=5))
        modules = [copy.deepcopy(libc), copy.deepcopy(libc)]
        modules[1]['symbols'] = []  # A distinct module without duplicate exports.
        for module in modules:
            module['elf'] = link_libc.FS_LOAD + module['elf'][len(link_libc.FS_LOAD):]
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {}, clear=True):
            out = Path(tmp)
            (out / 'boot.bin').write_bytes(package(link_libc.FS_LOAD + b'\xc3',
                                                  names=['fixture#q#q'], capabilities=1))
            with patch.object(link_modules, 'module', side_effect=[main, *modules]):
                link_modules.link(Path('fixture-game'), out)
            data = (out / 'boot-linked.bin').read_bytes()
            report = json.loads((out / 'link.json').read_text())
        self.assertEqual(data[:8], b'BBPROBE5')
        self.assertEqual(struct.unpack_from('<Q', data, 48)[0], 3)
        expected = 384 * 8 if sys.platform == 'darwin' else 0
        self.assertEqual(struct.unpack_from('<Q', data, 56)[0], expected)
        self.assertEqual(report['gs_displacement'], expected)
        self.assertEqual(report['fs_loads_patched'], 3)
        self.assertEqual([module['tls_module'] for module in report['modules']], [2, 3])
        image = data[-struct.unpack_from('<Q', data, 8)[0]:]
        for base in (0, 65536, 131072):
            self.assertEqual(image[base:base + 9], link_libc.gs_load())


if __name__ == '__main__':
    unittest.main()
