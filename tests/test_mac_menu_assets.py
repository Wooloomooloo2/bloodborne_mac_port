from paths import ROOT
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock
import zlib

spec = importlib.util.spec_from_file_location('mac_menu_assets', ROOT / 'scripts/mac_menu_assets.py')
assets = importlib.util.module_from_spec(spec)
spec.loader.exec_module(assets)

MESSAGES = {
    126000: {'text': 'Mac Settings', 'help': ''},
    126001: {'text': 'Mac Settings', 'help': 'Configure the port.'},
    126100: {'text': 'Display', 'help': 'Choose a display.'},
    126200: {'text': 'On', 'help': ''},
}


def placement(cid, depth, name=None, matrix=b'\0'):
    flags = 2 | (4 if matrix is not None else 0) | (32 if name is not None else 0)
    return assets._tag(26, bytes([flags]) + struct.pack('<HH', depth, cid) +
                       (matrix or b'') + (name.encode() + b'\0' if name is not None else b''))


def sprite(cid, *records):
    return assets._tag(39, struct.pack('<HH', cid, 1) + b''.join(records) +
                       assets._tag(1, b'') + assets._tag(0, b''))


def movie(extra=b'', rows=7, title=True, symbol=True):
    records = [sprite(10, placement(1, 1, 'StaticText_100000')),
               sprite(20, placement(10, 1, 'Title') if title else b'',
                      placement(1, 2, 'StaticText_110000'),
                      *(placement(1, 3 + row, f'Item_{row}_0') for row in range(rows)),
                      assets._tag(1, b''),
                      # Later anonymous updates and removal of donor-only labels.
                      placement(1, 2, matrix=None), placement(1, 9, matrix=None),
                      assets._tag(28, struct.pack('<H', 9))),
               sprite(30), placement(20, 382, 'ControllSetting'), placement(30, 500, 'Top')]
    if symbol:
        records.append(assets._tag(76, struct.pack('<HH', 1, 20) + b'DonorClass\0'))
    header = b'GFX\x0b' + b'\0' * 4 + b'\x08\x00' + struct.pack('<HH', 30 * 256, 1)
    result = bytearray(header + b''.join(records) + extra +
                       assets._tag(1, b'') + assets._tag(0, b''))
    struct.pack_into('<I', result, 4, len(result))
    return bytes(result)


def fmg(strings):
    template = bytearray(0x28)
    template[2] = 2
    struct.pack_into('<I', template, 8, 1)
    struct.pack_into('<I', template, 0x14, 0xff)
    return assets.write_fmg(template, strings)


def bnd(files):
    """Minimal BND4 fixture, with structural names and no shipped game bytes."""
    out = bytearray(0x40 + len(files) * 0x24)
    out[:4] = b'BND4'
    out[0x30] = 1
    struct.pack_into('<I', out, 0x0c, len(files))
    struct.pack_into('<Q', out, 0x20, 0x24)
    for k, (ident, _) in enumerate(files):
        struct.pack_into('<I', out, 0x40 + k * 0x24 + 0x20, len(out))
        out += f'fixture-{ident}.fmg'.encode('utf-16-le') + b'\0\0'
    struct.pack_into('<Q', out, 0x28, len(out))
    for k, (ident, data) in enumerate(files):
        out += b'\0' * (-len(out) % 16)
        entry = 0x40 + k * 0x24
        struct.pack_into('<QQIII', out, entry + 8, len(data), len(data), len(out), ident,
                         struct.unpack_from('<I', out, entry + 0x20)[0])
        out += data
    return bytes(out)


def dcx(body):
    header = bytearray(0x4c)
    header[:4] = b'DCX\0'
    header[0x18:0x1c] = b'DCS\0'
    header[0x24:0x2c] = b'DCP\0DFLT'
    header[0x44:0x4c] = b'DCA\0\0\0\0\x08'
    packed = zlib.compress(body)
    struct.pack_into('>II', header, 0x1c, len(body), len(packed))
    return bytes(header) + packed


def archive(original='Original'):
    return dcx(bnd([(200, fmg({1: original, 2: None, 42: '日本語 🐙'})),
                    (201, fmg({1: 'Original help'})), (203, b'untouched payload')]))


def movie_parts(data):
    sprites, places = {}, {}
    for code, head, body, end in assets._tags(data, assets._stage_start(data), len(data)):
        if code == 39:
            sprites[struct.unpack_from('<H', data, body)[0]] = (body, end, data[head:end])
        elif code in (26, 70):
            item = assets._place(data, code, body, end)
            if item['name']:
                places[item['name']] = (item, data[body:end])
    return sprites, places


def texts(data):
    body = assets._unpack_dcx(data)
    return {ident: body[offset:offset + size]
            for _, offset, size, ident in assets.bnd4_files(body)}


class MovieTests(unittest.TestCase):
    def test_hidden_section_keeps_six_widgets_and_clones_title(self):
        source = movie()
        output = assets.build_movie(source)
        before, _ = movie_parts(source)
        sprites, places = movie_parts(output)
        section, record = places['MacSettings']
        self.assertEqual(section['depth'], 383)
        self.assertEqual(record[1], 0x20)  # PlaceObject3 HasVisible
        self.assertEqual(record[-1], 0)  # hidden until the game opens it
        self.assertEqual(section['matrix'], places['ControllSetting'][0]['matrix'])
        body, end, _ = sprites[section['cid']]
        rows, title = set(), None
        for code, _, start, stop in assets._tags(output, body + 4, end):
            if code in (26, 70):
                item = assets._place(output, code, start, stop)
                self.assertNotIn(item['depth'], (2, 9))
                if item['name'] and item['name'].startswith('Item_'):
                    rows.add(item['name'])
                if item['name'] == 'Title':
                    title = sprites[item['cid']][2]
            if code == 28:
                self.fail('removed donor label animation was retained')
        self.assertEqual(rows, {f'Item_{i}_0' for i in range(6)})
        self.assertIn(b'StaticText_126000\0', title)
        self.assertNotIn(b'StaticText_100000\0', title)
        for cid, (_, _, original) in before.items():
            self.assertEqual(sprites[cid][2], original)
        self.assertEqual(struct.unpack_from('<I', output, 4)[0], len(output))
        self.assertIn(struct.pack('<H', section['cid']) + b'DonorClass\0', output)

    def test_movie_without_symbol_classes(self):
        self.assertIn(b'MacSettings\0', assets.build_movie(movie(symbol=False)))

    def test_reject_unsupported_donor_and_already_modified_movie(self):
        for source in (b'', b'CFX' + movie()[3:], movie(rows=6), movie(title=False),
                       movie(extra=placement(1, 383, 'occupied')),
                       assets.build_movie(movie())):
            with self.subTest(source=source[:12]), self.assertRaises(ValueError):
                assets.build_movie(source)

    def test_nested_tag_cannot_read_outside_its_timeline(self):
        # A long tag claims that its body continues into a different timeline.
        data = struct.pack('<HI', 39 << 6 | 63, 100) + b'\0' * 100
        with self.assertRaisesRegex(ValueError, 'beyond its timeline'):
            list(assets._tags(data, 0, 20))
        for data in (b'\xff', b'\xff\xff\0', assets._tag(1, b'')):
            with self.subTest(data=data), self.assertRaises(ValueError):
                list(assets._tags(data, 0, len(data)))

    def test_truncated_placement_and_unterminated_name(self):
        for body in (b'', b'\x26\0', b'\x22\x01\0\x01\0bad'):
            with self.subTest(body=body), self.assertRaises(ValueError):
                assets._place(body, 26, 0, len(body))


class MessageTests(unittest.TestCase):
    def test_roundtrip_and_preserve_all_original_texts_and_other_files(self):
        source = archive()
        before, after = texts(source), texts(assets.build_messages(source, MESSAGES))
        self.assertEqual(before[203], after[203])
        for group, field in ((200, 'text'), (201, 'help')):
            original, changed = assets.read_fmg(before[group]), assets.read_fmg(after[group])
            self.assertEqual({mid: changed[mid] for mid in original}, original)
            self.assertEqual({mid: changed[mid] for mid in MESSAGES},
                             {mid: item[field] or None for mid, item in MESSAGES.items()})

    def test_fmg_handles_empty_null_and_non_bmp_unicode(self):
        strings = {1: '', 2: None, 4: '日本語 🐙', 8: 'Repeated', 9: 'Repeated'}
        self.assertEqual(assets.read_fmg(fmg(strings)), strings)
        self.assertEqual(assets.read_fmg(fmg({})), {})

    def test_existing_ids_and_missing_groups_fail_closed(self):
        conflict = dcx(bnd([(200, fmg({126000: 'Existing'})), (201, fmg({1: 'help'}))]))
        for data in (conflict, dcx(bnd([(200, fmg({1: 'text'}))]))):
            with self.subTest(data=data[:8]), self.assertRaises(ValueError):
                assets.build_messages(data, MESSAGES)

    def test_reject_invalid_fmg_offsets_groups_and_encoding(self):
        valid = fmg({1: 'Text'})
        cases = [b'', valid[:0x27], valid[:-1]]
        for offset, fmt, value in ((2, '<B', 1), (0x0c, '<I', 0xffffffff),
                                   (0x18, '<Q', len(valid)), (0x28, '<i', -1),
                                   (0x30, '<i', 0x7fffffff), (0x38, '<Q', 1)):
            data = bytearray(valid)
            struct.pack_into(fmt, data, offset, value)
            cases.append(bytes(data))
        data = bytearray(valid)
        at, = struct.unpack_from('<Q', data, 0x38)
        data[at:at + 2] = b'\0\xd8'  # unpaired UTF-16 surrogate
        cases.append(bytes(data))
        for data in cases:
            with self.subTest(data=data[:16]), self.assertRaises(ValueError):
                assets.read_fmg(data)

    def test_reject_malformed_dcx_stream_and_size_fields(self):
        valid = archive()
        cases = [b'', valid[:40], valid[:-1], valid + b'junk', valid[:0x4c] + b'not zlib']
        for offset, value in ((0x1c, assets.MAX_ARCHIVE + 1), (0x1c, 1), (0x20, 1)):
            data = bytearray(valid)
            struct.pack_into('>I', data, offset, value)
            cases.append(bytes(data))
        for data in cases:
            with self.subTest(data=data[:16]), self.assertRaises(ValueError):
                assets._unpack_dcx(data)

    def test_reject_malformed_bnd_entries_and_names(self):
        valid = assets._unpack_dcx(archive())
        cases = [b'', valid[:63]]
        for offset, fmt, value in ((0x20, '<Q', 0x30), (0x0c, '<I', 0xffffffff),
                                   (0x40 + 8, '<Q', len(valid) + 1),
                                   (0x40 + 0x18, '<I', len(valid) + 1),
                                   (0x40 + 0x20, '<I', len(valid) + 1),
                                   (0x40 + 0x24 + 0x1c, '<I', 200)):
            data = bytearray(valid)
            struct.pack_into(fmt, data, offset, value)
            cases.append(bytes(data))
        for data in cases:
            with self.subTest(data=data[:16]), self.assertRaises(ValueError):
                assets.bnd4_files(data)

    def test_validate_message_schema(self):
        self.assertEqual(assets._messages(json.loads(json.dumps(MESSAGES))), MESSAGES)
        for messages in ([], {}, {'not an id': {}}, {126000: {'text': 'title'}},
                         dict(MESSAGES, **{'126000.0': {'text': 'bad'}}),
                         {**MESSAGES, 126999: {'text': 'bad\0text'}},
                         {**MESSAGES, 126999: {'text': '\ud800'}},
                         {**MESSAGES, 127000: {'text': 'outside namespace'}},
                         {**MESSAGES, 126999: {'text': 'extra', 'hepl': 'typo'}}):
            with self.subTest(messages=repr(messages)), self.assertRaises(ValueError):
                assets._messages(messages)


class GenerationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game = self.root / 'original'
        self.dvd = self.game / 'dvdroot_ps4'
        self.movie = self.dvd / 'menu/optionsetting.gfx'
        self.movie.parent.mkdir(parents=True)
        self.movie.write_bytes(movie())
        for language in ('engus', 'jpnjp'):
            path = self.dvd / 'msg' / language / 'menu.msgbnd.dcx'
            path.parent.mkdir(parents=True)
            path.write_bytes(archive())
        self.cache = self.root / 'cache'

    def snapshot(self):
        return {str(p.relative_to(self.game)): hashlib.sha256(p.read_bytes()).hexdigest()
                for p in self.game.rglob('*') if p.is_file()}

    def test_cached_generation_is_private_and_preserves_sources(self):
        before = self.snapshot()
        first = assets.generate(self.game, self.cache, MESSAGES)
        self.assertEqual(len(first.name), 64)
        movie = first / 'dvdroot_ps4/menu/optionsetting.gfx'
        mtime = movie.stat().st_mtime_ns
        self.assertEqual(assets.generate(self.dvd, self.cache, MESSAGES), first)
        self.assertEqual(movie.stat().st_mtime_ns, mtime)
        self.assertEqual(self.cache.stat().st_mode & 0o777, 0o700)
        self.assertEqual(movie.stat().st_mode & 0o777, 0o600)
        self.assertEqual(len(list((first / 'dvdroot_ps4/msg').glob('*/menu.msgbnd.dcx'))), 2)
        self.assertEqual(self.snapshot(), before)

    def test_source_recipe_and_message_changes_regenerate(self):
        first = assets.generate(self.game, self.cache, MESSAGES)
        messages = {**MESSAGES, 126100: {'text': 'Changed label', 'help': ''}}
        second = assets.generate(self.game, self.cache, messages)
        self.movie.write_bytes(movie(extra=assets._tag(77, b'recipe-neutral fixture metadata\0')))
        third = assets.generate(self.game, self.cache, MESSAGES)
        with mock.patch.object(assets, 'RECIPE_VERSION', assets.RECIPE_VERSION + 1):
            fourth = assets.generate(self.game, self.cache, MESSAGES)
        self.assertEqual(len({first, second, third, fourth}), 4)
        self.assertTrue(all(p.is_dir() for p in (first, second, third, fourth)))

    def test_corrupt_cache_regenerates(self):
        first = assets.generate(self.game, self.cache, MESSAGES)
        output = first / 'dvdroot_ps4/menu/optionsetting.gfx'
        original = output.read_bytes()
        output.write_bytes(b'partial')
        self.assertEqual(assets.generate(self.game, self.cache, MESSAGES), first)
        self.assertEqual(output.read_bytes(), original)
        self.assertFalse(any(p.name.startswith(('.building-', '.stale-')) for p in self.cache.iterdir()))

    def test_failed_generation_never_publishes_partial_assets(self):
        first = assets.generate(self.game, self.cache, MESSAGES)
        before = (first / assets.MANIFEST).read_bytes()
        (self.dvd / 'msg/jpnjp/menu.msgbnd.dcx').write_bytes(b'invalid')
        with self.assertRaises(ValueError):
            assets.generate(self.game, self.cache, MESSAGES)
        self.assertEqual((first / assets.MANIFEST).read_bytes(), before)
        self.assertEqual({p.name for p in self.cache.iterdir()}, {'.lock', first.name})

    def test_reject_output_in_source_and_nonprivate_existing_directory(self):
        before = self.snapshot()
        with self.assertRaisesRegex(ValueError, 'outside'):
            assets.generate(self.game, self.game / 'cache', MESSAGES)
        with self.assertRaisesRegex(ValueError, 'outside'):
            assets.generate(self.dvd, self.game / 'cache', MESSAGES)
        self.cache.mkdir(mode=0o755)
        self.cache.chmod(0o755)
        with self.assertRaisesRegex(ValueError, 'private'):
            assets.generate(self.game, self.cache, MESSAGES)
        self.assertEqual(self.cache.stat().st_mode & 0o777, 0o755)
        self.assertEqual(self.snapshot(), before)

    def test_reject_generation_symlink(self):
        first = assets.generate(self.game, self.cache, MESSAGES)
        saved = self.root / 'saved'
        first.rename(saved)
        first.symlink_to(saved, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'symlink'):
            assets.generate(self.game, self.cache, MESSAGES)
        self.assertTrue((saved / assets.MANIFEST).is_file())


if __name__ == '__main__':
    unittest.main()
