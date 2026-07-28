# E2E Test Strategy: BG3SE-macOS Parity Validation

*Synthesized from 3 parallel Codex GPT-5.5 researcher reviews — 2026-05-03*

## Executive Summary

Three independent GPT-5.5 researchers audited the entire BG3SE-macOS codebase from different angles: core C systems, Lua API surface, and Python harness pipeline. Their findings converge on a single structural critique: **the current 125-test suite validates presence, not behavior**. Functions exist but return stubs; tests pass but parity claims are indefensible. The fix is a 4-tier test architecture that starts below the game process and ends with live mod vetting.

## The 4-Tier Test Architecture

| Tier | Name | Runner | BG3 Required | Tests | Runtime |
|------|------|--------|-------------|-------|---------|
| **0** | Native C unit | Any (CI-safe) | No | ~30 | <5s |
| **1** | Lua console | Game running | Console only | 85 + ~15 new | <30s |
| **2** | Lua in-game | Loaded save | Yes | 40 + ~15 new | <60s |
| **H** | Harness pytest | macOS | Optional | ~20 new | <30s |

### Tier 0: Native C Unit Tests (NEW — highest ROI)

**All three reviewers independently recommended this.** A standalone C test binary linked against core modules, no game process required. Covers:

- `safe_memory_read` edge cases (null, GPU carveout, cross-page strings, ProbeStruct bounds)
- `version_detect` plist parsing, sentinel probe logic, version gate behavior
- Pattern scanning (`parse_pattern`, `find_pattern`)
- Osiris handle encode/decode roundtrips (including high-bit IDs)
- Entity event handle pack/unpack
- Component layout golden snapshots (registry count, property defs)
- ARM64 instruction encoding helpers

This tier runs in CI on Linux and macOS without any BG3 dependency. It catches memory-safety regressions in the most critical code paths.

### Tier 1+2: New Parity Tests (Lua)

Both the core and API reviewers proposed concrete Lua tests. Combined unique proposals:

**Tier 1 (console-only, 8 new tests):**
- `Parity.Debug.SafeMemoryEdges` — null reads, ProbeStruct on invalid base
- `Parity.Events.FunctorSubscribePair` — subscribe+unsubscribe ExecuteFunctor/AfterExecuteFunctor
- `Parity.Events.PriorityOncePrevent` — Priority, Once, Prevent flags on DoConsoleCommand
- `Parity.Stats.StubDetection` — AddAttribute should succeed (not stub-return false)
- `Parity.Stats.LoadedBefore` — GetStatsLoadedBefore returns data
- `Parity.Types.SerializeRoundtrip` — Serialize/Unserialize (currently unregistered)
- `Parity.Timer.PersistentExportImport` — persistent timer export/import/cancel
- `Parity.Audio.ExternalSoundInvalid` — PlayExternalSound with bad args doesn't crash

**Tier 2 (needs save, 7 new tests):**
- `Parity.Entity.TypeIdDiscoveryComplete` — DiscoverTypeIds returns complete=true, count >1500
- `Parity.Entity.RegistryCounts` — esv::Character count is sane (>0, <10000)
- `Parity.Entity.HostRoundtrip` — GetHostCharacter -> Get -> tostring roundtrip
- `Parity.Entity.HealthLayoutSnapshot` — GetComponentLayout has Hitpoints property
- `Parity.Entity.HandleRoundtrip` — Get -> GetHandle -> GetByHandle identity
- `Parity.Entity.InvalidInputs` — bad GUID/handle returns nil or errors, never crashes
- `Parity.Entity.ComponentEnumeration` — GetAllComponentNames returns non-empty, Health present
- `Parity.Stats.CanonicalCounts` — total >10k, weapons >100
- `Parity.Stats.LongswordShape` — WPN_Longsword Name/Type/Damage properties
- `Parity.Osi.DBPlayersAccessor` — DB_Players:Get() returns rows
- `Parity.Osi.ListenerBeforeAfter` — RegisterListener before/after both return handles
- `Parity.Level.RaycastShape` — RaycastClosest returns Position/Normal/Distance
- `Parity.IMGUI.WidgetSurface` — NewWindow, AddText, AddButton, SetVisible, Destroy
- `Parity.Net.RequestReplyLocal` — PostMessageToServer with callback

### Tier H: Harness pytest Suite (NEW)

The pipeline reviewer proposed 14 concrete pytest cases covering:

- **Test output parsing**: `[SLOW Nms]` token, missing summary, empty output
- **Process lifecycle**: socket reports process exit (not generic timeout)
- **Headless mode**: hide_window called only after socket, never on failure
- **Build pipeline**: CMake failure, non-universal binary detection
- **Mod management**: name-to-UUID resolution for `mod enable`
- **Save management**: restore should backup existing state
- **Compat scanning**: log scan should ignore stale lines

## Critical Parity Gaps Identified

### "100% Stats Parity" Is Not Defensible

The API reviewer found that `Ext.Stats` claims 100% parity but multiple functions are stubs:
- `AddAttribute` — returns false instead of succeeding
- `AddEnumerationValue` — stub
- `GetStatsLoadedBefore` — stub or missing
- `TreasureTable.*` / `TreasureCategory.*` — stubs
- `ExecuteFunctors` — partial (functor hooks version-gated)

**Fix:** Add `Parity.Stats.StubDetection` tests that intentionally fail on stubs, making the parity gap visible.

### Entity Namespace Missing Key Functions

macOS lacks: `HandleToUuid`, `UuidToHandle`, `GetAllEntitiesWithUuid`, `GetAllEntities`, `GetEntitiesAroundPosition`, `Create`, `Destroy`. `CreateComponent`/`RemoveComponent` are claimed in docs but not registered.

### Types Namespace Has Unregistered Functions

`Ext.Types.Serialize`/`Unserialize` appear in parity discussions but aren't registered in `lua_ext.c`. `Construct`, `AddCustomFunction`, `AddCustomProperty` are explicit stubs.

### Presence Tests Masquerade as Parity Tests

The most important structural issue: `type(Ext.X.Y) == 'function'` passes for stubs. The test suite needs to assert **semantic behavior** — call the function and validate the return shape, not just check that it exists.

## Version-Gated Test Strategy

The version mismatch (HF#35 -> HF#36) revealed that functor hooks are silently disabled on mismatch. Tests need to cover this:

1. **Version match**: all hooks fire, all APIs functional
2. **Version mismatch, sentinels pass**: data reads work, code hooks skip
3. **Version mismatch, sentinels fail**: all address-dependent systems disabled

Proposed: `Ext.Debug.GetHookStatus()` and `Ext.Debug.GetVersionStatus()` APIs to make C-layer state observable from Lua.

## New Debug APIs for Testability

All three reviews converge on needing C-layer observability from Lua:

| API | Purpose |
|-----|---------|
| `Ext.Debug.GetHookStatus()` | Hook count, which hooks fired, version gate |
| `Ext.Debug.GetVersionStatus()` | Match/mismatch, sentinel results |
| `Ext.Osiris.GetCacheStats()` | Function cache size, hit/miss |
| `Ext.Entity.GetEventStatus()` | Subscriber count, pool state |
| `Ext.Stats.GetManagerStatus()` | Stat count, prototype readiness |

## CI/CD Implementation Plan

### Phase 1: Offline-Fast (GitHub Actions, Linux + macOS)

- Tier 0 C unit tests
- Tier H pytest harness tests
- No BG3, no Accessibility, no Metal
- Target: <30s, runs on every push

### Phase 2: macOS-Integration (GitHub Actions macOS)

- Same as Phase 1 plus macOS path handling
- AppleScript/CGEvent command construction (mocked)
- Target: <1min

### Phase 3: Local-Live (Developer machine)

- Tier 1+2 Lua tests via socket
- Build/patch/launch pipeline
- Gated by `BG3SE_LIVE=1`
- Target: <5min

### Phase 4: Full E2E (Self-hosted macOS runner)

- All tiers + compat matrix + mod vetting
- Requires: Accessibility permission, BG3 installed, save fixtures, mod PAKs
- Target: <15min

## File Index

| File | Scope |
|------|-------|
| [core-systems-analysis.md](core-systems-analysis.md) | C layer gaps, ARM64/PAC, version detection, safe memory |
| [lua-api-analysis.md](lua-api-analysis.md) | 14 Ext.* namespaces, stub detection, mod compat patterns |
| [harness-pipeline-analysis.md](harness-pipeline-analysis.md) | Python CLI, mock architecture, CI tiers, pytest cases |
