#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Mark Hall
# Builds the macOS dependencies of bbport (no sudo, no x86 Homebrew).
#  - x86_64 (default) -> deps/x86_64: bb-probe and the game run under Rosetta 2, so every
#    library linked into that process must be x86-64.
#  - arm64 (BB_DEPS_ARCH=arm64) -> deps/arm64: the native GPU host process (bbgpu-host, see
#    docs/DESIGN_NATIVE_GPU_PROCESS.md). The same pinned set.
# Each architecture has its own prefix, build directory and stamps; neither ever sees the
# other's libraries. No Vulkan loader or MoltenVK (removed in 0.5.3: the renderer is Metal);
# Vulkan-Headers stay for Vulkan-Hpp's types, SPIRV-Cross turns the shaders' SPIR-V into MSL.
# Host tools (cmake, ninja, nasm, glslang, pkgconf) come from the native arm64 Homebrew.
#
#   bash scripts/macos/bootstrap_deps.sh                   # x86-64: build what is missing
#   BB_DEPS_ARCH=arm64 bash scripts/macos/bootstrap_deps.sh # arm64
#   BB_DEPS_CLEAN=1 bash scripts/macos/bootstrap_deps.sh   # rebuild everything (that arch)
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
ROOT=$PWD
DEPS=$ROOT/deps
SRC=$DEPS/src
ARCH=${BB_DEPS_ARCH:-x86_64}
case $ARCH in
    x86_64) BUILD=$DEPS/build; OTHER=$DEPS/arm64 ;;   # historical build dir name kept
    arm64) BUILD=$DEPS/build-arm64; OTHER=$DEPS/x86_64 ;;
    *) echo "BB_DEPS_ARCH must be x86_64 or arm64, not $ARCH" >&2; exit 1 ;;
esac
PREFIX=$DEPS/$ARCH
# Rosetta runs AVX2 code from macOS 15 on.
export MACOSX_DEPLOYMENT_TARGET=15.0
JOBS=$(sysctl -n hw.ncpu)

SDL_TAG=release-3.4.18
FMT_TAG=11.2.0
XXHASH_TAG=v0.8.3
MINIZ_TAG=3.1.2
VULKAN_TAG=vulkan-sdk-1.4.363.0
MAGIC_ENUM_TAG=v0.9.7
ROBIN_MAP_TAG=v1.4.0
FFMPEG_TAG=n7.1.5
# Online play (gpu/bbnet, libbbnet.dylib; x86-64 only: the module runs in the game process).
PROTOBUF_TAG=v21.12          # the last protobuf without abseil; protoc comes with it
OPENSSL_TAG=openssl-3.5.9
MINIUPNP_TAG=miniupnpc_2_3_3
JSON_TAG=v3.12.0
# The commit each tag pointed at when it was checked (a tag can be moved; a clone that lands
# elsewhere is refused and removed). MoltenVK comes as a release tarball: its SHA-256.
SDL_COMMIT=829a65d769d935c4852f8159e964312c0957260a FMT_COMMIT=40626af88bd7df9a5fb80be7b25ac85b122d6c21
XXHASH_COMMIT=e626a72bc2321cd320e953a0ccf1584cad60f363 MINIZ_COMMIT=77d0dce8627735138c51770d1799a1ef48f2117d
ZYDIS_COMMIT=a2278f1d254e492f6a6b39f6cb5d1f5d515659dc VULKAN_HEADERS_COMMIT=6802bb4733b63ed5efd3adb308a6c885ef180ea1
VULKAN_LOADER_COMMIT=fa218055ff4948b159f31d43f800c808a13d51df SPIRV_CROSS_COMMIT=f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7
MAGIC_ENUM_COMMIT=e046b69a3736d314fad813e159b1c192eaef92cd ROBIN_MAP_COMMIT=4ec1bf19c6a96125ea22062f38c2cf5b958e448e
VMA_COMMIT=1d8f600fd424278486eade7ed3e877c99f0846b1 XBYAK_COMMIT=cd6bad3cad2e07a3e277c23d2b1f065320024717
FFMPEG_COMMIT=3a0867c2bfda4a4d4309ca1a8cbdc6175e67f587 PROTOBUF_COMMIT=f0dc78d7e6e331b8c6bb2d5283e06aa26883ca7c
OPENSSL_COMMIT=45e844fa2a14ec92d146bd8f5778ac130b6625fb MINIUPNP_COMMIT=bf4215a7574f88aa55859db9db00e3ae58cf42d6
JSON_COMMIT=55f93686c01528224f448c19128836e7df245f72
MOLTENVK_SHA256=f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e

if [[ ${BB_DEPS_CLEAN:-} == 1 ]]; then rm -rf "$BUILD" "$PREFIX"; fi
mkdir -p "$SRC" "$BUILD" "$PREFIX"

need=()
for tool in cmake ninja nasm glslang pkg-config; do command -v $tool >/dev/null || need+=($tool); done
if ((${#need[@]})); then
    echo "Installing host tools: ${need[*]}"
    brew install cmake ninja nasm glslang pkgconf boost
fi
brew list boost >/dev/null 2>&1 || brew install boost
if [[ $ARCH == x86_64 ]]; then
    arch -x86_64 /usr/bin/true 2>/dev/null || { echo 'Rosetta 2 is required: softwareupdate --install-rosetta' >&2; exit 1; }
fi

# Nothing from the arm64 Homebrew prefix may leak into these builds (headers-only Boost aside),
# and nothing from the other architecture's prefix (or an x86 Homebrew in /usr/local).
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig
unset PKG_CONFIG_PATH
CMAKE_COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=$ARCH
    -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET -DCMAKE_INSTALL_PREFIX="$PREFIX"
    -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local;$OTHER"
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -Wno-dev)

pinned() { # dir name commit: the checkout is the commit the tag was pinned at, or it goes
    local got; got=$(git -C "$1" rev-parse HEAD 2>/dev/null || echo none)
    if [[ $got != "$3" ]]; then
        echo "$2: the tag now points at $got, not the pinned $3; refusing it (update the pin after checking the change)" >&2
        rm -rf "$1"; exit 1
    fi
}
fetch() { # name url tag commit
    local dir=$SRC/$1
    if [[ ! -d $dir ]]; then
        echo "== fetch $1 $3"
        git clone -q --depth 1 --branch "$3" --recurse-submodules --shallow-submodules "$2" "$dir"
        pinned "$dir" "$1" "$4"
    fi
}
fetch_plain() { # name url tag commit: without submodules (OpenSSL's fuzz corpora, protobuf's test suites)
    local dir=$SRC/$1
    if [[ ! -d $dir ]]; then
        echo "== fetch $1 $3"
        git clone -q --depth 1 --branch "$3" "$2" "$dir"
        pinned "$dir" "$1" "$4"
    fi
}
stamp() { [[ -f $PREFIX/.built-$1 ]]; }
done_stamp() { touch "$PREFIX/.built-$1"; }
cmake_dep() { # name srcsubdir args...
    local name=$1 sub=$2; shift 2
    stamp "$name" && return
    echo "== build $name"
    cmake -S "$SRC/$name/$sub" -B "$BUILD/$name" "${CMAKE_COMMON[@]}" "$@" >"$BUILD/$name.log" 2>&1 \
        && ninja -C "$BUILD/$name" install >>"$BUILD/$name.log" 2>&1 \
        || { tail -30 "$BUILD/$name.log" >&2; echo "$name failed (log: $BUILD/$name.log)" >&2; exit 1; }
    done_stamp "$name"
}

fetch SDL https://github.com/libsdl-org/SDL.git $SDL_TAG $SDL_COMMIT
fetch fmt https://github.com/fmtlib/fmt.git $FMT_TAG $FMT_COMMIT
fetch xxHash https://github.com/Cyan4973/xxHash.git $XXHASH_TAG $XXHASH_COMMIT
fetch miniz https://github.com/richgel999/miniz.git $MINIZ_TAG $MINIZ_COMMIT
fetch Vulkan-Headers https://github.com/KhronosGroup/Vulkan-Headers.git $VULKAN_TAG $VULKAN_HEADERS_COMMIT
fetch magic_enum https://github.com/Neargye/magic_enum.git $MAGIC_ENUM_TAG $MAGIC_ENUM_COMMIT
fetch robin-map https://github.com/Tessil/robin-map.git $ROBIN_MAP_TAG $ROBIN_MAP_COMMIT
fetch FFmpeg https://github.com/FFmpeg/FFmpeg.git $FFMPEG_TAG $FFMPEG_COMMIT
# SPIR-V -> MSL for the Metal renderer (the x86-64 library still compiles it: MACOS_BACKLOG).
fetch SPIRV-Cross https://github.com/KhronosGroup/SPIRV-Cross.git $VULKAN_TAG $SPIRV_CROSS_COMMIT

cmake_dep Vulkan-Headers .
cmake_dep SDL . -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cmake_dep fmt . -DFMT_TEST=OFF -DFMT_DOC=OFF
cmake_dep xxHash cmake_unofficial -DBUILD_SHARED_LIBS=OFF -DXXHASH_BUILD_XXHSUM=OFF
cmake_dep miniz . -DBUILD_SHARED_LIBS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DBUILD_FUZZERS=OFF
cmake_dep magic_enum . -DMAGIC_ENUM_OPT_BUILD_EXAMPLES=OFF -DMAGIC_ENUM_OPT_BUILD_TESTS=OFF
cmake_dep robin-map .
cmake_dep SPIRV-Cross . -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_STATIC=ON \
    -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_ENABLE_HLSL=OFF \
    -DSPIRV_CROSS_ENABLE_CPP=OFF -DSPIRV_CROSS_ENABLE_REFLECT=OFF -DSPIRV_CROSS_ENABLE_C_API=OFF \
    -DSPIRV_CROSS_ENABLE_UTIL=OFF

# Online module libraries (x86-64 only): all static, linked into libbbnet.dylib. zlib is the
# system's. protoc runs under Rosetta; deps/host/bin/protoc is the same protoc built arm64,
# which gpu/bbnet uses when it is there.
if [[ $ARCH == x86_64 ]]; then
    fetch_plain protobuf https://github.com/protocolbuffers/protobuf.git $PROTOBUF_TAG $PROTOBUF_COMMIT
    fetch_plain openssl https://github.com/openssl/openssl.git $OPENSSL_TAG $OPENSSL_COMMIT
    fetch_plain miniupnp https://github.com/miniupnp/miniupnp.git $MINIUPNP_TAG $MINIUPNP_COMMIT
    fetch_plain json https://github.com/nlohmann/json.git $JSON_TAG $JSON_COMMIT
    cmake_dep protobuf . -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_SHARED_LIBS=OFF \
        -Dprotobuf_WITH_ZLIB=OFF -Dprotobuf_BUILD_PROTOC_BINARIES=ON
    if ! stamp protoc-host; then
        echo "== build protoc (arm64 host tool)"
        cmake -S "$SRC/protobuf" -B "$BUILD/protoc-host" -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET \
            -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
            -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_BUILD_SHARED_LIBS=OFF -Dprotobuf_WITH_ZLIB=OFF \
            -Wno-dev >"$BUILD/protoc-host.log" 2>&1 \
            && ninja -C "$BUILD/protoc-host" protoc >>"$BUILD/protoc-host.log" 2>&1 \
            || { tail -30 "$BUILD/protoc-host.log" >&2; echo "protoc-host failed (log: $BUILD/protoc-host.log)" >&2; exit 1; }
        mkdir -p "$DEPS/host/bin"
        cp -L "$BUILD/protoc-host/protoc" "$DEPS/host/bin/protoc"
        done_stamp protoc-host
    fi
    if ! stamp OpenSSL; then
        echo "== build OpenSSL (a few minutes)"
        rm -rf "$BUILD/OpenSSL" && mkdir -p "$BUILD/OpenSSL"
        (cd "$BUILD/OpenSSL" && perl "$SRC/openssl/Configure" darwin64-x86_64-cc --prefix="$PREFIX" \
            --libdir=lib --openssldir="$PREFIX/ssl" no-shared no-tests no-docs no-apps \
            -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET >"$BUILD/OpenSSL.log" 2>&1 \
            && make -j"$JOBS" >>"$BUILD/OpenSSL.log" 2>&1 && make install_sw >>"$BUILD/OpenSSL.log" 2>&1) \
            || { tail -30 "$BUILD/OpenSSL.log" >&2; echo "OpenSSL failed (log: $BUILD/OpenSSL.log)" >&2; exit 1; }
        # HTTPS verification uses the system's certificate bundle (run.sh also sets SSL_CERT_FILE).
        mkdir -p "$PREFIX/ssl"
        [[ -e $PREFIX/ssl/cert.pem ]] || ln -s /etc/ssl/cert.pem "$PREFIX/ssl/cert.pem"
        done_stamp OpenSSL
    fi
    cmake_dep miniupnp miniupnpc -DUPNPC_BUILD_SHARED=OFF -DUPNPC_BUILD_STATIC=ON \
        -DUPNPC_BUILD_TESTS=OFF -DUPNPC_BUILD_SAMPLE=OFF
    cmake_dep json . -DJSON_BuildTests=OFF
fi

# Boost is used header-only; headers are architecture-independent.
if [[ ! -e $PREFIX/include/boost ]]; then
    ln -s "$(brew --prefix boost)/include/boost" "$PREFIX/include/boost"
fi

if ! stamp FFmpeg; then
    echo "== build FFmpeg (several minutes)"
    mkdir -p "$BUILD/FFmpeg"
    ffarch=$ARCH; [[ $ARCH == arm64 ]] && ffarch=aarch64
    (cd "$BUILD/FFmpeg" && "$SRC/FFmpeg/configure" --prefix="$PREFIX" \
        --arch=$ffarch --target-os=darwin --enable-cross-compile \
        --cc="clang -arch $ARCH" --cxx="clang++ -arch $ARCH" \
        --extra-cflags="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" \
        --extra-ldflags="-arch $ARCH -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" \
        --enable-shared --disable-static --disable-programs --disable-doc \
        --disable-autodetect --enable-videotoolbox --enable-audiotoolbox \
        --install-name-dir='@rpath' >"$BUILD/FFmpeg.log" 2>&1 \
        && make -j"$JOBS" >>"$BUILD/FFmpeg.log" 2>&1 && make install >>"$BUILD/FFmpeg.log" 2>&1) \
        || { tail -30 "$BUILD/FFmpeg.log" >&2; echo "FFmpeg failed" >&2; exit 1; }
    done_stamp FFmpeg
fi

echo
echo "$ARCH dependencies ready in $PREFIX"
for lib in libSDL3.0.dylib libavcodec.dylib; do
    printf '  %-22s %s\n' "$lib" "$(lipo -archs "$PREFIX/lib/$lib" 2>/dev/null || echo missing)"
done
if [[ $ARCH == x86_64 ]]; then
    for lib in libprotobuf.a libssl.a libminiupnpc.a; do
        printf '  %-22s %s\n' "$lib" "$(lipo -archs "$PREFIX/lib/$lib" 2>/dev/null || echo missing)"
    done
fi
