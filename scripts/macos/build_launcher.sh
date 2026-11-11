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
# From the macOS 27 SDK on, SwiftUI's property wrappers (@State, ...) are macros, and their
# compiler plugin (SwiftUIMacros) comes with Xcode, not with the Command Line Tools. Without it,
# build against the newest installed SDK that still compiles them (Command Line Tools 27 also
# carry the 26.x SDKs); an SDKROOT set by the caller is kept.
swiftui_compiles() { # [sdk]
    local probe=$out/swiftui-probe.swift
    printf 'import SwiftUI\nstruct Probe: View { @State var n = 0\n var body: some View { Text("\\(n)") } }\n' >"$probe"
    if [[ -n ${1:-} ]]; then
        SDKROOT=$1 swiftc -typecheck "${target[@]}" "$probe" 2>/dev/null
    else
        swiftc -typecheck "${target[@]}" "$probe" 2>/dev/null
    fi
}
if [[ -z ${SDKROOT:-} ]] && compgen -G 'launcher/App/*.swift' >/dev/null && ! swiftui_compiles; then
    sdks=$(dirname -- "$(xcrun --show-sdk-path)")
    found=''
    for sdk in $(cd "$sdks" && ls -d MacOSX[0-9]*.sdk 2>/dev/null | sort -V -r); do
        if swiftui_compiles "$sdks/$sdk"; then
            found=$sdks/$sdk
            break
        fi
    done
    if [[ -z $found ]]; then
        echo "The launcher needs SwiftUI's macro plugin, which this SDK lacks: install Xcode," \
            "or Command Line Tools that include a macOS 26 SDK." >&2
        exit 1
    fi
    echo "Launcher: $(xcrun --show-sdk-version) SDK has no SwiftUI macro plugin (Command Line" \
        "Tools); building with ${found##*/}" >&2
    export SDKROOT=$found
fi
if compgen -G 'launcher/App/*.swift' >/dev/null; then
    swiftc -O "${target[@]}" "${model[@]}" launcher/App/*.swift -o "$out/Bloodborne"
fi
if [[ ${1:-} == --test ]]; then
    swiftc "${target[@]}" "${model[@]}" launcher/Tests/main.swift -o "$out/launcher-tests"
    "$out/launcher-tests"
fi
