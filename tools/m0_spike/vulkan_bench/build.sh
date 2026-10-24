#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 Mark Hall
# Build vkbench + cpubench twice from one source: x86_64 (runs under Rosetta) and arm64 (native).
# Both vkbench builds link the universal MoltenVK dylib directly (no Vulkan loader).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "${HERE}/../../.." && pwd)"
DEPS="${ROOT}/deps/x86_64"
OUT="${ROOT}/out/m0_vulkan_bench"
mkdir -p "${OUT}"

lipo -info "${DEPS}/lib/libMoltenVK.dylib"
lipo "${DEPS}/lib/libMoltenVK.dylib" -verify_arch x86_64 arm64

# GLSL -> SPIR-V -> C arrays, embedded in one header
HDR="${OUT}/shaders_spv.h"
: > "${HDR}"
for s in draw.vert draw1.frag draw1s.frag draw5.frag draw5s.frag work.comp; do
    var="spv_$(echo "${s}" | tr '.' '_')"
    glslang -V --target-env vulkan1.3 --vn "${var}" -o "${OUT}/${s}.h" "${HERE}/shaders/${s}" >/dev/null 2>&1 || { echo "glslang failed: ${s}"; exit 1; }
    cat "${OUT}/${s}.h" >> "${HDR}"
done

CFLAGS=(-O2 -g -mmacosx-version-min=15.0 -Wall -Wno-missing-braces -I"${DEPS}/include" -I"${OUT}")
for arch in x86_64 arm64; do
    /usr/bin/clang -arch "${arch}" -std=c11 "${CFLAGS[@]}" "${HERE}/vkbench.c" -o "${OUT}/vkbench-${arch}" \
        -L"${DEPS}/lib" -lMoltenVK -Wl,-rpath,"${DEPS}/lib"
    /usr/bin/clang++ -arch "${arch}" -std=c++17 -O2 -g -mmacosx-version-min=15.0 -Wall \
        "${HERE}/cpubench.cpp" -o "${OUT}/cpubench-${arch}"
done
for a in x86_64 arm64; do lipo -info "${OUT}/vkbench-${a}" "${OUT}/cpubench-${a}"; done
otool -L "${OUT}/vkbench-arm64" | grep -i molten
