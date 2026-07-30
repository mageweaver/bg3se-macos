# Architecture

Technical deep-dive into BG3SE-macOS internals.

## How It Works

BG3SE-macOS injects its dylib by statically patching the game binary with `insert_dylib` (vendored at `tools/vendor/insert_dylib`), which appends an `LC_LOAD_WEAK_DYLIB` load command pointing at `libbg3se.dylib`. This works because:

1. BG3 macOS has **no hardened runtime** (`flags=0x0`)
2. The binary is re-signed ad-hoc after patching, which macOS accepts for non-hardened apps
3. libOsiris.dylib exports clean C/C++ symbols we can hook

### Why This Works

| Factor | Value |
|--------|-------|
| Hardened Runtime | `flags=0x0` (none) |
| Code Signing | Developer ID signed, but not hardened; ad-hoc re-sign accepted |
| Load Command Patching | `LC_LOAD_WEAK_DYLIB` honored at launch, Steam-compatible |
| libOsiris Exports | 1,013 symbols |

## Injection Method

- `insert_dylib` static Mach-O patching adds an `LC_LOAD_WEAK_DYLIB` command to the game binary; the dylib loads at every launch, including launches Steam initiates itself
- Launcher bypass: `defaults write com.larian.bg3 NoLauncher 1` (set automatically by the harness)
- Dobby framework for inline function hooking (ARM64 + x86_64 universal)
- Hooks into libOsiris.dylib for Osiris scripting integration

### Why Not DYLD_INSERT_LIBRARIES

The original injection method was `open --env "DYLD_INSERT_LIBRARIES=..."`. It is dead: launching through Steam crashes with it, and Steam's own relaunch path (the Steamworks DRM bounce) never carries the environment variable. Static patching survives both.

### Universal Binary Required

BG3 can run either natively (ARM64) or under Rosetta (x86_64), so our dylib must be a universal binary containing both architectures.

## Module Structure

```
src/
├── core/           # Logging, version info
├── console/        # Socket server + file-based console
├── overlay/        # NSWindow in-game overlay
├── entity/         # Entity Component System (modular)
│   ├── entity_system.c/h  # Core ECS, Lua bindings
│   ├── guid_lookup.c/h    # GUID parsing, HashMap operations
│   └── arm64_call.c/h     # ARM64 ABI wrappers (x8 indirect return)
├── hooks/          # Legacy hook stubs (actual hooks in main.c)
├── injector/       # Main injection logic (main.c)
├── lua/            # Lua API modules (lua_ext, lua_json, lua_osiris, lua_stats)
├── mod/            # Mod detection and loading
├── osiris/         # Osiris types, functions, pattern scanning
├── pak/            # LSPK v18 PAK file reading
├── stats/          # RPGStats system access (stats_manager)
└── strings/        # FixedString resolution
```

## Hooking Strategy

- **Dobby**: Inline hooking for internal library functions (C++ methods)
- **fishhook**: Available for imported symbols (PLT/GOT rebinding) if needed

Osiris functions like `COsiris::Load`, `COsiris::InitGame`, etc. are internal to `libOsiris.dylib`, requiring inline hooking via Dobby.

### Key libOsiris Symbols

```
_DebugHook                      - Debug interface
_CreateRule                     - Script rule creation
_DefineFunction                 - Function registration
_SetInitSection                 - Initialization hook
_ZN7COsiris8InitGameEv          - COsiris::InitGame()
_ZN7COsiris4LoadER12COsiSmartBuf - COsiris::Load(COsiSmartBuf&)
```

### Return Values Must Be Preserved

When hooking C++ member functions, the return value must be captured and returned from the hook. Failing to do so causes the game to fail silently (e.g., returning to main menu after load).

## ARM64 ABI Critical Pattern

**Large struct returns (>16 bytes) require x8 indirect return:**

Functions returning structs larger than 16 bytes on ARM64 use indirect return via x8:

1. Caller allocates buffer for return value
2. Caller passes buffer address in x8 before call
3. Callee writes result to buffer
4. Caller reads from buffer

### Example: TryGetSingleton

BG3's `TryGetSingleton<T>` returns `ls::Result<ComponentPtr, ls::Error>` which is a 64-byte struct:

```c
typedef struct __attribute__((aligned(16))) {
    void* value;           // 0x00: Component pointer on success
    uint64_t reserved1;    // 0x08: Reserved
    uint64_t reserved2[4]; // 0x10-0x2F: Additional data
    uint8_t has_error;     // 0x30: Error flag (0=success, 1=error)
    uint8_t _pad[15];      // 0x31-0x3F: Padding
} LsResult;

// Correct ARM64 calling convention
void* call_with_x8_buffer(void* fn, void* arg) {
    LsResult result = {0};
    result.has_error = 1;
    __asm__ volatile (
        "mov x8, %[buf]\n"   // x8 = return buffer address
        "mov x0, %[arg]\n"   // x0 = function argument
        "blr %[fn]\n"        // call function
        : "+m"(result)
        : [buf] "r"(&result), [arg] "r"(arg), [fn] "r"(fn)
        : "x0", "x1", "x8", "x9", "x10", "x11", "x12", "x13",
          "x14", "x15", "x16", "x17", "x19", "x20", "x21", "x22",
          "x23", "x24", "x25", "x26", "x30", "memory"
    );
    return (result.has_error == 0) ? result.value : NULL;
}
```

This was discovered through Ghidra analysis of `TryGetSingleton` which saves x8 to x19 at entry (`mov x19, x8`) and writes the result via `stp x10, xzr, [x19]` and error flag via `strb w8, [x19, #0x30]`.

See `src/entity/arm64_call.c` for implementation.

## Pattern Scanning (Cross-Version Support)

BG3SE-macOS includes a pattern scanning infrastructure for resilience across game updates:

- **Pattern Database**: Unique ARM64 byte sequences for key Osiris functions
- **Fallback Resolution**: If `dlsym` fails, pattern scanning locates functions by signature
- **Mach-O Support**: Scans `__TEXT,__text` section of loaded dylibs

Example patterns (BG3 Patch 7):
```
InternalQuery: FD 43 04 91 F3 03 01 AA 15 90 01 51 ...
InternalCall:  F3 03 00 AA 28 20 00 91 09 04 00 51 ...
```

When Larian updates the game, if symbol names change but function code remains similar, pattern scanning can still locate the functions.

## Modular Design Pattern

Each subsystem is self-contained:
- **Header file** (`.h`) - Public API declarations, constants, type definitions
- **Source file** (`.c`) - Implementation with static (private) helpers
- **Minimal coupling** - Modules communicate through well-defined interfaces

```c
// module.h - Public interface
#ifndef MODULE_H
#define MODULE_H
void module_init(void);
int module_get_count(void);
#endif

// module.c - Implementation
#include "module.h"
static int item_count = 0;  // Private state
void module_init(void) { ... }
```

### When to Extract from main.c

1. Code exceeds ~100 lines with related functionality
2. State (static variables) can be isolated
3. Multiple source files need the functionality

## Platform Notes

- Game binary is ARM64 on Apple Silicon, Rosetta for Intel
- libOsiris.dylib contains the Osiris scripting engine
- Some symbols stripped—pattern scanning is the fallback
- EntityWorld/EoCServer singletons not exported—must capture via hooks or direct memory read

## System Diagram

```
┌─────────────────────────────────────────────────┐
│                  BG3 Process                    │
├─────────────────────────────────────────────────┤
│  ┌──────────────┐    ┌───────────────────────┐  │
│  │ libOsiris    │◄───│ BG3SE Hooks (Dobby)   │  │
│  │ (Scripting)  │    │ - COsiris::InitGame   │  │
│  └──────────────┘    │ - COsiris::Load       │  │
│                      └───────────────────────┘  │
│  ┌──────────────┐              ▲               │
│  │ Main Game    │              │               │
│  │ Executable   │    ┌─────────┴─────────┐    │
│  └──────────────┘    │  Lua Runtime       │    │
│                      │  (Mod Scripts)     │    │
│                      └───────────────────┘    │
└─────────────────────────────────────────────────┘
```
