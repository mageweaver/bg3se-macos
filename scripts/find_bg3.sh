#!/bin/bash
#
# BG3SE-macOS - Shared game-path resolver (source this, then call find_bg3_app)
#
# Resolution order (#90, #86):
#   1. BG3SE_GAME_PATH env override — the .app bundle, or a directory containing it
#   2. Default Steam library (~/Library/Application Support/Steam)
#   3. Every additional library in steamapps/libraryfolders.vdf (external drives)
#   4. /Applications and ~/Applications (GOG, or a hand-placed bundle)
#
# find_bg3_app prints the .app bundle path and returns 0, or returns 1.
#
# find_bg3_exec prints the real game binary inside a bundle. On GOG that is not
# CFBundleExecutable -- that is a ~200KB arch-selector stub which hands off to
# "<CFBundleExecutable> GOG". Patching or injecting the stub misses the game.

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

    # Non-Steam installs: GOG offers both, and neither is under a Steam library.
    candidates+=("/Applications" "$HOME/Applications")

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

find_bg3_exec() {
    local app="$1"
    [[ -n "$app" ]] || app="$(find_bg3_app)" || return 1

    local exec_name
    exec_name="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' \
        "$app/Contents/Info.plist" 2>/dev/null)"
    # No readable plist (game not installed): the bundle stem matches in every
    # BG3 build shipped so far.
    [[ -n "$exec_name" ]] || exec_name="$(basename "$app" .app)"

    local suffix
    for suffix in " GOG" " Steam"; do
        if [[ -f "$app/Contents/MacOS/${exec_name}${suffix}" ]]; then
            echo "$app/Contents/MacOS/${exec_name}${suffix}"
            return 0
        fi
    done
    echo "$app/Contents/MacOS/${exec_name}"
}
