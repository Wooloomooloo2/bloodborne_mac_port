#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Mark Hall
# Builds dist/Bloodborne.app and dist/Bloodborne-macOS-<version>.zip: a self-contained app for
# release downloads (unlike scripts/macos/make_app.sh, which makes a launcher for this checkout).
# Contains the built port (x86-64 game process, arm64 GPU host), universal copies of the
# libraries, the launch scripts and a standalone Python for them. No Vulkan loader or MoltenVK:
# the GPU host renders with Metal, and packaging stops if a binary still imports a vk* function.
# No game files: the player picks their own game folder on first launch. Ad-hoc signed (not
# notarised): downloaded copies are quarantined, see the README.
#
# usage: bash scripts/macos/package_app.sh [version]     (build first: bash build_macos.sh)
#
# Layout of Bloodborne.app/Contents:
#   MacOS/Bloodborne              launcher window (Swift, launcher/): settings, folders, Play
#   MacOS/bb-probe                game process (x86-64, Rosetta 2)
#   MacOS/bbgpu-host              GPU process (arm64; owns the window, so the Dock shows the app)
#   MacOS/bbgpu-host-macos27      the same with the guest range as an exec-time segment (macOS 27+)
#   Frameworks/*.dylib            universal: libbbgpu, SDL3, FFmpeg
#   Resources/port/               run.sh, scripts/*.py, patches/
#   Resources/python/             standalone CPython (arm64) for the scripts
#   Resources/licenses/           GPL text and third-party notices
#   Resources/benchmark/          the benchmark's save and route (benchmark/ in the repository)
#                                 (docs/DESIGN_BENCHMARK.md; not in git)
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
repo=$PWD
version=${1:-$(git describe --tags --match 'mac-v*' 2>/dev/null | sed 's/^mac-v//' || echo dev)}

# Standalone Python (python-build-standalone, PSF licence). Pinned: name and SHA-256.
py_release=20260924
py_name=cpython-3.12.14+20260924-aarch64-apple-darwin-install_only_stripped.tar.gz
py_sha256=c2edb321cd32ec2b170df208db0446dccc4398db602ca27cf2079098fb1f7d9d

for f in out/bb-probe out/gpu/libbbgpu.dylib out/bbgpu-host out/bbgpu-host-macos27 out/gpu-arm64/libbbgpu.dylib; do
    [[ -f $f ]] || { echo "Missing $f: build first (bash build_macos.sh)." >&2; exit 1; }
done

dist=$repo/dist
app=$dist/Bloodborne.app
contents=$app/Contents
rm -rf "$app"
mkdir -p "$contents/MacOS" "$contents/Frameworks" "$contents/Resources/port/scripts" "$contents/Resources/licenses"

# Executables: library search goes to Frameworks/ only (the build's rpaths name this checkout).
install -m755 out/bb-probe out/bbgpu-host out/bbgpu-host-macos27 "$contents/MacOS/"
set_rpath() { # file new-rpath
    local file=$1 path
    while read -r path; do
        install_name_tool -delete_rpath "$path" "$file" 2>/dev/null || true
    done < <(otool -l "$file" | awk '/cmd LC_RPATH/ { getline; getline; print $2 }')
    install_name_tool -add_rpath "$2" "$file"
}
for exe in bb-probe bbgpu-host bbgpu-host-macos27; do
    set_rpath "$contents/MacOS/$exe" @executable_path/../Frameworks
done

# Libraries: one universal file each (the x86-64 slice for bb-probe, arm64 for bbgpu-host).
universal() { # name x86_64-file arm64-file
    lipo -create "$2" "$3" -output "$contents/Frameworks/$1"
    chmod 755 "$contents/Frameworks/$1"
}
universal libbbgpu.dylib out/gpu/libbbgpu.dylib out/gpu-arm64/libbbgpu.dylib
set_rpath "$contents/Frameworks/libbbgpu.dylib" @loader_path
for lib in libSDL3.0 libavformat.61 libavcodec.61 libswscale.8 libswresample.5 libavutil.59; do
    universal "$lib.dylib" "deps/x86_64/lib/$lib.dylib" "deps/arm64/lib/$lib.dylib"
done
# Every @rpath dependency must be in the bundle, and nothing may call Vulkan (Metal only).
for file in "$contents"/MacOS/* "$contents"/Frameworks/*; do
    [[ $file == */MacOS/Bloodborne ]] && continue
    for arch in x86_64 arm64; do
        lipo -info "$file" | grep -q "$arch" || continue
        while read -r dep; do
            [[ -f $contents/Frameworks/${dep#@rpath/} ]] || { echo "$file ($arch) needs $dep, not bundled" >&2; exit 1; }
        done < <(otool -arch "$arch" -L "$file" | awk 'NR > 1 && $1 ~ /^@rpath\// { print $1 }')
        if otool -arch "$arch" -L "$file" | awk 'NR > 1 { print $1 }' | grep -vE '^(@rpath/|/System/|/usr/lib/)'; then
            echo "$file ($arch) links a library outside the bundle and the system" >&2; exit 1
        fi
        if nm -u -arch "$arch" "$file" 2>/dev/null | grep -q '^_vk'; then
            echo "$file ($arch) imports Vulkan functions" >&2; exit 1
        fi
    done
done

# Launch scripts and patches (run.sh runs them; BB_PREBUILT=1 skips building).
install -m755 run.sh "$contents/Resources/port/"
install -m644 scripts/*.py "$contents/Resources/port/scripts/"
install -m644 scripts/mac_menu_messages.json "$contents/Resources/port/scripts/"
install -m644 scripts/launch_settings.sh "$contents/Resources/port/scripts/"
cp -R patches "$contents/Resources/port/patches"
if [[ -f benchmark/route.pad && -d benchmark/savedata ]]; then
    mkdir -p "$contents/Resources/benchmark"
    cp -R benchmark/route.pad benchmark/savedata "$contents/Resources/benchmark/"
else
    echo "No benchmark/ folder: the app's Benchmark button will be unavailable."
fi

# Python.
cache=$repo/deps/cache
mkdir -p "$cache"
if [[ ! -f $cache/$py_name ]]; then
    curl -fsSL --retry 3 -o "$cache/$py_name.part" \
        "https://github.com/astral-sh/python-build-standalone/releases/download/$py_release/${py_name//+/%2B}"
    mv "$cache/$py_name.part" "$cache/$py_name"
fi
echo "$py_sha256  $cache/$py_name" | shasum -a 256 -c - >/dev/null || { echo "Python archive checksum mismatch" >&2; exit 1; }
tar -xzf "$cache/$py_name" -C "$contents/Resources"
py=$contents/Resources/python
# Only the standard library the scripts use stays (about 20 MB instead of 66).
rm -rf "$py/include" "$py/share" "$py/bin/idle"* "$py/bin/pip"* "$py/bin/pydoc"* "$py/bin/2to3"* \
    "$py/bin/python3-config" "$py/bin/python3.12-config" "$py"/lib/libtcl* "$py"/lib/libtk* "$py"/lib/tcl* "$py"/lib/tk* \
    "$py"/lib/itcl* "$py"/lib/thread* "$py/lib/pkgconfig"
stdlib=$py/lib/python3.12
rm -rf "$stdlib"/{test,idlelib,tkinter,turtledemo,ensurepip,lib2to3,pydoc_data,site-packages/*} \
    "$stdlib"/config-3.12-darwin "$stdlib"/lib-dynload/_tkinter* "$stdlib"/lib-dynload/_test* \
    "$stdlib"/lib-dynload/_ctypes_test* "$stdlib"/lib-dynload/xxlimited*
find "$py" -name __pycache__ -type d -prune -exec rm -rf {} +
"$py/bin/python3" -c 'import argparse, base64, collections, hashlib, json, pathlib, shutil, struct, tempfile, xml.etree.ElementTree'

# Licences: the port's (GPL), Python's and the bundled or compiled-in third-party code.
lic=$contents/Resources/licenses
cp LICENSE "$lic/LICENSE-bloodborne-mac-port.txt"
cp LICENSES/GPL-3.0-or-later.txt "$lic/LICENSE-native-menu.txt"
cp docs/NATIVE_MAC_SETTINGS.md "$lic/native-menu-provenance.txt"
cp "$stdlib/LICENSE.txt" "$lic/LICENSE-Python.txt"
for pair in SDL:LICENSE.txt Vulkan-Headers:LICENSE.md SPIRV-Cross:LICENSE FFmpeg:COPYING.LGPLv2.1 \
            FFmpeg:LICENSE.md fmt:LICENSE magic_enum:LICENSE miniz:LICENSE robin-map:LICENSE \
            xxHash:LICENSE; do
    cp "deps/src/${pair%%:*}/${pair#*:}" "$lic/LICENSE-${pair%%:*}-${pair#*:}"
done
for pair in fsr-vulkan:LICENSE.txt fsr-vulkan:NOTICE.md half:LICENSE.txt imgui:LICENSE.txt sirit:LICENSE.txt \
            fonts:LICENSE-DejaVu.txt; do
    cp "gpu/third_party/${pair%%:*}/${pair#*:}" "$lic/LICENSE-${pair%%:*}-${pair#*:}"
done
cat > "$lic/LICENSE-Boost.txt" <<'EOF'
Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
EOF
commit=$(git rev-parse --short HEAD)
cat > "$lic/README.txt" <<EOF
Bloodborne macOS port $version (commit $commit)
Source code: https://github.com/Wooloomooloo2/bloodborne_mac_port/tree/$commit
Existing port sources are GPL-2.0-or-later (LICENSE-bloodborne-mac-port.txt). The native menu
adapters and combined runtime retain GPL-3.0-or-later (LICENSE-native-menu.txt).
The other files are the licences of bundled libraries. No game files are included.
EOF

cat > "$contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>Bloodborne</string>
    <key>CFBundleDisplayName</key><string>Bloodborne</string>
    <key>CFBundleIdentifier</key><string>io.github.wooloomooloo2.bloodborne-mac-port</string>
    <key>CFBundleExecutable</key><string>Bloodborne</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>${version%%-*}</string>
    <key>CFBundleVersion</key><string>$version</string>
    <key>BBCommit</key><string>$commit</string>
    <key>NSHumanReadableCopyright</key><string>bloodborne_mac_port contributors. GPL-2.0-or-later.</string>
    <key>LSMinimumSystemVersion</key><string>15.0</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.role-playing-games</string>
    <key>LSArchitecturePriority</key><array><string>arm64</string></array>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF

# The launcher window (launcher/, Swift): settings, folders, Play.
bash scripts/macos/build_launcher.sh
install -m755 out/launcher/Bloodborne "$contents/MacOS/Bloodborne"

# Ad-hoc signature (arm64 code must be signed to run; install_name_tool invalidated the build's).
find "$contents/MacOS" "$contents/Frameworks" "$py" -type f \( -perm -u+x -o -name '*.dylib' -o -name '*.so' \) -print0 |
    while IFS= read -r -d '' file; do
        # The main executable is signed with the bundle (after everything it contains).
        [[ $file == "$contents/MacOS/Bloodborne" ]] && continue
        if file -b "$file" | grep -q Mach-O; then codesign --force --sign - "$file" 2>/dev/null; fi
    done
codesign --force --sign - "$app"
codesign --verify --deep --strict "$app"

zip=$dist/Bloodborne-macOS-$version.zip
rm -f "$zip"
(cd "$dist" && ditto -c -k --keepParent --sequesterRsrc Bloodborne.app "$(basename "$zip")")
echo "Built $app ($(du -sh "$app" | cut -f1)) and $zip ($(du -h "$zip" | cut -f1))"
