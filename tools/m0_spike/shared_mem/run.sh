#!/bin/sh
# SPDX-FileCopyrightText: Copyright 2026 Mark Hall
# Run the spike. Optional arg: ring messages per repetition (default 10M; 3 reps per variant).
#   SPIKE_ENTRY = named (MAP_MEM_NAMED_CREATE) | share (MAP_MEM_VM_SHARE of an anonymous range)
#   SPIKE_XFER  = register (mach_ports_register, inherited) | bootstrap (posix_spawnattr_setspecialport_np;
#                 known-bad: libxpc in the child keeps using the hijacked port -> 1 expected FAIL)
set -u
cd "$(dirname "$0")"
./build.sh >/dev/null || exit 1
for cfg in "named register" "share register" "named bootstrap"; do
  set -- $cfg "${1:-10000000}"
  echo "################ SPIKE_ENTRY=$1 SPIKE_XFER=$2 ################"
  SPIKE_ENTRY=$1 SPIKE_XFER=$2 ./out/parent "$3"
  shift 2
done
