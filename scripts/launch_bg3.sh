#!/bin/bash
#
# BG3SE-macOS Launcher Script
#
# This script launches Baldur's Gate 3 with the Script Extender dylib injected.
# Usage: ./launch_bg3.sh [path_to_dylib]
#

set -e

# Find the game (BG3SE_GAME_PATH override, then Steam libraries)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
source "$SCRIPT_DIR/find_bg3.sh"

BG3_APP="$(find_bg3_app)" || {
    echo "Error: Baldur's Gate 3 not found in any Steam library."
    echo "Set BG3SE_GAME_PATH to the game's .app bundle (or its parent directory)."
    exit 1
}
BG3_EXEC="${BG3_APP}/Contents/MacOS/Baldur's Gate 3"

# Check for dylib in order of preference
if [[ -n "$1" ]]; then
    DYLIB="$1"
elif [[ -f "${PROJECT_ROOT}/build/lib/libbg3se.dylib" ]]; then
    DYLIB="${PROJECT_ROOT}/build/lib/libbg3se.dylib"
elif [[ -f "${PROJECT_ROOT}/lib/libbg3se.dylib" ]]; then
    DYLIB="${PROJECT_ROOT}/lib/libbg3se.dylib"
else
    echo "Error: Cannot find libbg3se.dylib"
    echo "Build it first with: cd build && cmake .. && make"
    exit 1
fi

# Verify paths exist
if [[ ! -f "$BG3_EXEC" ]]; then
    echo "Error: Baldur's Gate 3 not found at:"
    echo "  $BG3_EXEC"
    exit 1
fi

if [[ ! -f "$DYLIB" ]]; then
    echo "Error: Dylib not found at:"
    echo "  $DYLIB"
    exit 1
fi

# Session logs land here (latest.log is a per-session symlink)
LOG_DIR="$HOME/Library/Application Support/BG3SE/logs"

echo "=========================================="
echo "BG3SE-macOS Launcher"
echo "=========================================="
echo "Game:   $BG3_EXEC"
echo "Dylib:  $DYLIB"
echo ""

# Check if dylib is universal binary
echo "Dylib architecture:"
file "$DYLIB"
echo ""

# Launch with DYLD injection
echo "Launching Baldur's Gate 3 with Script Extender..."
echo "(Session log: $LOG_DIR/latest.log)"
echo ""

# Launch through LaunchServices, NOT by exec'ing the binary directly.
#
# The game binary carries a weak `@loader_path/libbg3se.dylib` load command, so
# it loads the extender from its own bundle directory on its own -- no
# DYLD_INSERT_LIBRARIES required. Deploying into the bundle and using `open` is
# therefore enough to get BG3SE loaded.
#
# Why this matters (2026-08-27): the previous form,
#     DYLD_INSERT_LIBRARIES="$DYLIB" "$BG3_EXEC" &
# execs the binary directly, bypassing Steam/LaunchServices. Sessions do not
# start that way -- loading a save either bounced back to the main menu or died
# with a null deref in esv::SpellSystem::ProcessInvalidateRequests. The same
# save loads cleanly via `open` with the extender fully active, so the launch
# method was the culprit, not BG3SE. It also double-loaded the dylib (the weak
# bundle copy plus the inserted one), which the loader logged as
# "duplicate dylib image IGNORED".
LAUNCH_TS=$(date +%s)

BUNDLE_DYLIB="$(dirname "$BG3_EXEC")/libbg3se.dylib"
if [[ "$DYLIB" != "$BUNDLE_DYLIB" ]]; then
    echo "Deploying dylib into the app bundle (@loader_path target)..."
    cp "$DYLIB" "$BUNDLE_DYLIB" || {
        echo "Error: could not copy dylib into $BUNDLE_DYLIB"
        exit 1
    }
fi

# LaunchServices does not inherit the shell environment, so forward any
# BG3SE_* vars explicitly.
OPEN_ARGS=()
while IFS='=' read -r _k _v; do
    OPEN_ARGS+=(--env "$_k=$_v")
done < <(env | grep -E '^BG3SE_[A-Z_]+=' || true)

BG3_APP_BUNDLE="$(cd "$(dirname "$BG3_EXEC")/../.." && pwd)"
open "${OPEN_ARGS[@]}" "$BG3_APP_BUNDLE" || {
    echo "Error: open failed for $BG3_APP_BUNDLE"
    exit 1
}

# `open` returns immediately and does not report the child pid; poll for it.
GAME_PID=""
for _ in $(seq 1 20); do
    GAME_PID=$(pgrep -f "$BG3_EXEC" | head -1)
    [[ -n "$GAME_PID" ]] && break
    sleep 0.5
done
if [[ -z "$GAME_PID" ]]; then
    echo "Warning: launched, but could not resolve the game PID."
fi

# Wait for the dylib to open its session log (proves injection ran)
LOADED=""
for _ in $(seq 1 15); do
    sleep 1
    if [[ -f "$LOG_DIR/latest.log" ]]; then
        LOG_TS=$(stat -L -f %m "$LOG_DIR/latest.log" 2>/dev/null || echo 0)
        if [[ "$LOG_TS" -ge "$LAUNCH_TS" ]]; then
            LOADED=1
            break
        fi
    fi
done

if [[ -n "$LOADED" ]]; then
    echo "SUCCESS: BG3SE-macOS loaded!"
    echo ""
    echo "=== Session Log (first lines) ==="
    head -20 "$LOG_DIR/latest.log"
else
    echo "WARNING: No fresh session log after 15s."
    echo "The game may still be starting, or injection failed."
    echo "Check: tail -f \"$LOG_DIR/latest.log\""
fi

echo ""
echo "Game is running in background (PID: $GAME_PID)"
echo "Press Ctrl+C to continue (game will keep running)"
