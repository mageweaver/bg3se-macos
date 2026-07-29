#!/bin/bash
# Auto-deploy libbg3se.dylib to the game's app bundle after build

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DYLIB="$PROJECT_DIR/build/lib/libbg3se.dylib"

source "$SCRIPT_DIR/find_bg3.sh"

if [[ ! -f "$BUILD_DYLIB" ]]; then
    echo "Error: Build dylib not found at $BUILD_DYLIB"
    exit 1
fi

# Missing game is a warning, not a build failure — build machines
# without BG3 installed (CI, contributors) still get a usable dylib.
BG3_APP="$(find_bg3_app)" || {
    echo "Warning: BG3 not found in any Steam library — skipping deploy."
    echo "Set BG3SE_GAME_PATH to deploy to a custom location."
    exit 0
}
DEPLOYED_DYLIB="$BG3_APP/Contents/MacOS/libbg3se.dylib"

# Compare timestamps
if [[ -f "$DEPLOYED_DYLIB" ]]; then
    BUILD_TIME=$(stat -f %m "$BUILD_DYLIB")
    DEPLOYED_TIME=$(stat -f %m "$DEPLOYED_DYLIB")

    if [[ "$BUILD_TIME" -le "$DEPLOYED_TIME" ]]; then
        echo "Deployed dylib is up to date"
        exit 0
    fi
fi

cp "$BUILD_DYLIB" "$DEPLOYED_DYLIB"
echo "Deployed: $(ls -lh "$DEPLOYED_DYLIB" | awk '{print $5, $6, $7, $8}')"
