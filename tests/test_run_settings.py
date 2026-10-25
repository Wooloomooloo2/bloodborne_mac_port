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
                     next_configs=None, env_extra=None):
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
            config.write_text('upscaler=fsr3\npreset=1\noutput_res=1280x720\n' + ini_extra)
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
                'import json, os, sys\n'
                'from pathlib import Path\n'
                'config=Path(os.environ["BB_CONFIG"])\n'
                'stage=int(os.environ.get("BB_TEST_STAGE", "0"))\n'
                'with (config.parent/"environments").open("a") as f:\n'
                '    row={key:os.environ.get(key) for key in '
                '("BB_RENDER_RES", "BB_OUTPUT_RES", "BB_AUTO_RENDER_RES", "BB_FPS", "BB_FPS_LIMIT", "BB_VBLANK_HZ")}\n'
                '    row["args"]=sys.argv[1:]\n'
                '    f.write(json.dumps(row)+"\\n")\n'
                f'next_configs={next_configs!r}\n'
                'if stage<2:\n'
                '    config.write_text(next_configs[stage] if next_configs else "upscaler=fsr3\\npreset=4\\noutput_res="+'
                '("1280x720" if stage==0 else "1920x1080")+"\\n")\n'
                '    os.environ["BB_TEST_STAGE"]=str(stage+1)\n'
                '    os.execlp("bash", "bash", "run.sh")\n')
            probe.chmod(0o755)
            env = dict(os.environ, BB_PREBUILT='1', BB_PROBE=str(probe), PYTHON=str(python),
                       BB_DATA_DIR=str(data), BB_CONFIG=str(config), BB_GAME_DIR=str(data))
            for key in ('BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AUTO_RENDER_RES', 'BB_TEST_STAGE'):
                env.pop(key, None)
            env.pop('BB_LIVE_RES', None)
            for key in ('BB_FPS', 'BB_FPS_AUTO', 'BB_FPS_LIMIT', 'BB_VBLANK_HZ', 'BB_FPS_PATCH',
                        'BB_LAUNCH_AUTO_LIMIT', 'BB_LAUNCH_AUTO_VBLANK'):
                env.pop(key, None)
            env.update(env_extra or {})
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
        # 720p Quality, 720p Ultra Performance after a restart, then 1080p (live host targets).
        # macOS also applies the startup patch at 1080p (patches.scaled_sizes).
        mac = sys.platform == 'darwin'
        self.assertEqual([row['BB_RENDER_RES'] for row in rows], ['854x480', '426x240', '640x360' if mac else None])
        self.assertEqual([row['BB_OUTPUT_RES'] for row in rows], ['1280x720', '1280x720', '1920x1080' if mac else None])
        self.assertEqual([row['BB_AUTO_RENDER_RES'] for row in rows], ['1', '1', '1' if mac else None])

    def test_adaptive_timing_recomputes_caps_and_switches_back_on_restart(self):
        rows=self.run_restarts(ini_extra='fps_cap=60\ngame_timing=adaptive\n', next_configs=[
            'fps_cap=120\ngame_timing=adaptive\n', 'fps_cap=30\ngame_timing=fixed\n'])
        self.assertEqual([r['BB_FPS'] for r in rows], ['adaptive','adaptive','30'])
        self.assertEqual([r['BB_FPS_LIMIT'] for r in rows], ['60','120','30'])
        self.assertEqual([r['BB_VBLANK_HZ'] for r in rows], ['0','0','60'])
        for row, cap in zip(rows[:2], ('60','120')):
            at=row['args'].index('--adaptive-fps')
            self.assertEqual(row['args'][at+1],cap)
        self.assertNotIn('--adaptive-fps',rows[2]['args'])

    def test_adaptive_explicit_overrides_and_disable(self):
        for overrides, expected in [({'BB_FPS_LIMIT':'144'},'144'), ({'BB_FPS_LIMIT':'0'},'0'),
                                     ({'BB_FPS':'uncap'},None), ({'BB_FPS_PATCH':'0'},None)]:
            with self.subTest(overrides=overrides):
                row=self.run_restarts(ini_extra='fps_cap=60\ngame_timing=adaptive\n',env_extra=overrides)[0]
                if expected is None: self.assertNotIn('--adaptive-fps',row['args'])
                else:
                    at=row['args'].index('--adaptive-fps')
                    self.assertEqual(row['args'][at+1],expected)

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
