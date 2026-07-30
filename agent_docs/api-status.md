# API Status (v0.41.0)

Full namespace-by-namespace parity status with Windows BG3SE.

Overall parity is approximately 97.3% across the supported macOS surface, sourced from the [ROADMAP.md matrix](../ROADMAP.md#feature-parity-matrix). Every intentionally fail-closed API is cataloged in [docs/deferrals.md](../docs/deferrals.md).

- **Osi.*** - Dynamic metatable (40+ functions), **OsirisFunctionHandle encoding** (v0.36.39), crash-resilient dispatch with breadcrumbs
- **Ext.Osiris** - RegisterListener, NewCall/NewQuery/NewEvent (server context guards)
- **Context System** - Ext.IsServer/IsClient/GetContext, two-phase bootstrap (v0.36.4)
- **Ext.Entity** - GUID lookup, **1,999 components registered** (462 layouts: 169 verified + 293 generated), **1,730 sizes** (1,577 Ghidra + 153 Windows-only, 87% coverage), GetByHandle, **Dual EntityWorld Complete** (client + server auto-captured), **Entity Events** (Subscribe/OnCreate/OnDestroy + 8 variants, Unsubscribe — salted pool, deferred queue, per-entity hooks), **CreateComponent** (verified ComponentOps registry at EntityWorld+0x390, vptr slot 5), **GetAllEntities/GetAllEntitiesWithComponent/GetAllComponents** (server-world archetype walks; live: 916 characters via `eoc::character::CharacterComponent`)[^entity-stubs]
- **Ext.Stats** - **100% Windows API function-count parity** (52 functions): Get/GetAll/Create/Sync, CopyFrom, SetRawAttribute, ExecuteFunctors, TreasureTable/TreasureCategory, all StatsObject methods[^stats-stubs]
- **Ext.Events** - 33 events with priority ordering, Once flag, Prevent pattern (13 lifecycle + 17 engine + 2 functor + 1 network events), **runtime mod attribution** (per-handler mod tracking, soft-disable, health stats)
- **Ext.Timer** - **20 functions**: WaitFor, WaitForRealtime, Cancel/Pause/Resume, GameTime/DeltaTime/Ticks, **Persistent timers** (save/load support)
- **Ext.Vars** - PersistentVars, User Variables, Mod Variables
- **Ext.StaticData** - Immutable game data (**All 9 types**: Feat, Race, Background, Origin, God, Class, Progression, ActionResource, FeatDescription via ForceCapture)
- **Ext.Resource** - Non-GUID resources (34 types: Visual, Material, Texture, Dialog, etc.)
- **Ext.Template** - Game object templates (14 functions, 10 properties, type detection via VMT)
- **Ext.Types** - **13/15 (86.7%)**: GetAllTypes (~2050), GetTypeInfo, GetObjectType, TypeOf, IsA, Validate, GetComponentLayout, GetAllLayouts, **GenerateIdeHelpers** (VS Code IntelliSense), **GetValueType**, **GetFunctionLocation**, and **Serialize/Unserialize** (component-proxy); Construct and GetHashSetValueAt are deferrals, and the macOS-only AddCustomFunction/AddCustomProperty compatibility stubs warn and return false (docs/deferrals.md)
- **Ext.Debug** - Memory introspection (ReadPtr, ProbeStruct, HexDump), **mod diagnostics** (ModHealthCount, ModHealthAll, ModDisable), **observability APIs** (GetHookStatus, GetVersionStatus, GetCacheStats, GetEventStatus, GetManagerStatus)
- **Ext.IMGUI** - **Complete widget system** (40 widget types): NewWindow, AddText, AddButton, AddCheckbox, AddInputText, AddCombo, AddSlider, AddColorEdit, AddProgressBar, AddTree, AddTable, AddTabBar, AddMenu, handle-based objects, event callbacks (OnClick, OnChange, OnClose, OnExpand, OnCollapse)
- **Ext.Mod** - Mod information (5 functions): IsModLoaded, GetLoadOrder, GetMod, GetBaseMod, GetModManager
- **Ext.Level** - **20/25 (80%)**: TestBox, TestSphere, GetHeightsAt, GetCurrentLevel, GetPhysicsScene, GetAiGrid, IsReady, all 8 sweeps (incl. **SweepCylinderClosest/All** at proven VMT 14/18), **GetEntitiesOnTile**, and the pathfinding suite (**GetPathById, ReleasePath, GetActivePathfindingRequests, FindPath** — verified AiGrid/AiPath/PathMap layouts, `-1337` sentinel rejection, copied-state-only returns); physics dispatch repaired against the audited macOS vtable (9 wrong-by-1 indices fixed, `ghidra/offsets/PHYSICS_VMT_AUDIT.md`); 5 deferrals: RaycastClosest/All/Any (quarantined by-value C++ params), GetTileDebugInfo, BeginPathfinding (docs/deferrals.md)
- **Ext.Audio** - **17/17 (100%)**: PostEvent, Stop, PauseAllSounds, ResumeAllSounds, SetSwitch, SetState, SetRTPC, GetRTPC, ResetRTPC, LoadEvent, UnloadEvent, GetSoundObjectId, IsReady, PlayExternalSound (STDString ABI), and **LoadBank/UnloadBank/PrepareBank/UnprepareBank** (dlsym'd AK::SoundEngine exports)
- **Ext.Net** - Network messaging (8 functions): PostMessageToServer, PostMessageToUser, PostMessageToClient, BroadcastMessage, Version, IsHost, IsReady, PeerVersion, **Request/Reply Callbacks**, **RakNet Backend** (Phase 4I)
- **Ext.RegisterNetListener** - Per-channel network message listener (MCM backbone)
- **Net.CreateChannel** - High-level NetChannel API for multiplayer mod sync (SetHandler, **SetRequestHandler**, SendToServer, **RequestToServer with callbacks**, SendToClient, Broadcast)
- **Ext.Utils** - Compatibility aliases (6 functions): Print, PrintWarning, PrintError, Version, MonotonicTime, GetGameState
- **Ext.Math** - **59/59 (100%)**: vector, matrix, quaternion, and scalar utilities, including Random, Fract, **Smoothstep**, and **IsNaN**
- **Ext.Localization** - Localization API (4 functions): GetLanguage, CreateHandle, **GetTranslatedString**, **UpdateTranslatedString** (live-verified round trip; update verified against the returned pool handle)
- **Ext.ModEvents** - Per-mod cross-mod event system: Subscribe, Throw, Unsubscribe (MCM compat)
- **Osi.DB_*** - Generic database query accessor (e.g. `Osi.DB_Players:Get()`, `Osi.DB_IsTag:Get()`)
- **Entity** - Extended entity API: **CreateComponent**, **RemoveComponent**, **GetEntityType**, **GetSalt**, **GetIndex**, **GetNetId**
- **Events** - **ExecuteFunctor** Dobby hook, BeforeDealDamage/DealDamage fire live (build 7209685, verified 51/51 paired)
- **Version Detection** - Sentinel address probes for game version mismatch tolerance (Issue #78)

[^stats-stubs]: Remaining gaps behind function-count parity: AddAttribute and AddEnumerationValue return false; ExecuteFunctors is partial; passive and interrupt prototype sync honestly return false (their build-7209685 loader population paths are inlined/unmapped, and neither prototype has a top-level vptr, `src/stats/prototype_managers.c`; evidence in `ghidra/offsets/COMPONENT_OPS_AND_PROTO_INIT.md`). TreasureTable/TreasureCategory reads, GetStatsLoadedMods, and spell/status prototype sync return real data (Wave 2).
[^entity-stubs]: EnableTracing, DisableTracing, and GetReplicationFlags are warn-and-nil stubs (`src/injector/main.c`); `entity:Replicate()` is a no-op. GetAllEntities, GetAllEntitiesWithComponent, and GetAllComponents are real server-world archetype walks (Wave 3). `entity:CreateComponent` dispatches through the verified ComponentOps registry; `entity:RemoveComponent` returns false (734 per-type templates, no generic entry point — `ghidra/offsets/COMPONENT_OPS_AND_PROTO_INIT.md`). Component property reads work; writes are real for INT32, UINT8, BOOL, FLOAT, and INT32_ARRAY fields and are refused (return false) for unknown-size layouts and unsupported field types (`src/entity/component_property.c`).
