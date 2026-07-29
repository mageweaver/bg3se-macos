# Troubleshooting

Common issues and solutions for BG3SE-macOS.

## Build Issues

### "cmake: command not found"

Install CMake via Homebrew:
```bash
brew install cmake
```

### "Submodule not initialized" or missing Dobby/Lua errors

Initialize git submodules:
```bash
git submodule update --init --recursive
```

### Build succeeds but code changes don't appear

Your CMake cache may be stale. This happens after moving the repository or switching branches. Clean rebuild:
```bash
rm -rf build
mkdir build && cd build
cmake .. && cmake --build .
```

### "CMake Error: source directory does not exist"

Same cause as above - stale CMake cache. Delete and rebuild:
```bash
rm -rf build
mkdir build && cd build
cmake .. && cmake --build .
```

### "fatal error: 'tuple' file not found"

CommandLineTools-only systems sometimes resolve the wrong SDK sysroot, so the
C++ standard headers the Metal/simd headers include are never found. Current
main auto-detects the SDK in `CMakeLists.txt` (via `xcrun --show-sdk-path`
with a CommandLineTools fallback) — pull, wipe `build/`, and reconfigure:
```bash
git pull
rm -rf build
mkdir build && cd build
cmake .. && cmake --build .
```
If auto-detection still misses, pass the sysroot explicitly:
```bash
cmake -DCMAKE_OSX_SYSROOT="$(xcrun --sdk macosx --show-sdk-path)" ..
```
Installing full Xcode (then `sudo xcode-select -s /Applications/Xcode.app/Contents/Developer`)
also resolves it.

## Injection Not Working

**Symptoms:** Game launches but mods don't load, no SE output in logs.

**Solutions:**

1. Check logs for errors:
   - Session logs: `~/Library/Application Support/BG3SE/logs/latest.log`
   - Legacy: `~/Library/Application Support/BG3SE/bg3se.log`
2. Verify the dylib is built:
   ```bash
   file build/lib/libbg3se.dylib
   ```
3. Ensure it's universal (should show both `x86_64` and `arm64`):
   ```bash
   file build/lib/libbg3se.dylib
   # Should show: Mach-O universal binary with 2 architectures
   ```
4. Ensure wrapper uses `open --env` (not just `export`)

## Game Crashes at Launch

**Symptoms:** Game crashes immediately on launch with injection.

**Solutions:**

1. Make sure wrapper script uses:
   ```bash
   open -W --env "DYLD_INSERT_LIBRARIES=/path/to/libbg3se.dylib" "$1"
   ```
2. Verify dylib is universal binary (check with `file` command)
3. Try running without injection: clear Steam launch options
4. Check Console.app for crash reports

## Game Returns to Menu After Loading

**Symptoms:** Game loads a save but immediately returns to the main menu.

**Cause:** Usually means a hook isn't preserving the return value.

**Solutions:**

1. Check that hooked functions return the original function's return value
2. Review `~/Library/Application Support/BG3SE/logs/latest.log` for hook call/return messages
3. Look for `COsiris::Load returned: X` messages

## Mod Not Loading

**Symptoms:** Mod is installed but doesn't appear to be running.

**Checklist:**

1. Ensure the mod is enabled in modsettings.lsx (use in-game mod manager or BG3 Mod Manager)
2. Ensure the mod's `.pak` file is in:
   ```
   ~/Documents/Larian Studios/Baldur's Gate 3/Mods/
   ```
3. Check that the mod has `ScriptExtender/Config.json` with `"Lua"` in FeatureFlags:
   ```json
   {
     "FeatureFlags": ["Lua"]
   }
   ```
4. Check that the path structure inside PAK is:
   ```
   Mods/<ModName>/ScriptExtender/Lua/BootstrapServer.lua
   ```
5. Review the log for "Scanning for SE Mods" and "Loading Mod Scripts" sections
6. For debugging, extract with `tools/extract_pak.py` to inspect mod structure

## Architecture Mismatch Error

**Symptoms:** Crash reports mention "incompatible architecture".

**Solutions:**

1. Rebuild with CMake (creates universal binary by default):
   ```bash
   rm -rf build
   mkdir build && cd build
   cmake .. && cmake --build .
   ```
2. Verify with:
   ```bash
   file build/lib/libbg3se.dylib
   # Should show: Mach-O universal binary with 2 architectures: [x86_64] [arm64]
   ```

### "BG3 requires arm64e" is a misdiagnosis

AI assistants sometimes conclude that injection fails because the dylib is
`arm64` while BG3 "requires `arm64e`". That is wrong: `arm64e` is reserved for
Apple system binaries — third-party apps like BG3 ship plain `arm64`, and a
dylib built `arm64` + `x86_64` is exactly correct. Verify yourself:
```bash
file "$HOME/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3"
# Shows arm64 (and x86_64), never arm64e
```
When injection genuinely fails on Apple Silicon, the real causes are usually:

1. **Missing submodules** — `git submodule update --init --recursive`, then clean rebuild
2. **Stale build cache** — `rm -rf build` and reconfigure (see above)
3. **Code signature invalidated** after patching the game binary with
   `insert_dylib` — re-sign it:
   ```bash
   codesign --force --deep --sign - "$HOME/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app"
   ```
4. **Steam launch options** not pointing at the wrapper script with `%command%` appended

## Console Not Connecting

**Symptoms:** `bg3se-console` can't connect to the game.

**Solutions:**

1. Ensure the game is running with BG3SE injected
2. Check if socket exists:
   ```bash
   ls -la /tmp/bg3se.sock
   ```
3. Try connecting with socat:
   ```bash
   socat - UNIX-CONNECT:/tmp/bg3se.sock
   ```
4. Check log for "Socket console listening" message

## Stats API Returns nil

**Symptoms:** `Ext.Stats.Get("StatName")` returns nil.

**Solutions:**

1. Check if stats system is ready:
   ```lua
   Ext.Print(tostring(Ext.Stats.IsReady()))
   ```
2. Wait for SessionLoaded event:
   ```lua
   Ext.Events.SessionLoaded:Subscribe(function()
       local stat = Ext.Stats.Get("WPN_Longsword")
       -- Now it should work
   end)
   ```
3. Verify the stat name is correct with `Ext.Stats.GetAll()`

## Entity API Returns nil

**Symptoms:** `Ext.Entity.Get(guid)` returns nil for valid GUIDs.

**Solutions:**

1. Check if entity system is ready:
   ```lua
   Ext.Print(tostring(Ext.Entity.IsReady()))
   ```
2. Wait for SessionLoaded event before querying entities
3. Verify GUID format is correct (with or without hyphens)
4. On Intel/Rosetta: Entity system has limited functionality

## Limited Functionality on Intel/Rosetta

**Symptoms:** Some features don't work when running under Rosetta.

**Cause:** The Ghidra-derived memory offsets are specific to the ARM64 binary.

**What works on Intel/Rosetta:**
- Basic Osiris hooks
- Lua runtime
- Mod loading

**What doesn't work:**
- Entity system (wrong offsets)
- Component access
- Stats property access (may crash or return wrong data)

**Solution:** Use Apple Silicon Mac for full functionality.

## Log File Not Found

**Symptoms:** Can't find the log file.

**Locations (in order of preference):**

1. **Session-based logs (v0.36.12+):**
   ```bash
   # Current session (symlink)
   tail -f ~/Library/Application\ Support/BG3SE/logs/latest.log

   # List all session logs
   ls ~/Library/Application\ Support/BG3SE/logs/
   ```

2. **Legacy location (deprecated):**
   ```bash
   tail -f ~/Library/Application\ Support/BG3SE/bg3se.log
   ```

If the directory doesn't exist, BG3SE hasn't been run yet. Launch the game with injection.

## Getting Help

If you're still stuck:

1. Check [GitHub Issues](https://github.com/tdimino/bg3se-macos/issues) for similar problems
2. Include relevant log output when reporting issues
3. Mention your macOS version, architecture (Apple Silicon or Intel), and BG3 version
