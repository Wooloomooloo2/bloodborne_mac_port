#!/usr/bin/env bash
# Builds the GPU library (gpu/: shadPS4 video core + drivers) natively for arm64, for the native
# GPU host process bbgpu-host (docs/DESIGN_NATIVE_GPU_PROCESS.md), which renders with Metal. The
# x86-64 build loaded by the game (bb-probe) stays in build_macos.sh (out/gpu); this one goes to
# out/gpu-arm64.
# Dependencies come from deps/arm64 (BB_DEPS_ARCH=arm64 bash scripts/macos/bootstrap_deps.sh);
# nothing from deps/x86_64 or the arm64 Homebrew is linked (Boost headers aside).
#
#   bash scripts/macos/build_gpu_arm64.sh   # out/gpu-arm64/libbbgpu.dylib
#   BB_LTO=OFF bash scripts/macos/build_gpu_arm64.sh
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
ROOT=$PWD
PREFIX=$ROOT/deps/arm64
OUT=$ROOT/out/gpu-arm64
export MACOSX_DEPLOYMENT_TARGET=15.0
if [[ ! -f $PREFIX/.built-SPIRV-Cross ]]; then
    echo 'arm64 dependencies missing: run BB_DEPS_ARCH=arm64 bash scripts/macos/bootstrap_deps.sh' >&2
    exit 1
fi
for tool in cmake ninja glslang pkg-config; do
    command -v $tool >/dev/null || { echo "Missing $tool (brew install cmake ninja glslang pkgconf)" >&2; exit 1; }
done
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig
unset PKG_CONFIG_PATH

if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ]]; then
    git submodule update --init --recursive
fi
# Boost is header-only: the Homebrew headers are architecture-independent.
boost=$(brew --prefix boost 2>/dev/null || echo /opt/homebrew/opt/boost)
cmake -S gpu -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET \
    -DCMAKE_PREFIX_PATH="$PREFIX;$boost" \
    -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local;$ROOT/deps/x86_64" \
    -DBoost_INCLUDE_DIR="$boost/include" \
    -DBB_PGO=off -DBB_LTO="${BB_LTO:-ON}" -DBB_PGO_DIR="$ROOT/pgo" -Wno-dev >/dev/null
echo "GPU library (arm64): LTO ${BB_LTO:-ON}"
if ! ninja -C "$OUT" bbgpu > "$ROOT/out/gpu-arm64-build.log" 2>&1; then
    grep -v '^\[' "$ROOT/out/gpu-arm64-build.log" | tail -40 >&2
    echo 'arm64 GPU library build failed (full log: out/gpu-arm64-build.log)' >&2; exit 1
fi
lipo "$OUT/libbbgpu.dylib" -verify_arch arm64
echo "Built $OUT/libbbgpu.dylib"
