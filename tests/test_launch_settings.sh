#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Mark Hall
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
compression() { # ini text, expected BB_LAZY_TEXTURE_USAGE (- = unset), extra environment...
    printf '%s' "$1" > "$ini"
    local got
    got=$(env -u BB_LAZY_TEXTURE_USAGE "${@:3}" /bin/bash -c \
        'source scripts/launch_settings.sh; bb_launch_settings "$1"; printf "%s" "${BB_LAZY_TEXTURE_USAGE:--}"' _ "$ini")
    if [[ $got != "$2" ]]; then
        echo "FAIL: texture_compression for $(printf %q "$1") ${*:3}: got '$got', want '$2'"
        exit 1
    fi
}
compression $'texture_compression=0\n' "0"
compression $'texture_compression=1\n' "-"
compression $'upscaler=off\n' "-"                     # default: compressed
compression $'texture_compression=0\n' "1" BB_LAZY_TEXTURE_USAGE=1
timing() { # ini text, expected "BB_FPS BB_FPS_AUTO" (- = unset), extra environment...
    printf '%s' "$1" > "$ini"
    local got
    got=$(env -u BB_FPS -u BB_FPS_AUTO -u BB_FPS_LIMIT "${@:3}" /bin/bash -c \
        'source scripts/launch_settings.sh; bb_launch_settings "$1"; printf "%s %s" "${BB_FPS:--}" "${BB_FPS_AUTO:--}"' _ "$ini")
    if [[ $got != "$2" ]]; then
        echo "FAIL: game_timing for $(printf %q "$1") ${*:3}: got '$got', want '$2'"
        exit 1
    fi
}
timing $'fps_cap=60\n' "60 1"                            # default: fixed steps
timing $'fps_cap=60\ngame_timing=fixed\n' "60 1"
timing $'fps_cap=30\n' "30 1"
timing $'fps_cap=90\n' "90 1"
timing $'fps_cap=40\n' "uncap 1"                         # no fixed-step patch for 40
timing $'fps_cap=120\n' "uncap 1"
timing $'fps_cap=60\ngame_timing=variable\n' "uncap 1"
timing $'upscaler=off\n' "uncap 1"                        # no cap: the presenter's default, variable
timing $'fps_cap=60\n' "uncap -" BB_FPS=uncap             # explicit BB_FPS wins
timing $'fps_cap=60\n' "90 1" BB_FPS_LIMIT=90             # explicit limit picks the patch
timing $'fps_cap=90\n' "60 1" BB_FPS=uncap BB_FPS_AUTO=1 BB_FPS_LIMIT=60
timing $'fps_cap=60\ngame_timing=variable\n' "uncap 1" BB_FPS=60 BB_FPS_AUTO=1  # restart re-picks
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
# The co-op UDP port (p2p_port -> BB_P2P_PORT): digits only, the environment wins.
check_p2p() { # ini text, expected BB_P2P_PORT (- = unset), extra environment...
    printf '%s' "$1" > "$ini"
    local got
    got=$(env -u BB_P2P_PORT "${@:3}" /bin/bash -c \
        'source scripts/launch_settings.sh; bb_launch_settings "$1"; printf "%s" "${BB_P2P_PORT:--}"' _ "$ini")
    if [[ $got != "$2" ]]; then
        echo "FAIL: p2p_port launch setting for $(printf %q "$1") ${*:3}: got '$got', want '$2'"
        exit 1
    fi
}
check_p2p $'online=1\np2p_port=3658\n' "3658"
check_p2p $'p2p_port=0\n' "0"
check_p2p $'p2p_port=abc\n' "-"
check_p2p $'p2p_port=3658\n' "55692" BB_P2P_PORT=55692
# The Hunter's Dream pool (online_pool, thd_online_id, thd_token -> BB_NET_BACKEND=thd and the
# account); the token never reaches the command line here either.
check_pool() { # ini text, expected "BACKEND ID TOKENLEN" (- = unset), extra environment...
    printf '%s' "$1" > "$ini"
    local got
    got=$(env -u BB_NET_BACKEND -u BB_THD_ONLINE_ID -u BB_THD_TOKEN "${@:3}" /bin/bash -c \
        'source scripts/launch_settings.sh; bb_launch_settings "$1"; printf "%s %s %s" "${BB_NET_BACKEND:--}" "${BB_THD_ONLINE_ID:--}" "${#BB_THD_TOKEN}"' _ "$ini")
    if [[ $got != "$2" ]]; then
        echo "FAIL: pool launch settings for $(printf %q "$1") ${*:3}: got '$got', want '$2'"
        exit 1
    fi
}
check_pool $'online=1\nonline_pool=huntersdream\nthd_online_id=hunter\nthd_token=abcdef\n' "thd hunter 6"
check_pool $'online_pool=shadnet\nthd_online_id=hunter\n' "- hunter 0"
check_pool $'online_pool=other\n' "thd - 0"                             # unknown value: the default pool
check_pool $'online_pool=huntersdream\n' "shadnet - 0" BB_NET_BACKEND=shadnet
check_pool $'online=1\n' "thd - 0"                                      # no pool named: The Hunter's Dream
check_pool $'online=1\nthd_online_id=hunter\n' "thd hunter 0"
check_pool $'online=1\nshadnet_npid=hunter\n' "- - 0"                   # only a shadNet account: shadNet
check_pool $'online=1\nshadnet_npid=hunter\nthd_online_id=hunter\n' "thd hunter 0"
# Secrets from the keychain: a fake `security` stands in (the real one is the user's).
fake=$(mktemp -d); trap 'rm -rf "$ini" "$fake"' EXIT
cat > "$fake/security" <<'FAKE'
#!/bin/bash
# find-generic-password -s SERVICE -a ACCOUNT -w; every call is counted in calls.
echo "$3/$5" >> "$(dirname "$0")/calls"
case "$3/$5" in
    bbport-shadnet/hunter) echo 'pw-from-keychain' ;;
    bbport-huntersdream/dreamer) echo 'tok-from-keychain' ;;
    *) exit 44 ;;
esac
FAKE
chmod +x "$fake/security"
check_secret() { # ini text, expected "PASSWORD TOKEN" (- = unset), extra environment...
    printf '%s' "$1" > "$ini"
    local got
    got=$(env -u BB_SHADNET_PASSWORD -u BB_THD_TOKEN BB_KEYCHAIN_TOOL="$fake/security" "${@:3}" /bin/bash -c \
        'source scripts/launch_settings.sh; bb_launch_settings "$1" 2>/dev/null; printf "%s %s" "${BB_SHADNET_PASSWORD:--}" "${BB_THD_TOKEN:--}"' _ "$ini")
    if [[ $got != "$2" ]]; then
        echo "FAIL: keychain launch settings for $(printf %q "$1") ${*:3}: got '$got', want '$2'"
        exit 1
    fi
}
check_secret $'online=1\nshadnet_npid=hunter\nshadnet_password=keychain\n' "pw-from-keychain -"
check_secret $'online=1\nshadnet_npid=hunter\n' "pw-from-keychain -"                      # no line: the keychain is tried
check_secret $'online=1\nshadnet_npid=hunter\nshadnet_password=inline\n' "inline -"       # the file's value wins over the keychain
check_secret $'online=1\nshadnet_npid=nobody\nshadnet_password=keychain\n' "- -"          # nothing stored: unset, not "keychain"
check_secret $'online=1\nthd_online_id=dreamer\nthd_token=keychain\n' "- tok-from-keychain"
check_secret $'online=1\nshadnet_npid=hunter\nshadnet_password=keychain\n' "env -" BB_SHADNET_PASSWORD=env
check_secret $'shadnet_npid=hunter\nthd_online_id=dreamer\nthd_token=keychain\n' "pw-from-keychain tok-from-keychain" BB_ONLINE=1  # the environment turns it on
# Offline (online=0 or no online line, unless BB_ONLINE=1): no lookup, no secret exported, from
# the keychain or from the file.
rm -f "$fake/calls"
check_secret $'shadnet_npid=hunter\nshadnet_password=keychain\nthd_online_id=dreamer\nthd_token=keychain\n' "- -"
check_secret $'online=0\nshadnet_npid=hunter\nthd_online_id=dreamer\n' "- -"
check_secret $'online=0\nshadnet_npid=hunter\nshadnet_password=inline\nthd_online_id=dreamer\nthd_token=abcdef\n' "- -"
check_secret $'online=1\nshadnet_npid=hunter\nshadnet_password=inline\nthd_online_id=dreamer\nthd_token=abcdef\n' "- -" BB_ONLINE=0
[[ ! -e $fake/calls ]] || { echo "FAIL: offline launch settings looked in the keychain: $(tr '\n' ' ' < "$fake/calls")"; exit 1; }
echo "PASS: launch settings (fps_cap, fullscreen, texture_compression, game_timing, online, p2p_port, online_pool, keychain secrets)"

# In-game restart inherits the old environment: disabling wide mode must clear only our
# overrides and keep the user's external patch list and explicit memory limit.
(
    source scripts/launch_settings.sh
    unset BB_DMEM_MB BB_ULTRAWIDE_AUTO_DMEM BB_ULTRAWIDE_SESSION BB_UPSCALER
    export BB_PATCHES='Skip Intro'
    bb_native_ultrawide 'Resolution Patch 3840x1080 (32:9)'
    [[ $launch_patches == 'Skip Intro' ]]
    [[ $BB_DMEM_MB == 6144 && $BB_ULTRAWIDE_AUTO_DMEM == 1 && $BB_UPSCALER == none && $BB_ULTRAWIDE_RES == 3840x1080 ]]
    [[ -z ${BB_RENDER_RES:-} && -z ${BB_OUTPUT_RES:-} ]]
    # An inherited .1 test environment must shed its unsafe automatic heap override.
    export BB_DMEM_MB=9152 BB_ULTRAWIDE_AUTO_DMEM=1
    bb_native_ultrawide 'Resolution Patch 3840x1080 (32:9)'
    [[ $BB_DMEM_MB == 6144 && $BB_ULTRAWIDE_AUTO_DMEM == 1 ]]
    bb_native_ultrawide 'Resolution Patch 2560x720 (32:9)'
    [[ -z ${BB_DMEM_MB:-} && -z ${BB_ULTRAWIDE_AUTO_DMEM:-} ]]
    bb_native_ultrawide ''
    [[ $launch_patches == 'Skip Intro' && -z ${BB_DMEM_MB:-} && -z ${BB_UPSCALER:-} && -z ${BB_ULTRAWIDE_RES:-} ]]
    export BB_DMEM_MB=10240
    bb_native_ultrawide 'Resolution Patch 2560x1080 (21:9)'
    bb_native_ultrawide ''
    [[ $BB_DMEM_MB == 10240 ]]
) || { echo 'FAIL: ultrawide restart/disable'; exit 1; }
echo 'PASS: native ultrawide restart and disable'
