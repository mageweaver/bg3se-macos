# Core C/C++ Systems E2E Test Analysis

*Codex GPT-5.5 researcher review — 2026-05-03*

## Gap Analysis

Major untested areas:

- **Injection/init**: no E2E assertion that `RegisterDIVFunctions` fired, that `g_divCall/g_divQuery` are non-null, that hook count is 4/4, or that fallback paths behave safely when `BG3SE_NO_HOOKS=1`.
- **Version safety**: no tests for exact match, mismatch with sentinel pass, mismatch with sentinel fail, or `BG3SE_FORCE_ADDRESSES=1`. `version_detect_addresses_safe()` gates entity/stats/staticdata/functor hooks but is not exposed to Lua.
- **Crash diagnostics**: no test that `crashlog_init()`, mmap ring buffer creation, breadcrumbs, signal/Mach exception paths, or crash file pre-open work.
- **Safe memory**: current tests cover invalid zero reads only. Missing GPU carveout rejection, cross-page string read behavior, partial string reads, write-protection behavior, and `ProbeStruct` bounds capping.
- **Osiris**: existing tests cover common queries, but not cache size, function handle encoding, high-bit IDs, DB accessor behavior, hash collisions, output slot type drift, event before/after ordering, or `DivCall` fail-closed behavior.
- **Entity**: current tests do not validate `DiscoverTypeIds().complete`, component registry count around 1999, component index sanity, dual server/client worlds, GUID cache roundtrip, entity lifetime expiry, `GetAllEntitiesWithComponent`, CCR bind state, or ARM64 signal handler dispatch.
- **Hooks/events**: functor hook tests only check API presence. No test causes `ExecuteFunctor` and `AfterExecuteFunctor` to fire, no ordering check, no context-type coverage, no version-mismatch skip check.
- **Stats**: tests do not verify manager object count invariants, modifier list counts, property count sanity, raw attribute roundtrip on shadow stats, prototype manager readiness, spell/status sync paths, or stat type detection beyond simple objects.

## Parity Test Proposals

### Entity
- Verify `DiscoverTypeIds()` returns `complete=true` and `count` near the generated registry count.
- Assert `Ext.Types.GetComponentLayout("eoc::HealthComponent")` has expected offsets for `Hitpoints`/`MaxHitpoints`.
- Compare `Osi.GetHostCharacter()` -> `Ext.Entity.Get()` -> handle -> `Ext.Entity.GetByHandle()` roundtrip.
- Assert `CountEntitiesWithComponent("esv::Character") > 0`, and that all returned userdata stringify as entities.
- Add a controlled create/destroy E2E using a spawned temporary item/status if a stable in-game action exists.

### Stats
- Snapshot counts: total stats > 10k, `Weapon` > 100, `SpellData` > 1000.
- Validate canonical stat properties: `WPN_Longsword.Name`, `Type == "Weapon"`, `PropertyCount > 0`, `Damage` non-empty.
- Shadow stat copy/set/sync roundtrip with unique test name.
- Prototype manager sync smoke for spell/status/passive types if exposed.

### Osiris
- `RegisterListener` before/after ordering with an event that can be deterministically triggered.
- DB accessor: `Osi.DB_Players:Get()` returns rows after load and accepts wildcard filters.
- Multi-output query coverage for string/int/float outputs.
- Harness-level test that `BG3SE_NO_HOOKS=1` makes Osi calls fail nil, not crash.

### Events/Hooks
- Subscribe to `Ext.Events.ExecuteFunctor` and `AfterExecuteFunctor`, perform a deterministic action, assert before count equals after count and before fires first.
- Version mismatch harness test should assert functor hooks are skipped even when sentinel data probes pass.

### ARM64/PAC
- Unit-test `osi_encode_handle`/decode with known Windows handles.
- Unit-test ARM64 signal trampoline ABI: `EntityRef` handle in x1, world in x2, component in x3.
- Harness crash-test PAC failure path in a sacrificial process, assert breadcrumbs/ring file are written.

## Architecture Recommendations

**Add Tier 0.** This should be a native C test binary linked against core modules, no game process. Cover `safe_memory`, `version_detect` plist parsing, pattern scanning, Osiris handle encode/decode, function-cache hashing, entity event handle pack/unpack, component layout snapshots, and ARM64 instruction encoding.

**Add property/fuzz tests** for `safe_memory_read_string`, `parse_pattern`, `find_pattern`, `ProbeStruct` range/stride handling, Osiris argument conversion, and component property array readers. These are memory-safety-critical and should not depend on BG3.

**Add golden snapshots:**
- Component layouts from `component_offsets.h`/generated property defs.
- Expected component registry names/count.
- Stats manager offsets and canonical stat property values.
- Known Osiris function names/arity/type/handle for a representative set.

**Add version-bump detection:**
- Harness launches with a fake/mismatched `Info.plist` or injected env toggle and asserts address-dependent systems skip closed.
- Separate "sentinel pass but version mismatch" test asserts data reads can run but code patching, especially functor hooks, remains disabled.

## Concrete Lua Tests

```lua
BG3SE_AddTest(2, 'Parity.Entity.TypeIdDiscoveryComplete', function()
  local r = Ext.Entity.DiscoverTypeIds()
  AssertType(r, 'table', 'DiscoverTypeIds result')
  assert(r.count and r.count > 1500, 'expected substantial component registry discovery')
  assert(r.complete == true, 'TypeId discovery should be complete in loaded save')
end)

BG3SE_AddTest(2, 'Parity.Entity.RegistryCounts', function()
  local n = Ext.Entity.CountEntitiesWithComponent('esv::Character')
  AssertType(n, 'number', 'character component count')
  assert(n > 0 and n < 10000, 'expected sane esv::Character count')
end)

BG3SE_AddTest(2, 'Parity.Entity.HostRoundtrip', function()
  local guid = Osi.GetHostCharacter()
  AssertGUID(guid, 'host guid')
  local e = Ext.Entity.Get(guid)
  AssertNotNil(e, 'host entity')
  local s = tostring(e)
  AssertContains(s, 'Entity', 'entity tostring')
end)

BG3SE_AddTest(2, 'Parity.Entity.HealthLayoutSnapshot', function()
  local layout = Ext.Types.GetComponentLayout('eoc::HealthComponent') or Ext.Types.GetComponentLayout('Health')
  AssertType(layout, 'table', 'health layout')
  local found = false
  for _, p in pairs(layout.Properties or layout.properties or {}) do
    if p.Name == 'Hitpoints' or p.name == 'Hitpoints' then found = true end
  end
  assert(found, 'Health layout should expose Hitpoints')
end)

BG3SE_AddTest(1, 'Parity.Debug.SafeMemoryEdges', function()
  assert(Ext.Debug.ReadString(0, 64) == nil, 'ReadString null')
  assert(Ext.Debug.ReadU64(0) == nil, 'ReadU64 null')
  local r = Ext.Debug.ProbeStruct(0, 0, 0x20000, 8)
  AssertType(r, 'table', 'ProbeStruct invalid base returns table')
end)

BG3SE_AddTest(2, 'Parity.Stats.CanonicalCounts', function()
  local all = Ext.Stats.GetAll()
  AssertType(all, 'table', 'all stats')
  assert(#all > 10000, 'expected >10k stats')
  local weapons = Ext.Stats.GetAll('Weapon')
  AssertType(weapons, 'table', 'weapon stats')
  assert(#weapons > 100, 'expected many weapons')
end)

BG3SE_AddTest(2, 'Parity.Stats.LongswordShape', function()
  local s = Ext.Stats.Get('WPN_Longsword')
  AssertNotNil(s, 'WPN_Longsword')
  AssertEquals(s.Name, 'WPN_Longsword', 'stat name')
  AssertEquals(s.Type, 'Weapon', 'stat type')
  assert((s.PropertyCount or 0) > 0, 'property count')
  assert(s:GetProperty('Damage') ~= nil, 'Damage property should resolve')
end)

BG3SE_AddTest(2, 'Parity.Osi.DBPlayersAccessor', function()
  local db = Osi.DB_Players
  AssertType(db, 'table', 'DB_Players accessor')
  AssertType(db.Get, 'function', 'DB_Players:Get')
  local rows = db:Get()
  AssertType(rows, 'table', 'DB_Players rows')
end)

BG3SE_AddTest(2, 'Parity.Osi.ListenerBeforeAfter', function()
  local before, after = 0, 0
  local hb = Ext.Osiris.RegisterListener('TurnStarted', 1, 'before', function(_) before = before + 1 end)
  local ha = Ext.Osiris.RegisterListener('TurnStarted', 1, 'after', function(_) after = after + 1 end)
  AssertNotNil(hb, 'before listener handle')
  AssertNotNil(ha, 'after listener handle')
end)

BG3SE_AddTest(1, 'Parity.Events.FunctorSubscribePair', function()
  local b = Ext.Events.ExecuteFunctor:Subscribe(function() end)
  local a = Ext.Events.AfterExecuteFunctor:Subscribe(function() end)
  AssertNotNil(b, 'ExecuteFunctor handle')
  AssertNotNil(a, 'AfterExecuteFunctor handle')
  Ext.Events.ExecuteFunctor:Unsubscribe(b)
  Ext.Events.AfterExecuteFunctor:Unsubscribe(a)
end)
```

## New Debug APIs for Testability

Add small debug-only Lua APIs:
- `Ext.Debug.GetHookStatus()` — hook count, which hooks fired, version gate state
- `Ext.Debug.GetVersionStatus()` — version match/mismatch, sentinel probe results
- `Ext.Osiris.GetCacheStats()` — function cache size, hit/miss ratio
- `Ext.Entity.GetEventStatus()` — subscriber count, pending events, pool state
- `Ext.Stats.GetManagerStatus()` — stat count, prototype readiness, modifier lists

Without these, the Lua E2E layer can detect user-visible behavior but cannot assert the most important C-layer invariants directly.

## Evidence

- `docs/testing.md:52` — existing suite is 125 Lua tests, Tier 1/Tier 2 only.
- `src/injector/main.c:2053` — Lua init/register order.
- `src/injector/main.c:2367` — `RegisterDIVFunctions` hook captures `DivFunctions::Call/Query`.
- `src/injector/main.c:3104` — Dobby hook installation.
- `src/core/version_detect.c:236` — version/address gate and sentinel probes.
- `src/core/safe_memory.c:70` — safe read implementation and GPU carveout guard.
- `src/osiris/osiris_functions.c:351` — function cache/probe path.
- `src/entity/entity_events.c:377` — ARM64 by-value `EntityRef` signal ABI wrapper.
- `src/stats/functor_hooks.c:184` — 9 functor Dobby hooks.
- `src/stats/stats_manager.c:142` — fragile RPGStats offsets.
