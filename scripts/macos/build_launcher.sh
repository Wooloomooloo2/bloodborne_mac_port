#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Mark Hall
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds the native launcher (launcher/): out/launcher/Bloodborne, the app's main executable
# (scripts/macos/package_app.sh, make_app.sh). --test also builds and runs the model tests.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
out=out/launcher
mkdir -p "$out"
target=(-target arm64-apple-macos15.0)
model=(launcher/Model/*.swift)
if compgen -G 'launcher/App/*.swift' >/dev/null; then
    swiftc -O "${target[@]}" "${model[@]}" launcher/App/*.swift -o "$out/Bloodborne"
fi
if [[ ${1:-} == --test ]]; then
    swiftc "${target[@]}" "${model[@]}" launcher/Tests/main.swift -o "$out/launcher-tests"
    "$out/launcher-tests"
fi
