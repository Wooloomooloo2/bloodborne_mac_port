# SPDX-License-Identifier: GPL-2.0-or-later
# Sourced by run.sh: bbport.ini settings the launcher writes that become environment variables.
#   fps_cap=30|40|60|90|120  the presenter lets at most that many frames through
#                            (BB_FPS_LIMIT); the game keeps run.sh's default frame rate patch
#   fullscreen=1             the window opens full screen (BB_FULLSCREEN=1)
# Online play (the online module, gpu/bbnet; run.sh):
#   online=1                 online play on (BB_ONLINE=1)
#   shadnet_server=host:port the shadNet server (BB_SHADNET_SERVER; default srv.shadps4.net:31313)
#   shadnet_webapi=url       its WebAPI (BB_SHADNET_WEBAPI; default http://<server host>:31315)
#   shadnet_npid=name        the shadNet account (BB_SHADNET_NPID) ...
#   shadnet_password=secret  ... and its password (BB_SHADNET_PASSWORD; the file is the player's own)
#   community_server=url     the game server (BB_COMMUNITY_SERVER; default https://thehuntersdream.com)
#   upnp=0                   no UPnP port mapping (BB_UPNP=0; on by default)
#   metal_hud=1              Apple's Metal Performance HUD (MTL_HUD_ENABLED=1)
# Values already in the environment win. Bash 3.2 (macOS /bin/bash) compatible.
bb_launch_settings() { # ini file
    local line cap='' full='' hud='' online='' server='' webapi='' npid='' password='' community='' upnp=''
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
    done < "$1"
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
    return 0
}
