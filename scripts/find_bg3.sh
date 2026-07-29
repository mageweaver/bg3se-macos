#!/bin/bash
#
# BG3SE-macOS - Shared game-path resolver (source this, then call find_bg3_app)
#
# Resolution order (#90, #86):
#   1. BG3SE_GAME_PATH env override — the .app bundle, or a directory containing it
#   2. Default Steam library (~/Library/Application Support/Steam)
#   3. Every additional library in steamapps/libraryfolders.vdf (external drives)
#
# find_bg3_app prints the .app bundle path and returns 0, or returns 1.

find_bg3_app() {
    local app_names=("Baldur's Gate 3.app" "Baldurs Gate 3.app")
    local name dir

    if [[ -n "$BG3SE_GAME_PATH" ]]; then
        if [[ "$BG3SE_GAME_PATH" == *.app && -d "$BG3SE_GAME_PATH" ]]; then
            echo "$BG3SE_GAME_PATH"
            return 0
        fi
        for name in "${app_names[@]}"; do
            if [[ -d "$BG3SE_GAME_PATH/$name" ]]; then
                echo "$BG3SE_GAME_PATH/$name"
                return 0
            fi
        done
        echo "Warning: BG3SE_GAME_PATH is set but no BG3 app bundle found there: $BG3SE_GAME_PATH" >&2
    fi

    local steam="$HOME/Library/Application Support/Steam"
    local candidates=("$steam/steamapps/common/Baldurs Gate 3")

    local vdf="$steam/steamapps/libraryfolders.vdf"
    if [[ -f "$vdf" ]]; then
        local root
        while IFS= read -r root; do
            candidates+=("$root/steamapps/common/Baldurs Gate 3")
        done < <(grep -o '"path"[[:space:]]*"[^"]*"' "$vdf" | sed 's/.*"path"[[:space:]]*"//; s/"$//')
    fi

    for dir in "${candidates[@]}"; do
        for name in "${app_names[@]}"; do
            if [[ -d "$dir/$name" ]]; then
                echo "$dir/$name"
                return 0
            fi
        done
    done
    return 1
}
