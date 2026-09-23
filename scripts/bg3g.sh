#!/bin/bash
# BG3SE-macOS GOG Launch Script
#
# Galaxy has no launch-options field and cannot set an environment variable, but
# it can launch a different executable. Point it at this script:
#
#   Galaxy -> the game -> Manage installation -> Configure
#     -> tick "Custom executables/arguments" -> Duplicate -> pick this script
#
# or run it directly. BG3ModManagerMac can wire it up for you.
#
# Execs the real game binary, not the bundle's CFBundleExecutable: on GOG that
# is a ~200KB arch-selector stub which re-execs "<name> GOG" with the same PID
# and environment, so injecting there gets the extender suppressed as a
# duplicate image when the game loads. The extender guards against that too.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# shellcheck source=scripts/find_bg3.sh
source "$SCRIPT_DIR/find_bg3.sh"

LOG=/tmp/bg3g_debug.log
{
    echo "=== BG3G Launch Script ==="
    echo "Date: $(date)"
    echo "Args: $*"
} >> "$LOG"

# Galaxy may pass the bundle, the executable, or nothing at all.
APP_PATH="$1"
[[ $# -gt 0 ]] && shift

if [[ "$APP_PATH" == *.app ]]; then
    EXEC_PATH="$(find_bg3_exec "$APP_PATH")"
elif [[ -x "$APP_PATH" && -f "$APP_PATH" ]]; then
    # An executable. If it is the stub, go up to the bundle and re-resolve.
    BUNDLE="${APP_PATH%/Contents/MacOS/*}"
    if [[ "$BUNDLE" == *.app && "$BUNDLE" != "$APP_PATH" ]]; then
        EXEC_PATH="$(find_bg3_exec "$BUNDLE")"
    else
        EXEC_PATH="$APP_PATH"
    fi
else
    # Nothing usable passed: discover the install ourselves.
    [[ -n "$APP_PATH" ]] && set -- "$APP_PATH" "$@"
    EXEC_PATH="$(find_bg3_exec)" || {
        echo "ERROR: Baldur's Gate 3 not found. Set BG3SE_GAME_PATH." | tee -a "$LOG" >&2
        exit 1
    }
fi

echo "Executable: $EXEC_PATH" >> "$LOG"

if [[ ! -f "$EXEC_PATH" ]]; then
    echo "ERROR: game executable not found: $EXEC_PATH" | tee -a "$LOG" >&2
    exit 1
fi

# Prefer the dylib deployed next to the game (what the mod manager installs),
# then a local build tree.
DYLIB="$(dirname "$EXEC_PATH")/libbg3se.dylib"
[[ -f "$DYLIB" ]] || DYLIB="${PROJECT_ROOT}/build/lib/libbg3se.dylib"

if [[ ! -f "$DYLIB" ]]; then
    {
        echo "ERROR: libbg3se.dylib not found."
        echo "ERROR: build it with: cmake -B build && cmake --build build"
        echo "ERROR: one dylib serves both stores; it reads the store from the"
        echo "ERROR: loaded game and selects that store's addresses at runtime."
    } | tee -a "$LOG" >&2
    exit 1
fi

echo "DYLIB: $DYLIB" >> "$LOG"

# Pass through BG3SE diagnostics (Issue #65), as bg3w.sh does, plus the log
# knobs -- subsystem discovery logs at DEBUG, so diagnosing a failed launch
# needs BG3SE_LOG_LEVEL=debug BG3SE_LOG_MODULES=Entity to reach the game.
BG3SE_ENVS=()
for var in BG3SE_NO_HOOKS BG3SE_NO_NET BG3SE_MINIMAL BG3SE_FORCE_ADDRESSES \
           BG3SE_LOG_LEVEL BG3SE_LOG_MODULES BG3SE_DISABLE; do
    if [[ -n "${!var}" ]]; then
        BG3SE_ENVS+=("${var}=${!var}")
        echo "  ${var}=${!var}" >> "$LOG"
    fi
done

echo "===========================" >> "$LOG"

# arch -arm64 for the same reason bg3w.sh does: force the native slice. Going
# direct means the stub never runs, so nothing else makes that choice.
CMD=(arch -arm64 env DYLD_INSERT_LIBRARIES="$DYLIB" "${BG3SE_ENVS[@]}" "$EXEC_PATH" "$@")

# Print the resolved command instead of running it, for diagnosing a Galaxy
# launch failure.
if [[ -n "$BG3SE_PRINT_COMMAND" ]]; then
    printf '%q ' "${CMD[@]}"; echo
    exit 0
fi

exec "${CMD[@]}"
