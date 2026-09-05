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

# Stage next to the destination, sign, then atomically swap it in.
#
# Two reasons this is not a plain cp:
#  1. macOS kills the game at launch with SIGKILL "Code Signature Invalid"
#     (CODESIGNING / Invalid Page in dyld) unless the dylib carries a real
#     signature. The linker only ad-hoc signs the arm64 slice of the universal
#     build; the x86_64 slice comes out unsigned, so `codesign -v` on the fat
#     file reports "code object is not signed at all". Signing here covers both.
#  2. cp rewrites the destination IN PLACE. Doing that while the game has the
#     dylib mapped corrupts the running image's pages; mv gives the new file a
#     new inode and leaves the running process's mapping alone.
#
# The identifier is passed explicitly: codesign would otherwise derive it from
# the staged filename ("libbg3se.dylib.new.<pid>"). Release zips are signed the
# same way, so a deployed build is byte-identical to its published asset.
STAGE="$DEPLOYED_DYLIB.new.$$"
cp "$BUILD_DYLIB" "$STAGE" || { echo "Error: copy to $STAGE failed"; exit 1; }

if ! codesign -f -s - -i libbg3se.dylib "$STAGE" 2>/dev/null; then
    rm -f "$STAGE"
    echo "Error: codesign failed — NOT deploying (the game would be killed at"
    echo "       launch with 'Code Signature Invalid')."
    exit 1
fi
if ! codesign -v "$STAGE" 2>/dev/null; then
    rm -f "$STAGE"
    echo "Error: signature did not verify — NOT deploying."
    exit 1
fi

mv -f "$STAGE" "$DEPLOYED_DYLIB" || { rm -f "$STAGE"; echo "Error: install failed"; exit 1; }
echo "Deployed: $(ls -lh "$DEPLOYED_DYLIB" | awk '{print $5, $6, $7, $8}') (signed)"
