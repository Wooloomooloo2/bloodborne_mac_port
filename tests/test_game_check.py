# SPDX-FileCopyrightText: Copyright 2026 Mark Hall
from paths import ROOT
import tempfile
import unittest
from pathlib import Path
from game_check import problem, explain, REQUIRED_DATA, SUPPORTED_IMAGE


class GameCheckTests(unittest.TestCase):
    def test_right_eboot_without_base_game_data_is_missing_data(self):
        with tempfile.TemporaryDirectory() as game:
            found = problem(game, image_hash=SUPPORTED_IMAGE)
            self.assertEqual(found[0], 'missing_data')
            self.assertIn('dvdroot_ps4', explain(*found))

    def test_right_eboot_with_base_game_data_passes(self):
        with tempfile.TemporaryDirectory() as game:
            for name in REQUIRED_DATA:
                path = Path(game) / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'x')
            self.assertIsNone(problem(game, image_hash=SUPPORTED_IMAGE))


if __name__ == '__main__':
    unittest.main()
