# Lua API Surface E2E Test Analysis

*Codex GPT-5.5 researcher review — 2026-05-03*

## API Surface Audit

| Namespace | Audit Finding |
|---|---|
| `Ext.Utils` | macOS has compatibility aliases in `src/injector/main.c`, but Windows exposes broader `Utils` functions: `Version`, `GameVersion`, `Include`, `LoadString`, handle conversion, command-line params, memory usage, profiling. Current tests only cover core `Ext.*`, not `Ext.Utils.*`. |
| `Ext.Math` | Broad coverage exists, but tests only check `Smoothstep` and `IsNaN`. Need vector, matrix, quaternion, scalar, NaN/Inf edge tests. Windows has `Mat3ToQuat`/`Mat4ToQuat`; macOS exposes canonical aliases. |
| `Ext.Loca` / Localization | macOS registers `GetTranslatedString`, `UpdateTranslatedString`, `GetLanguage`, `IsReady`, `DumpInfo`, `CreateHandle`. Existing tests only check handle creation. Need update/get roundtrip and invalid handle behavior. |
| `Ext.Stats` | "100% parity" is not defensible. `AddAttribute`, `AddEnumerationValue`, `GetStatsLoadedBefore`, `TreasureTable.*`, `TreasureCategory.*`, and `ExecuteFunctors` are stubs or partial. Need parity tests that fail on stubs. |
| `Ext.Entity` | macOS lacks Windows module functions such as `HandleToUuid`, `UuidToHandle`, `GetAllEntitiesWithUuid`, `GetAllEntities`, `GetEntitiesAroundPosition`, `Create`, `Destroy`, system update subscriptions, tracing APIs, registered component type list. `CreateComponent`/`RemoveComponent` are claimed in docs/changelog but not registered on entity userdata. |
| `Ext.Events` | macOS registers 44 event objects, more than docs mention. Existing tests are mostly subscription smoke tests. Need actual fire tests for priority, once, prevent, log, net, one-frame component bridge, and event payload shape. |
| `Ext.Timer` | macOS exposes persistent timer APIs, but current tests do not validate persistence export/import, repeated timers, realtime vs game-time behavior, invalid handles, or callbacks receiving handle arguments. |
| `Ext.Level` | macOS has raycast/sweep/test physics functions but misses Windows pathfinding/tile APIs: `GetEntitiesOnTile`, `GetTileDebugInfo`, `BeginPathfinding`, `FindPath`, `ReleasePath`, etc. Current tests mostly check readiness/presence. |
| `Ext.Audio` | API count exceeds claimed 13 and matches Windows client audio names well. Gaps are ABI/argument fidelity, especially `PlayExternalSound`, sound object name vs ID handling, invalid banks/events, and return booleans. |
| `Ext.Net` | macOS local message bus differs from Windows multiplayer backend. Need request/reply callback ordering, binary payload flag, module/channel fields, `PeerVersion`, `PlayerHasExtender`, and delivery through `Ext.Events.NetModMessage`/`NetMessage`. |
| `Ext.IMGUI` | macOS implements many widgets, but tests only create a window. Need widget property mutation, callbacks, flags, nested containers, table/tab/popup/menu controls, destruction, and viewport/input capture behavior. |
| `Ext.Osiris` | macOS registers `RegisterListener`, `NewCall`, `NewQuery`, `NewEvent`, `GetCustomFunctions`, `RaiseEvent`. Need before/after/delete listener behavior, custom query return values, wrong arity/type behavior, and registered function introspection. |
| `Ext.Types` | macOS exposes 15 functions, but `Construct`, `AddCustomFunction`, `AddCustomProperty` are explicitly unsupported stubs. `Serialize`/`Unserialize` appear in parity tests but are not registered in `lua_ext.c`. |
| `Ext.StaticData` | macOS exposes many capture/debug helpers but Windows parity is `Get`, `GetAll`, `GetSources`, `GetByModId`, `Create`. macOS does not expose `GetSources`, `GetByModId`, or `Create`; tests only cover readiness/types. |
| `Ext.Mod` | Registered names match Windows, but return structure parity is shallow. Need field presence/types for load order entries, base mod UUID/name, invalid UUID handling, and `GetModManager` semantics. |
| `Ext.Debug` | macOS debug API is different from Windows. Windows has `DumpStack`, `DebugDumpLifetimes`, `GenerateIdeHelpers`, `DebugBreak`, `SetEntityRuntimeCheckLevel`, `Crash`; macOS adds memory probes and mod health diagnostics. Treat as compatibility, not parity. |

## Coverage Gaps (Highest Risk)

- **Stats**: stubs claimed as parity, raw property index bounds, stat copy/template fidelity, enum invalid names, `GetStatsLoadedBefore`, treasure tables, functor execution.
- **Entity**: invalid GUID/handle, handle bit math, component enumeration, component names, missing `CreateComponent`/`RemoveComponent`, entity event fire behavior.
- **Events**: actual dispatch ordering, `Once`, preventable events, unsubscribe during dispatch, handler errors, payload fields for every event.
- **Timer**: callback execution, cancellation before fire, repeat timers, persistent export/import, handler unregister, invalid handle returns.
- **Level**: argument table shape `{1,2,3}` vs `{x,y,z}`, raycast result field shape, sweep result arrays, invalid physics masks.
- **Net**: request/reply callback lifecycle, module and channel fidelity, local bus vs RakNet divergence, binary payload.
- **IMGUI**: widget methods and event callback data.
- **StaticData**: all 9 supported types, missing Windows `GetSources`/`GetByModId`/`Create`.
- **Types**: unsupported stubs and missing serialize/unserialize.

## Key Insight: Presence Tests Are Not Parity Tests

**The most important correction is to stop treating presence tests as parity tests.** A function existing but returning a stub value is currently enough to pass several tests. The parity suite should assert semantic behavior and should intentionally fail on documented stubs such as `Ext.Stats.AddAttribute`, `Ext.Stats.GetStatsLoadedBefore`, `Ext.Types.Construct`, and missing `Ext.Types.Serialize`.

## Proposed Parity Regression Tests

```lua
BG3SE_AddTest(1, "Parity.Stats.StubDetection", function()
  AssertType(Ext.Stats.AddAttribute, "function", "AddAttribute")
  local ok = Ext.Stats.AddAttribute("Weapon", "BG3SE_TestAttr", "String")
  assert(ok == true, "Windows parity expects AddAttribute to succeed before stats load; macOS currently returns false")
end)

BG3SE_AddTest(1, "Parity.Stats.LoadedBefore", function()
  local base = Ext.Mod.GetBaseMod()
  local uuid = base and base.UUID or "00000000-0000-0000-0000-000000000000"
  local rows = Ext.Stats.GetStatsLoadedBefore(uuid, "Weapon")
  AssertType(rows, "table", "GetStatsLoadedBefore result")
  assert(#rows > 0, "Expected non-empty stats loaded before base mod")
end)

BG3SE_AddTest(1, "Parity.Types.SerializeRoundtrip", function()
  AssertType(Ext.Types.Serialize, "function", "Types.Serialize")
  AssertType(Ext.Types.Unserialize, "function", "Types.Unserialize")
  local s = Ext.Types.Serialize({a=1, b="x"})
  local t = Ext.Types.Unserialize(s)
  assert(t.a == 1 and t.b == "x", "Serialize/Unserialize roundtrip mismatch")
end)

BG3SE_AddTest(2, "Parity.Entity.HandleRoundtrip", function()
  local guid = Osi.GetHostCharacter()
  local e = Ext.Entity.Get(guid)
  AssertNotNil(e, "host entity")
  local h = e:GetHandle()
  local e2 = Ext.Entity.GetByHandle(h)
  AssertNotNil(e2, "entity by handle")
  assert(e2:GetHandle() == h, "GetByHandle handle mismatch")
end)

BG3SE_AddTest(2, "Parity.Entity.InvalidInputs", function()
  assert(Ext.Entity.Get("not-a-guid") == nil, "invalid GUID should return nil")
  assert(Ext.Entity.GetByHandle("0x0") == nil, "zero handle should return nil")
  local ok = pcall(function() return Ext.Entity.GetByHandle({}) end)
  assert(ok == false, "table handle should error, not crash")
end)

BG3SE_AddTest(2, "Parity.Entity.ComponentEnumeration", function()
  local e = Ext.Entity.Get(Osi.GetHostCharacter())
  local names = e:GetAllComponentNames()
  AssertType(names, "table", "component names")
  assert(#names > 0, "host should have components")
  assert(e:GetComponent("Health") ~= nil or e.Health ~= nil, "Health component expected")
end)

BG3SE_AddTest(1, "Parity.Events.PriorityOncePrevent", function()
  local order = {}
  local a = Ext.Events.DoConsoleCommand:Subscribe(function(e) order[#order+1] = "low" end, {Priority=200})
  local b = Ext.Events.DoConsoleCommand:Subscribe(function(e) order[#order+1] = "high"; e.Prevent = true end, {Priority=1, Once=true})
  Ext.Events.DoConsoleCommand:Unsubscribe(a)
  Ext.Events.DoConsoleCommand:Unsubscribe(b)
  assert(type(a) == "number" and type(b) == "number", "handler IDs must be numeric")
end)

BG3SE_AddTest(1, "Parity.Timer.PersistentExportImport", function()
  local fired = false
  Ext.Timer.RegisterPersistentHandler("BG3SE_TestPersistent", function(args, handle) fired = true end)
  local h = Ext.Timer.WaitForPersistent(5000, "BG3SE_TestPersistent", '{"x":1}')
  local dump = Ext.Timer.ExportPersistent()
  AssertType(dump, "string", "persistent export")
  Ext.Timer.CancelPersistent(h)
  Ext.Timer.ImportPersistent(dump)
  Ext.Timer.UnregisterPersistentHandler("BG3SE_TestPersistent")
end)

BG3SE_AddTest(2, "Parity.Level.RaycastShape", function()
  local src, dst = {0, 10, 0}, {0, -10, 0}
  local ok, hit = pcall(Ext.Level.RaycastClosest, src, dst)
  assert(ok, "RaycastClosest should not error with array vec3")
  if hit then
    AssertType(hit.Position, "table", "Position")
    AssertType(hit.Normal, "table", "Normal")
    AssertType(hit.Distance, "number", "Distance")
  end
end)

BG3SE_AddTest(1, "Parity.Audio.ExternalSoundInvalid", function()
  AssertType(Ext.Audio.PlayExternalSound, "function", "PlayExternalSound")
  local ok, ret = pcall(Ext.Audio.PlayExternalSound, "BG3SE_InvalidObj", "BG3SE_InvalidEvent", "missing.ogg", 1, 0)
  assert(ok, "PlayExternalSound invalid input should not crash")
  AssertType(ret, "boolean", "return")
end)

BG3SE_AddTest(1, "Parity.Net.RequestReplyLocal", function()
  local got = false
  Ext.Events.NetModMessage:Subscribe(function(e)
    if e.Channel == "BG3SE_TestReq" and e.RequestId ~= 0 then
      Ext.Net.PostMessageToServer("BG3SE_TestReply", "{}", e.Module, nil, e.RequestId)
    end
  end, {Once=true})
  Ext.Net.PostMessageToServer("BG3SE_TestReq", "{}", "BG3SE_Test", function(payload) got = true end)
end)

BG3SE_AddTest(2, "Parity.IMGUI.WidgetSurface", function()
  local w = Ext.IMGUI.NewWindow("BG3SE Test")
  AssertNotNil(w, "window")
  AssertType(w.AddText, "function", "AddText")
  AssertType(w.AddButton, "function", "AddButton")
  local btn = w:AddButton("OK")
  AssertNotNil(btn, "button")
  btn:SetVisible(false)
  btn:SetVisible(true)
  w:Destroy()
end)
```

## Mod Compatibility Test Patterns

- **MCM**: create IMGUI window with tabs, checkboxes, sliders, input text; register `Ext.ModEvents` save event; send `Ext.Net` message with request/reply; persist config through `Ext.Vars`.
- **CommunityLib**: `Stats.Get` and stat property reads, `Ext.Entity.Get(Osi.GetHostCharacter())`, `Ext.Osiris.RegisterListener`, `Ext.Events.SessionLoaded` and `StatsLoaded` subscriptions.
- **5eSpells**: enumerate spell stats, get static data types/resources, validate spell prototype cache via `GetCachedSpell`, modify a copied spell stat and sync.
- **CombatExtender**: read host entity stats/components, write persistent vars, build IMGUI controls, query combat Osiris functions.
- **PartyLimitBegone**: `Ext.Entity.GetAllEntitiesWithComponent("PartyMember")`, Osiris party DB/query calls, event listener for session/character changes.

## Evidence

- `docs/testing.md:57` — current test framework and tier split.
- `src/lua/lua_ext.c:1308` — embedded Tier 1/Tier 2 tests.
- `src/lua/lua_stats.c:1188` — macOS `Ext.Stats` registration.
- `src/entity/entity_system.c:2573` — macOS `Ext.Entity` registration.
- `src/entity/entity_events.c:1331` — `Ext.Entity` event registration.
- `src/lua/lua_events.c:79` — registered `Ext.Events` names.
- `src/lua/lua_level.c:491` — macOS `Ext.Level` registration.
- `src/lua/lua_audio.c:267` — macOS `Ext.Audio` registration.
- `src/lua/lua_net.c:319` — macOS `Ext.Net` registration.
- `src/lua/lua_imgui.c:424` — IMGUI widget methods.
