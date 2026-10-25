# SPDX-FileCopyrightText: Copyright 2026 Mark Hall
# SPDX-License-Identifier: GPL-2.0-or-later
# Sourced by run.sh: bbport.ini settings the launcher writes that become environment variables.
#   fps_cap=30|40|60|90|120  the presenter lets at most that many frames through
#                            (BB_FPS_LIMIT)
#   game_timing=fixed|variable  the game's frame rate patch (BB_FPS, run.sh). fixed (default):
#                            the game steps by 1/cap seconds (the unpatched game at 30, the
#                            60/90 FPS++ patches), slowing down when frames are late; variable:
#                            Uncap FPS++ steps by each frame's measured time, which keeps speed
#                            but lets you sink through moving lifts on uneven frames (user,
#                            2026-10-23). Caps without a fixed-step patch (40, 120) are variable.
#   fullscreen=1             the window opens full screen (BB_FULLSCREEN=1)
#   metal_hud=1              Apple's Metal Performance HUD (MTL_HUD_ENABLED=1)
#   texture_compression=0    textures keep every usage, no lossless compression
#                            (BB_LAZY_TEXTURE_USAGE=0; default 1, compressed)
# Values already in the environment win. Bash 3.2 (macOS /bin/bash) compatible.
bb_launch_settings() { # ini file
    local line cap='' full='' hud='' compress='' timing=''
    # Clear values exported from the previous config, keeping explicit caller overrides.
    if [[ ${BB_LAUNCH_AUTO_LIMIT:-} == "${BB_FPS_LIMIT:-unset}" ]]; then unset BB_FPS_LIMIT; fi
    if [[ ${BB_LAUNCH_AUTO_FULLSCREEN:-} == "${BB_FULLSCREEN:-unset}" ]]; then unset BB_FULLSCREEN; fi
    if [[ ${BB_LAUNCH_AUTO_HUD:-} == "${MTL_HUD_ENABLED:-unset}" ]]; then unset MTL_HUD_ENABLED; fi
    if [[ ${BB_LAUNCH_AUTO_COMPRESSION:-} == "${BB_LAZY_TEXTURE_USAGE:-unset}" ]]; then unset BB_LAZY_TEXTURE_USAGE; fi
    if [[ ${BB_LAUNCH_AUTO_VBLANK:-} == "${BB_VBLANK_HZ:-unset}" ]]; then unset BB_VBLANK_HZ; fi
    unset BB_LAUNCH_AUTO_LIMIT BB_LAUNCH_AUTO_FULLSCREEN BB_LAUNCH_AUTO_HUD BB_LAUNCH_AUTO_COMPRESSION BB_LAUNCH_AUTO_VBLANK
    # BB_FPS chosen here for the previous launch is chosen again after an in-game restart.
    if [[ ${BB_FPS_AUTO:-} == 1 ]]; then unset BB_FPS BB_FPS_AUTO; fi
    [[ -f $1 ]] || return 0
    while IFS= read -r line || [[ -n $line ]]; do
        line=${line%$'\r'}
        if [[ $line =~ ^fps_cap=(30|40|60|90|120)$ ]]; then cap=${BASH_REMATCH[1]}; fi
        if [[ $line =~ ^fullscreen=([01])$ ]]; then full=${BASH_REMATCH[1]}; fi
        if [[ $line =~ ^metal_hud=([01])$ ]]; then hud=${BASH_REMATCH[1]}; fi
        if [[ $line =~ ^texture_compression=([01])$ ]]; then compress=${BASH_REMATCH[1]}; fi
        if [[ $line =~ ^game_timing=(fixed|variable)$ ]]; then timing=${BASH_REMATCH[1]}; fi
    done < "$1"
    if [[ -z ${BB_FPS:-} ]]; then
        export BB_FPS=uncap BB_FPS_AUTO=1
        if [[ $timing != variable ]]; then
            case ${BB_FPS_LIMIT:-$cap} in 30|60|90) BB_FPS=${BB_FPS_LIMIT:-$cap} ;; esac
        fi
    fi
    if [[ -n $cap && -z ${BB_FPS_LIMIT:-} ]]; then export BB_FPS_LIMIT=$cap BB_LAUNCH_AUTO_LIMIT=$cap; fi
    if [[ $full == 1 && -z ${BB_FULLSCREEN:-} ]]; then export BB_FULLSCREEN=1 BB_LAUNCH_AUTO_FULLSCREEN=1; fi
    if [[ $hud == 1 && -z ${MTL_HUD_ENABLED:-} ]]; then export MTL_HUD_ENABLED=1 BB_LAUNCH_AUTO_HUD=1; fi
    if [[ $compress == 0 && -z ${BB_LAZY_TEXTURE_USAGE:-} ]]; then export BB_LAZY_TEXTURE_USAGE=0 BB_LAUNCH_AUTO_COMPRESSION=0; fi
    return 0
}

# Recomputed on every launch/restart. Never append automatic patches to the inherited
# BB_PATCHES, otherwise disabling ultrawide would leave the previous patch active.
bb_native_ultrawide() { # a patch name validated by patches.py, or empty for 16:9
    local wide_patch=$1 wide_w wide_h
    launch_patches=${BB_PATCHES:-}
    unset BB_ULTRAWIDE_RES
    if [[ ${BB_ULTRAWIDE_SESSION:-} == 1 ]]; then
        unset BB_UPSCALER BB_ULTRAWIDE_SESSION
    fi
    if [[ ${BB_ULTRAWIDE_AUTO_DMEM:-} == 1 ]]; then
        unset BB_DMEM_MB BB_ULTRAWIDE_AUTO_DMEM
    fi
    if [[ -n $wide_patch ]]; then
        export BB_ULTRAWIDE_RES=${wide_patch#Resolution Patch }
        BB_ULTRAWIDE_RES=${BB_ULTRAWIDE_RES%% *}
        # The complete game patch owns scene/UI sizes. Do not resize/composite it again.
        export BB_UPSCALER=none BB_ULTRAWIDE_SESSION=1
        unset BB_RENDER_RES BB_OUTPUT_RES BB_AUTO_RENDER_RES
        wide_w=${BB_ULTRAWIDE_RES%x*}
        wide_h=${BB_ULTRAWIDE_RES#*x}
        if (( wide_w * wide_h > 1920 * 1080 )) && [[ -z ${BB_DMEM_MB:-} ]]; then
            export BB_DMEM_MB=6144 BB_ULTRAWIDE_AUTO_DMEM=1
        fi
        # Complete resolution patch, retail first heap, bounded second texture pool.
        echo "Ultrawide: native $BB_ULTRAWIDE_RES; bounded texture pool; DMEM ${BB_DMEM_MB:-5056} MiB; no upscaling"
    fi
}
