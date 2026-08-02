# Deferral Registry

The canonical list of every Ext.* API that exists on macOS but intentionally
does not perform its Windows behavior. Every entry fails closed: it warns once
per process and returns `nil`/`false` without touching native code it cannot
defend. A deferral is never a silent stub — if an API is listed here, calling
it tells you so in the log.

**Doctrine:** a warn-once stub returning `nil`/`false` with a documented
rationale beats a guessed offset. Deferrals convert to implementations only
when instruction-level evidence (a `ghidra/offsets/` report) proves the
layout and ABI; they are never promoted by porting Windows constants.

Last audited: 2026-08-01 (Wave 6 truth pass + Wave 7 Phase 0 contract diff, v0.42.0, game build 4.1.1.7209685).

## Ext.Level

| API | Returns | Why deferred | Evidence | Unlock path |
|---|---|---|---|---|
| `RaycastClosest` | `nil` | Engine signature ends in a **by-value** `ls::Function<bool(PhysicsShape const*)>`; its C representation, construction, destruction, and ownership are unverified. Quarantined — the previous "working" binding dispatched `RemovePhysicsShape` (wrong-by-1 VMT index). | `ghidra/offsets/PHYSICS_VMT_AUDIT.md` | Prove the `ls::Function` value layout + lifecycle, or find a callable overload without it |
| `RaycastAll` | `nil` | Trailing by-value `ls::Optional<PhysicsSceneScopedReadLock&>`; value ABI unverified. | same | same |
| `RaycastAny` | `false` | Same unverified by-value optional lock. | same | same |
| `GetTileDebugInfo` | `nil` | `AiGridTile::MinHeight` (+0x0a) is OPEN; public cloud-surface enum conversion and material/extra flag decoders are PROVISIONAL. Raw flags, ground/cloud masks, max height, and metadata indices ARE recovered — a reduced raw-flags diagnostic is implementable if needed. | `ghidra/offsets/AIGRID_PATHFINDING.md` | Confirm the min-height load and the remaining public flag conversions |
| `BeginPathfinding` | `nil` | `AiGrid::CreatePath` is callable, but character path creation populates many `AiPath` fields beyond source/target/bounds; calling only `CreatePath` would enqueue an incompletely configured request. | same | Map the full `CreatePathForCharacter` field population (client `0x102de439c` + server callers) or find a stable high-level entry point |

Implemented for contrast: `GetPathById`, `ReleasePath`,
`GetActivePathfindingRequests`, `FindPath`, `GetEntitiesOnTile` (Wave 3,
verified live), all sweeps including cylinders, `TestBox`, `TestSphere`,
`GetHeightsAt`, singleton accessors.

## Ext.Entity / entity methods

| API | Returns | Why deferred | Evidence | Unlock path |
|---|---|---|---|---|
| `entity:RemoveComponent(name)` | `false` | macOS emits only 734 `ImmediateWorldCache::RemoveComponent<T>(EntityHandle)` template instantiations, each with a hard-coded `TypeId<T>`; there is no generic runtime-TypeId entry point. Calling a specialization for a different type would remove the wrong component. | `ghidra/offsets/COMPONENT_OPS_AND_PROTO_INIT.md` ("NOT GENERICALLY UNLOCKED") | Generate a per-build type-index→specialization dispatch table, or audit a generic reimplementation of the template body (pending-change handling + destroy callbacks) |
| `Ext.Entity.EnableTracing` / `DisableTracing` / `GetReplicationFlags` | warn + `nil` | No macOS tracing/replication-flag infrastructure recovered. Contract notes: current Windows BG3SE exposes `EnableTracing(bool)` only — `DisableTracing` is a macOS compat wrapper outside the Windows surface — and `GetReplicationFlags` lives on the **entity proxy** upstream, not on `Ext.Entity`; any implementation must match those placements. | `src/injector/main.c` | RE the server replication bookkeeping |
| `entity:Replicate()` | no-op | Same replication gap. **Highest-demand deferral in the registry**: 5 of 11 vetted mods call it (Community Library, 5e Spells, Expansion, Combat Extender, Transmog Enhanced) — masked in single-player, unproven in multiplayer. Cannot be reclassified as a scope exclusion. | same | same |
| Component property writes (unknown-size layouts, unsupported field types) | `false` | Writing through an unverified size or field type risks corruption; only INT32, UINT8, BOOL, FLOAT, INT32_ARRAY are proven. | `src/entity/component_property.c` | Verify additional layouts per component |
| `HandleToUuid` / `UuidToHandle` | absent | Missing Windows registrations surfaced by the Wave 7 Phase 0 per-function contract diff (Entity.inl:291-325). The Uuid↔Handle mapping component is already captured on macOS — these are portable engineering, not RE. | `bg3se/BG3Extender/Lua/Libs/Entity.inl:296-297`; `src/entity/guid_lookup.h` | Wave 7 Phase A1 |
| `GetAllEntitiesWithUuid` / `GetEntitiesAroundPosition` / `GetRegisteredComponentTypes` | absent | Same contract-diff finding: filters/dumps over machinery that already exists (archetype walks, component registry). | `Entity.inl:299,303,324` | Wave 7 Phase A1 |
| `Ext.Entity.Create` / `Destroy` | absent | Engine entity lifecycle (allocator + handle mint) not recovered on macOS. | `Entity.inl:305-306` | Wave 7 Phase D4 (recon first, go/no-go on allocator evidence) |
| `OnSystemUpdate` / `OnSystemPostUpdate` | absent | ECS system-update phase dispatch not located on macOS. | `Entity.inl:319-320` | Wave 7 Phase B6 recon |
| `GetTrace` / `ClearTrace` | absent | Tracing infrastructure not built; faithful Windows tracing scans command-buffer changes and replication dirties, so full credit is gated on the replication foundation. | `Entity.inl:322-323` | Wave 7 Phase A8 prototype → C-gated closure |

Implemented for contrast: `entity:CreateComponent` (verified ComponentOps
registry at `EntityWorld+0x390`, vptr slot 5), `GetAllEntities`,
`GetAllEntitiesWithComponent`, `GetAllComponents` (server-world archetype
walks). Note: client-world components (`ecl::*`) are not enumerated by the
walk; `esv::Character` is not a registered TypeId — use
`eoc::character::CharacterComponent` for character queries.

## Ext.Stats

| API | Returns | Why deferred | Evidence | Unlock path |
|---|---|---|---|---|
| Passive prototype sync | `false` | `eoc::PassivePrototype` (0x210 bytes) has **no top-level vptr** — the status path's VMT-copy trick would corrupt it — and its per-stat field population is inlined into the `DoLoadStats` loader lambda (no callable `Init`). Singleton is `eoc::Passives::m_ptr` at `0x1089bc228`. | `ghidra/offsets/COMPONENT_OPS_AND_PROTO_INIT.md` | Map the post-`Clean()` population region after `0x103062d00`, or find a narrower callable helper |
| Interrupt prototype sync | `false` | `eoc::InterruptPrototype` (0x1f0 bytes) has a `FixedString` at offset 0, not a vptr; the manager uses a hash table + contiguous 0x1f0-stride array, structurally incompatible with the generic RefMap insert helper. | same | Map the inlined object move/build path at `0x103063f94` onward |
| `AddAttribute` / `AddEnumerationValue` | `false` | Runtime modifier-list mutation paths unverified. | `src/stats/stats_manager.c` | RE the modifier-list growth path |
| `ExecuteFunctors` | partial | Full param-block construction unverified for all functor types. | `src/stats/functor_hooks.c` | Extend per-functor param evidence |

## Ext.Types

| API | Returns | Why deferred | Evidence | Unlock path |
|---|---|---|---|---|
| `Construct(typeName)` | warn + `nil` | **Matched contract, not a gap**: the Windows reference is itself unimplemented — `Types.inl:286` is `// TODO; return 0`, so it validates the name and returns nothing, exactly as we do. Removed from the parity denominator in the Wave 6 re-baseline. Arbitrary construction stays out of scope until upstream implements it. | `src/lua/lua_ext.c` | Track upstream; per-type constructor recovery if it ever lands (see the PassivePrototype ctor precedent) |
| `GetHashSetValueAt` | warn + `nil` | No hash-set proxy exists on macOS. Contract note: the Windows index is **1-based** (Lua convention); any implementation must preserve that. | same | Implement an ls::HashSet layout proxy |
| `AddCustomFunction` | warn + `false` | **Scored gap** (Wave 7 Phase 0 correction — a prior entry wrongly claimed this sits "outside the 15-function Windows baseline"; the 15 was manufactured). Windows registers it inside its 14-function Types block and implements it *functionally* as Lua-side custom-property registration (`GetCustomProperties().RegisterProperty`, `Types.inl:328`) — no engine call involved. macOS lacks the property-map layer the proxies would consult. | `bg3se/BG3Extender/Lua/Libs/Types.inl:328` | Wave 7 Phase A2: per-state custom-property registry consulted by proxy `__index`/`__newindex` |
| `AddCustomProperty` | warn + `false` | Same correction as AddCustomFunction — functional on Windows (`Types.inl:347`), scored gap here. | `bg3se/BG3Extender/Lua/Libs/Types.inl:347` | Same (Wave 7 Phase A2) |

## Excluded by scope decision (not deferrals)

These four surfaces sit outside the parity denominator. This list is now the
single authority — ROADMAP.md defers to it (Wave 6 reconciled a silent
disagreement where ROADMAP assumed exclusions this registry never named).

- **Ext.UI** — Windows NsGui/Noesis surface; compatibility stub layer only.
- **Lua Debugger / DAP** — deferred to Phase 11.
- **Virtual Textures** — no macOS GTS/GTP pipeline; no vetted-corpus caller.
  Explicit exclusion as of Wave 6 (previously assumed silently).
- **Input injection** (`Ext.Input` synthetic event injection à la Windows) —
  the macOS surface is CGEventTap capture + hotkeys; injection is excluded.
  Explicit exclusion as of Wave 6 (previously assumed silently).

**Deliberately NOT excludable: entity replication.** `entity:Replicate()` has
proven mod demand (5 of 11 vetted mods) and therefore stays a scored deferral
inside the `Ext.Entity` row, not a scope exclusion.

## History

- 2026-08-01 (Wave 6): scope-exclusion list reconciled with ROADMAP (Virtual
  Textures + input injection made explicit; replication ruled non-excludable).
  Construct reclassified as a matched Windows contract (upstream `// TODO`)
  and removed from the parity denominator. Contract notes added for
  GetHashSetValueAt (1-based), GetReplicationFlags (entity-proxy placement),
  and DisableTracing (macOS compat wrapper). GetCachedPassive/GetCachedInterrupt
  rebound from the layout-wrong generic refmap walk to the native getters
  (`eoc::Passives::Get` 0x101c0f27c — LTO arg-promoted, index by value;
  `InterruptPrototypeManager::GetPrototype` 0x101b7adcc), live-verified.
- 2026-07-30 (v0.41.0): registry created (Wave 3 Goal 3.3). Five AiGrid APIs,
  cylinder sweeps, CreateComponent, Math ×2, Audio banks ×4, Types
  Serialize/Unserialize, and Loca update left the list; the three raycasts
  entered it (previously miscounted as implemented while dispatching the
  wrong VMT slots).
