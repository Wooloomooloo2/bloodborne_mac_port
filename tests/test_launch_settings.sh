#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# bbport.ini fps_cap / fullscreen -> environment (scripts/launch_settings.sh).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
ini=$(mktemp)
trap 'rm -f "$ini"' EXIT
check() { # ini text, expected "LIMIT FULLSCREEN" (- = unset), extra environment...
    printf '%s' "$1" > "$ini"
    local got
    got=$(env -u BB_FPS_LIMIT -u BB_FULLSCREEN "${@:3}" /bin/bash -c \
        'source scripts/launch_settings.sh; bb_launch_settings "$1"; printf "%s %s" "${BB_FPS_LIMIT:--}" "${BB_FULLSCREEN:--}"' _ "$ini")
    if [[ $got != "$2" ]]; then
        echo "FAIL: launch settings for $(printf %q "$1") ${*:3}: got '$got', want '$2'"
        exit 1
    fi
}
check $'fps_cap=40\nfullscreen=1\n' "40 1"
check $'fps_cap=120\n' "120 -"
check $'fps_cap=30\nfullscreen=0\n' "30 -"
check $'fps_cap=60' "60 -"                      # no final newline
check $'fps_cap=40\r\nfullscreen=1\r\n' "40 1"  # CRLF
check $'fps_cap=45\n' "- -"                     # not a cap the launcher offers: ignored
check $'upscaler=off\n' "- -"                   # no keys: today's behaviour
check $'fps_cap=40\nfullscreen=1\n' "90 1" BB_FPS_LIMIT=90
check $'fullscreen=1\n' "- 0" BB_FULLSCREEN=0
rm -f "$ini"
/bin/bash -c 'source scripts/launch_settings.sh; bb_launch_settings /nonexistent/bbport.ini' ||
    { echo "FAIL: launch settings with a missing file"; exit 1; }
# Online keys (the environment wins; the password never reaches the command line here).
check_online() { # ini text, expected "ONLINE SERVER NPID UPNP" (- = unset), extra environment...
    printf '%s' "$1" > "$ini"
    local got
    got=$(env -u BB_ONLINE -u BB_SHADNET_SERVER -u BB_SHADNET_NPID -u BB_UPNP "${@:3}" /bin/bash -c \
        'source scripts/launch_settings.sh; bb_launch_settings "$1"; printf "%s %s %s %s" "${BB_ONLINE:--}" "${BB_SHADNET_SERVER:--}" "${BB_SHADNET_NPID:--}" "${BB_UPNP:--}"' _ "$ini")
    if [[ $got != "$2" ]]; then
        echo "FAIL: online launch settings for $(printf %q "$1") ${*:3}: got '$got', want '$2'"
        exit 1
    fi
}
check_online $'online=1\nshadnet_server=example.org:31313\nshadnet_npid=hunter\nupnp=0\n' "1 example.org:31313 hunter 0"
check_online $'online=0\nshadnet_npid=hunter\n' "- - hunter -"
check_online $'fps_cap=60\n' "- - - -"
check_online $'online=1\nshadnet_npid=hunter\n' "0 - other -" BB_ONLINE=0 BB_SHADNET_NPID=other
echo "PASS: launch settings (fps_cap, fullscreen, online)"
