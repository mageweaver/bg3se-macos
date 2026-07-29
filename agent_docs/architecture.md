# Architecture

## Injection Method
- `DYLD_INSERT_LIBRARIES` loads dylib before game starts
- Dobby framework for inline function hooking (ARM64 + x86_64 universal)
- Hooks into libOsiris.dylib for Osiris scripting integration

## Module Structure
```
src/
├── core/           # Logging, version info, crash diagnostics, version detection
│   ├── logging.c/h       # Structured logging with callbacks
│   ├── crashlog.c/h      # mmap ring buffer, SIGSEGV handler, breadcrumbs
│   ├── mach_exception.c/h # Mach exception handler (EXC_BAD_ACCESS before CrashReporter)
│   ├── mach_exc_stubs/   # MIG-generated stubs from mach_exc.defs
│   ├── safe_memory.c/h   # Safe mach_vm_read wrappers
│   └── version_detect.c/h # Game binary version detection, sentinel address probes
├── entity/         # Entity Component System (modular)
│   ├── entity_system.c/h  # Core ECS, Lua bindings
│   ├── guid_lookup.c/h    # GUID parsing, HashMap operations
│   └── arm64_call.c/h     # ARM64 ABI wrappers (x8 indirect return)
├── game/           # Game-level modules
│   ├── game_state.c/h        # Game state tracking
│   ├── global_switches.c/h   # Global configuration switches
│   ├── video_skip.c/h        # Intro video skip (UserDefaults + graphicSettings)
│   └── focus_hack.c/h        # BaseApp focus force (0x142 flag bypass for Noesis input)
├── hooks/          # ARM64 safe hooking + legacy stubs
│   └── arm64_hook.c/h     # Skip-and-redirect hooks, MAP_JIT trampolines
├── imgui/          # Dear ImGui overlay system
│   ├── imgui_metal_backend.mm  # Metal rendering, coord conversion
│   ├── imgui_input_hooks.mm    # NSView method swizzling
│   └── lua_imgui.c             # Ext.IMGUI Lua bindings
├── injector/       # Main injection logic (main.c)
├── input/          # System-level input capture
│   ├── input_hooks.m           # CGEventTap for keyboard/mouse
│   └── focusless_input.m       # Focusless input for headless splash dismiss (CGEvent)
├── level/          # LevelManager, PhysicsScene, AiGrid access
│   └── level_manager.c/h   # Singleton access, VMT-based physics dispatch
├── audio/          # WWise sound engine access
│   └── audio_manager.c/h   # SoundManager singleton, audio control, PlayExternalSound (STDString ABI)
├── lua/            # Lua API modules + thread gate
│   ├── lua_gate.c/h        # Recursive mutex serializing all Lua entry points
│   └── lua_ext, lua_json, lua_osiris, lua_stats, lua_events, lua_logging, lua_level, lua_audio
├── mod/            # Mod detection and loading
├── osiris/         # Osiris types, functions, handle encoding, pattern scanning
├── pak/            # LSPK v18 PAK file reading
└── stats/          # RPGStats system access (stats_manager)
```

## Key Files
- `src/injector/main.c` - Core injection, Dobby hooks, Osi.* namespace, Lua state, OsirisFunctionHandle dispatch
- `src/core/crashlog.c` - Crash-resilient logging (mmap ring buffer, SIGSEGV handler, breadcrumbs)
- `src/core/mach_exception.c` - Mach exception handler (catches PAC failures before CrashReporter)
- `src/core/version_detect.c` - Game binary version detection via Info.plist, sentinel address probes for version mismatch tolerance
- `src/osiris/osiris_types.h` - OsiFunctionHandle encoding/decoding, function types, argument structs
- `src/osiris/osiris_functions.c` - Function cache, type reading from game memory, struct probe
- `src/mod/mod_loader.c` - Mod detection from modsettings.lsx, PAK loading
- `src/lua/lua_*.c` - Ext.* API implementations
- `src/stats/stats_manager.c` - RPGStats global access, stat property resolution
- `src/game/game_state.c` - Game state tracking, state change callbacks
- `src/game/focus_hack.c` - BaseApp focus force (0x142 bypass for Noesis input gate)
- `src/lua/lua_gate.c/h` - Recursive pthread mutex serializing all `lua_State` access across threads (see Lua Thread Safety below)
- `src/hooks/arm64_hook.c/h` - ARM64 skip-and-redirect inline hooks with MAP_JIT trampoline allocation
- `ghidra/offsets/` - Modular offset documentation

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

## Console Poll Timer (GCD)

The socket console (`/tmp/bg3se.sock`) is polled by a GCD dispatch timer at 100ms intervals, independent of Osiris events. Without this, the socket only responded during gameplay (when `fake_Event()` fired Osiris hooks). The timer acquires the Lua gate via `lua_gate_trylock()` — if another thread holds the gate, the tick is skipped (retry next 100ms). Fifty consecutive misses (~5s) log a starvation warning.

Cleanup: timer cancelled in `shutdown_lua()` before Lua state destruction.

## Lua Thread Safety (lua_gate)

The Lua VM is single-threaded, but multiple code paths enter it from different threads:

| Thread | Entry points | Gate strategy |
|--------|--------------|---------------|
| Game / ServerWorker | `fake_Event` tick block, `dispatch_event_to_lua`, `fake_InitGame`, `fake_Load` | `lua_gate_lock()` |
| GCD console timer | `console_poll` (100ms) | `lua_gate_trylock()` — skip on contention |
| Metal render thread | `lua_imgui_fire_event`, `lua_imgui_cleanup_refs` | `lua_gate_lock()` |
| Input hook thread | `lua_hotkey_callback` | `lua_gate_lock()` |
| Network callbacks | `callback_registry_invoke` | `lua_gate_lock()` |
| Hooked game execution | functor event dispatch (`functor_hooks.c`) | `lua_gate_lock()` |
| Any logging thread | `log_event_callback` (Ext.Events Log) | `lua_gate_lock()` |
| Dylib teardown | Shutdown event, `shutdown_lua` | `lua_gate_lock()` |

**Any new code path that calls into the shared `lua_State` MUST acquire the gate**, and MUST resolve its `lua_State` pointer *under* the gate (shutdown clears the published pointers while holding it, so a blocked waiter re-resolves to NULL instead of entering a freed state). The mutex is recursive (same-thread re-entry is safe for nested dispatch). Implementation: `src/lua/lua_gate.c/h` (pthread_mutex_t with PTHREAD_MUTEX_RECURSIVE, pthread_once init).

**Lock order:** Lua gate → logging mutex, never the reverse. The logging system snapshots callbacks under `g_config.mutex` and invokes them only after releasing it (`src/core/logging.c`), so log-to-Lua dispatch can take the gate safely.

History: before lua_gate (v0.37.1), the GCD timer used a bare atomic flag that only prevented self-re-entry, not cross-thread races. The race was latent and surfaced under high Osiris event rates (2026-07-28 crash, PID 97193).

## MAP_JIT Trampolines (ARM64)

`arm64_alloc_near()` (`src/hooks/arm64_hook.c`) allocates trampoline pages with `MAP_JIT`. On Apple Silicon, MAP_JIT pages are execute-protected by default; writing requires bracketing with:

```c
pthread_jit_write_protect_np(0);  // enable writes (per-thread)
// ... write trampoline instructions ...
pthread_jit_write_protect_np(1);  // re-protect
sys_icache_invalidate(tramp, size);
```

Without the write gate, the trampoline build faults with `KERN_PROTECTION_FAILURE`. This is normally masked because `arm64_alloc_near` usually lands within ±128MB (near branch); the far-trampoline fallback (absolute branch) exercises the write path and faults under unlucky ASLR slides (2026-07-28 crash, PID 83926).

## Platform Notes
- Game binary is ARM64 on Apple Silicon, Rosetta for Intel
- libOsiris.dylib contains the Osiris scripting engine
- Some symbols stripped - pattern scanning is the fallback
- EntityWorld/EoCServer singletons not exported - must capture via hooks
- **BG3 macOS uses native Cocoa/AppKit, NOT SDL** (unlike Windows)

## ImGui Overlay System

The debug overlay uses Dear ImGui with Metal rendering and CGEventTap input.

### Key Difference from Windows BG3SE
- **Windows**: Hooks `SDL_PollEvent` via Detours, uses `ImGui_ImplSDL2`
- **macOS**: Uses CGEventTap + NSView swizzling, uses `ImGui_ImplOSX` + `ImGui_ImplMetal`

### Input Architecture
```
CGEventTap (system-level)
    │
    ├── Keyboard events → F11 toggle, key forwarding
    │
    └── Mouse events → Quartz screen coords
                            │
                            ▼
                   Cocoa Coordinate Conversion
                   (4-step: CG → Screen → Window → View)
                            │
                            ▼
                   Mouse Position Cache (s_cgevent_mouse)
                            │
                            ▼
                   Direct io.MousePos Assignment
                   (bypasses ImGui_ImplOSX_NewFrame)
```

**Critical Fix (v0.36.19):** We skip `ImGui_ImplOSX_NewFrame()` because it calls
`[NSEvent mouseLocation]` internally, which overwrites our CGEventTap coordinates.
Instead, we cache the converted position and apply it directly to `io.MousePos`.

### Coordinate Conversion (CGEventTap → ImGui)
CGEventTap provides Quartz coordinates (origin at top-left of main display).
Must convert through Cocoa APIs:
1. CG (top-left) → Cocoa screen (bottom-left): `screenHeight - y`
2. Screen → Window: `convertPointFromScreen:`
3. Window → View: `convertPoint:fromView:`
4. Flip Y if view not flipped: `viewHeight - y`

The converted coordinates are cached in `s_cgevent_mouse` and applied directly
to `io.MousePos` before `ImGui::NewFrame()`. This bypasses the OSX backend's
mouse position update which doesn't work for BG3's fullscreen Metal window.

### Key Files
- `src/imgui/imgui_metal_backend.mm` - Metal rendering, coordinate conversion
- `src/imgui/imgui_input_hooks.mm` - NSView method swizzling (fallback)
- `src/input/input_hooks.m` - CGEventTap for keyboard/mouse
- `lib/imgui/backends/imgui_impl_osx.mm` - Official ImGui OSX backend

## ARM64 ABI Critical Pattern

**Large struct returns (>16 bytes) require x8 indirect return:**

Functions returning structs larger than 16 bytes on ARM64 use indirect return via x8:

1. Caller allocates buffer for return value
2. Caller passes buffer address in x8 before call
3. Callee writes result to buffer
4. Caller reads from buffer

Example: `TryGetSingleton<T>` returns 64-byte `ls::Result`:

```c
typedef struct __attribute__((aligned(16))) {
    void* value;           // 0x00: Result on success
    uint64_t reserved[5];  // 0x08-0x2F
    uint8_t has_error;     // 0x30: Error flag
    uint8_t _pad[15];
} LsResult;

// Correct ARM64 call with x8
void* call_with_x8_buffer(void* fn, void* arg) {
    LsResult result = {0};
    result.has_error = 1;
    __asm__ volatile (
        "mov x8, %[buf]\n"
        "mov x0, %[arg]\n"
        "blr %[fn]\n"
        : "+m"(result)
        : [buf] "r"(&result), [arg] "r"(arg), [fn] "r"(fn)
        : "x0", "x1", "x8", "x9", "x10", "x11", "x12", "x13",
          "x14", "x15", "x16", "x17", "x19", "x20", "x21",
          "x22", "x23", "x24", "x25", "x26", "x30", "memory"
    );
    return (result.has_error == 0) ? result.value : NULL;
}
```

See `src/entity/arm64_call.c` for implementation.
