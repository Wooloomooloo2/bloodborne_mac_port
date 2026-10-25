"""Stop the launcher's own SDL child through run.sh, with and without a mod view.

Uses synthetic files and a small real SDL probe, never game data or saves. Game preparation
is stubbed; run.sh, mods.py, GameRunner.start and Foundation Process.terminate are real.
"""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin', 'macOS launcher lifecycle')
class LauncherStopTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='bb-launcher-stop-')
        cls.root = Path(cls.temp.name)
        cls.probe = cls.root / 'probe'
        source = cls.root / 'probe.c'
        source.write_text(r'''
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (!SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) return 2;
    const char *app0 = "";
    for (int i=1;i+1<argc;i++) if (!strcmp(argv[i],"--app0")) app0=argv[i+1];
    FILE *f=fopen(getenv("BB_STOP_TEST_READY"),"w");
    if (!f) return 3;
    fprintf(f,"%d\n%s\n",getpid(),app0); fclose(f);
    for (;;) sleep(1);
}
''')
        sdk = os.environ.get('SDKROOT') or subprocess.check_output(
            ['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
        deps = ROOT / 'deps/x86_64'
        subprocess.run(['/usr/bin/clang', '-arch', 'x86_64', '-isysroot', sdk,
                        '-I' + str(deps / 'include'), str(source), '-L' + str(deps / 'lib'),
                        '-lSDL3', '-Wl,-rpath,' + str(deps / 'lib'), '-o', str(cls.probe)], check=True)
        swift = cls.root / 'main.swift'
        swift.write_text('''
import Foundation
let env = ProcessInfo.processInfo.environment
let ready = env["BB_STOP_TEST_READY"]!
let root = URL(fileURLWithPath: CommandLine.arguments[1])
let plan = LaunchPlan(executable: URL(fileURLWithPath: "/bin/bash"),
    arguments: [root.appendingPathComponent("run.sh").path], environment: env, directory: root)
let logs = URL(fileURLWithPath: ready + ".logs")
let process = try GameRunner.start(plan, log: try SessionLogWriter(folder: logs, appVersion: "test")) { _ in }
let startDeadline = Date().addingTimeInterval(10)
while !FileManager.default.fileExists(atPath: ready) && process.isRunning && Date() < startDeadline {
    RunLoop.current.run(until: Date().addingTimeInterval(0.02))
}
guard FileManager.default.fileExists(atPath: ready) else { exit(2) }
process.terminate()
let stopDeadline = Date().addingTimeInterval(5)
while process.isRunning && Date() < stopDeadline {
    RunLoop.current.run(until: Date().addingTimeInterval(0.02))
}
guard !process.isRunning else { exit(3) }
print("Stopped launcher child: \\(process.terminationStatus)")
''')
        cls.harness = cls.root / 'launcher-stop'
        subprocess.run(['swiftc', '-target', 'arm64-apple-macos15.0',
                        *map(str, sorted((ROOT / 'launcher/Model').glob('*.swift'))),
                        str(ROOT / 'launcher/App/GameRunner.swift'),
                        str(ROOT / 'launcher/App/SessionLogWriter.swift'), str(swift),
                        '-o', str(cls.harness)], check=True)
        # Use the real mod builder and EXIT-trap cleanup. Skip only game preparation.
        cls.python = cls.root / 'preparation-python'
        cls.python.write_text('#!' + sys.executable + '\n' + '''import os, sys
if sys.argv[1] == '-c' or sys.argv[1].endswith('/mods.py'):
    os.execv(sys.executable, [sys.executable, *sys.argv[1:]])
''')
        cls.python.chmod(0o755)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_stop(self, with_mod):
        folder = self.root / ('mod' if with_mod else 'plain')
        game = folder / 'game'
        asset = game / 'dvdroot_ps4/script/test.txt'
        asset.parent.mkdir(parents=True)
        asset.write_text('original')
        (game / 'eboot.bin').write_bytes(b'synthetic, never executed')
        mods = folder / 'mods'
        if with_mod:
            mod = mods / 'test/dvdroot_ps4/script/test.txt'
            mod.parent.mkdir(parents=True)
            mod.write_text('replacement')
        data = folder / 'data'
        data.mkdir()
        ready = folder / 'ready'
        # Do not inherit developer/game flags from the caller.
        env = {k: v for k, v in os.environ.items() if not k.startswith(('BB_', 'SDL_'))}
        env.update(BB_GAME_DIR=str(game), BB_DATA_DIR=str(data), BB_CONFIG=str(data / 'bbport.ini'),
                   BB_MODS_DIR=str(mods), BB_MODS_CONFIG=str(data / 'mods.json'),
                   BB_PREBUILT='1', BB_PROBE=str(self.probe), BB_STOP_TEST_READY=str(ready),
                   PYTHON=str(self.python), SDL_AUDIODRIVER='dummy')
        proc = subprocess.Popen([str(self.harness), str(ROOT)], env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, start_new_session=True)
        try:
            output, _ = proc.communicate(timeout=20)
            logs = ready.with_name(ready.name + '.logs')
            text = ''.join(p.read_text() for p in sorted(logs.glob('game-*.log'))) if logs.is_dir() else ''
            self.assertEqual(proc.returncode, 0, output + text)
            pid, view = ready.read_text().splitlines()
            with self.assertRaises(ProcessLookupError):
                os.kill(int(pid), 0)
            if with_mod:
                self.assertNotEqual(Path(view).resolve(), game.resolve())
                self.assertFalse(Path(view).exists(), 'merged view must be removed after child exits')
                self.assertEqual(mod.read_text(), 'replacement')
            else:
                self.assertEqual(Path(view).resolve(), game.resolve())
            self.assertEqual(asset.read_text(), 'original')
        finally:
            # Test-only process group: also clean up a deliberately failing regression.
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.wait()

    def test_stop_direct_exec_child(self):
        self.run_stop(False)

    def test_stop_mod_child_before_removing_merged_view(self):
        self.run_stop(True)


if __name__ == '__main__':
    unittest.main()
