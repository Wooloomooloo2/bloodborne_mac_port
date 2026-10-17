#!/usr/bin/env bash
# macOS (Apple Silicon) build: everything is x86-64 and runs under Rosetta 2, because the game's
# eboot.bin is x86-64 code executed directly. Dependencies come from deps/x86_64
# (scripts/macos/bootstrap_deps.sh); nothing from the arm64 Homebrew may be linked.
#
#   bash build_macos.sh            # libbbgpu.dylib, bb-probe, bb-gpu-capabilities
#   bash build_macos.sh --test     # also build and run the runtime tests
#   bash build_macos.sh --gpu-only # only the GPU library
set -euo pipefail
cd -- "$(dirname -- "$0")"
ROOT=$PWD
PREFIX=$ROOT/deps/x86_64
export MACOSX_DEPLOYMENT_TARGET=15.0
if [[ ! -f $PREFIX/.built-MoltenVK ]]; then
    echo 'x86-64 dependencies missing: run bash scripts/macos/bootstrap_deps.sh' >&2; exit 1
fi
for tool in cmake ninja glslang pkg-config; do
    command -v $tool >/dev/null || { echo "Missing $tool (brew install cmake ninja glslang pkgconf)" >&2; exit 1; }
done
mkdir -p out
CC=${CC:-/usr/bin/clang}
ARCH=(-arch x86_64 -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET)
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig
unset PKG_CONFIG_PATH
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"

# GPU library (shadPS4 video core + drivers): out/gpu/libbbgpu.dylib. PGO is GCC-specific
# (.gcda profiles) and stays off on macOS; BB_LTO=OFF disables link-time optimization.
if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ]]; then
    git submodule update --init --recursive
fi
for patch in gpu/patches/fsr-vulkan/*.patch; do
    if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
        git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
    fi
done
# Boost is header-only: the arm64 Homebrew headers are fine (nothing is linked from there).
boost=$(brew --prefix boost 2>/dev/null || echo /opt/homebrew/opt/boost)
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="${CXX:-/usr/bin/clang++}" \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET \
    -DCMAKE_PREFIX_PATH="$PREFIX;$boost" -DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew \
    -DBoost_INCLUDE_DIR="$boost/include" \
    -DBB_PGO=off -DBB_LTO="${BB_LTO:-ON}" -DBB_PGO_DIR="$PWD/pgo" -Wno-dev >/dev/null
echo "GPU library: LTO ${BB_LTO:-ON}"
# A failed GPU build must stop here: an older libbbgpu.dylib would otherwise be used silently.
if ! ninja -C out/gpu bbgpu > out/gpu-build.log 2>&1; then
    grep -v '^\[' out/gpu-build.log | tail -40 >&2
    echo 'GPU library build failed (full log: out/gpu-build.log)' >&2; exit 1
fi
if [[ ${1:-} == --gpu-only ]]; then echo "Built $PWD/out/gpu/libbbgpu.dylib"; exit 0; fi

# @executable_path/gpu: packaged copies keep the library next to the binary without patching it.
gpu=(-Lout/gpu -lbbgpu -Wl,-rpath,@executable_path/gpu -Wl,-rpath,"$PWD/out/gpu"
    -Wl,-rpath,"$PREFIX/lib")
runtime=(src/runtime*.c)
# Third-party decoders: compiled once, without this project's -Werror policy.
atrac9=(third_party/LibAtrac9/C/src/*.c)
if [[ ! -f out/libatrac9.a || -n $(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c') ]]; then
    rm -rf out/atrac9 && mkdir -p out/atrac9
    for source in "${atrac9[@]}"; do "$CC" "${ARCH[@]}" -std=c99 -O2 -g -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"; done
    ar rcs out/libatrac9.a out/atrac9/*.o
fi
cflags=(-std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${ARCH[@]}")
"$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc src/probe.c "${runtime[@]}" src/gpu_process.c src/vulkan_smoke.c out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -framework CoreFoundation -o out/bb-probe
echo "Built $PWD/out/bb-probe"
# Native arm64 GPU process (docs/DESIGN_NATIVE_GPU_PROCESS.md; BB_GPU_PROCESS=1): the GPU
# library built arm64 (scripts/macos/build_gpu_arm64.sh, needs deps/arm64 from
# BB_DEPS_ARCH=arm64 scripts/macos/bootstrap_deps.sh) linked into out/bbgpu-host.
ARM64_PREFIX=$ROOT/deps/arm64
if [[ -f $ARM64_PREFIX/lib/pkgconfig/sdl3.pc ]]; then
    bash scripts/macos/build_gpu_arm64.sh
    /usr/bin/clang -arch arm64 -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -std=c11 -O2 -g -Wall -Wextra -Werror \
        -I. host/main.c host/host_runtime.c host/guest_reservation.s \
        -Wl,-segaddr,__GUESTRSV,0x7000000000 -Wl,-segprot,__GUESTRSV,---,--- \
        -Lout/gpu-arm64 -lbbgpu -Wl,-rpath,"$ROOT/out/gpu-arm64" \
        -Wl,-rpath,"$ARM64_PREFIX/lib" -framework CoreFoundation -o out/bbgpu-host
    echo "Built $PWD/out/bbgpu-host (arm64, renderer: out/gpu-arm64/libbbgpu.dylib)"
else
    echo "Skipped out/bbgpu-host: no arm64 dependencies (BB_DEPS_ARCH=arm64 bash scripts/macos/bootstrap_deps.sh)"
fi
# GPU check for run.sh (live_resolution=auto): links only the Vulkan loader.
"$CC" -std=c11 -O2 -Wall -Wextra -Werror "${ARCH[@]}" "${includes[@]}" tools/gpu_capabilities.c "${libraries[@]}" \
    -Wl,-rpath,"$PREFIX/lib" -o out/bb-gpu-capabilities
if [[ ${1:-} == --test ]]; then
    "$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc tests/test_pad.c src/runtime_host.c "${libraries[@]}" -Wl,-rpath,"$PREFIX/lib" -o out/pad-test
    out/pad-test
    "$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc tests/test_runtime.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/runtime-test
    out/runtime-test
    "$CC" "${cflags[@]}" -Isrc tests/test_file_mods.c -o out/file-mods-test
    out/file-mods-test
    "$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc tests/test_sema.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/sema-test
    out/sema-test
    "$CC" "${cflags[@]}" -I. -Isrc tests/test_content.c src/runtime_content.c -o out/content-test
    out/content-test
    /usr/bin/clang++ -std=c++20 -O1 -Wall -Wextra -Igpu/shim tests/test_settings_keep.cpp \
        gpu/shim/bbport_settings.cpp -o out/settings-keep-test
    out/settings-keep-test
    bash tests/test_launch_settings.sh
    bash scripts/macos/build_launcher.sh --test
    # Shared control block rings between bb-probe (x86-64) and bbgpu-host (arm64).
    /usr/bin/clang -arch x86_64 -arch arm64 -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -std=c11 -O2 -g \
        -Wall -Wextra -Werror -I. tests/test_bbgpu_rings.c -o out/bbgpu-rings-test
    arch -x86_64 out/bbgpu-rings-test
    # The GPU process split end to end (opens the host's window for a moment).
    if [[ -x out/bbgpu-host ]]; then
        "$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc tests/test_gpu_split.c "${runtime[@]}" src/gpu_process.c \
            out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/gpu-split-test
        out/gpu-split-test
    fi
    # Portable SRT walker, on both architectures (arm64 when build_gpu_arm64.sh has run).
    for walker in "x86_64:out/gpu" "arm64:out/gpu-arm64"; do
        [[ -f ${walker#*:}/libbbgpu.dylib ]] || continue
        /usr/bin/clang++ -arch "${walker%%:*}" -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -std=c++20 -O2 -g \
            -Wall -Wextra -Werror tests/test_srt_walker.cpp -L"${walker#*:}" -lbbgpu \
            -Wl,-rpath,"$ROOT/${walker#*:}" -o "out/srt-walker-test-${walker%%:*}"
        "out/srt-walker-test-${walker%%:*}"
    done
fi
