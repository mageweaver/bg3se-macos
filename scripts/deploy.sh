#!/bin/bash
# Auto-deploy libbg3se.dylib to the game's app bundle after build

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

source "$SCRIPT_DIR/find_bg3.sh"

# Which build tree to deploy from. One dylib serves every store, so there is
# one build tree; BG3SE_BUILD_DIR overrides it for out-of-tree builds.
if [[ -n "$BG3SE_BUILD_DIR" ]]; then
    BUILD_DYLIB="$BG3SE_BUILD_DIR/lib/libbg3se.dylib"
else
    BUILD_DYLIB="$PROJECT_DIR/build/lib/libbg3se.dylib"
fi

if [[ ! -f "$BUILD_DYLIB" ]]; then
    echo "Error: Build dylib not found at $BUILD_DYLIB"
    exit 1
fi
echo "Deploying from: $BUILD_DYLIB"

# Missing game is a warning, not a build failure — build machines
# without BG3 installed (CI, contributors) still get a usable dylib.
BG3_APP="$(find_bg3_app)" || {
    echo "Warning: BG3 not found in any Steam library — skipping deploy."
    echo "Set BG3SE_GAME_PATH to deploy to a custom location."
    exit 0
}
DEPLOYED_DYLIB="$BG3_APP/Contents/MacOS/libbg3se.dylib"

# Is the deployed dylib already this exact build?
#
# Compare LC_UUID, not mtimes. The linker gives every build a new UUID and
# codesign does not change it, so equal UUIDs mean the same compiled image is
# already in place. File size is no good here: signing changes it.
#
# mtimes were wrong twice while developing this branch. The deploy below
# uses a plain cp, so the installed file is stamped with the time of the
# deploy, not the time of the build. Every later build then has to beat that
# stamp, and `-le` meant an equal second lost. A rebuilt dylib was silently
# not deployed, the game kept loading the old one, and it read like a code bug
# rather than a skipped copy.
#
# Pass --force to deploy regardless.
dylib_uuids() {
    dwarfdump --uuid "$1" 2>/dev/null | awk '{print $2}' | sort | tr '\n' ' '
}

FORCE=0
[[ "${1:-}" == "--force" || -n "${BG3SE_FORCE_DEPLOY:-}" ]] && FORCE=1

if [[ "$FORCE" == "0" && -f "$DEPLOYED_DYLIB" ]]; then
    BUILD_UUID="$(dylib_uuids "$BUILD_DYLIB")"
    DEPLOYED_UUID="$(dylib_uuids "$DEPLOYED_DYLIB")"

    if [[ -z "$BUILD_UUID" ]]; then
        # No UUID to compare, so deploy rather than guess.
        echo "Note: could not read a UUID from the build; deploying anyway."
    elif [[ "$BUILD_UUID" == "$DEPLOYED_UUID" ]]; then
        echo "Deployed dylib is up to date (same build)"
        echo "  UUID: ${BUILD_UUID% }"
        echo "  Use --force to deploy anyway."
        exit 0
    else
        echo "Build differs from deployed:"
        echo "  build:    ${BUILD_UUID% }"
        echo "  deployed: ${DEPLOYED_UUID:-<none>}"
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
