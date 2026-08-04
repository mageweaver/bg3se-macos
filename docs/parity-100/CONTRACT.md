# Wave 7 Contract Manifest

Per-function inventory of every Windows-registered Lua contract classified
against the macOS port, plus the supplemental dynamic `Osi.DB_*:Delete`
contract. Zero unclassified entries.

- **macOS version:** v0.43.0
- **Windows ref:** `v28.0-34-g1ae33289-dirty`
- **Generated:** 2026-08-03
- **Structured data:** `contract.json` (same directory)

## Classification Key

| Status | Meaning |
|--------|---------|
| implemented | macOS registers the function and it produces real results |
| behavioral_gap | Function is absent, or present as a warn-once stub returning nil/false |
| matched_upstream_todo | macOS matches Windows behavior and the contract leaves the scored denominator; the enum name is historical |
| excluded | Outside the parity denominator by scope decision (see deferrals.md) |

## Scope Exclusions

These four surfaces sit outside the parity denominator. The single authority
is `docs/deferrals.md`.

1. **Ext.UI** (Noesis) -- Windows NsGui surface; macOS has compatibility stub layer only
2. **Lua Debugger / DAP** -- deferred to Phase 11
3. **Virtual Textures** -- no macOS GTS/GTP pipeline; no vetted-corpus caller
4. **Ext.Input synthetic injection** -- macOS uses CGEventTap capture; injection excluded

---

## Ext.Entity

Source: `BG3Extender/Lua/Libs/Entity.inl:RegisterEntityLib()` (context: Both)

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | HandleToUuid | implemented | Behavioral GUID lookup through the captured Uuid-Handle mapping component. Wave 7 A1. |
| 2 | UuidToHandle | implemented | Behavioral reverse lookup through the captured mapping component. Wave 7 A1. |
| 3 | Get | implemented | GUID lookup via entity_system.c |
| 4 | GetAllEntitiesWithUuid | implemented | UUID-filtered archetype walk. Wave 7 A1. |
| 5 | GetAllEntitiesWithComponent | implemented | Server-world archetype walk |
| 6 | GetAllEntities | implemented | Server-world archetype walk |
| 7 | GetEntitiesAroundPosition | implemented | Windows 2D XZ-circle contract; Y ignored; includeCharacters/includeItems default true. Uses CharacterComponent/item-marker archetype walks plus TransformComponent because ARM64 GridStructure is unrecovered. Full behavior met. Wave 7 A1. |
| 8 | Create | behavioral_gap | Absent. Entity allocator + handle mint not recovered. Wave 7 D4. |
| 9 | Destroy | behavioral_gap | Absent. Same allocator gap. Wave 7 D4. |
| 10 | Subscribe | implemented | entity_events.c:1331 |
| 11 | OnChange | implemented | Alias for Subscribe. entity_events.c:1332 |
| 12 | OnCreate | implemented | entity_events.c:1333 |
| 13 | OnCreateDeferred | implemented | entity_events.c:1334 |
| 14 | OnCreateOnce | implemented | entity_events.c:1335 |
| 15 | OnCreateDeferredOnce | implemented | entity_events.c:1336 |
| 16 | OnDestroy | implemented | entity_events.c:1337 |
| 17 | OnDestroyDeferred | implemented | entity_events.c:1338 |
| 18 | OnDestroyOnce | implemented | entity_events.c:1339 |
| 19 | OnDestroyDeferredOnce | implemented | entity_events.c:1340 |
| 20 | OnSystemUpdate | behavioral_gap | Absent pending implementation. B6 recon proved the swappable `SystemTypeEntry::UpdateProc` at +0x18. See `ghidra/offsets/ECS_SYSTEM_UPDATE_RECON.md`. |
| 21 | OnSystemPostUpdate | behavioral_gap | Absent pending implementation. Same implementation-ready B6 dispatch evidence. |
| 22 | Unsubscribe | implemented | entity_events.c:1341 |
| 23 | EnableTracing | behavioral_gap | Warn-once stub returning nil. No tracing infrastructure. |
| 24 | GetTrace | behavioral_gap | Absent. Wave 7 A8. |
| 25 | ClearTrace | behavioral_gap | Absent. Wave 7 A8. |
| 26 | GetRegisteredComponentTypes | implemented | Component-registry enumeration with mapped and oneFrame filters. Wave 7 A1. |

**Subtotal:** 19 implemented, 7 behavioral_gap

---

## Entity Proxy Methods

Source: `BG3Extender/Lua/Shared/Proxies/LuaEntityProxy.inl:StaticInitialize()` (context: Both)

| # | Method | Status | Note |
|---|--------|--------|------|
| 1 | CreateComponent | implemented | Verified ComponentOps registry |
| 2 | RemoveComponent | behavioral_gap | Returns false. 734 per-type templates, no generic entry. |
| 3 | GetComponent | implemented | Via __index. FLOAT_ARRAY writer code is present, but A7 remains non-credited pending ARM64 offset verification. |
| 4 | HasRawComponent | behavioral_gap | Absent. Raw presence check without proxy construction. |
| 5 | GetAllComponents | implemented | |
| 6 | GetAllComponentNames | implemented | |
| 7 | IsAlive | implemented | |
| 8 | GetNetId | implemented | |
| 9 | GetReplicationFlags | behavioral_gap | Warn-once stub returning nil. |
| 10 | SetReplicationFlags | behavioral_gap | Absent. Replication not recovered. |
| 11 | Replicate | behavioral_gap | No-op. Highest-demand deferral (5/11 vetted mods). |
| 12 | OnCreate | behavioral_gap | Absent from entity proxy (exists on Ext.Entity module). |
| 13 | OnCreateDeferred | behavioral_gap | Absent from entity proxy. |
| 14 | OnCreateDeferredOnce | behavioral_gap | Absent from entity proxy. |
| 15 | OnCreateOnce | behavioral_gap | Absent from entity proxy. |
| 16 | OnDestroy | behavioral_gap | Absent from entity proxy. |
| 17 | OnDestroyDeferred | behavioral_gap | Absent from entity proxy. |
| 18 | OnDestroyDeferredOnce | behavioral_gap | Absent from entity proxy. |
| 19 | OnDestroyOnce | behavioral_gap | Absent from entity proxy. |
| 20 | OnChanged | behavioral_gap | Absent. Per-entity component-change subscription. |
| 21 | Vars | implemented | Returns user-variable proxy |

**Subtotal:** 7 implemented, 14 behavioral_gap

---

## Ext.Stats

Source: `BG3Extender/Lua/Libs/Stats.inl:RegisterStatsLib()` (context: Both)

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | GetStatsManager | implemented | |
| 2 | GetModifierAttributes | implemented | |
| 3 | GetStats | implemented | |
| 4 | GetStatsLoadedBefore | implemented | |
| 5 | Get | implemented | Wave 7 A4: Windows also ignores level. Stats.inl:479-508 calls StatFindObject(statName, warnOnError) name-only; the docstring is a DOS2 vestige. |
| 6 | GetCachedSpell | implemented | |
| 7 | GetCachedStatus | implemented | |
| 8 | GetCachedPassive | implemented | Native getter at 0x101c0f27c (Wave 6) |
| 9 | GetCachedInterrupt | implemented | Native getter at 0x101b7adcc (Wave 6) |
| 10 | Create | implemented | |
| 11 | Sync | implemented | |
| 12 | SetPersistence | implemented | |
| 13 | EnumIndexToLabel | implemented | |
| 14 | EnumLabelToIndex | implemented | |
| 15 | AddAttribute | behavioral_gap | Returns false. Modifier-list mutation unverified. |
| 16 | AddEnumerationValue | implemented | Wave 7 B1. Calls engine `ValueList::Insert` at `0x101c44920` with an interned FixedString label, then verifies forward/reverse readback and one count increment. Duplicates and unknown enums return false. See `ghidra/offsets/VALUELIST_INSERT.md`. |
| 17 | ExecuteFunctors | behavioral_gap | Partial. Full param-block unverified for all types. |
| 18 | ExecuteFunctor | implemented | Single-functor dispatch via Dobby hook |
| 19 | PrepareFunctorParams | implemented | |

**Subtotal:** 17 implemented, 2 behavioral_gap

### Ext.Stats.TreasureTable

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | Get | implemented | |
| 2 | GetLegacy | implemented | |
| 3 | Update | implemented | |

### Ext.Stats.TreasureCategory

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | GetLegacy | implemented | |
| 2 | Update | implemented | |

### Stats Proxy Methods

| # | Method | Status | Note |
|---|--------|--------|------|
| 1 | Sync | implemented | |
| 2 | SetPersistence | implemented | |
| 3 | CopyFrom | implemented | |
| 4 | SetRawAttribute | implemented | |

**Stats combined:** 26 implemented, 2 behavioral_gap (across Stats, TreasureTable, TreasureCategory, and proxy)

---

## Ext.Types

Source: `BG3Extender/Lua/Libs/Types.inl:RegisterTypesLib()` (context: Both)

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | GetValueType | implemented | |
| 2 | GetObjectType | implemented | |
| 3 | GetTypeInfo | implemented | |
| 4 | TypeOf | implemented | |
| 5 | IsA | implemented | |
| 6 | GetAllTypes | implemented | |
| 7 | Validate | implemented | |
| 8 | Serialize | implemented | Component-proxy serialization |
| 9 | Unserialize | implemented | |
| 10 | Construct | matched_upstream_todo | Wave 7 A3 exactly matches Windows Types.inl:286-302 errors: `Unknown type name '%s'`, `Unable to construct non-object type '%s'`, and `Type '%s' is not constructible`. Object types reach the shared upstream TODO and return 0 values. Removed from denominator. |
| 11 | GetHashSetValueAt | behavioral_gap | Warn-once stub. No hash-set proxy. Windows index is 1-based. |
| 12 | GetFunctionLocation | implemented | |
| 13 | AddCustomFunction | behavioral_gap | Warn-once stub. Property-map layer missing. |
| 14 | AddCustomProperty | behavioral_gap | Warn-once stub. Same gap. |

**Subtotal:** 10 implemented, 3 behavioral_gap, 1 matched_upstream_todo

---

## Ext.Net

Source: `BG3Extender/Lua/Libs/ClientNet.inl` + `ServerNet.inl` (context: merged Both)

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | PostMessageToServer | implemented | Client context |
| 2 | PostMessageToUser | implemented | Server context |
| 3 | PostMessageToClient | implemented | Server context |
| 4 | BroadcastMessage | implemented | Server context |
| 5 | Version | implemented | |
| 6 | IsHost | implemented | |
| 7 | PlayerHasExtender | implemented | Wave 7 A6: GUID branch resolves via peer_manager_find_by_guid; unknown characters and unassigned users return nil, otherwise returns peer_manager_can_send_extender. Matches ServerNet.inl:78-86. |

**Subtotal:** 7 implemented, 0 behavioral gaps

---

## Osi.DB_* Dynamic Accessor

Source: `OsirisBinding` dynamic database accessor (context: Server)

This supplemental contract is dynamic rather than part of a fixed Windows
registration block.

| # | Method | Status | Note |
|---|--------|--------|------|
| 1 | Delete | implemented | Wave 7 A5. Exact-match deletion via CReteDBase::erase plus ForwardDelToken RETE propagation. `Wave7.Osi.DBDelete` covers arity errors, nil rejection, and no-op invariance; destructive deletion is live-verified manually. |

**Subtotal:** 1 implemented

---

## Ext.Timer

Source: `BG3Extender/Lua/Libs/Timer.inl:RegisterTimerLib()` (context: Both)

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | MonotonicTime | implemented | |
| 2 | MicrosecTime | implemented | |
| 3 | GameTime | implemented | |
| 4 | ClockEpoch | implemented | |
| 5 | ClockTime | implemented | |
| 6 | WaitFor | implemented | |
| 7 | WaitForPersistent | implemented | |
| 8 | WaitForRealtime | implemented | |
| 9 | RegisterPersistentHandler | implemented | |
| 10 | Pause | implemented | |
| 11 | Resume | implemented | |
| 12 | IsPaused | implemented | |
| 13 | Cancel | implemented | |

**Subtotal:** 13 implemented

---

## Ext.Level

Source: `BG3Extender/Lua/Libs/Level.inl:RegisterLevelLib()` (context: Both)

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | GetEntitiesOnTile | implemented | |
| 2 | GetTileDebugInfo | behavioral_gap | Warn-once stub. MinHeight open; flag decoders provisional. |
| 3 | GetHeightsAt | implemented | Wave 7: B3 exposed the old binding as a stub; the real multi-subgrid walk (Ai.inl:262/406 contract) landed the same day over CONFIRMED subgrid world-bounds offsets (AIGRID_PATHFINDING.md); live verify pending |
| 4 | BeginPathfinding | behavioral_gap | Warn-once stub. Incomplete field population. |
| 5 | BeginPathfindingImmediate | behavioral_gap | Absent. Same underlying gap. |
| 6 | FindPath | implemented | |
| 7 | ReleasePath | implemented | |
| 8 | GetPathById | implemented | |
| 9 | GetActivePathfindingRequests | implemented | |
| 10 | RaycastClosest | behavioral_gap | Warn-once stub. By-value ls::Function ABI unverified. |
| 11 | RaycastAny | behavioral_gap | Warn-once stub. By-value optional lock ABI. |
| 12 | RaycastAll | behavioral_gap | Warn-once stub. Same ABI gap. |
| 13 | SweepSphereClosest | implemented | |
| 14 | SweepCapsuleClosest | implemented | |
| 15 | SweepBoxClosest | implemented | |
| 16 | SweepSphereAll | implemented | |
| 17 | SweepCapsuleAll | implemented | |
| 18 | SweepBoxAll | implemented | |
| 19 | TestBox | implemented | |
| 20 | TestSphere | implemented | |

**Subtotal:** 14 implemented, 6 behavioral_gap

---

## Ext.Audio

Source: `BG3Extender/Lua/Libs/ClientAudio.inl:RegisterAudioLib()` (context: Client)

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | SetSwitch | implemented | |
| 2 | SetState | implemented | |
| 3 | SetRTPC | implemented | |
| 4 | GetRTPC | implemented | |
| 5 | ResetRTPC | implemented | |
| 6 | Stop | implemented | |
| 7 | PauseAllSounds | implemented | |
| 8 | ResumeAllSounds | implemented | |
| 9 | PostEvent | implemented | |
| 10 | LoadEvent | implemented | |
| 11 | UnloadEvent | implemented | |
| 12 | PlayExternalSound | implemented | STDString ABI |
| 13 | LoadBank | implemented | dlsym'd AK::SoundEngine |
| 14 | UnloadBank | implemented | |
| 15 | PrepareBank | implemented | |
| 16 | UnprepareBank | implemented | |

**Subtotal:** 16 implemented

---

## Ext.Mod

Source: `BG3Extender/Lua/Libs/Mod.inl:RegisterModLib()` (context: Both)

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | IsModLoaded | implemented | |
| 2 | GetLoadOrder | implemented | |
| 3 | GetMod | implemented | |
| 4 | GetBaseMod | implemented | |
| 5 | GetModManager | implemented | |

**Subtotal:** 5 implemented

---

## Ext.Vars

Source: `BG3Extender/Lua/Libs/Vars.inl:RegisterVarsLib()` (context: Both)

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | RegisterUserVariable | implemented | src/vars/user_variables.c:754 (Phase 0 adjudication corrected a wrong "absent" classification) |
| 2 | SyncUserVariables | implemented | src/vars/user_variables.c:846 |
| 3 | DirtyUserVariables | implemented | src/vars/user_variables.c:855 |
| 4 | GetEntitiesWithVariable | implemented | src/vars/user_variables.c:837 |
| 5 | RegisterModVariable | implemented | src/vars/user_variables.c:1360 |
| 6 | GetModVariables | implemented | src/vars/user_variables.c:1402 |
| 7 | SyncModVariables | implemented | src/vars/user_variables.c:1417 |
| 8 | DirtyModVariables | implemented | src/vars/user_variables.c:1426 |

macOS has `SyncPersistentVars`, `IsPersistentVarsLoaded`, `ReloadPersistentVars`,
`MarkDirty` -- a local persistence layer that does not match the Windows variable
schema registration model.

**Subtotal:** 8 implemented (cross-VM synchronization semantics ride Wave 7 Phase E2.5)

---

## Ext.IO

Source: `BG3Extender/Lua/Libs/IO.inl:RegisterIOLib()` (context: Both)

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | LoadFile | implemented | |
| 2 | SaveFile | implemented | |
| 3 | AddPathOverride | implemented | |
| 4 | GetPathOverride | implemented | |

**Subtotal:** 4 implemented

---

## Ext.Json

Source: `BG3Extender/Lua/Libs/Json.inl:RegisterJsonLib()` (context: Both)

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | Parse | implemented | |
| 2 | Stringify | implemented | |

**Subtotal:** 2 implemented

---

## Ext.Math

Source: `BG3Extender/Lua/Libs/Math.inl:RegisterMathLib()` (context: Both)

All 59 functions implemented: Add, Sub, Mul, Div, Reflect, Angle, Cross,
Distance, Dot, Length, Normalize, Perpendicular, Project, Determinant, Inverse,
Transpose, OuterProduct, Rotate, Translate, Scale, ExtractEulerAngles,
BuildFromEulerAngles3, BuildFromEulerAngles4, Decompose, ExtractAxisAngle,
BuildFromAxisAngle3, BuildFromAxisAngle4, BuildRotation3, BuildRotation4,
BuildTranslation, BuildScale, QuatFromEuler, QuatFromToRotation, QuatDot,
QuatSlerp, QuatToMat3, QuatToMat4, Mat3ToQuat, Mat4ToQuat, QuatNormalize,
QuatInverse, QuatRotate, QuatRotateAxisAngle, QuatLength, QuatMul, Random,
Round, Fract, Trunc, Sign, Clamp, Smoothstep, Lerp, Asin, Acos, Atan, Atan2,
IsNaN, IsInf.

**Subtotal:** 59 implemented

---

## Ext.Debug

Source: `BG3Extender/Lua/Libs/Debug.inl:RegisterDebugLib()` (context: Both)

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | DumpStack | behavioral_gap | Absent. Lua stack introspection. |
| 2 | DebugDumpLifetimes | behavioral_gap | Absent. Lifetime tracking dump. |
| 3 | GenerateIdeHelpers | implemented | Registered under Ext.Types. |
| 4 | DebugBreak | behavioral_gap | Absent. DAP integration excluded by scope. |
| 5 | IsDeveloperMode | implemented | src/lua/lua_debug.c:942 (MCM compat, Issue #68; Phase 0 adjudication) |
| 6 | SetEntityRuntimeCheckLevel | behavioral_gap | Absent. Entity validation level. |
| 7 | Crash | behavioral_gap | Absent. Intentional crash trigger. |
| 8 | Reset | implemented | src/lua/lua_debug.c:945 (Phase 0 adjudication) |

macOS has its own Ext.Debug with 23 functions (ReadPtr, ProbeStruct,
HexDump, etc.) that have no Windows counterpart -- these are macOS-only
diagnostic tools and are not counted in the contract.

**Subtotal:** 3 implemented, 5 behavioral_gap

---

## Ext.Utils

Source: `BG3Extender/Lua/Libs/Utils.inl:RegisterUtilsLib()` (context: Both)

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | Version | implemented | main.c:1023, aliased |
| 2 | GameVersion | behavioral_gap | Absent. Engine build string. |
| 3 | Include | behavioral_gap | Absent. Mod-script include facility. |
| 4 | LoadString | behavioral_gap | Absent. Lua chunk loader. |
| 5 | GetValueType | implemented | Via Ext.Types |
| 6 | IsValidHandle | behavioral_gap | Absent. Handle validation. |
| 7 | HandleToInteger | behavioral_gap | Absent. Handle serialization. |
| 8 | IntegerToHandle | behavioral_gap | Absent. Handle deserialization. |
| 9 | ShowErrorAndExitGame | behavioral_gap | Absent. Error dialog. |
| 10 | ShowError | behavioral_gap | Absent. |
| 11 | GetGlobalSwitches | behavioral_gap | Absent. GlobalSwitches struct not recovered. |
| 12 | GetCommandLineParams | behavioral_gap | Absent. |
| 13 | GetDialogManager | behavioral_gap | Absent. Dialog system access. |
| 14 | GetGameState | implemented | main.c:1025, aliased |
| 15 | GetMemoryUsage | behavioral_gap | Absent. Lua allocator tracking. |
| 16 | ProfileBegin | behavioral_gap | Absent. Optick profiler (Windows-only). |
| 17 | ProfileEnd | behavioral_gap | Absent. Same. |

**Subtotal:** 3 implemented, 14 behavioral_gap

---

## Ext.Log

Source: `BG3Extender/Lua/Libs/Log.inl:RegisterLogLib()` (context: Both)

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | Print | implemented | |
| 2 | PrintError | implemented | |
| 3 | PrintWarning | implemented | |

**Subtotal:** 3 implemented

---

## Ext.Loca

Source: `BG3Extender/Lua/Libs/Localization.inl:RegisterLocalizationLib()` (context: Both)

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | GetTranslatedString | implemented | Live-verified round trip |
| 2 | UpdateTranslatedString | implemented | Live-verified |

**Subtotal:** 2 implemented

---

## Ext.StaticData

Source: `BG3Extender/Lua/Libs/StaticData.inl:RegisterStaticDataLib()` (context: Both)

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | Get | implemented | |
| 2 | GetAll | implemented | |
| 3 | GetSources | behavioral_gap | Absent. Resource-source enumeration. |
| 4 | GetByModId | behavioral_gap | Absent. Per-mod GUID resource filter. |
| 5 | Create | behavioral_gap | Absent. GUID resource creation. |

**Subtotal:** 2 implemented, 3 behavioral_gap

---

## Ext.Resource

Source: `BG3Extender/Lua/Libs/StaticData.inl:RegisterResourceLib()` (context: Both)

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | Get | implemented | |
| 2 | GetAll | implemented | |

**Subtotal:** 2 implemented

---

## Ext.Table

Source: `BG3Extender/Lua/Libs/Table.inl:RegisterTableLib()` (context: Both)

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | Find | behavioral_gap | Absent. Lua table search utility. |

**Subtotal:** 0 implemented, 1 behavioral_gap

---

## Ext.IMGUI

Source: `BG3Extender/Lua/Libs/ClientIMGUI.inl:RegisterIMGUILib()` (context: Client)

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | NewWindow | implemented | |
| 2 | EnableDemo | behavioral_gap | Absent. Demo window toggle. |
| 3 | LoadFont | behavioral_gap | Absent. Custom font loading. |
| 4 | SetScale | behavioral_gap | Absent. Global UI scale. |
| 5 | SetUIScaleMultiplier | behavioral_gap | Absent. |
| 6 | SetFontScaleMultiplier | behavioral_gap | Absent. |
| 7 | GetViewportSize | implemented | |

**Subtotal:** 2 implemented, 5 behavioral_gap

---

## Ext.ClientInput (EXCLUDED)

Source: `BG3Extender/Lua/Libs/ClientInput.inl` (context: Client)

All 4 functions excluded by scope decision (Ext.Input synthetic injection):
InjectKeyPress, InjectKeyDown, InjectKeyUp, GetInputManager.

**Subtotal:** 4 excluded

---

## Ext.ClientTemplate

Source: `BG3Extender/Lua/Libs/ClientTemplate.inl` (context: Client)

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | GetTemplate | implemented | Unified Get covers client + server |
| 2 | GetAllRootTemplates | behavioral_gap | Absent |
| 3 | GetRootTemplate | behavioral_gap | Absent |

**Subtotal:** 1 implemented, 2 behavioral_gap

---

## Ext.ServerTemplate

Source: `BG3Extender/Lua/Libs/ServerTemplate.inl` (context: Server)

| # | Function | Status | Note |
|---|----------|--------|------|
| 1 | GetTemplate | implemented | Unified Get |
| 2 | GetAllRootTemplates | behavioral_gap | Absent |
| 3 | GetRootTemplate | behavioral_gap | Absent |
| 4 | GetAllLocalTemplates | behavioral_gap | Absent |
| 5 | GetLocalTemplate | behavioral_gap | Absent |
| 6 | GetAllCacheTemplates | behavioral_gap | Absent |
| 7 | GetCacheTemplate | behavioral_gap | Absent |
| 8 | GetAllLocalCacheTemplates | behavioral_gap | Absent |
| 9 | GetLocalCacheTemplate | behavioral_gap | Absent |

**Subtotal:** 1 implemented, 8 behavioral_gap

---

## Ext.ClientUI (EXCLUDED)

Source: `BG3Extender/Lua/Libs/ClientUI/Module.inl` (context: Client)

All 9 functions excluded by scope decision (Ext.UI / Noesis):
GetRoot, GetStateMachine, SetState, RegisterType, Instantiate,
GetPickingHelper, GetCursorControl, GetDragDrop, EnableErrorReporting.

**Subtotal:** 9 excluded

---

## Events Inventory (best-effort)

Source: Windows `EventDeclarations.h` / `EventDefinitions.h`; macOS `lua_events.c` + `functor_hooks.c`.

All 35 macOS-registered events are implemented:
SessionLoading, SessionLoaded, ModuleLoading, ModuleLoadStarted,
ModuleResume, StatsLoaded, StatsStructureLoaded, GameStateChanged, Tick,
ResetCompleted, LevelGameplayStarted, LevelUnloading, TurnStarted, TurnEnded,
CombatStarted, CombatEnded, CombatRoundStarted, TeleportedToCamp,
ReturnedFromCamp, DialogStarted, DialogEnded, ShortRest, LongRest,
TradeStarted, TradeEnded, GainedControl, LostControl, DownedChanged,
DyingChanged, StatusApplied, StatusRemoved, NetMessage, Log,
BeforeDealDamage, DealDamage.

The Windows event surface is larger (40+ events including input, visual,
camera, and UI events that depend on excluded subsystems). A full Windows
event enumeration is best-effort and not attempted here -- the 35 events
above are the ones macOS registers and fires.

---

## Summary

| Classification | Count |
|----------------|-------|
| implemented | 210 |
| behavioral_gap | 70 |
| matched_upstream_todo | 1 |
| excluded | 13 |
| **Total contracts** | **294** |

### Per-namespace breakdown

| Namespace | Impl | Gap | Todo | Excl | Total |
|-----------|------|-----|------|------|-------|
| Ext.Entity | 19 | 7 | 0 | 0 | 26 |
| entity_proxy | 7 | 14 | 0 | 0 | 21 |
| Ext.Stats | 17 | 2 | 0 | 0 | 19 |
| Stats.TreasureTable | 3 | 0 | 0 | 0 | 3 |
| Stats.TreasureCategory | 2 | 0 | 0 | 0 | 2 |
| stats_proxy | 4 | 0 | 0 | 0 | 4 |
| Ext.Types | 10 | 3 | 1 | 0 | 14 |
| Ext.Net | 7 | 0 | 0 | 0 | 7 |
| Osi.DB_* | 1 | 0 | 0 | 0 | 1 |
| Ext.Timer | 13 | 0 | 0 | 0 | 13 |
| Ext.Level | 14 | 6 | 0 | 0 | 20 |
| Ext.Audio | 16 | 0 | 0 | 0 | 16 |
| Ext.Mod | 5 | 0 | 0 | 0 | 5 |
| Ext.Vars | 8 | 0 | 0 | 0 | 8 |
| Ext.IO | 4 | 0 | 0 | 0 | 4 |
| Ext.Json | 2 | 0 | 0 | 0 | 2 |
| Ext.Math | 59 | 0 | 0 | 0 | 59 |
| Ext.Debug | 3 | 5 | 0 | 0 | 8 |
| Ext.Utils | 3 | 14 | 0 | 0 | 17 |
| Ext.Log | 3 | 0 | 0 | 0 | 3 |
| Ext.Loca | 2 | 0 | 0 | 0 | 2 |
| Ext.StaticData | 2 | 3 | 0 | 0 | 5 |
| Ext.Resource | 2 | 0 | 0 | 0 | 2 |
| Ext.Table | 0 | 1 | 0 | 0 | 1 |
| Ext.IMGUI | 2 | 5 | 0 | 0 | 7 |
| Ext.ClientInput | 0 | 0 | 0 | 4 | 4 |
| Ext.ClientTemplate | 1 | 2 | 0 | 0 | 3 |
| Ext.ServerTemplate | 1 | 8 | 0 | 0 | 9 |
| Ext.ClientUI | 0 | 0 | 0 | 9 | 9 |

### Incomplete inventories

- **Events:** Best-effort. The 35 macOS events are inventoried; the Windows
  event surface is larger (input, visual, camera, UI events gated on excluded
  subsystems) and not exhaustively enumerated.
- **Ext.Osiris:** Dynamic metatable (`__index` / `__newindex`); no fixed
  registration block to inventory. Osi.* dispatch, RegisterListener,
  NewCall/NewQuery/NewEvent, and DB_* Get are implemented. Wave 7 A5's
  DB_* Delete method is included above as a supplemental dynamic contract;
  the remaining dynamic surface is not enumerable as individual contracts.
