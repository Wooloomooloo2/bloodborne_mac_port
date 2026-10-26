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
# Online play (the online module, gpu/bbnet; run.sh):
#   online=1                 online play on (BB_ONLINE=1); the secrets below are read (file or
#                            keychain) and exported only then
#   shadnet_server=host:port the shadNet server (BB_SHADNET_SERVER; default srv.shadps4.net:31313)
#   shadnet_webapi=url       its WebAPI (BB_SHADNET_WEBAPI; default http://<server host>:31315)
#   shadnet_npid=name        the shadNet account (BB_SHADNET_NPID) ...
#   shadnet_password=secret  ... and its password (BB_SHADNET_PASSWORD; the file is the player's own),
#                            or shadnet_password=keychain (or no such line) to take it from the login
#                            keychain: item service bbport-shadnet, account = the npid
#                            (`python3 scripts/online.py keychain shadnet --npid NAME --ini bbport.ini`)
#   community_server=url     the game server (BB_COMMUNITY_SERVER; default https://thehuntersdream.com)
#   upnp=0                   no UPnP port mapping (BB_UPNP=0; on by default)
#   p2p_port=3658            the UDP port for co-op traffic (BB_P2P_PORT; 3658, the PS4's, by
#                            default; 0 = any free port; a busy port falls back to a free one)
#   online_pool=huntersdream bells and summons through The Hunter's Dream (BB_NET_BACKEND=thd), the
#                            default; online_pool=shadnet picks a shadNet server instead (also the
#                            choice when no pool is named but only a shadNet account is) ...
#   thd_online_id=name       ... with the Discord-linked account (BB_THD_ONLINE_ID) ...
#   thd_token=secret         ... and its token (BB_THD_TOKEN; `scripts/online.py signin` writes both),
#                            or thd_token=keychain: item service bbport-huntersdream, account = the
#                            online id (`scripts/online.py signin --keychain` stores it there)
#   metal_hud=1              Apple's Metal Performance HUD (MTL_HUD_ENABLED=1)
#   texture_compression=0    textures keep every usage, no lossless compression
#                            (BB_LAZY_TEXTURE_USAGE=0; default 1, compressed)
# Values already in the environment win. Bash 3.2 (macOS /bin/bash) compatible.
bb_launch_settings() { # ini file
    local line cap='' full='' hud='' compress='' online='' server='' webapi='' npid='' password='' community='' upnp='' p2p=''
    local pool='' thd_id='' thd_token='' timing=''
    # BB_FPS chosen here for the previous launch is chosen again after an in-game restart.
    if [[ ${BB_FPS_AUTO:-} == 1 ]]; then unset BB_FPS BB_FPS_AUTO; fi
    [[ -f $1 ]] || return 0
    while IFS= read -r line || [[ -n $line ]]; do
        line=${line%$'\r'}
        if [[ $line =~ ^fps_cap=(30|40|60|90|120)$ ]]; then cap=${BASH_REMATCH[1]}; fi
        if [[ $line =~ ^fullscreen=([01])$ ]]; then full=${BASH_REMATCH[1]}; fi
        if [[ $line =~ ^metal_hud=([01])$ ]]; then hud=${BASH_REMATCH[1]}; fi
        if [[ $line =~ ^online=([01])$ ]]; then online=${BASH_REMATCH[1]}; fi
        if [[ $line =~ ^shadnet_server=(.+)$ ]]; then server=${BASH_REMATCH[1]}; fi
        if [[ $line =~ ^shadnet_webapi=(.+)$ ]]; then webapi=${BASH_REMATCH[1]}; fi
        if [[ $line =~ ^shadnet_npid=(.+)$ ]]; then npid=${BASH_REMATCH[1]}; fi
        if [[ $line =~ ^shadnet_password=(.+)$ ]]; then password=${BASH_REMATCH[1]}; fi
        if [[ $line =~ ^community_server=(.+)$ ]]; then community=${BASH_REMATCH[1]}; fi
        if [[ $line =~ ^upnp=([01])$ ]]; then upnp=${BASH_REMATCH[1]}; fi
        if [[ $line =~ ^p2p_port=([0-9]{1,5})$ ]]; then p2p=${BASH_REMATCH[1]}; fi
        if [[ $line =~ ^online_pool=(shadnet|huntersdream)$ ]]; then pool=${BASH_REMATCH[1]}; fi
        if [[ $line =~ ^thd_online_id=(.+)$ ]]; then thd_id=${BASH_REMATCH[1]}; fi
        if [[ $line =~ ^thd_token=(.+)$ ]]; then thd_token=${BASH_REMATCH[1]}; fi
        if [[ $line =~ ^texture_compression=([01])$ ]]; then compress=${BASH_REMATCH[1]}; fi
        if [[ $line =~ ^game_timing=(fixed|variable)$ ]]; then timing=${BASH_REMATCH[1]}; fi
    done < "$1"
    # The password and the token are looked up and exported only for online play (online=1, or
    # BB_ONLINE=1 from the environment, which wins): offline, no secret enters the environment.
    if [[ ${BB_ONLINE:-$online} != 1 ]]; then password='' thd_token=''; fi
    # Secrets kept in the login keychain instead of the file (`security find-generic-password`;
    # BB_KEYCHAIN_TOOL names another command, for tests). A lookup that finds nothing leaves the
    # value empty; an explicit "keychain" that finds nothing is reported.
    local tool=${BB_KEYCHAIN_TOOL:-security}
    if [[ ${BB_ONLINE:-$online} == 1 && -n $npid && -z ${BB_SHADNET_PASSWORD:-} && ( -z $password || $password == keychain ) ]]; then
        local want=$password
        password=$("$tool" find-generic-password -s bbport-shadnet -a "$npid" -w 2>/dev/null) || password=''
        if [[ $want == keychain && -z $password ]]; then
            echo "Online: no keychain item bbport-shadnet for $npid (python3 scripts/online.py keychain shadnet --npid $npid)" >&2
        fi
    fi
    if [[ ${BB_ONLINE:-$online} == 1 && -n $thd_id && -z ${BB_THD_TOKEN:-} && ( -z $thd_token || $thd_token == keychain ) ]]; then
        local want_token=$thd_token
        thd_token=$("$tool" find-generic-password -s bbport-huntersdream -a "$thd_id" -w 2>/dev/null) || thd_token=''
        if [[ $want_token == keychain && -z $thd_token ]]; then
            echo "Online: no keychain item bbport-huntersdream for $thd_id (python3 scripts/online.py signin --keychain)" >&2
        fi
    fi
    if [[ -z ${BB_FPS:-} ]]; then
        export BB_FPS=uncap BB_FPS_AUTO=1
        if [[ $timing != variable ]]; then
            case ${BB_FPS_LIMIT:-$cap} in 30|60|90) BB_FPS=${BB_FPS_LIMIT:-$cap} ;; esac
        fi
    fi
    if [[ -n $cap && -z ${BB_FPS_LIMIT:-} ]]; then export BB_FPS_LIMIT=$cap; fi
    if [[ $full == 1 && -z ${BB_FULLSCREEN:-} ]]; then export BB_FULLSCREEN=1; fi
    if [[ $hud == 1 && -z ${MTL_HUD_ENABLED:-} ]]; then export MTL_HUD_ENABLED=1; fi
    if [[ $online == 1 && -z ${BB_ONLINE:-} ]]; then export BB_ONLINE=1; fi
    if [[ -n $server && -z ${BB_SHADNET_SERVER:-} ]]; then export BB_SHADNET_SERVER=$server; fi
    if [[ -n $webapi && -z ${BB_SHADNET_WEBAPI:-} ]]; then export BB_SHADNET_WEBAPI=$webapi; fi
    if [[ -n $npid && -z ${BB_SHADNET_NPID:-} ]]; then export BB_SHADNET_NPID=$npid; fi
    if [[ -n $password && -z ${BB_SHADNET_PASSWORD:-} ]]; then export BB_SHADNET_PASSWORD=$password; fi
    if [[ -n $community && -z ${BB_COMMUNITY_SERVER:-} ]]; then export BB_COMMUNITY_SERVER=$community; fi
    if [[ -n $upnp && -z ${BB_UPNP:-} ]]; then export BB_UPNP=$upnp; fi
    if [[ -n $p2p && -z ${BB_P2P_PORT:-} ]]; then export BB_P2P_PORT=$p2p; fi
    # The pool: The Hunter's Dream unless the file says shadnet, or names a shadNet account and
    # no Hunter's Dream one (a file from before the pool key).
    if [[ -z $pool ]]; then
        if [[ -n $npid && -z $thd_id ]]; then pool=shadnet; else pool=huntersdream; fi
    fi
    if [[ $pool == huntersdream && -z ${BB_NET_BACKEND:-} ]]; then export BB_NET_BACKEND=thd; fi
    if [[ -n $thd_id && -z ${BB_THD_ONLINE_ID:-} ]]; then export BB_THD_ONLINE_ID=$thd_id; fi
    if [[ -n $thd_token && -z ${BB_THD_TOKEN:-} ]]; then export BB_THD_TOKEN=$thd_token; fi
    if [[ $compress == 0 && -z ${BB_LAZY_TEXTURE_USAGE:-} ]]; then export BB_LAZY_TEXTURE_USAGE=0; fi
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
