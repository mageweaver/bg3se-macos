# Testing Guide

Comprehensive reference for the BG3SE-macOS test suite.

## Table of Contents

- [Overview](#overview)
- [Quick Reference](#quick-reference)
- [Test Tiers](#test-tiers)
  - [Tier 1: General Tests](#tier-1-general-tests)
  - [Tier 2: In-Game Tests](#tier-2-in-game-tests)
- [Test Categories](#test-categories)
  - [Core](#core-6-tests)
  - [Json](#json-4-tests)
  - [Helpers](#helpers-5-tests)
  - [Stats](#stats-14-tier-1-tests)
  - [Timer](#timer-8-tests)
  - [Events](#events-5-tests)
  - [Debug](#debug-10-tests)
  - [Types](#types-6-tests)
  - [Enums](#enums-3-tests)
  - [IO](#io-3-tests)
  - [Memory](#memory-3-tests)
  - [Mod](#mod-4-tests)
  - [Vars](#vars-2-tests)
  - [Osi (Tier 1)](#osi-tier-1-4-tests)
  - [MCM](#mcm-10-tests)
  - [Entity](#entity-8-tests)
  - [Level](#level-5-tests)
  - [Audio](#audio-4-tests)
  - [Net](#net-4-tests)
  - [IMGUI](#imgui-2-tests)
  - [StaticData](#staticdata-2-tests)
  - [Osi Dispatch (Tier 2)](#osi-dispatch-8-tests)
  - [Osi Edge Cases](#osi-edge-cases-5-tests)
  - [Entity Events](#entity-events-5-tests)
- [Running Tests](#running-tests)
  - [Console Commands](#console-commands)
  - [Filtering](#filtering)
  - [Automated via Socket](#automated-via-socket)
- [Test Output Format](#test-output-format)
- [Assertion Helpers](#assertion-helpers)
- [Writing New Tests](#writing-new-tests)
- [Init Timing](#init-timing)
- [Troubleshooting](#troubleshooting)
- [Performance Notes](#performance-notes)

---

## Overview

| Metric | Value |
|--------|-------|
| **Total tests** | 502 (294 offline + 208 Lua) |
| **Tier 0 (C unit)** | 55 — native binary, no game, CI-safe |
| **Tier H (pytest)** | 239 — Python harness, no game, CI-safe |
| **Tier 1 (General)** | 113 — Lua, run anytime, no save needed |
| **Tier 2 (In-Game)** | 95 — Lua, require loaded save |
| **CI pipeline** | `.github/workflows/test-offline.yml` (Tier 0 + Tier H) |

### Tier 0: Native C Unit Tests

55 tests in `tests/tier0/`. Built as standalone binary `bg3se_test_tier0` via CMake.
Tests memory-safety-critical C code without any game process.

```bash
cd build && cmake --build . --target bg3se_test_tier0 && ./bin/bg3se_test_tier0
```

| Category | Count | What it Tests |
|----------|-------|---------------|
| safe_memory | 17 | GPU region detection, null/edge reads, stack reads, address validation |
| pattern_scan | 10 | parse_pattern (null, empty, simple, wildcard), find_pattern (exact, wildcard, boundaries) |
| osiris_handles | 8 | encode/decode roundtrips, low/high type funcIndex, part4 bit 31 |
| entity_events | 6 | MAKE_SUB_ID/SUB_ID_TYPE/SUB_ID_INDEX macro roundtrips, all types, max index |
| mod_paths | 14 | pak stem parsing and Public directory path matching |

### Tier H: Python Harness Tests

239 tests in `tests/harness/`. Run with pytest, no game dependency.

```bash
PYTHONPATH=tools pytest tests/harness/ -v
```

| Module | Count | What it Tests |
|--------|-------|---------------|
| test_cli | 11 | headless lifecycle, build failures, memory-pressure flags |
| test_compat | 23 | log scan scoping + honesty, Nexus download hardening, sentinel protocol, enable/quit fail-closed paths, diff |
| test_component_ops_audit | 5 | ComponentOps registry invariants |
| test_component_write_audit | 2 | component write layout guards |
| test_crashlog | 3 | IPS parsing, crash classification, enabled-mod extraction |
| test_functor_abi_audit | 1 | functor ABI invariant |
| test_game_path | 10 | Steam library parsing and BG3 app bundle resolution |
| test_headless_graphics | 4 | windowed mode graphics prep and restore |
| test_launch | 6 | wait_for_socket lifecycle and menu-stall watchdog |
| test_launch_lifecycle | 51 | process adoption, identity, graphics, Steam, and memory gates |
| test_menu | 5 | menu detection, click delivery, coordinate geometry |
| test_mod | 16 | UUID resolution, pak metadata, registry reconciliation, preflight |
| test_monitor | 8 | process tracking, atomic health writes, graphics restoration |
| test_offset_audit | 71 | symbol manifest, offset-table, remap, and functor invariants |
| test_physics_vmt_audit | 4 | physics VMT index invariants |
| test_savegames | 10 | fixture snapshot/restore source tracking, backup safety, save-mod marker classification |
| test_scenarios | 3 | scenario manifest schema, MCM injection, Lua assertion syntax (luac) |
| test_test_runner | 6 | output parsing and socket error handling |

### Tier 1+2: Lua In-Game Tests

Framework: Custom Lua, registered via `BG3SE_AddTest(tier, name, fn)`.
Location: `src/lua/lua_ext.c` (C string constants).
String limit: 4095 chars per constant (ISO C99).
Assertion lib: 6 helpers loaded before all tests.

## Quick Reference

```bash
# Run all Tier 1 tests (always works)
echo '!test' | nc -U /tmp/bg3se.sock

# Run all Tier 2 tests (needs loaded save)
echo '!test_ingame' | nc -U /tmp/bg3se.sock

# Filter by category
echo '!test Stats' | nc -U /tmp/bg3se.sock
echo '!test_ingame Osi' | nc -U /tmp/bg3se.sock
```

---

## Test Tiers

### Tier 1: General Tests

113 tests that run without a loaded save. Test API registration and behavior, including 24 parity tests.

| Category | Count | What it Tests |
|----------|-------|---------------|
| Core | 6 | Print, GetVersion, IsServer, IsClient, GetContext, RegisterConsoleCommand |
| Json | 4 | Parse, ParseArray, Roundtrip, ParseInvalid (returns nil) |
| Helpers | 5 | _P, _H, _D, _DS, _PE global helpers |
| Stats | 16 | Core reads/writes plus Goal 2.3 honest surface, module load order, and Wave 2/3 additions |
| Timer | 8 | WaitFor, Cancel, PauseResume, MonotonicTime, MicrosecTime, GameTime, DeltaTime, Ticks |
| Events | 5 | TickSubscribe, TickUnsubscribe, SessionLoaded, OnNextTick, SubscribeOptions |
| Debug | 10 | ReadPtr, ReadU32, ReadI32, ReadFloat, IsValidPointer, ClassifyNull, ClassifySmallInt, Time, Timestamp, SessionAge |
| Types | 6 | GetAllTypes, GetTypeInfo, GetAllLayouts, GetComponentLayout, TypeOf, GenerateIdeHelpers |
| Enums | 3 | DamageType, DamageTypeFire, AbilityId |
| IO | 3 | LoadFile, SaveFile, AddPathOverride |
| Memory | 3 | GetModuleBase, ReadInvalid, Search |
| Mod | 4 | GetLoadOrder, GetBaseMod, IsModLoaded, GetModManager |
| Vars | 2 | Exists, ReloadPersistentVars |
| Osi | 4 | Exists, SafeCall, MetatableExists, IndexReturnsFunction |
| MCM | 10 | ModEventsExists, SubscribeExists, ThrowExists, UnsubscribeExists, EventRoundtrip, RegisterNetListener, NetCreateChannel, PostMessageToServer, OsirisRegisterListener, OsirisNewCall |
| Parity | 24 | Behavioral compatibility assertions, including functor event subscription lifecycles and Audio/Types/Math/Loca surface checks |

### Tier 2: In-Game Tests

95 tests requiring a loaded save (entity access, Osiris queries, physics, pathfinding, and parity contracts).

| Category | Count | What it Tests |
|----------|-------|---------------|
| Entity | 10 | Core entity access plus component write round-trip/refusal guards |
| Level | 5 | IsReady, GetCurrentLevel, GetPhysicsScene, GetAiGrid, GetHeightsAt |
| Audio | 4 | IsReady, GetSoundObjectId, PostEvent, SetState |
| Net | 4 | IsReady, IsHost, Version, PostMessageToServer |
| IMGUI | 2 | IsReady, NewWindow |
| StaticData | 2 | IsReady, GetTypes |
| Osi | 13 | Dispatch (GetHostCharacter, MetatableIndex, IsInCombat, NonexistentSafe, CacheConsistency, GetLevel, GetHitpoints, IsAlive) + edge cases (WrongArgCount, WrongArgType, NilArg, TooManyArgs, LongStringArg) |
| EntityEvents | 5 | SubscribeExists, OnCreateExists, OnDestroyExists, SubscribeReturnsHandle, UnsubscribeWorks |
| Stats | 3 | Goal 2.3 treasure reads and prototype sync honesty |
| Parity | 41 | Loaded-save compatibility: damage event lifecycles, sweep/overlap dispatch, AiGrid pathfinding contracts, CreateComponent/RemoveComponent guards |
| Wave3 | 6 | GetAllEntities walk, GetAllComponents, Serialize roundtrip, Loca update roundtrip, live path lookup dispatch, audio bank dispatch smoke |

---

## Test Categories

### Core (6 tests)
Basic BG3SE runtime verification.
- `Core.Print` — `Ext.Print()` doesn't crash
- `Core.GetVersion` — Returns string matching pattern
- `Core.IsServer` / `Core.IsClient` — Context booleans exist
- `Core.GetContext` — Returns "Server", "Client", or "None"
- `Core.RegisterConsoleCommand` — Function exists

### Json (4 tests)
JSON parsing via `Ext.Json`.
- `Json.Parse` — Parse valid object, verify fields
- `Json.ParseArray` — Parse array, verify length
- `Json.Roundtrip` — Parse → Stringify → Parse
- `Json.ParseInvalid` — Invalid JSON returns nil (not crash)

### Helpers (5 tests)
Global debug helper functions.
- `Helpers.Print` — `_P("test")` doesn't crash
- `Helpers.Hex` — `_H(255)` returns "0xff"
- `Helpers.Dump` / `Helpers.DumpShallow` — `_D()` / `_DS()` on tables
- `Helpers.PrintError` — `_PE()` logs error

### Stats (14 Tier 1 tests)
RPGStats system access via `Ext.Stats`.
- `Stats.Get` — Fetch "WPN_Longsword" stat object
- `Stats.GetName` — Stat name matches input
- `Stats.GetProperty` — Read "Damage" property
- `Stats.GetNonexistent` — Returns nil for bad name
- `Stats.GetAll` — Returns array of all stat names
- `Stats.GetAllFiltered` — Returns filtered by type ("Weapon") — **[SLOW ~4s]**
- `Stats.IsReady` — System initialized
- `Stats.Sync` — Sync function exists and is callable
- `Stats.EnumIndexToLabel` / `EnumLabelToIndex` — Enum conversion
- `Stats.CreateSync` — Create stat + Sync + verify
- `Stats.SetRawAttribute` — Shadow-stat write/read/restore round-trip
- `Stats.Goal23.HonestSurface` — Calls gated mutations and missing treasure lookups, asserting honest results
- `Stats.Goal23.ModuleLoadOrder` — Executes `GetStatsLoadedMods` / `GetStatsLoadedBefore` and checks prefix semantics

Tier 2 adds `Stats.Goal23.TreasureReads` and `Stats.Goal23.PrototypeSyncHonesty`.

### Timer (8 tests)
Timer system via `Ext.Timer`.
- `Timer.WaitFor` / `Cancel` / `PauseResume` — Core timer API
- `Timer.MonotonicTime` / `MicrosecTime` — Timing functions
- `Timer.GameTime` / `DeltaTime` / `Ticks` — Game clock

### Events (5 tests)
Event system via `Ext.Events`.
- `Events.TickSubscribe` / `TickUnsubscribe` — Tick handler lifecycle
- `Events.SessionLoaded` — Session event exists
- `Events.OnNextTick` — Deferred execution
- `Events.SubscribeOptions` — Priority/Once/Prevent

### Debug (10 tests)
Memory introspection via `Ext.Debug`.
- `Debug.ReadPtr` / `ReadU32` / `ReadI32` / `ReadFloat` — Memory reading
- `Debug.IsValidPointer` / `ClassifyNull` / `ClassifySmallInt` — Pointer classification
- `Debug.Time` / `Timestamp` / `SessionAge` — Timing utilities

### Types (6 tests)
Type reflection via `Ext.Types`.
- `Types.GetAllTypes` — Returns ~2050 types
- `Types.GetTypeInfo` — Info for specific type
- `Types.GetAllLayouts` / `GetComponentLayout` — Component layouts
- `Types.TypeOf` — Type identification
- `Types.GenerateIdeHelpers` — VS Code IntelliSense generation

### Enums (3 tests)
Enum/bitfield registration.
- `Enums.DamageType` / `DamageTypeFire` — Enum table access
- `Enums.AbilityId` — Ability enum

### IO (3 tests)
File I/O via `Ext.IO`.
- `IO.LoadFile` / `SaveFile` — File read/write
- `IO.AddPathOverride` — Path redirection

### Memory (3 tests)
Low-level memory access.
- `Memory.GetModuleBase` — Module base address
- `Memory.ReadInvalid` — Invalid address returns nil
- `Memory.Search` — Pattern search function exists

### Mod (4 tests)
Mod management via `Ext.Mod`.
- `Mod.GetLoadOrder` / `GetBaseMod` / `IsModLoaded` / `GetModManager`

### Vars (2 tests)
Variable persistence via `Ext.Vars`.
- `Vars.Exists` — PersistentVars namespace exists
- `Vars.ReloadPersistentVars` — Function callable

### Osi Tier 1 (4 tests)
Osiris metatable structure (no save needed).
- `Osi.Exists` — Osi global exists
- `Osi.SafeCall` — pcall on nonexistent function doesn't crash
- `Osi.MetatableExists` — Osi has metatable with __index
- `Osi.IndexReturnsFunction` — `Osi.GetHostCharacter` is a function

### MCM (10 tests)
Mod Configuration Menu compatibility (Issue #68).
- `MCM.ModEventsExists` — `Ext.ModEvents` namespace exists
- `MCM.SubscribeExists` / `ThrowExists` / `UnsubscribeExists` — Table namespaces present
- `MCM.EventRoundtrip` — Subscribe/Throw/Unsubscribe don't crash
- `MCM.RegisterNetListener` — Function exists
- `MCM.NetCreateChannel` — Optional API check
- `MCM.PostMessageToServer` — Network function
- `MCM.OsirisRegisterListener` / `OsirisNewCall` — Osiris listener API

### Entity (8 tests)
Entity system with loaded save.
- `Entity.ModuleExists` — Ext.Entity namespace
- `Entity.Get` / `GetByHandle` — Entity lookup
- `Entity.HostChar` — `Osi.GetHostCharacter()` → entity (validates Issue #66)
- `Entity.ComponentAccess` — Health component readable
- `Entity.ComponentWrite.HealthHpRoundTrip` — Write/read/restore verified Health HP
- `Entity.ComponentWrite.FixedStringRefused` — FixedString write errors without changing memory
- `Entity.ComponentWrite.OneFrameRefused` — One-frame component write is rejected

### Level (5 tests)
Level/physics access.
- `Level.IsReady` / `GetCurrentLevel` / `GetPhysicsScene` / `GetAiGrid` / `GetHeightsAt`

### Audio (4 tests)
WWise audio engine.
- `Audio.IsReady` / `GetSoundObjectId` / `PostEvent` / `SetState`

### Net (4 tests)
Network messaging.
- `Net.IsReady` / `IsHost` / `Version` / `PostMessageToServer`

### IMGUI (2 tests)
Debug overlay.
- `IMGUI.IsReady` / `NewWindow`

### StaticData (2 tests)
Immutable game data.
- `StaticData.IsReady` / `GetTypes`

### Osi Dispatch (8 tests)
Osiris function dispatch with loaded save (Issue #66).
- `Osi.GetHostCharacter` — Returns valid GUID
- `Osi.MetatableIndex` — __index returns callable
- `Osi.IsInCombat` — 2-arg query (GUID in, int out)
- `Osi.NonexistentSafe` — Nonexistent function doesn't crash
- `Osi.CacheConsistency` — Repeated calls return same value
- `Osi.GetLevel` — Level query (1-20 range)
- `Osi.GetHitpoints` — HP query (positive integer)
- `Osi.IsAlive` — Boolean query (0 or 1)

### Osi Edge Cases (5 tests)
Crash-safety validation for malformed Osiris calls.
- `Osi.WrongArgCount` — Too few args → nil, no crash
- `Osi.WrongArgType` — Wrong type → nil, no crash
- `Osi.NilArg` — Nil argument → no crash
- `Osi.TooManyArgs` — **Regression test**: Extra args clamped to arity (was EXC_BAD_ACCESS at NULL+0xC)
- `Osi.LongStringArg` — 1000-char string doesn't overflow

### Entity Events (5 tests)
Entity event subscription system.
- `EntityEvents.SubscribeExists` / `OnCreateExists` / `OnDestroyExists` — API presence
- `EntityEvents.SubscribeReturnsHandle` — Returns numeric handle
- `EntityEvents.UnsubscribeWorks` — Unsubscribe succeeds

---

## Running Tests

### Console Commands

```lua
!test                    -- Run all 113 Tier 1 tests
!test Stats              -- Filter: only Stats.* tests
!test_ingame             -- Run all 95 Tier 2 tests (needs save)
!test_ingame Osi         -- Filter: only Osi.* Tier 2 tests
!test_ingame EntityEvents -- Filter: Entity Events tests
```

### Filtering

The filter argument is a Lua `string.find` pattern applied to the test name:
- `!test Stats` — matches `Stats.Get`, `Stats.GetAll`, etc.
- `!test_ingame Osi.Get` — matches `Osi.GetHostCharacter`, `Osi.GetLevel`, etc.
- `!test ^Core` — matches tests starting with "Core"

### Automated via Socket

```bash
# One-liner from terminal (Claude can run these directly)
echo '!test' | nc -U /tmp/bg3se.sock

# With timeout
echo '!test' | nc -U -w 30 /tmp/bg3se.sock

# Capture output
echo '!test' | nc -U /tmp/bg3se.sock > /tmp/test_results.txt 2>&1
```

---

## Test Output Format

```
=== BG3SE General Tests (113 tests) ===

--- Stats ---
  PASS: Stats.Get (175ms) [16/113]
  PASS: Stats.GetAllFiltered (4276ms) [SLOW 4276ms] [21/113]
  FAIL: Stats.Example (0ms) - assertion failed: expected X [22/113]

=== Results: 112/113 passed, 1 failed, 0 skipped (9443ms) ===
Failures:
  * Stats.Example: assertion failed: expected X
SOME TESTS FAILED
```

Features:
- **Category headers** (`--- Stats ---`) for visual grouping
- **Per-test timing** in milliseconds
- **Running counter** `[N/total]`
- **[SLOW Xms]** tag for tests exceeding 500ms
- **Failure summary** at end with deduped error list
- **Total elapsed time** for the full suite

---

## Assertion Helpers

Loaded before all tests via `console_cmd_test_assertions`:

```lua
AssertNotNil(v, msg)              -- assert(v ~= nil)
AssertEquals(a, b, msg)           -- assert(a == b)
AssertType(v, t, msg)             -- assert(type(v) == t)
AssertContains(s, pat, msg)       -- assert(s:find(pat))
AssertEqualsFloat(a, b, eps, msg) -- assert(abs(a-b) < eps)
AssertGUID(v, msg)                -- AssertType + length >= 36
```

---

## Writing New Tests

### Adding a Tier 1 Test

1. Find the appropriate string constant in `src/lua/lua_ext.c` (e.g., `console_cmd_test_stats`)
2. Add before the closing `";`:

```c
"BG3SE_AddTest(1, 'Category.TestName', function()\n"
"  -- test code here\n"
"  AssertNotNil(result, 'description')\n"
"end)\n"
```

3. Keep the total string under 4095 chars. If it exceeds, create a new string constant and add it to `console_cmds[]`.

### Adding a Tier 2 Test

Same process, but use `BG3SE_AddTest(2, ...)` and add to a Tier 2 string constant.

### String Constant Naming Convention

| Pattern | Purpose |
|---------|---------|
| `console_cmd_test_core` | Tier 1: Core tests |
| `console_cmd_test_stats` | Tier 1: Stats tests |
| `console_cmd_test_mcm` | Tier 1: MCM tests |
| `console_cmd_test_ingame` | Tier 2: Entity/Level/Audio |
| `console_cmd_test_ingame2` | Tier 2: Net/IMGUI/StaticData |
| `console_cmd_test_osiris` | Tier 2: Osiris dispatch |
| `console_cmd_test_osiris_edge` | Tier 2: Osiris edge cases |
| `console_cmd_test_entity_events` | Tier 2: Entity events |

---

## Init Timing

As of v0.36.50, all initialization phases log elapsed time:

```
[Lua] luaL_openlibs: Xms
[Lua] enum_registry: Xms
[Lua] register_ext_api: Xms
[Lua] entity_register_lua: Xms
[Lua] input_init + overlay: Xms
[Lua] console_cmds (N chunks): Xms
[Lua] Lua 5.4 initialized (Xms total)
[Core] mod_detect_enabled: Xms
[Core] init_lua: Xms
[Core] enumerate_loaded_images: Xms
[Core] check_osiris_library: Xms
[Core] install_hooks: Xms
[Core] === Initialization complete (Xms total) ===
```

To view init timing, check the log after launch:
```bash
grep -E "(ms|initialized|complete)" "/Users/tomdimino/Library/Application Support/BG3SE/bg3se.log" | head -20
```

---

## Troubleshooting

### Tests stall / never complete
- **Cause**: O(n²) algorithm in filtered queries (fixed in v0.36.49)
- **Symptom**: `Stats.GetAllFiltered` blocks forever
- **Fix**: `stats_get_all_names_filtered()` single-pass O(n) collector

### Tests show `[SLOW]` tag
- Expected for `Stats.GetAll` (~1.5s) and `Stats.GetAllFiltered` (~4s)
- These iterate 15,774 stat objects — inherent cost, not a bug
- See Issue #71 for optimization opportunities

### Game crashes during Tier 2 tests
- Check `~/Library/Application Support/BG3SE/crash.log`
- Check `~/Library/Logs/DiagnosticReports/*.ips`
- Osi.TooManyArgs crash was fixed in v0.36.50 (arg clamping)

### Tests report wrong count
- Rebuild: `cd build && cmake --build .`
- Restart game (dylib is loaded at launch)
- Verify build timestamp: `ls -la build/lib/libbg3se.dylib`

---

## Performance Notes

### Slow Tests (>500ms as of v0.36.50)

| Test | Time | Cause |
|------|------|-------|
| Stats.GetNonexistent | ~1.1s | Full 15,774 stat scan, no match |
| Stats.GetAll | ~1.5s | Collect all stat names |
| Stats.GetAllFiltered | ~4s | Filter + type comparison across 15,774 stats |
| Stats.CreateSync | ~1s | Stat creation + sync to engine |

### Test Registration Overhead

All 208 test definitions (113 Tier 1 + 95 Tier 2) are compiled at Lua init via `luaL_dostring()`. This adds ~10-30ms to startup (visible in init timing as `console_cmds`). Tests themselves only execute when `!test` / `!test_ingame` is invoked.
