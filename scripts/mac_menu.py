#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Add the generated native menu after user mods, without modifying either source."""
import json
from pathlib import Path
import sys

from mac_menu_assets import generate
from mods import build_overlay


def prepare(game, out):
    messages = json.loads(Path(__file__).with_name('mac_menu_messages.json').read_text())
    layer = generate(game, Path(out) / 'mac-menu-cache', messages)
    return build_overlay(Path(game), Path(out), [('Mac Settings', layer)])


if __name__ == '__main__':
    try:
        print(prepare(sys.argv[1], sys.argv[2]))
    except (OSError, ValueError, KeyError) as error:
        print(f'Mac Settings assets unavailable: {error}', file=sys.stderr)
        sys.exit(1)
