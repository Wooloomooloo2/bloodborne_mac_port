"""Preparation/runtime TLS contract: synthetic images, never game files."""
from paths import EXE
from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import link_libc
import link_modules
import test_probe
from test_probe import native_package, package


def linked_package(version, native=b'\x0f\x0b'):
    """The same owned native fixture in each supported linked header layout."""
    # Trap at the native export proves admission, init and binding independently
    # of the runtime's policy for otherwise unsupported imports.
    data = bytearray(native_package(native=native))
    if version == 4:
        data[128:128] = bytes(32)  # BBPROBE4 adds the main TLS tuple.
    elif version == 5:
        base, size, init, tls, memsz, filesz, bindings, procparam = struct.unpack_from('<8Q', data, 64)
        metadata = struct.pack('<6Q', procparam, 0, 0, 0, 0, 1)
        metadata += struct.pack('<8Q', base, size, init, tls, memsz, filesz, 2, bindings)
        data[64:128] = metadata
    data[:8] = f'BBPROBE{version}'.encode()
    return data


class SlotPreparationTests(unittest.TestCase):
    def test_default_and_matching_overrides_emit_the_runtime_slot(self):
        for platform, slot in [('darwin', 384), ('linux', 0)]:
            with self.subTest(platform=platform), patch.object(link_libc.sys, 'platform', platform):
                with patch.dict(os.environ, {}, clear=True):
                    default = link_libc.gs_load()
                    self.assertEqual(default, link_libc.gs_load(slot))
                with patch.dict(os.environ, {'BBPORT_GS_SLOT': hex(slot)}):
                    self.assertEqual(default, link_libc.gs_load())
                self.assertEqual(struct.unpack_from('<I', default, 5)[0], slot * 8)

    def test_explicit_valid_slot_overrides_invalid_environment(self):
        with patch.object(link_libc.sys, 'platform', 'darwin'), \
                patch.dict(os.environ, {'BBPORT_GS_SLOT': '385'}):
            self.assertEqual(link_libc.gs_load(384)[5:], struct.pack('<I', 384 * 8))

    def test_both_linkers_reject_mismatch_before_reading_inputs(self):
        for linker in (link_libc, link_modules):
            for slot in (0, 383, 385, 511):
                with self.subTest(linker=linker.__name__, slot=slot), \
                        patch.object(link_libc.sys, 'platform', 'darwin'), \
                        patch.object(linker, 'module') as reader:
                    with self.assertRaisesRegex(ValueError, 'does not match the runtime slot 384'):
                        linker.link(Path('unused-game'), Path('unused-output'), gs_slot=slot)
                    reader.assert_not_called()
            with patch.object(link_libc.sys, 'platform', 'darwin'), \
                    patch.dict(os.environ, {'BBPORT_GS_SLOT': '385'}), \
                    patch.object(linker, 'module') as reader:
                with self.assertRaisesRegex(ValueError, 'does not match the runtime slot 384'):
                    linker.link(Path('unused-game'), Path('unused-output'))
                reader.assert_not_called()

    def test_out_of_range_and_invalid_environment_slots_are_rejected(self):
        for slot in (-1, 512, 1 << 32):
            with self.subTest(slot=slot), self.assertRaisesRegex(ValueError, 'outside the pthread key range'):
                link_libc.gs_load(slot)
        with patch.dict(os.environ, {'BBPORT_GS_SLOT': 'invalid'}):
            with self.assertRaises(ValueError):
                link_libc.gs_load()

    def test_cli_and_environment_mismatch_leave_existing_output_untouched(self):
        # The actual CLI validates before trying to read missing game inputs.
        incompatible = 385 if sys.platform == 'darwin' else 1
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            for script, output in [('link_libc.py', 'boot-libc.bin'),
                                   ('link_modules.py', 'boot-linked.bin')]:
                target = out / output
                target.write_bytes(b'existing preparation')
                for explicit in (False, True):
                    cmd = [sys.executable, str(Path(link_libc.__file__).with_name(script)),
                           str(out / 'missing-game'), '--out', str(out)]
                    env = dict(os.environ, BBPORT_GS_SLOT=str(incompatible))
                    if explicit:
                        cmd += ['--gs-slot', str(incompatible)]
                        env.pop('BBPORT_GS_SLOT')
                    with self.subTest(script=script, explicit=explicit):
                        result = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=5)
                        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                        self.assertIn('does not match the runtime slot', result.stderr)
                        self.assertNotIn('FileNotFoundError', result.stderr)
                        self.assertEqual(target.read_bytes(), b'existing preparation')


@unittest.skipUnless(EXE.exists(), 'build native probe first')
class SlotAdmissionTests(unittest.TestCase):
    run_image = test_probe.LoaderTests.run_image

    def assert_refused_before_entry(self, data, message, *options):
        result = self.run_image(data, *options)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(message, result.stderr)
        self.assertNotIn('Entering original', result.stdout)
        self.assertNotIn('Module 0 initializer returned', result.stdout)

    def test_matching_slot_still_initializes_and_calls_native_export(self):
        for version in (3, 4, 5):
            with self.subTest(version=version):
                result = self.run_image(linked_package(version))
                self.assertEqual(result.returncode, 132, result.stdout + result.stderr)
                self.assertIn('Module 0 initializer returned 0', result.stdout)
                self.assertIn('at guest offset 0x1010,', result.stderr)

    def test_emitted_load_reads_the_attached_guest_tcb(self):
        # GS load -> rax; compare TCB.self with that pointer. A matching self
        # reaches the first UD2, while a mismatch reaches the second one.
        native = link_libc.gs_load() + bytes.fromhex('488b084839c175020f0b0f0b')
        result = self.run_image(linked_package(5, native=native))
        self.assertEqual(result.returncode, 132, result.stdout + result.stderr)
        self.assertIn('at guest offset 0x1021,', result.stderr)

    def test_cached_image_for_another_slot_is_refused_even_in_strict_mode(self):
        expected = 384 * 8 if sys.platform == 'darwin' else 0
        for slot in (0, 383, 384, 385, 511):
            if slot * 8 == expected:
                continue
            data = bytearray(native_package(init=b'\x0f\x0b'))
            struct.pack_into('<Q', data, 56, slot * 8)
            for options in ((), ('--strict-imports',)):
                with self.subTest(slot=slot, options=options):
                    self.assert_refused_before_entry(data, 'does not match this runtime', *options)

    def test_legacy_linked_cache_requires_preparation_on_macos(self):
        for version in (3, 4, 5):
            data = linked_package(version)
            struct.pack_into('<Q', data, 48, 1)
            del data[56:64]
            with self.subTest(version=version):
                if sys.platform == 'darwin':
                    for options in ((), ('--strict-imports',)):
                        self.assert_refused_before_entry(data, 'lacks TLS slot metadata; prepare the game again',
                                                         *options)
                else:
                    result = self.run_image(data)
                    self.assertEqual(result.returncode, 132, result.stdout + result.stderr)

    def test_truncated_slot_word_is_refused(self):
        data = native_package()
        for length in range(56, 64):
            with self.subTest(length=length):
                self.assert_refused_before_entry(data[:length], 'truncated boot file')

    def test_displacement_bounds_and_alignment_are_checked(self):
        for displacement in (1, 3073, 4095, 4096, (1 << 64) - 1):
            with self.subTest(displacement=displacement):
                data = bytearray(native_package())
                struct.pack_into('<Q', data, 56, displacement)
                self.assert_refused_before_entry(data, 'invalid boot TLS displacement')

    def test_unknown_flags_and_unlinked_slot_metadata_are_refused(self):
        data = bytearray(native_package())
        struct.pack_into('<Q', data, 48, 7)
        self.assert_refused_before_entry(data, 'unknown runtime capabilities')
        self.assert_refused_before_entry(package(b'\xc3', capabilities=2),
                                         'TLS slot metadata requires a linked image')


if __name__ == '__main__':
    unittest.main()
