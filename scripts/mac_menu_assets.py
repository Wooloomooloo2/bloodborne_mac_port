#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build native Mac Settings and optional Exit Game assets from the user's own files.

``generate(game, cache, messages)`` returns an immutable mod-layer directory containing
dvdroot_ps4/menu/optionsetting.gfx and every existing language's menu.msgbnd.dcx.
``messages`` is {126xxx: {"text": str, "help": str}}; 126000 and 126001 are required.
The caller supplies all wording, including the title and System menu entry.

GFX cloning is adapted from engine/menu_assets.cpp, add_option_section, in
bloodborne_mac's bbhost vendor tree at bbacb95c3338efcd627c0edaec05ff7534e555da.
The FMG/BND4 rebuild follows that project's scripts/quit_menu.py. This focused
generator keeps six donor widgets, removes Defaults and static donor labels, and
adds one hidden MacSettings sprite. The optional title-menu expansion follows the
same revision's scripts/quit_menu.py. No original game file is changed or bundled.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import sys
import tempfile
import zlib

TITLE_ID = 126000
SYSTEM_ID = 126001
SECTION = 'MacSettings'
SECTION_DEPTH = 383
MAX_ARCHIVE = 128 * 1024 * 1024
MANIFEST = '.mac-menu-assets.json'
RECIPE_VERSION = 1
EXIT_ID = 401307  # The title menu's unused Opinion Box entry.
TITLE_MOVIES = ('title.gfx', 'title_dlc.gfx', 'title_dlc_eu.gfx')


def _span(data, start, length):
    if start < 0 or length < 0 or start > len(data) - length:
        raise ValueError('truncated asset or invalid offset')
    return data[start:start + length]


def _unpack(fmt, data, offset):
    return struct.unpack(fmt, _span(data, offset, struct.calcsize(fmt)))


def _cstr(data, start, end, unicode=False):
    step = 2 if unicode else 1
    if start < 0 or start >= end or end > len(data):
        raise ValueError('invalid string offset')
    for at in range(start, end - step + 1, step):
        if data[at:at + step] == b'\0' * step:
            return data[start:at], at + step
    raise ValueError('unterminated asset string')


class _Bits:
    def __init__(self, data, offset):
        self.data, self.bit = data, offset * 8

    def read(self, count):
        if count < 0 or self.bit + count > len(self.data) * 8:
            raise ValueError('truncated bit field')
        value = 0
        for _ in range(count):
            value = value << 1 | (self.data[self.bit >> 3] >> (7 - (self.bit & 7))) & 1
            self.bit += 1
        return value

    def end(self):
        return (self.bit + 7) // 8

    def signed(self, count):
        value = self.read(count)
        return value - (1 << count) if count and value >> (count - 1) else value


def _matrix_end(data, offset):
    bits = _Bits(data, offset)
    for _ in range(2):  # scale, rotate
        if bits.read(1):
            bits.read(bits.read(5) * 2)
    bits.read(bits.read(5) * 2)  # translate
    return bits.end()


def _stage_start(data):
    if len(data) < 16 or data[:3] != b'GFX':
        raise ValueError('optionsetting.gfx must be an uncompressed GFX movie')
    if _unpack('<I', data, 4)[0] != len(data):
        raise ValueError('GFX file size does not match its header')
    bits = _Bits(data, 8)
    bits.read(bits.read(5) * 4)
    start = bits.end() + 4  # frame rate and frame count
    _span(data, 0, start)
    return start


def _tags(data, start, end):
    """(code, tag start, body start, body end), bounded by this timeline."""
    _span(data, start, end - start)
    while start < end:
        head = start
        if end - start < 2:
            raise ValueError('truncated GFX tag header')
        header = _unpack('<H', data, start)[0]
        code, size = header >> 6, header & 63
        start += 2
        if size == 63:
            if end - start < 4:
                raise ValueError('truncated long GFX tag header')
            size = _unpack('<I', data, start)[0]
            start += 4
        if size > end - start:
            raise ValueError('GFX tag runs beyond its timeline')
        yield code, head, start, start + size
        start += size
        if code == 0:
            if size or start != end:
                raise ValueError('invalid GFX End tag or trailing timeline data')
            return
    raise ValueError('GFX timeline lacks an End tag')


def _tag(code, body):
    if len(body) < 63:
        return struct.pack('<H', code << 6 | len(body)) + body
    return struct.pack('<HI', code << 6 | 63, len(body)) + body


def _place(data, code, start, end):
    """Placement fields, with byte spans retained for edits without re-encoding matrices."""
    body = _span(data, start, end - start)
    flags = _span(body, 0, 1)[0]
    flags2, at = (_span(body, 1, 1)[0], 2) if code == 70 else (0, 1)
    depth = _unpack('<H', body, at)[0]
    at += 2
    if code == 70 and (flags2 & 8 or (flags2 & 16 and flags & 2)):
        _, at = _cstr(body, at, len(body))
    cid_at = at if flags & 2 else None
    cid = _unpack('<H', body, at)[0] if cid_at is not None else None
    at += 2 if cid_at is not None else 0
    matrix = matrix_at = matrix_end = None
    if flags & 4:
        matrix_at = at
        matrix_end = _matrix_end(body, at)
        matrix = body[at:matrix_end]
        at = matrix_end
    if flags & 8:
        bits = _Bits(body, at)
        add, mult, width = bits.read(1), bits.read(1), bits.read(4)
        bits.read(width * 4 * (add + mult))
        at = bits.end()
    if flags & 16:
        _span(body, at, 2)
        at += 2
    name, name_at, name_end = None, at, at
    if flags & 32:
        raw, name_end = _cstr(body, at, len(body))
        name = raw.decode('latin-1')
    return dict(depth=depth, cid=cid, cid_at=cid_at, matrix=matrix,
                matrix_at=matrix_at, matrix_end=matrix_end,
                name=name, name_at=name_at, name_end=name_end)


def build_movie(data):
    """Clone ControllSetting to the hidden six-widget MacSettings section."""
    start = _stage_start(data)
    tags = list(_tags(data, start, len(data)))
    sprites, placements, classes, used_depths = {}, {}, {}, set()
    defines = {2, 6, 20, 21, 22, 32, 33, 34, 35, 36, 37, 39, 46, 48, 60,
               75, 83, 84, 87, 90, 91, 1001, 1003, 1005, 1006, 1007,
               1008, 1009, 1010}
    best = 0
    symbol_tag = None
    for code, head, body, end in tags:
        if code in defines:
            best = max(best, _unpack('<H', data, body)[0])
        if code == 39:
            cid, _ = _unpack('<HH', data, body)
            if cid in sprites:
                raise ValueError('duplicate GFX sprite id')
            sprites[cid] = (body, end)
        elif code in (26, 70):
            p = _place(data, code, body, end)
            used_depths.add(p['depth'])
            if p['name']:
                placements[p['name']] = (p, head, body, end)
        elif code == 76:
            if symbol_tag is not None:
                raise ValueError('multiple GFX SymbolClass tags are unsupported')
            symbol_tag = (head, body, end)
            count = _unpack('<H', data, body)[0]
            at = body + 2
            for _ in range(count):
                cid = _unpack('<H', data, at)[0]
                name, at = _cstr(data, at + 2, end)
                classes[cid] = name
            if at != end:
                raise ValueError('invalid GFX SymbolClass length')
    if SECTION in placements:
        raise ValueError('movie already contains MacSettings; use the original game dump')
    if 'ControllSetting' not in placements or 'Top' not in placements:
        raise ValueError('movie lacks ControllSetting or Top')
    if SECTION_DEPTH in used_depths or best > 65533:
        raise ValueError('no free character ids or depth for MacSettings')
    donor = placements['ControllSetting'][0]
    if donor['cid'] not in sprites or donor['matrix'] is None:
        raise ValueError('ControllSetting lacks its sprite or placement matrix')
    cs, ce = sprites[donor['cid']]
    children = list(_tags(data, cs + 4, ce))
    new_cid, title_cid = best + 1, best + 2
    title_old = title_body = None
    for code, _, body, end in children:
        if code not in (26, 70):
            continue
        p = _place(data, code, body, end)
        if p['cid'] not in sprites:
            continue
        ts, te = sprites[p['cid']]
        for cc, child_head, cb, cend in list(_tags(data, ts + 4, te)):
            if cc not in (26, 70):
                continue
            title = _place(data, cc, cb, cend)
            if not (title['name'] or '').startswith('StaticText_'):
                continue
            new_name = f'StaticText_{TITLE_ID}'.encode() + b'\0'
            replacement = (data[cb:cb + title['name_at']] + new_name +
                           data[cb + title['name_end']:cend])
            title_body = struct.pack('<H', title_cid) + data[ts + 2:te]
            offset = child_head - ts
            title_body = title_body[:offset] + _tag(cc, replacement) + title_body[cend - ts:]
            title_old = p['cid']
            break
        if title_body is not None:
            break
    if title_body is None:
        raise ValueError('ControllSetting has no title StaticText sprite')
    dropped, rows = set(), set()
    for code, _, body, end in children:
        if code not in (26, 70):
            continue
        p = _place(data, code, body, end)
        name = p['name'] or ''
        if name.startswith('StaticText') or name == 'Item_6_0':
            dropped.add(p['depth'])
        if name.startswith('Item_'):
            rows.add(name)
    if rows != {f'Item_{i}_0' for i in range(7)}:
        raise ValueError('unexpected ControllSetting widget layout')
    section_body = bytearray(struct.pack('<H', new_cid) + data[cs + 2:cs + 4])
    for code, head, body, end in children:
        record = data[body:end]
        if code in (26, 70):
            p = _place(data, code, body, end)
            if p['depth'] in dropped:
                continue
            if p['cid'] == title_old:
                record = bytearray(record)
                struct.pack_into('<H', record, p['cid_at'], title_cid)
        elif code == 28 and _unpack('<H', data, body)[0] in dropped:
            continue
        section_body += _tag(code, record)
    hidden = (b'\x26\x20' + struct.pack('<HH', SECTION_DEPTH, new_cid) +
              donor['matrix'] + SECTION.encode() + b'\0\0')
    result = bytearray(data[:start])
    for code, head, body, end in tags:
        if code == 76 and donor['cid'] in classes:
            count = _unpack('<H', data, body)[0]
            if count == 65535:
                raise ValueError('GFX SymbolClass is full')
            result += _tag(code, struct.pack('<H', count + 1) + data[body + 2:end] +
                           struct.pack('<H', new_cid) + classes[donor['cid']] + b'\0')
        else:
            result += data[head:end]
        if code == 39 and body == cs:
            result += _tag(39, title_body) + _tag(39, section_body)
        if head == placements['Top'][1]:
            result += _tag(70, hidden)
    struct.pack_into('<I', result, 4, len(result))
    return bytes(result)


def _read_matrix(data):
    bits = _Bits(data, 0)
    pairs = []
    for _ in range(2):
        width = bits.read(5) if bits.read(1) else None
        pairs.append(tuple(bits.signed(width) for _ in range(2)) if width is not None else None)
    width = bits.read(5)
    return (*pairs, (bits.signed(width), bits.signed(width)))


def _write_matrix(matrix):
    bits = []

    def write(value, count):
        bits.extend((value >> shift) & 1 for shift in reversed(range(count)))

    def pair(values):
        width = max((value if value >= 0 else ~value).bit_length() + 1 for value in values)
        if width > 31:
            raise ValueError('GFX matrix translation is too large')
        write(width, 5)
        for value in values:
            write(value, width)

    for values in matrix[:2]:
        write(values is not None, 1)
        if values is not None:
            pair(values)
    pair(matrix[2])
    bits.extend([0] * (-len(bits) % 8))
    return bytes(sum(bit << (7 - i) for i, bit in enumerate(bits[at:at + 8]))
                 for at in range(0, len(bits), 8))


def build_title(data):
    """Give the title menu a sixth animated row, adapted from bloodborne_mac/quit_menu.py."""
    start = _stage_start(data)
    tags = list(_tags(data, start, len(data)))
    sprites = {_unpack('<H', data, body)[0]: (head, body, end)
               for code, head, body, end in tags if code == 39}

    def places(begin, end):
        result = {}
        for code, _, body, stop in _tags(data, begin, end):
            if code == 1:
                break
            if code in (26, 70):
                p = _place(data, code, body, stop)
                if p['name']:
                    result[p['name']] = p
        return result

    try:
        top = sprites[places(start, len(data))['Top']['cid']]
        timeline = sprites[places(top[1] + 4, top[2])['CommandList']['cid']]
        rows = places(timeline[1] + 4, timeline[2])
        if 'Item_5_0' in rows:
            return data
        donor = rows['Item_4_0']
        step = _read_matrix(donor['matrix'])[2][1] - _read_matrix(rows['Item_3_0']['matrix'])[2][1]
    except (KeyError, TypeError) as exc:
        raise ValueError('title movie lacks Top/CommandList or its last two rows') from exc
    if step <= 0 or donor['cid'] is None:
        raise ValueError('title movie has an invalid row spacing or donor')
    records = list(_tags(data, timeline[1] + 4, timeline[2]))
    used = {_place(data, code, body, end)['depth']
            for code, _, body, end in records if code in (26, 70)}
    depth = next((d for d in range(donor['depth'] + 1, 65536) if d not in used), None)
    if depth is None:
        raise ValueError('title movie has no free row depth')
    result = bytearray(data[timeline[1]:timeline[1] + 4])
    for code, head, body, end in records:
        result += data[head:end]
        if code in (26, 70):
            p = _place(data, code, body, end)
            if p['depth'] != donor['depth']:
                continue
            copy = bytearray(data[body:end])
            struct.pack_into('<H', copy, 2 if code == 70 else 1, depth)
            if p['name'] == 'Item_4_0':
                copy[p['name_at']:p['name_end']] = b'Item_5_0\0'
            if p['matrix'] is not None:
                scale, rotate, (x, y) = _read_matrix(p['matrix'])
                copy[p['matrix_at']:p['matrix_end']] = _write_matrix((scale, rotate, (x, y + step)))
            result += _tag(code, copy)
        elif code == 28 and _unpack('<H', data, body)[0] == donor['depth']:
            result += _tag(28, struct.pack('<H', depth))
    output = bytearray(data[:timeline[0]] + _tag(39, result) + data[timeline[2]:])
    struct.pack_into('<I', output, 4, len(output))
    return bytes(output)


def read_fmg(data):
    """Read the little-endian, 64-bit-offset FMG version used by Bloodborne."""
    if len(data) < 0x28 or data[0] != 0 or data[2] != 2:
        raise ValueError('unsupported FMG version or byte order')
    size, = _unpack('<I', data, 4)
    groups, count = _unpack('<II', data, 0x0c)
    offsets, = _unpack('<Q', data, 0x18)
    if size != len(data) or groups > len(data) // 16 or count > len(data) // 8:
        raise ValueError('invalid FMG size or counts')
    _span(data, 0x28, groups * 16)
    _span(data, offsets, count * 8)
    if offsets < 0x28 + groups * 16:
        raise ValueError('FMG offset table overlaps its groups')
    strings = {}
    expected_index, previous = 0, -1
    for g in range(groups):
        index, first, last, _ = _unpack('<iiii', data, 0x28 + 16 * g)
        if first <= previous or last < first or index != expected_index or last - first >= count:
            raise ValueError('invalid FMG string group')
        length = last - first + 1
        if index + length > count:
            raise ValueError('FMG group runs beyond its string table')
        for k in range(length):
            at, = _unpack('<Q', data, offsets + 8 * (index + k))
            text = None
            if at:
                if at < offsets + count * 8 or at % 2:
                    raise ValueError('invalid FMG string offset')
                raw, _ = _cstr(data, at, len(data), unicode=True)
                try:
                    text = raw.decode('utf-16-le')
                except UnicodeDecodeError as exc:
                    raise ValueError('invalid FMG UTF-16 text') from exc
            strings[first + k] = text
        expected_index += length
        previous = last
    if expected_index != count:
        raise ValueError('FMG groups do not cover its string table')
    return strings


def write_fmg(template, strings):
    groups, ids = [], sorted(strings)
    for index, mid in enumerate(ids):
        if groups and groups[-1][2] == mid - 1:
            groups[-1][2] = mid
        else:
            groups.append([index, mid, mid])
    offsets = 0x28 + len(groups) * 16
    text_start = offsets + len(ids) * 8
    out, pool, slots = bytearray(template[:0x28]), bytearray(), []
    shared = {}
    for mid in ids:
        text = strings[mid]
        if text is None:
            slots.append(0)
            continue
        if text not in shared:
            shared[text] = text_start + len(pool)
            pool += text.encode('utf-16-le') + b'\0\0'
        slots.append(shared[text])
    struct.pack_into('<II', out, 0x0c, len(groups), len(ids))
    struct.pack_into('<Q', out, 0x18, offsets)
    for index, first, last in groups:
        out += struct.pack('<iiii', index, first, last, 0)
    for slot in slots:
        out += struct.pack('<Q', slot)
    out += pool
    out += b'\0' * (4 + (-(len(out) + 4) % 4))
    struct.pack_into('<I', out, 4, len(out))
    return bytes(out)


def _unpack_dcx(data):
    if (len(data) < 0x4c or data[:4] != b'DCX\0' or data[0x18:0x1c] != b'DCS\0' or
            data[0x24:0x2c] != b'DCP\0DFLT' or data[0x44:0x4c] != b'DCA\0\0\0\0\x08'):
        raise ValueError('unsupported DCX DFLT header')
    size, compressed = _unpack('>II', data, 0x1c)
    if size > MAX_ARCHIVE or compressed != len(data) - 0x4c:
        raise ValueError('invalid DCX sizes')
    try:
        stream = zlib.decompressobj()
        body = stream.decompress(data[0x4c:], MAX_ARCHIVE + 1)
    except zlib.error as exc:
        raise ValueError('invalid DCX compressed data') from exc
    if len(body) != size or not stream.eof or stream.unused_data or stream.unconsumed_tail:
        raise ValueError('DCX decompressed size or stream does not match its header')
    return body


def bnd4_files(data):
    """Bounded BND4 entries: (header offset, data offset, byte count, file id)."""
    if len(data) < 0x40 or data[:4] != b'BND4':
        raise ValueError('not a BND4 archive')
    count, = _unpack('<I', data, 0x0c)
    entry_size, = _unpack('<Q', data, 0x20)
    if entry_size != 0x24 or not count or count > (len(data) - 0x40) // entry_size:
        raise ValueError('unsupported BND4 entry size or count')
    table_end = 0x40 + count * entry_size
    entries, seen = [], set()
    for k in range(count):
        entry = 0x40 + k * entry_size
        size, raw_size, offset, mid, name = _unpack('<QQIII', data, entry + 8)
        if size != raw_size or offset < table_end or mid in seen:
            raise ValueError('compressed, overlapping, or duplicate BND4 entry')
        _span(data, offset, size)
        entries.append((entry, offset, size, mid))
        seen.add(mid)
    first = min(offset for _, offset, _, _ in entries)
    for entry, _, _, _ in entries:
        name, = _unpack('<I', data, entry + 0x20)
        if name < table_end:
            raise ValueError('BND4 filename overlaps its entry table')
        _cstr(data, name, first, unicode=bool(data[0x30]))
    previous_end = first
    for _, offset, size, _ in sorted(entries, key=lambda e: e[1]):
        if offset < previous_end:
            raise ValueError('overlapping BND4 file data')
        previous_end = offset + size
    return entries


def build_messages(archive, messages, *, exit_game=False):
    bnd = _unpack_dcx(archive)
    files = bnd4_files(bnd)
    if not {200, 201} <= {mid for _, _, _, mid in files}:
        raise ValueError('message archive lacks menu text (200) or line help (201)')
    replacements = {}
    for _, offset, size, group in files:
        if group not in (200, 201):
            continue
        template = bnd[offset:offset + size]
        strings = read_fmg(template)
        for mid, message in messages.items():
            if mid in strings:
                raise ValueError(f'message id {mid} already exists in FMG {group}')
            strings[mid] = message['help' if group == 201 else 'text'] or None
        if exit_game:
            strings[EXIT_ID] = 'Exit Game' if group == 200 else 'Close the game and return to the desktop.'
        replacements[group] = write_fmg(template, strings)
    result = bytearray(bnd[:min(offset for _, offset, _, _ in files)])
    for entry, offset, size, group in sorted(files, key=lambda e: e[1]):
        body = replacements.get(group, bnd[offset:offset + size])
        result += b'\0' * (-len(result) % 16)
        struct.pack_into('<QQI', result, entry + 8, len(body), len(body), len(result))
        result += body
    packed = zlib.compress(result, 9)
    header = bytearray(archive[:0x4c])
    struct.pack_into('>II', header, 0x1c, len(result), len(packed))
    return bytes(header) + packed


def _messages(messages):
    if not isinstance(messages, dict):
        raise ValueError('messages must be an object mapping 126xxx ids to text/help objects')
    result = {}
    for key, item in messages.items():
        try:
            mid = int(key)
        except (TypeError, ValueError) as exc:
            raise ValueError(f'invalid message id: {key!r}') from exc
        if str(mid) != str(key) or not 126000 <= mid <= 126999 or mid in result:
            raise ValueError(f'message id must be a unique 126xxx integer: {key!r}')
        if not isinstance(item, dict) or set(item) - {'text', 'help'}:
            raise ValueError(f'{mid}: expected a text/help object')
        text, help_text = item.get('text'), item.get('help', '')
        if not isinstance(text, str) or not isinstance(help_text, str) or not text:
            raise ValueError(f'{mid}: text must be nonempty and help must be a string')
        for value in (text, help_text):
            if '\0' in value:
                raise ValueError(f'{mid}: message contains a NUL character')
            try:
                value.encode('utf-16-le')
            except UnicodeEncodeError as exc:
                raise ValueError(f'{mid}: message contains invalid Unicode') from exc
        result[mid] = dict(text=text, help=help_text)
    if not {TITLE_ID, SYSTEM_ID} <= result.keys():
        raise ValueError('messages must include title 126000 and System entry 126001')
    return result


def _digest(data):
    return hashlib.sha256(data).hexdigest()


def generate(game, out, messages, *, mac_settings=True, exit_game=False):
    """Return a cached mod-layer root. Failures never publish an incomplete generation."""
    messages = _messages(messages) if mac_settings else {}
    if not mac_settings and not exit_game:
        raise ValueError('no native menu features selected')
    game, out = Path(game).resolve(), Path(out).resolve()
    dvd = (game if game.name.casefold() == 'dvdroot_ps4' else game / 'dvdroot_ps4').resolve()
    dump = dvd.parent if game.name.casefold() == 'dvdroot_ps4' else game
    if out == dump or dump in out.parents or out == dvd or dvd in out.parents:
        raise ValueError('asset cache must be outside the original game dump')
    movie = dvd / 'menu/optionsetting.gfx'
    languages = sorted((dvd / 'msg').glob('*/menu.msgbnd.dcx'))
    if not languages or (mac_settings and not movie.is_file()):
        raise ValueError('game dump lacks required menu assets or language message archives')
    movies = [movie] if mac_settings else []
    if exit_game:
        titles = [dvd / 'menu' / name for name in TITLE_MOVIES if (dvd / 'menu' / name).is_file()]
        if not titles:
            raise ValueError('game dump lacks title menu movies')
        movies += titles
    sources = {f'dvdroot_ps4/{p.relative_to(dvd).as_posix()}': p.read_bytes()
               for p in [*movies, *languages]}
    identity = dict(version=RECIPE_VERSION, generator=_digest(Path(__file__).read_bytes()),
                    messages=messages, mac_settings=mac_settings, exit_game=exit_game,
                    sources={key: _digest(data) for key, data in sources.items()})
    key = _digest(json.dumps(identity, sort_keys=True, ensure_ascii=True).encode())
    out.mkdir(parents=True, exist_ok=True, mode=0o700)
    if out.stat().st_uid != os.getuid():
        raise ValueError('asset cache must be owned by the current user')
    if stat.S_IMODE(out.stat().st_mode) & 0o077:
        raise ValueError('asset cache must be private (directory mode 0700)')
    lock_fd = os.open(out / '.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    with os.fdopen(lock_fd, 'a+b') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        target = out / key
        if target.is_symlink():
            raise ValueError('asset cache generation must not be a symlink')
        if target.exists() and not target.is_dir():
            raise ValueError('asset cache generation must be a directory')
        if target.exists():
            try:
                manifest = json.loads((target / MANIFEST).read_text())
                outputs = manifest['outputs']
                if (manifest.get('key') == key and set(outputs) == set(sources) and
                        all(not (target / p).is_symlink() and
                            _digest((target / p).read_bytes()) == outputs[p] for p in sources)):
                    return target
            except (OSError, ValueError, TypeError, KeyError):
                pass
        staging = Path(tempfile.mkdtemp(prefix='.building-', dir=out))
        try:
            outputs = {}
            for relative, data in sources.items():
                if relative.endswith('/optionsetting.gfx'):
                    body = build_movie(data)
                elif relative.endswith('.gfx'):
                    body = build_title(data)
                else:
                    body = build_messages(data, messages, exit_game=exit_game)
                destination = staging / relative
                destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
                fd = os.open(destination, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
                with os.fdopen(fd, 'wb') as stream:
                    stream.write(body)
                outputs[relative] = _digest(body)
            manifest = dict(key=key, recipe=identity, outputs=outputs)
            (staging / MANIFEST).write_text(json.dumps(manifest, sort_keys=True, indent=2) + '\n')
            (staging / MANIFEST).chmod(0o600)
            if target.exists():
                # Only this generator's content-addressed private cache is replaced.
                stale = out / ('.stale-' + key)
                if stale.exists():
                    shutil.rmtree(stale)
                target.rename(stale)
                try:
                    staging.rename(target)
                except OSError:
                    stale.rename(target)
                    raise
                shutil.rmtree(stale)
            else:
                staging.rename(target)
            return target
        finally:
            if staging.exists():
                shutil.rmtree(staging)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path, help='original game root or dvdroot_ps4')
    parser.add_argument('out', type=Path, help='private cache outside the game dump')
    parser.add_argument('messages', type=Path, help='JSON map of 126xxx message ids to text/help')
    args = parser.parse_args(argv)
    try:
        messages = json.loads(args.messages.read_text())
        print(generate(args.game, args.out, messages))
    except (OSError, ValueError) as exc:
        parser.exit(1, f'Mac Settings assets: {exc}\n')


if __name__ == '__main__':
    main()
