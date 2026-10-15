# Shared by Bloodborne.command and Bloodborne.app (scripts/macos/make_app.sh). Source it from the
# repository root. Reads launch.conf, finds the game folder (asks once with a folder picker and
# remembers it) and exports BB_GAME_DIR (and BB_DATA_DIR when set). Returns 1 with a message on
# stderr when there is nothing to launch.

bb_conf=$PWD/launch.conf

bb_prepare_launch() {
    if [[ -f $bb_conf ]]; then
        set -a
        # shellcheck disable=SC1090
        source "$bb_conf"
        set +a
    fi
    if [[ ! -x out/bb-probe || ! -x out/bbgpu-host ]]; then
        echo 'The port is not built yet. In Terminal, from the repository folder, run once:' >&2
        echo '  bash scripts/macos/bootstrap_deps.sh' >&2
        echo '  BB_DEPS_ARCH=arm64 bash scripts/macos/bootstrap_deps.sh' >&2
        echo '  bash build_macos.sh' >&2
        return 1
    fi
    # The game folder: launch.conf, else ../CUSA03173, else ask (and remember the answer).
    if [[ -z ${BB_GAME_DIR:-} && -f ../CUSA03173/eboot.bin ]]; then
        BB_GAME_DIR=$(cd ../CUSA03173 && pwd)
    fi
    if [[ -z ${BB_GAME_DIR:-} || ! -f $BB_GAME_DIR/eboot.bin ]]; then
        local picked
        picked=$(osascript -e 'POSIX path of (choose folder with prompt "Choose your Bloodborne game folder (CUSA03173, the one containing eboot.bin):")' 2>/dev/null) || {
            echo 'No game folder chosen.' >&2
            return 1
        }
        picked=${picked%/}
        if [[ ! -f $picked/eboot.bin ]]; then
            echo "No eboot.bin in $picked." >&2
            return 1
        fi
        BB_GAME_DIR=$picked
        { grep -v '^BB_GAME_DIR=' "$bb_conf" 2>/dev/null; printf 'BB_GAME_DIR=%q\n' "$BB_GAME_DIR"; } > "$bb_conf.tmp" &&
            mv "$bb_conf.tmp" "$bb_conf"
        echo "Game folder saved in launch.conf: $BB_GAME_DIR"
    fi
    export BB_GAME_DIR
    if [[ -n ${BB_DATA_DIR:-} ]]; then export BB_DATA_DIR; fi
    return 0
}
