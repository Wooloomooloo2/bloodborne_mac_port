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
echo "PASS: launch settings (fps_cap, fullscreen, texture_compression, game_timing)"

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

# Config-derived values must be recomputed on restart, including enabled -> disabled.
(
    source scripts/launch_settings.sh
    unset BB_FPS BB_FPS_AUTO BB_FPS_LIMIT BB_FULLSCREEN MTL_HUD_ENABLED BB_LAZY_TEXTURE_USAGE
    printf 'fps_cap=60\nfullscreen=1\nmetal_hud=1\ntexture_compression=0\n' > "$ini"
    bb_launch_settings "$ini"
    [[ $BB_FPS == 60 && $BB_FPS_LIMIT == 60 && $BB_FULLSCREEN == 1 && $MTL_HUD_ENABLED == 1 && $BB_LAZY_TEXTURE_USAGE == 0 ]]
    printf 'fps_cap=90\nfullscreen=0\nmetal_hud=0\ntexture_compression=1\n' > "$ini"
    export BB_VBLANK_HZ=60 BB_LAUNCH_AUTO_VBLANK=60
    bb_launch_settings "$ini"
    [[ -z ${BB_VBLANK_HZ:-} && -z ${BB_LAUNCH_AUTO_VBLANK:-} ]]
    [[ $BB_FPS == 90 && $BB_FPS_LIMIT == 90 && -z ${BB_FULLSCREEN:-} && -z ${MTL_HUD_ENABLED:-} && -z ${BB_LAZY_TEXTURE_USAGE:-} ]]
    # An explicit override changed after the first launch stays authoritative.
    export BB_FPS_LIMIT=120 BB_FULLSCREEN=0 MTL_HUD_ENABLED=0 BB_LAZY_TEXTURE_USAGE=1
    export BB_VBLANK_HZ=480
    bb_launch_settings "$ini"
    [[ $BB_VBLANK_HZ == 480 ]]
    [[ $BB_FPS_LIMIT == 120 && $BB_FULLSCREEN == 0 && $MTL_HUD_ENABLED == 0 && $BB_LAZY_TEXTURE_USAGE == 1 ]]
) || { echo 'FAIL: native menu launch settings restart'; exit 1; }
echo 'PASS: native menu launch settings restart'
