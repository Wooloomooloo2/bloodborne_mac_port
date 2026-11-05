# SPDX-License-Identifier: GPL-2.0-or-later
import struct
import tempfile
import unittest
from pathlib import Path

from test_mac_menu_assets import assets, archive, bnd, dcx, fmg, movie, movie_parts, placement, sprite, texts, MESSAGES


def title_movie(rows=5):
    def matrix(y):
        return assets._write_matrix(((65536, 65536), (12, -8), (120, y)))

    commands = sprite(20, *(placement(1, i + 1, f'Item_{i}_0', matrix(100 + i * 600))
                             for i in range(rows)),
                      assets._tag(1, b''),
                      placement(1, 5, matrix=matrix(2600)),
                      placement(1, 5, matrix=None),
                      assets._tag(28, struct.pack('<H', 5)))
    top = sprite(30, placement(20, 1, 'CommandList'))
    source = movie()
    header = source[:assets._stage_start(source)]
    result = bytearray(header + commands + top + placement(30, 1, 'Top') +
                       assets._tag(1, b'') + assets._tag(0, b''))
    struct.pack_into('<I', result, 4, len(result))
    return bytes(result)


class ExitMenuAssetsTests(unittest.TestCase):
    def test_sixth_row_preserves_spacing_transforms_animation_and_removal(self):
        source = title_movie()
        output = assets.build_title(source)
        sprites, _ = movie_parts(output)
        body, end, _ = sprites[20]
        rows = []
        removals = []
        for code, _, start, stop in assets._tags(output, body + 4, end):
            if code in (26, 70):
                rows.append(assets._place(output, code, start, stop))
            elif code == 28:
                removals.append(struct.unpack_from('<H', output, start)[0])
        original = [p for p in rows if p['depth'] == 5]
        added = [p for p in rows if p['depth'] == 6]
        self.assertEqual(len(original), 3)
        self.assertEqual(len(added), 3)
        self.assertEqual(added[0]['name'], 'Item_5_0')
        for old, new in zip(original[:2], added[:2]):
            old_matrix, new_matrix = map(assets._read_matrix, (old['matrix'], new['matrix']))
            self.assertEqual(old_matrix[:2], new_matrix[:2])
            self.assertEqual(new_matrix[2], (old_matrix[2][0], old_matrix[2][1] + 600))
        self.assertIsNone(added[2]['matrix'])
        self.assertEqual(removals, [5, 6])
        self.assertEqual(assets.build_title(output), output)
        self.assertEqual(movie_parts(source)[0][30][2], sprites[30][2])
        self.assertEqual(struct.unpack_from('<I', output, 4)[0], len(output))

    def test_reject_unsupported_title_and_bad_spacing(self):
        with self.assertRaises(ValueError):
            assets.build_title(movie())
        data = title_movie().replace(assets._write_matrix(((65536, 65536), (12, -8), (120, 2500))),
                                    assets._write_matrix(((65536, 65536), (12, -8), (120, 1900))))
        with self.assertRaises(ValueError):
            assets.build_title(data)

    def test_exit_caption_replaces_hidden_entry_in_both_fmgs(self):
        source = dcx(bnd([(200, fmg({assets.EXIT_ID: 'Opinion Box', 42: '日本語'})),
                          (201, fmg({assets.EXIT_ID: 'Old help'})), (203, b'unchanged')]))
        generated = {group: assets.read_fmg(data) for group, data in
                     texts(assets.build_messages(source, MESSAGES, exit_game=True)).items()
                     if group in (200, 201)}
        self.assertEqual(generated[200][assets.EXIT_ID], 'Exit Game')
        self.assertEqual(generated[201][assets.EXIT_ID], 'Close the game and return to the desktop.')
        self.assertEqual(generated[200][42], '日本語')

    def test_all_title_variants_are_cached_with_every_language_without_source_writes(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            game, cache = root / 'game', root / 'cache'
            dvd = game / 'dvdroot_ps4'
            sources = {'menu/optionsetting.gfx': movie()}
            sources.update({f'menu/{name}': title_movie() for name in assets.TITLE_MOVIES})
            sources.update({f'msg/{lang}/menu.msgbnd.dcx': archive() for lang in ('engus', 'jpnjp')})
            for name, data in sources.items():
                path = dvd / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            normal = assets.generate(game, cache, MESSAGES)
            combined = assets.generate(game, cache, MESSAGES, exit_game=True)
            self.assertNotEqual(normal, combined)
            self.assertEqual(assets.generate(game, cache, MESSAGES, exit_game=True), combined)
            for name, data in sources.items():
                self.assertEqual((dvd / name).read_bytes(), data)
                generated = (combined / 'dvdroot_ps4' / name).read_bytes()
                if name.endswith('msgbnd.dcx'):
                    self.assertEqual(assets.read_fmg(texts(generated)[200])[assets.EXIT_ID], 'Exit Game')
                elif 'title' in name:
                    self.assertIn(b'Item_5_0\0', generated)
            # Disabling Mac Settings must still allow the independent title-menu exit.
            (dvd / 'menu/optionsetting.gfx').unlink()
            exit_only = assets.generate(game, cache, {}, mac_settings=False, exit_game=True)
            self.assertFalse((exit_only / 'dvdroot_ps4/menu/optionsetting.gfx').exists())
            generated = assets.read_fmg(texts((exit_only / 'dvdroot_ps4/msg/engus/menu.msgbnd.dcx').read_bytes())[200])
            self.assertEqual(generated[assets.EXIT_ID], 'Exit Game')
            self.assertNotIn(126001, generated)


if __name__ == '__main__':
    unittest.main()
