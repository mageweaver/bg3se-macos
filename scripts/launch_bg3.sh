#!/bin/bash
#
# BG3SE-macOS Launcher Script
#
# This script launches Baldur's Gate 3 with the Script Extender dylib injected.
# Usage: ./launch_bg3.sh [path_to_dylib]
#

set -e

# Configuration
BG3_APP="/Users/tomdimino/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app"
BG3_EXEC="${BG3_APP}/Contents/MacOS/Baldur's Gate 3"

# Find the dylib
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

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

# Set the environment variable and launch
LAUNCH_TS=$(date +%s)
DYLD_INSERT_LIBRARIES="$DYLIB" "$BG3_EXEC" &
GAME_PID=$!

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
