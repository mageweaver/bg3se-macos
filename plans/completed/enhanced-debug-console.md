# Enhanced Debug Console for BG3SE-macOS

## Overview

Implement a robust, developer-focused debug console for BG3SE-macOS that accelerates offset discovery, struct probing, and runtime debugging. Based on patterns from the Windows BG3SE reference implementation and lessons learned during Issue #3 (Ext.Stats API) development.

## Problem Statement

During development of the Stats API, we spent significant time:
1. Writing C probe functions, rebuilding, and restarting the game to test offsets
2. Manually scanning memory ranges to find struct layouts
3. Unable to execute multi-line Lua for complex debugging
4. Lacking introspection tools to examine internal game structures

The current console is a single-line REPL that processes `commands.txt` - functional but limited for rapid iteration on reverse engineering tasks.

## Proposed Solution

Enhance the console with three tiers of functionality:

### Tier 1: Multi-line Lua Support (Quick Win)
### Tier 2: Memory Introspection APIs (Core Value)
### Tier 3: Developer Productivity Tools (Polish)

---

## Technical Approach

### Tier 1: Multi-line Lua Support

**Reference:** `BG3Extender/Extender/Shared/Console.cpp:237-250`

**Current Behavior:**
```c
// Each line is a separate luaL_dostring() call
while (fgets(line, sizeof(line), f)) {
    luaL_dostring(L, line);
}
```

**Proposed Behavior:**
```c
// Accumulate lines between --[[ and ]]-- markers
if (strcmp(trimmed, "--[[") == 0) {
    multiline_mode = true;
    multiline_buffer_clear();
    continue;
}
if (multiline_mode) {
    if (strcmp(trimmed, "]]--") == 0) {
        multiline_mode = false;
        luaL_dostring(L, multiline_buffer);
    } else {
        multiline_buffer_append(line);
        multiline_buffer_append("\n");
    }
    continue;
}
```

**Files to modify:**
- `src/injector/main.c` - Console command processing loop

**Usage:**
```lua
--[[
local stat = Ext.Stats.Get("WPN_Longsword")
for k,v in pairs(stat) do
    Ext.Print(k .. " = " .. tostring(v))
end
]]--
```

---

### Tier 2: Memory Introspection APIs

**New Module:** `src/lua/lua_debug.c`

#### 2.1 Low-level Memory Reading

```c
// Ext.Debug.ReadPtr(address) -> number|nil
// Ext.Debug.ReadU32(address) -> number|nil
// Ext.Debug.ReadU64(address) -> number|nil
// Ext.Debug.ReadI32(address) -> number|nil
// Ext.Debug.ReadFloat(address) -> number|nil
// Ext.Debug.ReadString(address, maxLen) -> string|nil
// Ext.Debug.ReadFixedString(address) -> string|nil
```

**Safety considerations:**
- Wrap all reads in signal handlers (SIGSEGV/SIGBUS) to catch bad addresses
- Return `nil` on invalid memory access instead of crashing
- Log warnings for suspicious address ranges

**Implementation pattern:**
```c
static int lua_debug_read_ptr(lua_State *L) {
    uintptr_t addr = (uintptr_t)luaL_checkinteger(L, 1);

    void *result = NULL;
    if (!safe_read_ptr((void*)addr, &result)) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushinteger(L, (lua_Integer)(uintptr_t)result);
    return 1;
}
```

#### 2.2 Struct Probing Utilities

```c
// Ext.Debug.ProbeStruct(baseAddr, startOffset, endOffset, stride)
// Returns table: { [offset] = { ptr=..., u32=..., i32=..., float=... } }

// Ext.Debug.FindArrayPattern(baseAddr, searchRange)
// Scans for (ptr, u32 capacity, u32 size) patterns typical of game arrays
// Returns table of candidate offsets with their values

// Ext.Debug.ProbeFixedStringArray(baseAddr, offset, count)
// Reads array of FixedString values, returns table of strings
```

**Example usage for offset discovery:**
```lua
-- Probe RPGStats for FixedStrings array
local rpgstats = Ext.Stats.GetRawPtr()
local results = Ext.Debug.ProbeStruct(rpgstats, 0x300, 0x400, 0x10)
for offset, data in pairs(results) do
    if data.u32_size > 100 and data.u32_size < 50000 then
        Ext.Print(string.format("+0x%x: likely array, size=%d", offset, data.u32_size))
    end
end
```

#### 2.3 RPGStats-specific Introspection

```c
// Ext.Stats.GetRawPtr() -> number
// Returns base address of gRPGStats for manual probing

// Ext.Stats.GetFixedStringByIndex(index) -> string|nil
// Direct access to FixedStrings[index]

// Ext.Stats.DumpModifierList(typeName) -> table
// Returns all attributes for a modifier list type (e.g., "Weapon")

// Ext.Stats.GetObjectRaw(name) -> table
// Returns raw object data including IndexedProperties array
```

---

### Tier 3: Developer Productivity Tools

#### 3.1 Console Commands (! prefix)

**Reference:** `BG3Extender/LuaScripts/Libs/Events/EventManager.lua:102-153`

```lua
-- Register custom console commands
Ext.RegisterConsoleCommand("probe", function(cmd, baseAddr, range)
    -- Probe memory at address
end)

Ext.RegisterConsoleCommand("findstr", function(cmd, searchStr)
    -- Find string references in memory
end)

Ext.RegisterConsoleCommand("dumpstat", function(cmd, statName)
    -- Dump all properties of a stat object
end)
```

**Usage:**
```
!probe 0x12345678 0x100
!findstr "WPN_Longsword"
!dumpstat WPN_Longsword
```

#### 3.2 Global Helper Functions

**Reference:** `BG3Extender/LuaScripts/BuiltinLibrary.lua:34-59`

```lua
-- Global aliases for rapid debugging
_D = Ext.Dump           -- Full JSON dump
_DS = Ext.DumpShallow   -- Limited depth dump
_P = Ext.Print          -- Print shorthand
_PE = Ext.PrintError    -- Error print

-- Hex formatting helper
_H = function(n) return string.format("0x%x", n) end

-- Pointer arithmetic helper
_PTR = function(base, offset) return base + offset end
```

#### 3.3 Type Introspection

```c
// Ext.Types.GetObjectType(obj) -> string
// Returns the internal type name of a userdata object

// Ext.Types.Validate(obj) -> boolean
// Checks if object reference is still valid

// Ext.Types.GetTypeInfo(typeName) -> table
// Returns metadata about a registered type
```

---

## Implementation Phases

### Phase 1: Multi-line Support (1-2 hours)
- [ ] Add multiline buffer to console processing in `main.c`
- [ ] Implement `--[[` / `]]--` delimiter detection
- [ ] Test with complex multi-line Lua scripts

### Phase 2: Core Memory APIs (4-6 hours)
- [ ] Create `src/lua/lua_debug.c` module
- [ ] Implement safe memory reading with signal handling
- [ ] Add `Ext.Debug.ReadPtr/U32/U64/I32/Float/String/FixedString`
- [ ] Add `Ext.Debug.ProbeStruct` for bulk offset scanning
- [ ] Add `Ext.Stats.GetRawPtr()` for RPGStats access
- [ ] Add `Ext.Stats.GetFixedStringByIndex()` for direct array access
- [ ] Register all functions in `lua_ext_register_debug()`
- [ ] Add to CMakeLists.txt

### Phase 3: Console Commands (2-3 hours)
- [ ] Implement `!` command prefix detection
- [ ] Add `Ext.RegisterConsoleCommand()` Lua function
- [ ] Create command dispatch system
- [ ] Add built-in commands: `!probe`, `!findstr`, `!dumpstat`

### Phase 4: Helper Functions & Polish (1-2 hours)
- [ ] Add global helper aliases (`_D`, `_DS`, `_P`, `_H`, `_PTR`)
- [ ] Implement `Ext.DumpExport()` with JSON options
- [ ] Add `Ext.Types.*` introspection functions
- [ ] Update documentation

---

## Acceptance Criteria

### Functional Requirements
- [ ] Multi-line Lua blocks execute correctly with `--[[` / `]]--` delimiters
- [ ] `Ext.Debug.ReadPtr(addr)` returns value or nil without crashing on bad addresses
- [ ] `Ext.Debug.ProbeStruct()` can scan memory ranges and report potential arrays
- [ ] `Ext.Stats.GetRawPtr()` returns the actual gRPGStats address
- [ ] `!command` syntax dispatches to registered handlers
- [ ] Global helpers (`_D`, `_P`, etc.) work in console

### Non-Functional Requirements
- [ ] Bad memory addresses never crash the game (graceful nil return)
- [ ] Console remains responsive during large memory scans

### Quality Gates
- [ ] All memory read functions handle SIGSEGV/SIGBUS safely
- [ ] No memory leaks in probe functions
- [ ] Code follows existing module patterns (`lua_debug.h`/`.c`)

---

## File Structure

```
src/
├── lua/
│   ├── lua_debug.c      # NEW: Ext.Debug.* implementations
│   ├── lua_debug.h      # NEW: Debug API declarations
│   ├── lua_ext.c        # Update: Register debug module
│   └── lua_stats.c      # Update: Add GetRawPtr, GetFixedStringByIndex
├── injector/
│   └── main.c           # Update: Multi-line console support
```

---

## API Reference (Proposed)

### Ext.Debug

| Function | Parameters | Returns | Description |
|----------|------------|---------|-------------|
| `ReadPtr` | `addr: number` | `number\|nil` | Read pointer at address |
| `ReadU32` | `addr: number` | `number\|nil` | Read uint32 at address |
| `ReadU64` | `addr: number` | `number\|nil` | Read uint64 at address |
| `ReadI32` | `addr: number` | `number\|nil` | Read int32 at address |
| `ReadFloat` | `addr: number` | `number\|nil` | Read float at address |
| `ReadString` | `addr: number, maxLen: number` | `string\|nil` | Read C string |
| `ReadFixedString` | `addr: number` | `string\|nil` | Read FixedString |
| `ProbeStruct` | `base: number, start: number, end: number, stride: number` | `table` | Scan struct for patterns |
| `FindArrayPattern` | `base: number, range: number` | `table` | Find array-like structures |

### Ext.Stats (Extensions)

| Function | Parameters | Returns | Description |
|----------|------------|---------|-------------|
| `GetRawPtr` | none | `number` | Get gRPGStats base address |
| `GetFixedStringByIndex` | `index: number` | `string\|nil` | Direct FixedStrings access |
| `DumpModifierList` | `typeName: string` | `table` | Dump all attributes |
| `GetObjectRaw` | `name: string` | `table` | Raw object data |

### Console Commands

| Command | Arguments | Description |
|---------|-----------|-------------|
| `!probe` | `<addr> <range>` | Probe memory range |
| `!findstr` | `<string>` | Find string references |
| `!dumpstat` | `<statName>` | Dump stat object |
| `!help` | none | List available commands |

---

## Success Metrics

1. **Offset Discovery Time**: Reduce time to find new offsets from hours to minutes
2. **Iteration Speed**: Test memory hypotheses without rebuilding dylib
3. **Debugging Clarity**: Full Lua tracebacks on errors
4. **Code Quality**: Zero crashes from bad memory addresses

---

## References

### Internal References
- Console processing: `src/injector/main.c:process_command_file()`
- Stats manager: `src/stats/stats_manager.c`
- Lua registration pattern: `src/lua/lua_ext.c:lua_ext_register_*()`

### External References (Windows BG3SE)
- Console implementation: `BG3Extender/Extender/Shared/Console.cpp:97-254`
- Multi-line handling: `BG3Extender/Extender/Shared/Console.cpp:237-250`
- Console commands: `BG3Extender/LuaScripts/Libs/Events/EventManager.lua:102-153`
- Debug helpers: `BG3Extender/LuaScripts/BuiltinLibrary.lua:34-59`
- Type introspection: `BG3Extender/LuaScripts/Libs/DevelopmentHelpers.lua`

### Related Work
- Issue #3: Ext.Stats API (demonstrated need for better debugging)
- Ghidra offset discovery workflow (could be partially replaced by runtime probing)

---

## Notes

- Signal handling for safe memory reads is critical - use `sigsetjmp`/`siglongjmp` pattern
- Consider rate-limiting ProbeStruct to prevent game stutter during large scans
- Multi-line mode state should reset on any error to prevent stuck state
