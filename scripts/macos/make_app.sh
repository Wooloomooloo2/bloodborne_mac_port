#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Mark Hall
# Builds Bloodborne.app in the repository folder: double-click (or keep in the Dock) to play
# without a Terminal window. The app is the launcher window (launcher/) for this checkout: it
# shares launch.conf with Bloodborne.command, runs run.sh (which rebuilds what changed) and writes
# the game's logs to ~/Library/Logs/Bloodborne (one dated .log and .jsonl per session; game.log links
# to the newest). If the game stops with an error, the launcher shows a crash report and the log.
#
# The game window runs from inside the bundle (a link to out/bbgpu-host), so the Dock shows
# "Bloodborne". The icon is made from your game's sce_sys/icon0.png, so the app is built
# locally and never committed (Bloodborne.app is in .gitignore).
#
# usage: bash scripts/macos/make_app.sh     (again after moving the repository)
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
repo=$PWD
app=$repo/Bloodborne.app
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"

# Version for the About panel and the update check (Updates in launcher/): 0.5.2-5-g0b50589 from
# a checkout is offered the next numbered release.
version=$(git describe --tags --match 'mac-v*' --dirty 2>/dev/null | sed 's/^mac-v//' || true)
version=${version:-dev}
commit=$(git rev-parse --short HEAD 2>/dev/null || true)
repo_xml=$(printf '%s' "$repo" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g')
cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>Bloodborne</string>
    <key>CFBundleDisplayName</key><string>Bloodborne</string>
    <key>CFBundleIdentifier</key><string>io.github.wooloomooloo2.bloodborne-mac-port</string>
    <key>CFBundleExecutable</key><string>Bloodborne</string>
    <key>CFBundleIconFile</key><string>Bloodborne</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>${version%%-*}</string>
    <key>CFBundleVersion</key><string>$version</string>
    <key>BBCommit</key><string>$commit</string>
    <key>NSHumanReadableCopyright</key><string>bloodborne_mac_port contributors. GPL-2.0-or-later.</string>
    <key>LSMinimumSystemVersion</key><string>15.0</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.role-playing-games</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>BBRepository</key><string>$repo_xml</string>
</dict>
</plist>
EOF

# The launcher window (launcher/, Swift). The checkout's path is in Info.plist (BBRepository).
bash scripts/macos/build_launcher.sh
install -m755 out/launcher/Bloodborne "$app/Contents/MacOS/Bloodborne"
codesign --force --sign - "$app/Contents/MacOS/Bloodborne"
ln -s "$repo/out/bbgpu-host" "$app/Contents/MacOS/bbgpu-host"
ln -s "$repo/out/bbgpu-host-macos27" "$app/Contents/MacOS/bbgpu-host-macos27"

# Icon from the game's own artwork, when the game folder is known.
game=
if [[ -f launch.conf ]]; then game=$(set -a; source ./launch.conf; printf '%s' "${BB_GAME_DIR:-}"); fi
[[ -z $game && -f ../CUSA03173/eboot.bin ]] && game=$(cd ../CUSA03173 && pwd)
if [[ -n $game && -f $game/sce_sys/icon0.png ]]; then
    set_dir=$(mktemp -d)/Bloodborne.iconset
    mkdir -p "$set_dir"
    for size in 16 32 128 256 512; do
        sips -z $size $size "$game/sce_sys/icon0.png" --out "$set_dir/icon_${size}x${size}.png" >/dev/null
        double=$((size * 2))
        sips -z $double $double "$game/sce_sys/icon0.png" --out "$set_dir/icon_${size}x${size}@2x.png" >/dev/null
    done
    iconutil -c icns "$set_dir" -o "$app/Contents/Resources/Bloodborne.icns"
    rm -rf "$(dirname "$set_dir")"
else
    echo "No game folder known yet (launch.conf): the app has no icon. Run this again after the first launch."
fi
touch "$app"
echo "Built $app"
