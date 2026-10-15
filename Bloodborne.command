#!/usr/bin/env bash
# Double-click in Finder to play: opens Terminal and runs run.sh (which rebuilds the game
# process if the sources changed). Keep the Terminal window open while playing; closing it
# ends the game. Without a Terminal window: Bloodborne.app (bash scripts/macos/make_app.sh).
#
# Settings live in launch.conf next to this file (not committed), as shell assignments:
#   BB_GAME_DIR=/path/to/CUSA03173   the game folder (asked for on first launch)
#   BB_DATA_DIR=/path/to/data        saves, shader caches and bbport.ini
#                                    (default: ~/Library/Application Support/Bloodborne)
# Any other run.sh variable (BB_FPS, BB_CONFIG, ...) can be set there too.
set -uo pipefail
cd -- "$(dirname -- "$0")" || exit 1

finish() {
    echo
    read -r -p 'Press Return to close this window.' _
    exit "$1"
}

# shellcheck source=scripts/macos/launch_common.sh
source scripts/macos/launch_common.sh
bb_prepare_launch || finish 1

echo "Game:  $BB_GAME_DIR"
echo "Data:  ${BB_DATA_DIR:-$HOME/Library/Application Support/Bloodborne}"
echo
bash run.sh "$@"
status=$?
if (( status != 0 )); then
    echo "The game exited with status $status."
    finish "$status"
fi
