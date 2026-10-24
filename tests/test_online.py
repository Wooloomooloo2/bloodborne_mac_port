# SPDX-FileCopyrightText: Copyright 2026 Mark Hall
# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/online.py: the community address rule, the sign-in's limits, keychain quoting and the
owner-only bbport.ini (no network, no keychain)."""
from paths import ROOT  # noqa: F401 (puts scripts/ on the path)
import os
import stat
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import online


class CommunityAddressTests(unittest.TestCase):
    def test_https_and_defaults(self):
        self.assertEqual(online.community_address(None), 'https://thehuntersdream.com')
        self.assertEqual(online.community_address('  '), 'https://thehuntersdream.com')
        self.assertEqual(online.community_address('example.org'), 'https://example.org')
        self.assertEqual(online.community_address('HTTPS://example.org:20443/bb-eu/ss.info'),
                         'https://example.org')

    def test_plain_http_only_for_this_mac(self):
        self.assertEqual(online.community_address('http://localhost:8080'), 'http://localhost')
        self.assertEqual(online.community_address('http://127.0.0.1/'), 'http://127.0.0.1')
        for bad in ('http://example.org', 'ftp://example.org', 'http://localhost.example.org', 'https://'):
            with self.assertRaises(ValueError, msg=bad):
                online.community_address(bad)

    def test_sign_in_page_on_the_community_host(self):
        base = 'https://example.org'
        self.assertTrue(online.on_community_host('https://example.org/link', base))
        self.assertTrue(online.on_community_host('https://www.example.org/link', base))
        for bad in ('http://example.org/link', 'https://example.org.evil.test/', 'https://evil.test/',
                    'javascript:alert(1)', None, ''):
            self.assertFalse(online.on_community_host(bad, base), bad)


class SignInTests(unittest.TestCase):
    def sign_in(self, start, polls):
        replies = iter([start] + polls)
        pages, sleeps = [], []
        with mock.patch.object(online, 'thd_call', lambda url, body: next(replies)), \
                mock.patch.object(online.time, 'sleep', sleeps.append):
            result = online.thd_sign_in('https://example.org', lambda page, code: pages.append(page))
        return result, pages, sleeps

    def test_limits_and_page(self):
        start = {'DeviceCode': 'd', 'UserCode': 'C', 'Interval': 10 ** 12, 'ExpiresIn': 10 ** 12,
                 'VerificationUri': 'https://evil.test/link'}
        approved = {'Status': 'approved', 'OnlineId': 'dreamer', 'Token': 'tok'}
        result, pages, sleeps = self.sign_in(start, [{'Status': 'slow_down'}, approved])
        self.assertEqual(result, ('dreamer', 'tok'))
        self.assertEqual(pages, ['https://example.org/link'])  # not the reply's foreign page
        self.assertEqual(sleeps, [60, 60])                     # clamped, slow_down too

    def test_invalid_online_id(self):
        start = {'DeviceCode': 'd', 'UserCode': 'C', 'Interval': 1}
        for bad in ('ab', 'a' * 17, 'name\nthd_token=x', 'sp ace'):
            with self.assertRaises(ValueError, msg=bad):
                self.sign_in(start, [{'Status': 'approved', 'OnlineId': bad, 'Token': 'tok'}])


class KeychainAndIniTests(unittest.TestCase):
    def test_security_quote(self):
        self.assertEqual(online.security_quote('a b'), '"a b"')
        self.assertEqual(online.security_quote('a"b\\c'), '"a\\"b\\\\c"')
        with self.assertRaises(ValueError):
            online.security_quote('two\nlines')

    def test_secret_in_clear_is_owner_only(self):
        with tempfile.TemporaryDirectory() as folder:
            ini = Path(folder) / 'bbport.ini'
            old = os.umask(0o022)
            try:
                online.write_ini_keys(ini, {'online': '1'})
                self.assertEqual(stat.S_IMODE(ini.stat().st_mode), 0o644)
                online.write_ini_account(ini, 'dreamer', 'secret-token')
                self.assertEqual(stat.S_IMODE(ini.stat().st_mode), 0o600)
                ini.unlink()
                online.write_ini_account(ini, 'dreamer', 'secret-token')  # a new file: 0600 from the start
                self.assertEqual(stat.S_IMODE(ini.stat().st_mode), 0o600)
                self.assertIn('thd_token=secret-token', ini.read_text())
            finally:
                os.umask(old)


if __name__ == '__main__':
    unittest.main()
