# SPDX-FileCopyrightText: Copyright 2026 Mark Hall
"""Exercise run.sh across a re-exec with lightweight preparation/probe stand-ins."""
from paths import ROOT
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


class RestartResolutionTests(unittest.TestCase):
    def run_restarts(self, explicit=False, live=False, ini_extra='', caps=None, bare_path=False,
                     upscaler='fsr3', output='1280x720'):
        with tempfile.TemporaryDirectory() as directory:
            data = Path(directory)
            # Preparation is unrelated to this test; allow the real patch compiler to
            # validate writes against a single ELF load segment spanning the game image.
            out = data / 'out'
            out.mkdir()
            elf = bytearray(120)
            struct.pack_into('<Q', elf, 0x20, 64)
            struct.pack_into('<HH', elf, 0x36, 56, 1)
            struct.pack_into('<IIQQQQQQ', elf, 64, 1, 0, 0, 0, 0, 0, 0x6000000, 0)
            (out / 'eboot.elf').write_bytes(elf)
            (data / 'eboot.bin').touch()
            config = data / 'bbport.ini'
            config.write_text(f'upscaler={upscaler}\npreset=1\noutput_res={output}\n' + ini_extra)
            if caps is not None:  # the GPU check next to the probe (live_resolution=auto)
                tool = data / 'bb-gpu-capabilities'
                tool.write_text(f'#!/bin/sh\necho {caps}\n')
                tool.chmod(0o755)
            python = data / 'python'
            python.write_text(f'#!{sys.executable}\n' +
                'import subprocess, sys\n'
                'if sys.argv[1] in ("scripts/patches.py", "scripts/mods.py"):\n'
                '    sys.exit(subprocess.call([sys.executable, *sys.argv[1:]]))\n')
            python.chmod(0o755)
            probe = data / 'probe'
            probe.write_text(f'#!{sys.executable}\n' +
                'import json, os\n'
                'from pathlib import Path\n'
                'config=Path(os.environ["BB_CONFIG"])\n'
                'stage=int(os.environ.get("BB_TEST_STAGE", "0"))\n'
                'with (config.parent/"environments").open("a") as f:\n'
                '    f.write(json.dumps({key:os.environ.get(key) for key in '
                '("BB_RENDER_RES", "BB_OUTPUT_RES", "BB_AUTO_RENDER_RES")})+"\\n")\n'
                'if stage<2:\n'
                '    config.write_text("upscaler=fsr3\\npreset=4\\noutput_res="+'
                '("1280x720" if stage==0 else "1920x1080")+"\\n")\n'
                '    os.environ["BB_TEST_STAGE"]=str(stage+1)\n'
                '    os.execlp("bash", "bash", "run.sh")\n')
            probe.chmod(0o755)
            env = dict(os.environ, BB_PREBUILT='1', BB_PROBE=str(probe), PYTHON=str(python),
                       BB_DATA_DIR=str(data), BB_CONFIG=str(config), BB_GAME_DIR=str(data))
            for key in ('BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AUTO_RENDER_RES', 'BB_TEST_STAGE',
                        'BB_UPSCALER'):
                env.pop(key, None)
            env.pop('BB_LIVE_RES', None)
            if explicit:
                env['BB_RENDER_RES'] = '800x450'
            if live:
                env['BB_LIVE_RES'] = '1'
            if bare_path:  # a bare PATH: coreutils and bash, no sed/grep
                tools = data / 'bin'
                tools.mkdir()
                for name in ('bash', 'dirname', 'mkdir', 'realpath'):
                    (tools / name).symlink_to(shutil.which(name))
                env['PATH'] = str(tools)
            subprocess.run([shutil.which('bash'), 'run.sh'], cwd=ROOT, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True, timeout=30)
            return [json.loads(line) for line in (data / 'environments').read_text().splitlines()]

    def test_outputs_other_than_1080p_patch_the_render_size_and_restarts_recompute_it(self):
        rows = self.run_restarts()
        # FSR 3.1: 720p Quality, 720p Ultra Performance, then 1080p Ultra Performance.
        self.assertEqual([row['BB_RENDER_RES'] for row in rows], ['854x480', '426x240', '640x360'])
        self.assertEqual([row['BB_OUTPUT_RES'] for row in rows], ['1280x720', '1280x720', '1920x1080'])
        self.assertEqual([row['BB_AUTO_RENDER_RES'] for row in rows], ['1', '1', '1'])

    def test_live_resolution_keeps_guest_sizes_native(self):
        rows = self.run_restarts(live=True)
        self.assertTrue(all(row['BB_RENDER_RES'] is None for row in rows))
        self.assertTrue(all(row['BB_OUTPUT_RES'] is None for row in rows))
        self.assertTrue(all(row['BB_AUTO_RENDER_RES'] is None for row in rows))

    def test_live_resolution_setting_and_gpu_check(self):
        # Only the first launch matters here: the probe rewrites bbport.ini for the restarts.
        self.assertIsNone(self.run_restarts(ini_extra='live_resolution=1\n')[0]['BB_RENDER_RES'])
        self.assertEqual(self.run_restarts(ini_extra='live_resolution=0\n', caps=1)[0]['BB_RENDER_RES'],
                         '854x480')
        # Unset and auto: off (the startup patch), whatever the GPU.
        self.assertEqual(self.run_restarts(caps=1)[0]['BB_RENDER_RES'], '854x480')
        self.assertEqual(self.run_restarts(ini_extra='live_resolution=auto\n', caps=1)[0]['BB_RENDER_RES'],
                         '854x480')
        self.assertEqual(self.run_restarts(ini_extra='live_resolution=auto\n', caps=0)[0]['BB_RENDER_RES'],
                         '854x480')

    def test_live_resolution_without_sed_or_grep(self):
        # A missing sed once ended run.sh (exit 127) before the game.
        self.assertEqual(self.run_restarts(ini_extra='live_resolution=0\n', caps=1,
                                           bare_path=True)[0]['BB_RENDER_RES'], '854x480')
        self.assertIsNone(self.run_restarts(ini_extra='live_resolution=1\n', caps=1,
                                            bare_path=True)[0]['BB_RENDER_RES'])

    def test_explicit_render_override_survives_restart(self):
        rows = self.run_restarts(explicit=True)
        self.assertEqual([row['BB_RENDER_RES'] for row in rows], ['800x450'] * 3)
        self.assertTrue(all(row['BB_AUTO_RENDER_RES'] is None for row in rows))

    def test_metalfx_above_1080p_launch_keeps_guest_targets_native(self):
        # #116: the 1440p Quality startup patch creates 1708x960 guest targets and the
        # scene can turn white before MetalFX. Starting off and enabling it later works
        # because that session uses native guest targets with reduced host proxies.
        for output in ('2560x1440', '3456x1944', '3840x2160'):
            with self.subTest(output=output):
                first = self.run_restarts(upscaler='metalfx', output=output,
                                          ini_extra='live_resolution=0\n',
                                          bare_path=output == '2560x1440')[0]
                self.assertEqual(first, dict(BB_RENDER_RES=None, BB_OUTPUT_RES=None,
                                             BB_AUTO_RENDER_RES=None))

    def test_metalfx_lower_outputs_keep_existing_startup_behavior(self):
        for output, render in (('1280x720', '854x480'), ('1920x1080', '1280x720')):
            with self.subTest(output=output):
                first = self.run_restarts(upscaler='metalfx', output=output)[0]
                self.assertEqual(first['BB_RENDER_RES'], render)

    def test_metalfx_guard_reads_trimmed_ini_values(self):
        first = self.run_restarts(output='2560x1440',
                                  ini_extra='  upscaler = metalfx \r\n')[0]
        self.assertIsNone(first['BB_RENDER_RES'])

    def test_metalfx_native_targets_do_not_stick_after_provider_change(self):
        rows = self.run_restarts(upscaler='metalfx', output='2560x1440')
        # The next launch selects FSR 3.1 at 720p: keep its existing startup behavior.
        self.assertEqual([row['BB_RENDER_RES'] for row in rows], [None, '426x240', '640x360'])

    def test_metalfx_explicit_render_override_remains_available(self):
        first = self.run_restarts(upscaler='metalfx', output='2560x1440', explicit=True)[0]
        self.assertEqual(first['BB_RENDER_RES'], '800x450')
        self.assertIsNone(first['BB_AUTO_RENDER_RES'])
