# BG3SE-macOS Contributor Workflow Guide

A comprehensive guide for contributors to understand and replicate our reverse-engineering and feature implementation workflow.

## Table of Contents

1. [Overview](#overview)
2. [Workflow Phases](#workflow-phases)
3. [Phase 1: Research & Analysis](#phase-1-research--analysis)
4. [Phase 2: Ghidra Reverse Engineering](#phase-2-ghidra-reverse-engineering)
5. [Phase 3: Offset Documentation](#phase-3-offset-documentation)
6. [Phase 4: Runtime Verification](#phase-4-runtime-verification)
7. [Phase 5: Implementation](#phase-5-implementation)
8. [Phase 6: Testing & Iteration](#phase-6-testing--iteration)
9. [Example Workflows](#example-workflows)
10. [Tools Reference](#tools-reference)
11. [AI-Assisted Development with MCP Servers](#ai-assisted-development-with-mcp-servers)
12. [Getting Help](#getting-help)

---

## Overview

BG3SE-macOS is a ground-up rebuild of Norbyte's Windows Script Extender for macOS ARM64. Since the Windows BG3SE uses x86_64 assembly and Windows-specific APIs, we cannot port it directly. Instead, we:

1. **Study the Windows BG3SE** as architectural reference
2. **Reverse-engineer the macOS binary** using Ghidra to find equivalent offsets
3. **Document discoveries** in structured markdown files
4. **Verify offsets at runtime** using our debug console
5. **Implement features** in C/Lua following the established patterns

### Key Principles

- **Windows BG3SE is the API spec** - We implement the same Lua APIs documented in their `Docs/` folder
- **macOS offsets differ** - ARM64 has different struct packing, alignment, and calling conventions
- **Document everything** - Offset documentation enables maintenance when the game updates
- **Runtime verification first** - Before writing code, verify offsets work in the live game

---

## Workflow Phases

```
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│  1. Research    │────▶│  2. Ghidra      │────▶│  3. Document    │
│  Windows BG3SE  │     │  Analysis       │     │  Offsets        │
└─────────────────┘     └─────────────────┘     └─────────────────┘
                                                        │
                                                        ▼
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│  6. Test &      │◀────│  5. Implement   │◀────│  4. Runtime     │
│  Iterate        │     │  Code           │     │  Verification   │
└─────────────────┘     └─────────────────┘     └─────────────────┘
```

---

## Phase 1: Research & Analysis

### Step 1.1: Identify the Feature

Start with the [ROADMAP.md](../ROADMAP.md) to find what needs implementing:

```markdown
### 2.6 User & Mod Variables
**Status:** ❌ Not Started - **CRITICAL**

From API.md: "v10 adds support for attaching custom properties to entities."
```

### Step 1.2: Study Windows BG3SE Reference

Use semantic search (osgrep) to find relevant code in the Windows implementation:

```bash
# Search the Windows BG3SE codebase
osgrep "user variables entity" -p /Users/tomdimino/Desktop/Programming/bg3se
osgrep "RegisterUserVariable" -p /Users/tomdimino/Desktop/Programming/bg3se
osgrep "stats property resolution" -p /Users/tomdimino/Desktop/Programming/bg3se
```

**Key directories in Windows BG3SE:**

| Directory | Contents |
|-----------|----------|
| `BG3Extender/Lua/` | Lua API implementations |
| `BG3Extender/Lua/Libs/` | Individual API modules (Entity.inl, Stats.inl) |
| `BG3Extender/GameDefinitions/` | C++ struct definitions |
| `BG3Extender/Osiris/` | Osiris scripting engine bindings |
| `CoreLib/` | Core utilities, memory patterns |

### Step 1.3: Document the Target API

Create a plan file in `plans/` with:

```markdown
# Plan: Feature Name (ROADMAP Phase X.X)

## Target API (from Windows BG3SE Docs)

```lua
-- Example API from Windows BG3SE documentation
Ext.Vars.RegisterUserVariable("MyVar", { Server = true })
entity.Vars.MyVar = { data = 123 }
```

## Architecture Analysis

How Windows BG3SE implements this...

## macOS Approach

How we'll adapt it for ARM64...
```

### Step 1.4: Identify What Offsets Are Needed

List the memory addresses/offsets you need to discover:

- Global variables (e.g., `RPGStats::m_ptr`)
- Function addresses (e.g., `EntityWorld::GetComponent`)
- Struct member offsets (e.g., `Object.IndexedProperties` at +0x08)
- TypeIds for components

---

## Phase 2: Ghidra Reverse Engineering

### Step 2.1: Setup

**Prerequisites:**
```bash
brew install ghidra
brew install openjdk@21
```

**First-time project setup:**
```bash
# Extract ARM64 slice from the game binary
lipo -thin arm64 \
  "/Users/$USER/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3" \
  -output ~/ghidra_projects/BG3_arm64_thin

# Import into Ghidra (this takes a while for the 1GB+ binary)
JAVA_HOME="/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home" \
  ~/ghidra/support/analyzeHeadless ~/ghidra_projects BG3Analysis \
  -import ~/ghidra_projects/BG3_arm64_thin \
  -processor "AARCH64:LE:64:v8A"
```

### Step 2.2: Write or Use Analysis Scripts

Analysis scripts live in `ghidra/scripts/`. They automate discovery of specific offsets.

**Script structure pattern:**

```python
#!/usr/bin/env python3
"""
find_something.py - Brief description

Strategy:
1. Search for relevant strings
2. Find XREFs to those strings
3. Analyze surrounding code
4. Extract offsets/addresses
"""

from ghidra.program.model.symbol import SourceType
from progress_utils import init_progress, progress, finish_progress

def find_string_address(search_str):
    """Find address of a string in the binary."""
    memory = currentProgram.getMemory()
    for block in memory.getBlocks():
        if not block.isInitialized():
            continue
        start = block.getStart()
        try:
            data = bytearray(block.getSize())
            block.getBytes(start, data)
            idx = bytes(data).find(search_str.encode('utf-8'))
            if idx >= 0:
                return start.add(idx)
        except:
            continue
    return None

def find_xrefs(addr):
    """Find cross-references to an address."""
    refs = []
    refManager = currentProgram.getReferenceManager()
    for ref in refManager.getReferencesTo(addr):
        refs.append(ref.getFromAddress())
    return refs

def main():
    init_progress("find_something.py")

    # Your analysis logic here
    addr = find_string_address("SearchString")
    if addr:
        xrefs = find_xrefs(addr)
        for xref in xrefs:
            print("Found XREF at {}".format(xref))

    finish_progress()

if __name__ == "__main__":
    main()
```

### Step 2.3: Run Analysis Scripts

**Fast mode (read-only, uses existing analysis):**
```bash
./ghidra/scripts/run_analysis.sh find_rpgstats.py
```

**With re-analysis (slow, only when needed):**
```bash
./ghidra/scripts/run_analysis.sh find_rpgstats.py -analyze
```

**Monitor progress:**
```bash
tail -f /tmp/ghidra_progress.log
```

### Step 2.4: Ghidra MCP (AI-Assisted Decompilation)

When Ghidra is running with the pyghidra-mcp plugin enabled, the assistant has direct access to decompilation via MCP tools. This enables automated bulk extraction.

**Available MCP tools:**
```
mcp__ghidra__search_functions_by_name(query, offset, limit)  # Find functions
mcp__ghidra__decompile_function(name)                        # Decompile by name
mcp__ghidra__decompile_function_by_address(address)          # Decompile by address
mcp__ghidra__list_strings(filter, limit)                     # Search strings
mcp__ghidra__get_xrefs_to(address)                           # Find references
```

**Example: Batch component size extraction**
```
# Search for AddComponent functions
mcp__ghidra__search_functions_by_name(query="AddComponent", offset=0, limit=50)

# Decompile specific function
mcp__ghidra__decompile_function(name="AddComponent<eoc::HealthComponent>")
# Output includes: ComponentFrameStorageAllocRaw(..., SIZE, ...)
#                                                     ^^^^
#                                       Second arg = component size
```

**Parallel extraction with subagents:**
Launch multiple the assistant subagents to process different offset ranges simultaneously. See "Subagent Workflow Patterns" section below.

### Step 2.5: Manual Ghidra Investigation

Sometimes you need interactive Ghidra. Key techniques:

**Symbol search:**
- Window → Symbol Table
- Filter by name containing your search term

**String search:**
- Search → For Strings
- Look for component names like "eoc::StatsComponent"

**Decompilation:**
- Right-click function → Decompile
- Look for struct access patterns (offsets like `param_1 + 0x348`)

**Finding global variables:**
```
ARM64 pattern:
    adrp x8, #0x1089c5000    ; Load page address
    ldr  x8, [x8, #0x730]    ; Load from page + offset
    ; Combined: 0x1089c5730
```

---

## Phase 3: Offset Documentation

### Step 3.1: Create/Update Offset Files

Offset documentation lives in `ghidra/offsets/`. Each system has its own file:

| File | System |
|------|--------|
| `STATS.md` | RPGStats, stat objects, properties |
| `ENTITY_SYSTEM.md` | ECS, EntityWorld, components |
| `OSIRIS.md` | Osiris scripting engine |
| `GLOBAL_STRING_TABLE.md` | FixedString resolution |
| `COMPONENTS.md` | Component TypeIds and addresses |

### Step 3.2: Documentation Format

Follow this structure:

```markdown
# System Name Offsets (macOS ARM64)

## Overview

Brief description of what this system does.

## Key Symbols

| Symbol | Address | Description |
|--------|---------|-------------|
| `SomeClass::m_ptr` | `0x1089c5730` | Global pointer to instance |

## Mangled Symbol Names

```
__ZN8RPGStats5m_ptrE -> RPGStats::m_ptr
```

## Structure Layout

```c
struct MyStruct {
    void* VMT;                    // 0x00
    Array<int32_t> SomeArray;     // 0x08
    FixedString Name;             // 0x20
};
```

### Verified Offsets (Date)

| Field | Offset | Size | Verified Value | Notes |
|-------|--------|------|----------------|-------|
| VMT | +0x00 | 8 | 0x010cf54608 | Virtual method table |

## Verification Method

```lua
-- Console commands used to verify
local ptr = Ext.Memory.Read(base + 0x08, 8)
Ext.Print(string.format("0x%x", ptr))
```
```

### Step 3.3: Cross-Reference with Windows BG3SE

Always note the Windows struct for comparison:

```markdown
## Windows BG3SE Reference

From `BG3Extender/GameDefinitions/Stats/Stats.h`:

```c
struct RPGStats {
    // Windows layout (may differ on macOS ARM64)
    CNamedElementManager<RPGEnumeration> ModifierValueLists;
    CNamedElementManager<ModifierList> ModifierLists;
    // ...
};
```

**Note:** macOS ARM64 offsets differ due to alignment/packing differences.
```

---

## Phase 4: Runtime Verification

Before writing implementation code, verify discovered offsets work in the live game.

### Step 4.1: Launch the Game with BG3SE

```bash
./scripts/launch_bg3.sh
```

### Step 4.2: Connect to Console

**Option A: Socket console (recommended)**
```bash
./build/bin/bg3se-console
```

**Option B: File-based**
```bash
echo 'Ext.Print("test")' > ~/Library/Application\ Support/BG3SE/commands.txt
```

### Step 4.3: Probe Memory

Use `Ext.Debug` APIs to verify offsets:

```lua
-- Get module base address
local base = Ext.Memory.GetModuleBase("Baldur")
_P(string.format("Module base: 0x%x", base))

-- Read global pointer (e.g., RPGStats::m_ptr at offset 0x89c5730)
local rpgstats_ptr = Ext.Debug.ReadPtr(base + 0x89c5730)
_P(string.format("RPGStats: 0x%x", rpgstats_ptr))

-- Probe struct members
local results = Ext.Debug.ProbeStruct(rpgstats_ptr, 0, 0x100, 8)
for offset, data in pairs(results) do
    if data.ptr and data.ptr > 0x100000000 then
        _P(string.format("+0x%x: ptr=0x%x", offset, data.ptr))
    end
end

-- Hex dump
Ext.Debug.HexDump(rpgstats_ptr, 64)
```

### Step 4.4: Verify Specific Offsets

```lua
-- Example: Verify FixedStrings pool at RPGStats + 0x348
local fs_pool = Ext.Debug.ReadPtr(rpgstats_ptr + 0x348)
_P(string.format("FixedStrings pool: 0x%x", fs_pool))

-- Read first few entries
for i = 0, 10 do
    local fs_idx = Ext.Debug.ReadU32(fs_pool + i * 4)
    _P(string.format("[%d] = %d", i, fs_idx))
end
```

### Step 4.5: Document Verified Values

Update the offset documentation with runtime-verified values:

```markdown
### Runtime-Verified Offsets (Dec 9, 2025)

**Verified via console probing:**

| Member | Offset | Verified Value |
|--------|--------|----------------|
| `FixedStrings.buf_` | `+0x348` | 0x1231b4e00 |

**Console commands used:**
```lua
local rpg = Ext.Debug.ReadPtr(base + 0x89c5730)
local fs = Ext.Debug.ReadPtr(rpg + 0x348)
```
```

---

## Phase 5: Implementation

### Step 5.1: Plan the Code Structure

Identify which files to create/modify:

| File | Action | Description |
|------|--------|-------------|
| `src/module/new_module.c` | CREATE | Core implementation |
| `src/module/new_module.h` | CREATE | Public API |
| `src/lua/lua_newapi.c` | CREATE | Lua bindings |
| `src/injector/main.c` | MODIFY | Hook registration |
| `CMakeLists.txt` | MODIFY | Add new source files |

### Step 5.2: Follow Codebase Patterns

**Module pattern:**
```c
// module.h - Public interface
#ifndef MODULE_H
#define MODULE_H

void module_init(void);
int module_get_count(void);

#endif

// module.c - Implementation
#include "module.h"
#include "core/logging.h"

static int item_count = 0;  // Private state

void module_init(void) {
    LOG_MODULE_INFO("Initializing...");
}

int module_get_count(void) {
    return item_count;
}
```

**Lua binding pattern:**
```c
// lua_module.c
static int lua_module_get_count(lua_State *L) {
    lua_pushinteger(L, module_get_count());
    return 1;  // Number of return values
}

void lua_module_register(lua_State *L, int ext_table_idx) {
    lua_newtable(L);  // Create Ext.Module table

    lua_pushcfunction(L, lua_module_get_count);
    lua_setfield(L, -2, "GetCount");

    lua_setfield(L, ext_table_idx, "Module");
}
```

### Step 5.3: Handle ARM64 Specifics

**Large struct returns (>16 bytes):**
```c
// ARM64 requires x8 register for indirect return
typedef struct __attribute__((aligned(16))) {
    void* value;
    uint64_t reserved[5];
    uint8_t has_error;
    uint8_t _pad[15];
} LsResult;

void* call_with_x8_buffer(void* fn, void* arg) {
    LsResult result = {0};
    result.has_error = 1;

    __asm__ volatile (
        "mov x8, %[buf]\n"
        "mov x0, %[arg]\n"
        "blr %[fn]\n"
        : "+m"(result)
        : [buf] "r"(&result), [arg] "r"(arg), [fn] "r"(fn)
        : "x0", "x1", "x8", "memory"
    );

    return (result.has_error == 0) ? result.value : NULL;
}
```

**Safe memory reads:**
```c
#include "core/safe_memory.h"

// Never crashes, returns false on bad address
bool success = safe_read_ptr(address, &value);
if (!success) {
    LOG_ERROR("Invalid memory at 0x%llx", address);
}
```

---

## Phase 6: Testing & Iteration

### Step 6.1: Build

```bash
cd build && cmake .. && cmake --build .
```

### Step 6.2: Test with Console

```bash
./scripts/launch_bg3.sh
# In another terminal:
./build/bin/bg3se-console
```

```lua
-- Test new API
local result = Ext.NewModule.GetSomething()
_P(tostring(result))
```

### Step 6.3: Watch Logs

```bash
tail -f ~/Library/Application\ Support/BG3SE/bg3se.log
```

### Step 6.4: Iterate

If offsets don't work:
1. Re-verify with Ghidra
2. Check ARM64 alignment differences
3. Use `Ext.Debug.HexDump` to inspect actual memory

### Step 6.5: Update Documentation

- Update ROADMAP.md with new status
- Update offset files with any new discoveries

---

## Example Workflows

### Example A: Adding a New Ext.Stats Property

**1. Research:**
```bash
osgrep "stats property" -p /Users/tomdimino/Desktop/Programming/bg3se
```
Found: Properties use `IndexedProperties` array + `FixedStrings` pool.

**2. Ghidra:**
```bash
./ghidra/scripts/run_analysis.sh find_getfixedstring.py
```
Discovered: `FixedStrings` at `RPGStats + 0x348`.

**3. Document:**
Updated `ghidra/offsets/STATS.md` with new offset.

**4. Verify:**
```lua
local fs = Ext.Debug.ReadPtr(rpgstats + 0x348)
-- Confirmed valid pointer
```

**5. Implement:**
Added `stats_get_fixed_string()` in `stats_manager.c`.

**6. Test:**
```lua
local stat = Ext.Stats.Get("WPN_Longsword")
_P(stat.Damage)  -- Output: "1d8"
```

### Example B: Discovering a Component TypeId

**1. Research:**
Need `eoc::HealthComponent` TypeId for entity health access.

**2. Ghidra:**
Search for string "eoc::HealthComponent", find XREFs to TypeId registration.

**3. Runtime discovery:**
```lua
-- TypeId globals are populated at SessionLoaded
Ext.Events.SessionLoaded:Subscribe(function()
    local type_id = Ext.Entity.GetTypeId("eoc::HealthComponent")
    _P("HealthComponent TypeId: " .. type_id)
end)
```

**4. Document:**
Added to `ghidra/offsets/COMPONENTS.md`:
```markdown
| eoc::HealthComponent | 575 | SessionLoaded |
```

---

## Tools Reference

### osgrep (Semantic Code Search)

```bash
# Search this project
osgrep "how does event dispatch work"

# Search Windows BG3SE reference
osgrep "entity manager" -p /Users/tomdimino/Desktop/Programming/bg3se
```

### Ghidra Scripts

| Script | Purpose |
|--------|---------|
| `find_rpgstats.py` | gRPGStats global pointer |
| `find_entity_offsets.py` | Entity system offsets |
| `find_getfixedstring.py` | FixedStrings pool |
| `quick_component_search.py` | Component string XREFs |
| `decompile_getcomponent.py` | GetComponent analysis |

### Ext.Debug API

| Function | Purpose |
|----------|---------|
| `ReadPtr(addr)` | Read pointer (8 bytes) |
| `ReadU32(addr)` | Read uint32 |
| `ReadU64(addr)` | Read uint64 |
| `ReadString(addr, max)` | Read C string |
| `ProbeStruct(base, start, end, stride)` | Bulk offset discovery |
| `HexDump(addr, size)` | Hex dump memory |
| `FindArrayPattern(base, range)` | Find ptr+cap+size patterns |

### Console Commands

```bash
# Quick probing
!probe 0x12345678 256
!hexdump 0x12345678 64

# Custom commands (registered in Lua)
!dumpstat WPN_Longsword
!types
```

---

## AI-Assisted Development with MCP Servers

This project uses Model Context Protocol (MCP) servers, CLI tools, and the CLI skills for AI-assisted research, planning, and review. These tools significantly accelerate the reverse-engineering workflow.

### Available Tools

| Tool | Type | Purpose |
|------|------|---------|
| **bg3se-macos-ghidra** | the assistant Skill | Project-specific patterns, offsets, and workflows |
| **osgrep** | CLI | Semantic code search - finds code by concept, not keywords |
| **Exa** | MCP Server | Web search + code context from documentation and GitHub |
| **Perplexity** | MCP Server | AI-powered research and documentation lookup |
| **Context7** | MCP Server | Fetch up-to-date library documentation |

### bg3se-macos-ghidra Skill

This the CLI skill provides project-specific context for BG3SE-macOS development. It includes:

- **Project locations** for both bg3se-macos and the Windows reference implementation
- **Build and test commands** for quick reference
- **Ghidra headless commands** with optimized analyzer settings
- **Key offsets** (EoCServer, EntityWorld, RPGStats, etc.)
- **Critical ARM64 patterns** (x8 indirect return, GUID byte order)
- **Module design patterns** for consistent code structure
- **Lua API registration patterns**

**When the CLI loads this skill, it has immediate context about:**

- How to build and test the project
- Where key files are located
- What offsets have been discovered
- ARM64-specific calling conventions
- Common tasks like adding new Ext.* APIs or discovering offsets

**Invoking the skill:** The skill is automatically available when working in the bg3se-macos project. the CLI can reference it for project-specific guidance on Ghidra analysis, offset documentation, and implementation patterns.

### osgrep for Codebase Research

osgrep is a local semantic code search tool that uses AI embeddings to find code by concept rather than literal string matching. It's ideal for exploring unfamiliar codebases.

**Basic usage:**

```bash
# Search with natural language queries
osgrep "user registration flow"
osgrep "how does authentication work"

# Scope to a specific directory
osgrep "entity component access" path/to/scope

# Control result count
osgrep "validation logic" -m 20           # Max 20 results
osgrep "validation logic" --per-file 3    # Up to 3 matches per file
```

**Example queries for this project:**

```bash
# Research how Windows BG3SE implements a feature
osgrep "stats property resolution" -p /Users/tomdimino/Desktop/Programming/bg3se
osgrep "GUID to entity handle lookup" -p /Users/tomdimino/Desktop/Programming/bg3se
osgrep "how does Lua component binding work" -p /Users/tomdimino/Desktop/Programming/bg3se

# Search our macOS implementation
osgrep "where are stats properties resolved" -p /Users/tomdimino/Desktop/Programming/bg3se-macos
osgrep "ARM64 indirect return pattern" -p /Users/tomdimino/Desktop/Programming/bg3se-macos
```

**Index management:**

```bash
osgrep index                    # Incremental update
osgrep index --reset            # Full re-index from scratch
osgrep list                     # Show indexed repositories
osgrep doctor                   # Check health and configuration
```

**When to use osgrep:**

- Understanding how Windows BG3SE implements a feature before porting
- Finding conceptual patterns across large codebases
- Locating cross-cutting concerns (e.g., "all database migrations")

**When to use traditional grep/Glob instead:**

- Searching for exact strings or identifiers
- Finding files by name pattern
- Already know the exact location

### Exa for External Research

Exa MCP server provides web search and code context from the broader ecosystem. Use it via the CLI's MCP integration.

**Capabilities:**

- `web_search_exa` - General web search with AI-optimized results
- `get_code_context_exa` - Find code examples and documentation for APIs/libraries

**Example use cases:**

- Finding ARM64 calling convention documentation
- Searching for Ghidra scripting examples
- Looking up game modding documentation and tutorials
- Researching macOS-specific patterns

### Perplexity for Documentation & Research

Perplexity MCP server provides AI-powered research with citation support.

**Capabilities:**

- `search` - General research queries with configurable detail level
- `get_documentation` - Fetch documentation for technologies/frameworks
- `check_deprecated_code` - Verify APIs are still current

**Example use cases:**

- Getting comprehensive overviews of unfamiliar technologies (e.g., "Dobby ARM64 inline hooking")
- Understanding macOS security mechanisms (Hardened Runtime, code signing)
- Validating that APIs haven't been deprecated in recent OS versions

### Context7 for Framework Docs

Context7 MCP server fetches up-to-date documentation for frameworks and libraries.

**Capabilities:**

- `resolve-library-id` - Find the Context7 ID for a library
- `get-library-docs` - Fetch documentation with optional topic filtering

**Example use cases:**

- Getting current Lua 5.4 documentation for userdata handling
- Looking up CMake best practices for library linking
- Checking current API signatures for dependencies

### Integration in Our Workflow

**Phase 1 (Research):** Use osgrep to understand Windows BG3SE implementation patterns. Use Exa to find external documentation about ARM64 or macOS-specific techniques.

**Phase 2 (Ghidra):** Use Perplexity to research ARM64 calling conventions and struct layouts when analyzing decompiled code.

**Phase 3 (Documentation):** Use Context7 to ensure our code follows current best practices for C/Lua/CMake.

**Phase 5 (Implementation):** Use Exa code context to find examples of similar implementations in other projects.

**Code Review:** Use specialized review agents (architecture-strategist, security-sentinel, performance-oracle) for multi-perspective code review before merging.

**Plan Review:** Use `/compounding-engineering:plan_review` to have multiple AI agents analyze implementation plans in parallel before starting work.

---

## Subagent Workflow Patterns

the CLI's Task tool enables launching parallel subagents for bulk extraction and analysis tasks. This dramatically accelerates repetitive work.

### When to Use Subagents

- **Bulk extraction**: Processing 100+ similar items (components, functions, offsets)
- **Independent work**: Tasks that don't depend on each other's results
- **Large search spaces**: Covering different ranges of a large dataset

### Pattern: Parallel Ghidra MCP Extraction

**Goal:** Extract ARM64 component sizes from 2000+ AddComponent functions

**Step 1: Determine total scope**
```
mcp__ghidra__search_functions_by_name(query="AddComponent", offset=0, limit=1)
# Note total count from response
```

**Step 2: Launch parallel agents**
```
Task tool with subagent_type="general-purpose":

Agent 1: "Search AddComponent functions at offset 0-50, decompile each,
          extract SIZE from ComponentFrameStorageAllocRaw calls.
          Report component name and size for each."

Agent 2: "Search AddComponent functions at offset 50-100, decompile each,
          extract SIZE from ComponentFrameStorageAllocRaw calls.
          Report component name and size for each."

... (8-10 agents covering offset 0-500+)
```

**Step 3: Collect results**
```
TaskOutput(task_id="agent_1_id", block=true)
TaskOutput(task_id="agent_2_id", block=true)
# Consolidate into documentation
```

### Best Practices

1. **Clear, specific prompts**: Tell agents exactly what to extract and report
2. **Consistent output format**: Request structured output (tables, lists)
3. **Error tolerance**: Agents should skip failures and continue
4. **Namespace awareness**: Group results by component prefix
5. **Documentation target**: Tell agents which doc file to reference/update

### Example Agent Prompt

```
You are extracting ARM64 component sizes via Ghidra MCP.

TASK:
1. Search for AddComponent functions: offset=700, limit=50
2. For each function, decompile and look for ComponentFrameStorageAllocRaw
3. Extract the SIZE parameter (second argument, in hex or decimal)
4. Report results as: ComponentName | Size (bytes) | Notes

PATTERN TO FIND:
ComponentFrameStorageAllocRaw((ComponentFrameStorage*)(this_00 + 0x48), SIZE, ...)

Skip any functions that don't match this pattern.
Organize results by namespace prefix (eoc::, esv::, ls::, etc.).
```

### Results from Dec 2025 Extraction

Using 10 parallel agents, extracted 438 component sizes in ~20 minutes:
- `COMPONENT_SIZES_EOC_NAMESPACED.md`: 185 components (56 sub-namespaces)
- `COMPONENT_SIZES_LS.md`: 60 engine components
- `COMPONENT_SIZES_ESV.md`: 58 server components
- Key discovery: OneFrameComponent pattern (28+ event types, 1-488 bytes)

---

## Agent Teams (Minoan Swarm)

the CLI Agent Teams extend the subagent pattern into persistent, coordinated teams with shared task lists and bi-directional messaging. This is a paradigm shift from the subagent model above: instead of fire-and-forget tasks, agents are persistent teammates that claim work, communicate, and coordinate in real-time.

### When Agent Teams Beat Subagents

| Situation | Use Subagents | Use Agent Teams |
|-----------|--------------|-----------------|
| Bulk extraction (components, offsets) | ✅ Independent, no coordination | |
| Multi-file feature with dependencies | | ✅ Agents need to sequence work |
| Research + implement + review pipeline | | ✅ Pipeline stages with handoff |
| Cross-cutting audit (security + perf + arch) | | ✅ Parallel reviewers, merged findings |
| Same-file edits | Neither — do it sequentially | |

### Team Structure

Teams use the **Minoan Swarm** naming convention — names from the Minoan-Semitic divine feminine, chosen by archetype:

- **Leaders** (Opus): athirat, qedesha, tiamat — orchestrate, don't implement
- **Builders** (Sonnet): kaptaru, mami, nintu — feature implementers
- **Researchers** (Haiku/Sonnet): devorah, melissa — exploration, API verification
- **Testers** (Sonnet): sassuratu, phikola — live testing, regression validation

### File Ownership Matrix

**Critical:** Two agents editing the same file causes overwrites. Every team plan must include an explicit ownership matrix:

```
| Agent      | Owns                      | Must NOT Touch        |
|------------|---------------------------|-----------------------|
| kaptaru    | lua_ext.c lines 1126-1220 | console_cmds[] array  |
| sassuratu  | lua_ext.c lines 1221-1300 | lines above 1220      |
| athirat    | console_cmds[] array only | test string variables |
```

### Workflow: Plan → Research → Build → Review

Every non-trivial feature follows this pipeline:

1. **Plan**: Write a plan file to `plans/`. Include constraints, test inventory, team structure, file ownership, and execution pipeline.
2. **Research**: Launch research agents to verify assumptions before implementation. Research agents confirmed that `Ext.Stats.Get()` returns userdata (not table), `Ext.Types.TypeOf()` returns table (not string), and `Ext.Timer.Pause()` returns boolean — preventing 3 bugs.
3. **Build**: Agents claim tasks from the shared task list. The lead orchestrates but does not implement.
4. **Review**: Parallel review agents check code quality, constraint compliance (e.g., ISO C99 4095-char string literal limit), and API accuracy. This step caught 1 bug pre-runtime in the comprehensive test suite.

### Example: Comprehensive Test Suite (v0.36.41)

**Plan**: `plans/comprehensive-test-suite-swarm.md`
**Team**: `kaptaru-test-forge` (athirat-lead + kaptaru builder + sassuratu tester)

Pipeline:
```
Phase 1: Research agents verify API signatures across 20 namespaces
Phase 2: kaptaru writes 7 Tier 1 test strings, sassuratu writes 2 Tier 2 strings
Phase 3: athirat writes framework + registration, updates console_cmds[], builds
Phase 4: 3 parallel review agents (code quality, string lengths, API signatures)
Result:  ~94 tests, 1 bug caught pre-runtime, clean build
```

### Example: Component Size Extraction (Dec 2025)

10 parallel Ghidra MCP agents, each covering a 50-function range:
- **Input**: 2,000+ `AddComponent<T>` functions in the BG3 binary
- **Output**: 1,577 ARM64 component sizes across 6 documentation files
- **Time**: ~20 minutes (vs days of manual decompilation)

### Starting a Team

```
1. TeamCreate: name="team-name", description="purpose"
2. TaskCreate: create all tasks with descriptions and dependencies
3. Task tool: spawn teammates with team_name, name, subagent_type
4. TaskUpdate: assign tasks to idle teammates
5. Monitor: check TaskList, respond to teammate messages
6. Shutdown: SendMessage type="shutdown_request" to each teammate
7. TeamDelete: clean up after all teammates confirm
```

For naming conventions, archetype mappings, and ready-to-use team templates, see the [Minoan Swarm](https://github.com/tdimino/claude-code-minoan/tree/main/skills/planning-productivity/minoan-swarm) skill documentation.

---

## Getting Help

- **agent_docs/**: Detailed architecture documentation
- **ROADMAP.md**: Feature status and priorities
- **plans/**: Implementation plans for features

When stuck:

1. Search Windows BG3SE for similar functionality
2. Use `osgrep` with natural language queries
3. Probe with `Ext.Debug` in the live game
4. Use MCP servers (Exa, Perplexity) for external research
5. Document your findings even if incomplete
