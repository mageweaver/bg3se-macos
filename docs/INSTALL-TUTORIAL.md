# Installing BG3SE-macOS (binary release)

The Script Extender for Baldur's Gate 3 on **native macOS** — the runtime that
mods like Mod Configuration Menu, 5eSpells, and Compatibility Framework need.

> **Version check first.** This build targets game build **4.1.1.7398727**
> (Steam). The version is printed in the bottom-right of the game's main menu
> (`v4.1.1.7398727`). On any other build the extender loads, logs the mismatch,
> and deliberately does nothing — it fails closed rather than guessing at
> memory layouts.

## 1. Download

Grab the latest zip from the releases page:

**https://github.com/mageweaver/bg3se-macos/releases/latest**

(direct: [bg3se-macos-v0.44.0.zip](https://github.com/mageweaver/bg3se-macos/releases/download/v0.44.0/bg3se-macos-v0.44.0.zip))

Unzip it. You get `libbg3se.dylib` and `INSTALL.txt`.

## 2. Quit the game

Fully quit Baldur's Gate 3 (⌘Q, not just the window).

## 3. Copy the dylib into the game bundle

```bash
cp libbg3se.dylib \
  "$HOME/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/"
```

If Steam lives in a non-default library, adjust the path accordingly
(Steam → Settings → Storage shows your library folders).

## 4. Clear Gatekeeper quarantine

The dylib is **unsigned**. macOS quarantines downloaded files and will refuse
to load it until you clear the flag:

```bash
xattr -d com.apple.quarantine \
  "$HOME/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/libbg3se.dylib"
```

This is the step people skip. If the extender never logs anything, do this.

## 5. One-time binary patch (first install only)

The game loads the extender through a load command added to its binary. If you
have never installed BG3SE-macOS before:

```bash
git clone https://github.com/mageweaver/bg3se-macos.git && cd bg3se-macos
PYTHONPATH=tools python3 -m bg3se_harness patch
```

The patch is idempotent and reversible (`unpatch` restores the original;
a backup is kept automatically). Steam's "verify integrity" will undo it, and
game updates replace the binary — just re-run `patch` afterwards.

## 6. Launch and verify

Start the game normally through Steam. Then check for the session log:

```bash
ls -t "$HOME/Library/Application Support/BG3SE/logs/" | head -1
```

A fresh `bg3se_<date>.log` whose first lines say
`=== BG3SE-macOS v0.44.0 initialized ===` means it's working. Load a save with
SE mods enabled and they will bootstrap; MCM's window opens via its keybind.

## Troubleshooting

| symptom | cause / fix |
|---|---|
| No log file appears | Quarantine not cleared (step 4), or the binary isn't patched (step 5) |
| Log says a build mismatch | Your game version ≠ 4.1.1.7398727; the extender idles by design. Wait for a matching release. |
| Mods say SE is missing | The load order in BG3 Mod Manager must include the mods; only enabled mods bootstrap (set `BG3SE_LOAD_UNREGISTERED_MODS=1` to restore scan-everything behavior) |
| A specific mod's SE scripts misbehave | `BG3SE_SKIP_MOD_LUA=<name substring>` suppresses just that mod's Lua for a session |
| Game updated and SE went quiet | Re-run the `patch` step; the dylib survives, the load command doesn't |

## Uninstall

```bash
PYTHONPATH=tools python3 -m bg3se_harness unpatch   # restores the original binary
rm ".../Baldur's Gate 3.app/Contents/MacOS/libbg3se.dylib"
```

Saves are untouched either way — the extender never modifies save files.
