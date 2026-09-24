#!/bin/bash
# BG3SE-macOS Steam Launch Script
# Steam launch options: /path/to/bg3se-macos/scripts/bg3w.sh %command%
#
# Launch options must be EXACTLY that -- the script path and one %command%.
# Do NOT prefix an environment assignment: macOS Steam runs the first token as
# the executable, so `BG3SE_LOG_LEVEL=WARN /path/bg3w.sh %command%` fails with
# "Failed to start process for this game" (os error 260). Set variables in the
# "Extender settings" block below instead.
#
# Any BG3SE_* variable works, not only the ones listed in the loop further down:
# everything except DYLD_* is inherited straight through `arch -arm64 env`
# (verified 2026-09-23). Useful ones:
#   BG3SE_LOG_LEVEL=DEBUG|INFO|WARN|ERROR|NONE   default INFO; an unrecognised
#                                                value silently falls back to INFO
#   BG3SE_LOG_MODULES=Osiris,Entity              per-module overrides
#   BG3SE_NO_GUID_INSERT_GUARD=1                 disable the CReteDBase::insert
#                                                guard that refuses a non-GUID
#                                                GUIDSTRING row
#   BG3SE_MINIMAL=1                              skip all subsystem init
#
# Steam passes the .app bundle path, but we need to run the actual executable
# inside Contents/MacOS/ for DYLD_INSERT_LIBRARIES to work.

# Get script directory (works even when called via symlink or absolute path)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# --- Extender settings -------------------------------------------------------
# Set them HERE, not in Steam's launch options. Steam on macOS does not accept an
# environment-assignment prefix: it treats the first token of the launch options
# as the executable to run, so
#     BG3SE_LOG_LEVEL=WARN /path/to/bg3w.sh %command%
# fails with "Failed to start process for this game" (os error 260) and this
# script is never invoked at all (confirmed 2026-09-23 -- no new entry appeared
# in the debug log below). Launch options must be exactly:
#     /path/to/scripts/bg3w.sh %command%
#
# `:=` means an externally exported value still wins, so launching from a
# terminal with `BG3SE_LOG_LEVEL=debug ...` overrides this without an edit.
: "${BG3SE_LOG_LEVEL:=WARN}"
export BG3SE_LOG_LEVEL
# Uncomment to take the CReteDBase::insert guard out of the picture:
#: "${BG3SE_NO_GUID_INSERT_GUARD:=1}"; export BG3SE_NO_GUID_INSERT_GUARD
# ----------------------------------------------------------------------------

# Debug output
echo "=== BG3W Launch Script ===" >> /tmp/bg3w_debug.log
echo "Date: $(date)" >> /tmp/bg3w_debug.log
echo "Script dir: $SCRIPT_DIR" >> /tmp/bg3w_debug.log
echo "Project root: $PROJECT_ROOT" >> /tmp/bg3w_debug.log
echo "Args: $@" >> /tmp/bg3w_debug.log

# Get the first argument (should be the .app bundle path)
APP_PATH="$1"
shift  # Remove first arg, keep any additional args

# If it's a .app bundle, extract the actual executable
if [[ "$APP_PATH" == *.app ]]; then
    EXEC_PATH="${APP_PATH}/Contents/MacOS/Baldur's Gate 3"
    echo "Detected .app bundle, using executable: $EXEC_PATH" >> /tmp/bg3w_debug.log
else
    EXEC_PATH="$APP_PATH"
    echo "Using path directly: $EXEC_PATH" >> /tmp/bg3w_debug.log
fi

# Verify executable exists
if [[ ! -f "$EXEC_PATH" ]]; then
    echo "ERROR: Executable not found: $EXEC_PATH" >> /tmp/bg3w_debug.log
    exit 1
fi

# Find dylib relative to script location
DYLIB="${PROJECT_ROOT}/build/lib/libbg3se.dylib"

if [[ ! -f "$DYLIB" ]]; then
    echo "ERROR: libbg3se.dylib not found at: $DYLIB" >> /tmp/bg3w_debug.log
    echo "ERROR: Build it first with: cd build && cmake .. && cmake --build ." >> /tmp/bg3w_debug.log
    exit 1
fi

echo "DYLIB: $DYLIB" >> /tmp/bg3w_debug.log

# Re-state a few diagnostics explicitly so they show up in the debug log above.
# NOT an allowlist -- `env` inherits the whole environment, so any other BG3SE_*
# variable reaches the game whether or not it is named here.
BG3SE_ENVS=""
for var in BG3SE_NO_HOOKS BG3SE_NO_NET BG3SE_MINIMAL; do
    if [[ -n "${!var}" ]]; then
        BG3SE_ENVS="${BG3SE_ENVS} ${var}=${!var}"
        echo "  ${var}=${!var}" >> /tmp/bg3w_debug.log
    fi
done

echo "Forcing ARM64 architecture with inline DYLD_INSERT_LIBRARIES" >> /tmp/bg3w_debug.log
echo "===========================" >> /tmp/bg3w_debug.log

# Force ARM64 architecture with inline env var (export doesn't work with arch)
# shellcheck disable=SC2086
exec arch -arm64 env DYLD_INSERT_LIBRARIES="$DYLIB" $BG3SE_ENVS "$EXEC_PATH" "$@"
