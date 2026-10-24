#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Mark Hall
# Builds a standalone pkg_extract for release downloads: arm64, macOS 15 or newer, Crypto++ built
# from source for that target and linked in (Homebrew's is built for the build machine's macOS).
#
# usage: bash tools/pkg_extract/build_release.sh [output-file]    (default dist/pkg_extract)
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
repo=$PWD
out=${1:-$repo/dist/pkg_extract}
work=$repo/deps/pkg_extract-release
cryptopp_zip=cryptopp890.zip
cryptopp_sha256=4cc0ccc324625b80b695fcd3dee63a66f1a460d3e51b71640cdbfc4cd1a3779c
export MACOSX_DEPLOYMENT_TARGET=15.0

mkdir -p "$work" "$(dirname "$out")"
if [[ ! -f $work/cryptopp/libcryptopp.a ]]; then
    [[ -f $work/$cryptopp_zip ]] || curl -fsSL --retry 3 -o "$work/$cryptopp_zip" \
        "https://github.com/weidai11/cryptopp/releases/download/CRYPTOPP_8_9_0/$cryptopp_zip"
    echo "$cryptopp_sha256  $work/$cryptopp_zip" | shasum -a 256 -c - >/dev/null ||
        { echo "Crypto++ archive checksum mismatch" >&2; exit 1; }
    rm -rf "$work/cryptopp"
    mkdir -p "$work/cryptopp"
    (cd "$work/cryptopp" && unzip -q "../$cryptopp_zip" &&
        make -j"$(sysctl -n hw.ncpu)" static CXX=clang++ \
            CXXFLAGS="-O2 -DNDEBUG -fPIC -arch arm64 -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" >/dev/null)
fi

# pkg-config view of the local Crypto++ (headers included as <cryptopp/...>).
mkdir -p "$work/pkgconfig" "$work/include"
ln -sfn "$work/cryptopp" "$work/include/cryptopp"
cat > "$work/pkgconfig/libcryptopp.pc" <<EOF
Name: libcryptopp
Description: Crypto++ (static, release build of pkg_extract)
Version: 8.9.0
Cflags: -I$work/include
Libs: -L$work/cryptopp -lcryptopp
EOF

PKG_CONFIG_LIBDIR=$work/pkgconfig cmake -S tools/pkg_extract -B "$work/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DPKG_EXTRACT_STATIC=ON -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET -DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew \
    -DPKG_CONFIG_EXECUTABLE="$(command -v pkg-config)" >/dev/null
cmake --build "$work/build"
install -m755 "$work/build/pkg_extract" "$out"
if otool -L "$out" | awk 'NR > 1 { print $1 }' | grep -vE '^/usr/lib/'; then
    echo "pkg_extract links a library outside macOS" >&2; exit 1
fi
codesign --force --sign - "$out"
echo "Built $out"
