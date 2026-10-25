#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Mark Hall
set -euo pipefail
cd -- "$(dirname -- "$0")"
# BB_PREBUILT=1 (packaged builds, Bloodborne.app): out/bb-probe and its GPU library are installed
# next to this script; nothing is built.
# BB_DATA_DIR: writable directory for the generated files (out/), saves (user/) and bbport.ini;
# by default ~/Library/Application Support/Bloodborne.
# The game process (x86-64) runs under Rosetta 2; a native arm64 process renders with Metal.
if [[ $OSTYPE != darwin* ]]; then echo 'This port runs on macOS (Apple Silicon) only.' >&2; exit 1; fi
data=${BB_DATA_DIR:-.}
# Data defaults to ~/Library/Application Support/Bloodborne. A checkout that already keeps
# saves in this directory goes on using them (moving saves is left to the player).
if [[ -z ${BB_DATA_DIR:-} ]]; then
    data="$HOME/Library/Application Support/Bloodborne"
    if [[ ! -d $data && ( -d user/savedata || -f bbport.ini ) ]]; then
        data=.
        echo "Saves and settings stay in $PWD (move user/ and bbport.ini to $HOME/Library/Application Support/Bloodborne to use the default)."
    fi
    # A "Saves" link next to this script leads to the save folder from Finder.
    if [[ $data != . ]]; then
        mkdir -p "$data/user/savedata"
        [[ -e Saves || -L Saves ]] || ln -s "$data/user/savedata" Saves 2>/dev/null || true
    fi
fi
out=$data/out
mkdir -p "$out"
export BB_CONFIG=${BB_CONFIG:-$data/bbport.ini}
# The launcher's fps_cap / fullscreen (bbport.ini) as BB_FPS_LIMIT / BB_FULLSCREEN.
# The windowless guest does not pump SDL quit events. Leave SIGTERM at its normal
# disposition so Stop works after SDL audio/gamepad init, including the mod-view child.
# The native GPU host installs its own termination handling and watches parent exit.
export SDL_NO_SIGNAL_HANDLERS=1
source scripts/launch_settings.sh
bb_launch_settings "$BB_CONFIG"
# GPU caches (pipelines, shaders) next to the saves. Exported before anything starts: the GPU
# library reads it while loading, in bb-probe and in the native GPU host alike.
export BB_GPU_USER_DIR=${BB_GPU_USER_DIR:-${BB_USER_DIR:-$data/user}}
mkdir -p "$BB_GPU_USER_DIR"
if [[ -z ${PYTHON:-} ]]; then PYTHON=$(command -v python3 || true); fi
if [[ -z ${PYTHON:-} ]]; then echo 'Install Python 3 or set PYTHON.' >&2; exit 1; fi
# BB_GAME_DIR: the game's folder (eboot.bin, sce_module, ...); default next to this directory.
game=${BB_GAME_DIR:-../CUSA03173}
if [[ ! -f $game/eboot.bin ]]; then echo "No eboot.bin in $game (set BB_GAME_DIR)." >&2; exit 1; fi
# Hooks and patches use the addresses of the 1.09 executable (BB_SKIP_GAME_CHECK=1 skips this).
if ! "$PYTHON" scripts/game_check.py "$game" >&2; then exit 1; fi
original_game=$game
mod_views=()
cleanup_mod_views() {
    for view in "${mod_views[@]}"; do
        "$PYTHON" -c 'import shutil,sys; shutil.rmtree(sys.argv[1])' "$view"
    done
}
game=$("$PYTHON" scripts/mods.py "$game" --out "$out" \
    --mods-dir "${BB_MODS_DIR:-$data/mods}" --config "${BB_MODS_CONFIG:-$data/mods.json}" \
    --enabled "${BB_MODS_ENABLED:-1}")
# A private merged view lasts for this launch, including restarts. Cleanup only our own view.
if [[ $game != "$(realpath "$original_game")" ]]; then
    mod_game=$game
    mod_views+=("$mod_game")
    trap cleanup_mod_views EXIT
fi
unset BB_MAC_SETTINGS_READY BB_EXIT_GAME_READY
if [[ ${BB_MAC_SETTINGS:-1} != 0 || ${BB_EXIT_GAME:-1} != 0 ]]; then
    if mac_game=$("$PYTHON" scripts/mac_menu.py "$game" "$out") && [[ -n $mac_game && -d $mac_game ]]; then
        game=$mac_game
        mod_game=$mac_game
        mod_views+=("$mac_game")
        export BB_MAC_SETTINGS_READY=1
        export BB_EXIT_GAME_READY=1
        trap cleanup_mod_views EXIT
    fi
fi
"$PYTHON" scripts/prepare.py "$game" --out "$out"
"$PYTHON" scripts/link_libc.py "$game" --out "$out"
"$PYTHON" scripts/link_modules.py "$game" --out "$out"
"$PYTHON" scripts/content_profile.py "$game" --out "$out" --sku "${BB_CONTENT_SKU:-full}"
# Sizes chosen below for the previous launch are recomputed after an in-game restart.
if [[ ${BB_AUTO_RENDER_RES:-} == 1 ]]; then
    unset BB_RENDER_RES BB_OUTPUT_RES BB_AUTO_RENDER_RES
fi
# BB_RENDER_RES=WxH explicitly sets the game's render resolution (a patch at start).
# Frame rate: BB_FPS=uncap (delta-time patch, vblank follows the display), 60/90 (fixed-timestep
# patches) or 30 (unpatched). Unset, scripts/launch_settings.sh picks it from bbport.ini fps_cap
# and game_timing (fixed steps for caps 30/60/90 unless game_timing=variable). BB_PATCHES adds patch names ("a;b").
fps=${BB_FPS:-uncap}
# bbport.ini output_res other than 1080p (720p for the Steam Deck, 1440p, 2160p): the whole game
# renders at the preset's size of the output (a patch), the upscaler fills the output, the UI is
# drawn at the output size. Preset and output changes need a restart.
# Live resolution changes keep the guest at 1920x1080 and scale host targets at run time instead
# (output and presets change in the menu without a restart, but post-processing stays at 1080p
# and scene targets are copied back: much slower on the Steam Deck and older GPUs). Chosen by
# BB_LIVE_RES=0/1, else bbport.ini live_resolution=0/1/auto (auto: the GPU check, strong
# discrete GPUs get them); off when unset. 1080p output and TAA always use the live path.
wide_patch=$("$PYTHON" scripts/patches.py --print-ultrawide --settings "$BB_CONFIG")
bb_native_ultrawide "$wide_patch"
scaled_render='' scaled_output=''
if [[ -z $wide_patch && -z ${BB_RENDER_RES:-} ]]; then
    read -r scaled_render scaled_output < <("$PYTHON" scripts/patches.py --print-scaled --settings "$BB_CONFIG") || true
fi
live=0 above_1080=0
if [[ -n ${scaled_output:-} ]]; then
    live=${BB_LIVE_RES:-}
    # Bash builtins only (no sed/grep): a bare PATH must not end run.sh silently.
    if [[ -z $live && -f $BB_CONFIG ]]; then
        while IFS= read -r line || [[ -n $line ]]; do
            [[ $line =~ ^live_resolution=([01]|auto)$ ]] && live=${BASH_REMATCH[1]}
        done < "$BB_CONFIG"
    fi
    # auto is off: the old Vulkan check wanted a discrete GPU with 8 GB, which a Mac never has.
    [[ $live == 1 ]] || live=0
    # Above 1920x1080 the startup patch breaks the scene's geometry (stretched polygons across
    # the screen, seen at 2560x1440 on macOS in both process modes): the live path keeps the
    # game's buffers at 1080p and renders host targets at the size instead.
    IFS=x read -r render_w render_h <<< "$scaled_render"
    if (( render_w > 1920 || render_h > 1080 )); then live=1 above_1080=1; fi
fi
if [[ $live == 1 && $above_1080 == 1 ]]; then
    echo "Output ${scaled_output}: scene ${scaled_render} through live resolution changes (the startup patch breaks scenes above 1080p)"
elif [[ $live == 1 ]]; then
    echo "Output ${scaled_output}: live resolution changes (live_resolution=0: startup patch)"
elif [[ -n ${scaled_output:-} ]]; then
    export BB_RENDER_RES=$scaled_render BB_OUTPUT_RES=$scaled_output BB_AUTO_RENDER_RES=1
    # More direct memory only with patches.py's heap patch (scenes larger than 1920x1080). At
    # or below 1080p the retail 5056 MiB is enough, and 9152 made the GPU arena 10 GiB, past a
    # 16 GB Mac's Metal working set (#22: 9 FPS, Metal 11.9 GB allocated at 720p).
    if (( render_w * render_h > 1920 * 1080 )); then export BB_DMEM_MB=${BB_DMEM_MB:-9152}; fi
    echo "Output ${scaled_output}: scene ${scaled_render}, direct memory ${BB_DMEM_MB:-5056} MiB (live_resolution=1: live changes)"
fi
"$PYTHON" scripts/patches.py --out "$out" --fps "$fps" --extra "$launch_patches" --settings "$BB_CONFIG" --game-dir "$game" --render-res "${BB_RENDER_RES:-}" --output-res "${BB_OUTPUT_RES:-}" \
    --patches-dir "${BB_PATCHES_DIR:-$data/patches}" --patches-config "${BB_PATCHES_CONFIG:-$data/patches.json}"
if [[ -z ${BB_VBLANK_HZ:-} ]]; then
    case $fps in uncap) export BB_VBLANK_HZ=0 ;; 90) export BB_VBLANK_HZ=90 ;; *) export BB_VBLANK_HZ=60 ;; esac
    export BB_LAUNCH_AUTO_VBLANK=$BB_VBLANK_HZ
fi
if [[ -n ${BB_PREBUILT:-} ]]; then
    probe=${BB_PROBE:-bin/bb-probe}
else
    bash build_macos.sh
    probe=out/bb-probe
fi
probe_args=("$out/boot-linked.bin" --content-profile "$out/content.bin" --patches "$out/patches.bin" --app0 "$game" --user "${BB_USER_DIR:-$data/user}" --timeout "${BB_TIMEOUT:-0}" "$@")
if [[ -n ${mod_game:-} ]]; then
    "$probe" "${probe_args[@]}" &
    mod_pid=$!
    trap 'kill -TERM "$mod_pid" 2>/dev/null || true' TERM INT
    mod_status=0
    wait "$mod_pid" || mod_status=$?
    # An interrupted wait must finish the child before removing its mounted view.
    if kill -0 "$mod_pid" 2>/dev/null; then wait "$mod_pid" || mod_status=$?; fi
    exit "$mod_status"
fi
exec "$probe" "${probe_args[@]}"
