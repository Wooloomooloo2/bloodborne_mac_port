"""Exercise the actual x86-64 loader with a tiny module in checkout and app layouts."""
from paths import ROOT
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('clang'), 'macOS clang required')
class OnlineLoaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory(prefix='bb-online-loader-')
        cls.base = Path(cls.folder.name)
        main = cls.base / 'main.c'
        main.write_text(r'''
#include "runtime.h"
#include <stdlib.h>
#include <stdio.h>
const char *runtime_symbol(const char *name) { return name; }
int32_t runtime_thread_spawn(void **t, void *(ABI *f)(void *), void *a, uint64_t s, const char *n) {
    (void)t; (void)f; (void)a; (void)s; (void)n; return 1;
}
int32_t runtime_thread_join_spawned(void *t, void **r) { (void)t; (void)r; return 0; }
void runtime_thread_exit_spawned(void *r) { (void)r; abort(); }
int runtime_memory_is_mapped(uintptr_t a, uint64_t s) { (void)a; (void)s; return 0; }
int main(void) {
    runtime_net_configure("/fixture", "CUSA03173", "fixture", "1.09");
    printf("address=%lu\n", (unsigned long)runtime_net_resolve("sceNetInit"));
}
''')
        module = cls.base / 'module.c'
        module.write_text(r'''
#include "gpu/bbnet/bbnet.h"
#include <string.h>
int bbnet_init(const BbNetHost *host) {
    return host->abi == BBNET_ABI_VERSION && strcmp(host->serial, "CUSA03173") == 0 ? 0 : 1;
}
uintptr_t bbnet_resolve(const char *name) { return strcmp(name, "sceNetInit") == 0 ? 4660 : 0; }
''')
        flags = ['clang', '-arch', 'x86_64', '-std=c11', '-Wall', '-Wextra', '-Werror',
                 '-I', str(ROOT), '-I', str(ROOT / 'src')]
        subprocess.run(flags + [str(main), str(ROOT / 'src/runtime_net.c'), '-o', str(cls.base / 'probe')], check=True)
        subprocess.run(flags + ['-dynamiclib', str(module), '-o', str(cls.base / 'module.dylib')], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def run_layout(self, name, library=None, *, enabled=True, forced=None):
        folder = self.base / name
        exe = folder / 'Contents/MacOS/bb-probe'
        exe.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(self.base / 'probe', exe)
        if library:
            target = folder / library
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(self.base / 'module.dylib', target)
        env = os.environ.copy()
        env['BB_ONLINE'] = '1' if enabled else '0'
        env.pop('BB_NET_MODULE', None)
        if forced:
            env['BB_NET_MODULE'] = str(forced)
        return subprocess.check_output([str(exe)], env=env, text=True)

    def test_checkout_module(self):
        self.assertIn('address=4660', self.run_layout('checkout', 'Contents/MacOS/gpu/libbbnet.dylib'))

    def test_bundled_module(self):
        self.assertIn('address=4660', self.run_layout('packaged app', 'Contents/Frameworks/libbbnet.dylib'))

    def test_explicit_module_and_missing_override(self):
        self.assertIn('address=4660', self.run_layout('forced', forced=self.base / 'module.dylib'))
        text = self.run_layout('bad-override', 'Contents/Frameworks/libbbnet.dylib', forced=self.base / 'missing')
        self.assertIn('address=0', text)  # an explicit override must not silently use another module

    def test_offline_and_missing_module(self):
        self.assertEqual(self.run_layout('offline', 'Contents/Frameworks/libbbnet.dylib', enabled=False), 'address=0\n')
        self.assertIn('address=0', self.run_layout('no-module'))
