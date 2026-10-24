#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2026 Mark Hall
# Build the M0 shared-memory spike: x86-64 parent (Rosetta) + native arm64 child.
set -eu
cd "$(dirname "$0")"
mkdir -p out
CFLAGS="-std=c11 -O2 -g -Wall -Wextra -Wno-unused-function -D_DARWIN_C_SOURCE"
/usr/bin/clang $CFLAGS -arch x86_64 -mmacosx-version-min=15.0 -o out/parent parent.c
/usr/bin/clang $CFLAGS -arch arm64  -mmacosx-version-min=15.0 -o out/child  child.c
file out/parent out/child
