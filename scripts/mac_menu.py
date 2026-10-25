#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Add the generated native menu after user mods, without modifying either source."""
import json
import os
from pathlib import Path
import sys

from mac_menu_assets import generate
from mods import build_overlay


def prepare(game, out):
    messages = json.loads(Path(__file__).with_name('mac_menu_messages.json').read_text())
    layer = generate(game, Path(out) / 'mac-menu-cache', messages,
                     mac_settings=os.environ.get('BB_MAC_SETTINGS', '1') != '0',
                     exit_game=os.environ.get('BB_EXIT_GAME', '1') != '0')
    return build_overlay(Path(game), Path(out), [('Mac Settings', layer)])


if __name__ == '__main__':
    try:
        print(prepare(sys.argv[1], sys.argv[2]))
    except (OSError, ValueError, KeyError) as error:
        print(f'Native menu assets unavailable: {error}', file=sys.stderr)
        sys.exit(1)
