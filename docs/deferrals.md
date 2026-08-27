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

Last audited: 2026-08-18 (v0.44.0, game build 4.1.1.7398727, arm64 LC_UUID
`0C51CAED-6D60-3DCD-9299-8519C92631B0` — first live-verified session on this build:
154/154 offset-audit checks and Tier 1 112/114 passed).

**Partial corrections 2026-08-27** (three entries re-examined against
4.1.1.7398727; NOT a full re-audit — the 2026-08-18 sweep above still stands as
the last complete pass):

- `Ext.Stats` / `ExecuteFunctors` — stated rationale was wrong; corrected, with a
  design in `plans/2026-08-27-001-feat-ext-stats-executefunctors-plan.md`.
- `Ext.Stats` / `AddAttribute` — rationale re-verified on this build (its stub
  comment cited 7209685); still blocked, only the build reference was stale.
- `Ext.UI` scope exclusion — "stub layer only" no longer true; read-only tree
  surface is real and live-verified.
- `Ext.Entity.EnableTracing`/`DisableTracing` and `GetTrace`/`ClearTrace` —
  both rows said the infrastructure did not exist; the Wave 7 A8 work they each
  named as their own unlock path had already shipped. Live-verified.

**Counting note.** Four rows moved out of "deferred" on 2026-08-27 by
correction rather than by new work — they were already implemented and the
registry had not caught up. Anyone recomputing parity from this file should
re-derive the denominator rather than trust a prior count.

**Test-suite caveat (2026-08-27).** Tier 2 reported 7 failures against 2 real
defects. Five tests asserted contracts that were superseded (RaycastAll's
deferral, tracing's `Events`→`Entities` rename) or never existed on this build
(`Osi.IsAlive`, whose funcId is 0xffffffff; `DamageType`, which is not a
ValueList here). A failing test is not evidence of a deferral — check the API
by hand before trusting either.

## Ext.Level

| API | Returns | Why deferred | Evidence | Unlock path |
|---|---|---|---|---|
| `RaycastClosest` | ~~deferred~~ **IMPLEMENTED 2026-08-20** | Left the registry. All four stated unknowns resolved by decompilation: `ls::Function` is 0x40 bytes, passed indirectly by pointer, and a NULL MethodTable routes to the engine's own `s_IgnoreFilterClosest` -- no construction or destruction needed. Live 5/6 (the sixth was an over-strong test expectation about degenerate zero-distance hits, not a defect). | `ghidra/offsets/PHYSICS_VMT_AUDIT.md` | — |
| `RaycastAll` | ~~deferred~~ **IMPLEMENTED 2026-08-20** | Left the registry. The stated blocker (trailing by-value `ls::Optional<PhysicsSceneScopedReadLock&>` "cannot be constructed safely from C") was false -- it is the same disengaged optional `RaycastAny` ships, and `ls::PhysicsHitAll&` is the out-param the working SweepAll/TestBox bindings already pass. Live 5/5: 16 hits on a downward ray with geometry consistent to the host position, agrees with RaycastAny on hit and miss, mask exclusion clean, 300 casts -> 3138 hits in 6ms. | `ghidra/offsets/PHYSICS_VMT_AUDIT.md` | — |
| `RaycastAny` | ~~deferred~~ **IMPLEMENTED 2026-08-20** | Left the registry. All 18 physics VMT slots re-audited on 4.1.1.7398727 from shipped C++ symbols (no Ghidra needed): slot 10 is `phx::SimplePhysXScene::RaycastAny` with exactly the by-value `ls::Optional<PhysicsSceneScopedReadLock&>` signature proven GO. UUID gate advanced on that evidence, then the live stress ladder passed **6/6** (discriminator true, no false positives, masks honored, 5000 casts -> 2550 hits in 41ms, degenerate inputs safe). | `ghidra/offsets/PHYSICS_VMT_AUDIT.md`, `RAYCAST_ABI_B4A.md` | — |
| `GetTileDebugInfo` | `nil` | `AiGridTile::MinHeight` (+0x0a) is now **CONFIRMED** (Wave 7 B3: two independent accessor sites with the shared `/50` scaling), but the public cloud-surface enum conversion and material/extra flag decoders remain PROVISIONAL. The raw fields ship as the differently-named `GetTileRawDebugInfo(x, z)` diagnostic (no parity credit). | `ghidra/offsets/AIGRID_PATHFINDING.md` (2026-08-03 MinHeight recon) | Build the public flag conversions so the Windows-shaped table can be returned honestly |
| `BeginPathfinding` | `nil` | `AiGrid::CreatePath` is callable, but character path creation populates many `AiPath` fields beyond source/target/bounds; calling only `CreatePath` would enqueue an incompletely configured request. | same | Map the full `CreatePathForCharacter` field population (client `0x102de439c` + server callers) or find a stable high-level entry point |

Implemented for contrast: `GetPathById`, `ReleasePath`,
`GetActivePathfindingRequests`, `FindPath`, `GetEntitiesOnTile` (Wave 3,
verified live), all sweeps including cylinders, `TestBox`, `TestSphere`,
singleton accessors, `GetHeightsAt` (Wave 7: the B3 truth pass exposed the
old binding as a stub; the real multi-subgrid walk landed the same day over
CONFIRMED subgrid world-bounds offsets), plus the `GetTileRawDebugInfo`
diagnostic surface.

## Ext.Entity / entity methods

| API | Returns | Why deferred | Evidence | Unlock path |
|---|---|---|---|---|
| `entity:RemoveComponent(name)` | `false` | macOS emits only 734 `ImmediateWorldCache::RemoveComponent<T>(EntityHandle)` template instantiations, each with a hard-coded `TypeId<T>`; there is no generic runtime-TypeId entry point. Calling a specialization for a different type would remove the wrong component. | `ghidra/offsets/COMPONENT_OPS_AND_PROTO_INIT.md` ("NOT GENERICALLY UNLOCKED") | Generate a per-build type-index→specialization dispatch table, or audit a generic reimplementation of the template body (pending-change handling + destroy callbacks) |
| `Ext.Entity.EnableTracing` / `DisableTracing` | ~~warn + `nil`~~ **IMPLEMENTED (Wave 7 A8; row corrected 2026-08-27)** | The row's claim — "No macOS tracing infrastructure recovered" — was stale: `src/entity/entity_tracing.c` implements it and the Wave 7 A8 work this row named as its own unlock path had already landed. Live-verified 2026-08-27 in a loaded session: `EnableTracing(true)` → `true`, `DisableTracing()` → `true`. Remains a **prototype, partial vs Windows** (flat bounded log, not the full change tree). Contract note still holds: Windows exposes `EnableTracing(bool)` only; `DisableTracing` is a macOS compat wrapper. | `src/entity/entity_tracing.c` | Close the gap to Windows' full change tree |
| `entity:GetReplicationFlags(component[,qword])` | number (gated) | Wave 7 C step 2: now a real **read-only** proxy method on the entity proxy (matching the Windows placement, `LuaEntityProxy.inl:415`), traversing the CONFIRMED SyncBuffers → HashMap → DynamicBitSet chain with fully guarded reads. Resolves only the 9 confirmed replicated-type globals; version-gated and fail-closed. **Still a scored deferral**: no parity credit until the live-probe checklist validates the runtime int32 indices and pointer chain in-game (step 2 of the 9-step Phase C plan). | `ghidra/offsets/REPLICATION_SYNCBUFFERS.md`, `src/entity/replication_flags.c` | Run the live-probe checklist; then credit and lift the deferral |
| `entity:Replicate()` | no-op | Same replication gap. **Highest-demand deferral in the registry**: 5 of 11 vetted mods call it (Community Library, 5e Spells, Expansion, Combat Extender, Transmog Enhanced) — masked in single-player, unproven in multiplayer. Cannot be reclassified as a scope exclusion. | same | same |
| Component property writes (unknown-size layouts, unsupported field types) | `false` | Writing through an unverified size or field type risks corruption; INT32, UINT8, BOOL, FLOAT, and INT32_ARRAY are proven. Wave 7 A7 added a FLOAT_ARRAY writer with exact-length validation, NaN/Inf rejection, a staged buffer, and atomic commit, but it earns no parity credit yet: the only candidate, `ls::EffectComponent::OverrideFadeCapacity`, has unverified ARM64 offsets because the Ghidra bridge was down. | `src/entity/component_property.c` | Verify `OverrideFadeCapacity` offsets in Ghidra, then exercise the writer against the live component before crediting parity |
| `Ext.Entity.GetEntitiesAroundPosition` spatial-grid backend | behavioral results | **No contract deferral:** Wave 7 A1 meets the Windows 2D XZ-circle behavior, ignores Y, and defaults includeCharacters/includeItems to true. The backend walks `eoc::character::CharacterComponent` and the item marker component, then reads `ls::TransformComponent` positions. The native spatial-grid `GridStructure` ARM64 layout remains unrecovered. | `BG3Extender/Lua/Libs/Ai.inl:502-537`; `src/entity/entity_system.c` | Recover `GridStructure` only if profiling shows the archetype fallback needs replacement |
| `Ext.Entity.Create` / `Destroy` | absent | Engine entity lifecycle (allocator + handle mint) not recovered on macOS. | `Entity.inl:305-306` | Wave 7 Phase D4 (recon first, go/no-go on allocator evidence) |
| `OnSystemUpdate` / `OnSystemPostUpdate` | ~~deferred~~ **IMPLEMENTED 2026-08-20** | Left the registry. The table walk in `ecs_system_update.c` was already correct; a live array probe confirmed the layout (buffer +0x30, size +0x3c = 934, stride 0xf8, 506 populated from slot 311, UpdateProc +0x18) and a ServerPassive hook fired 462 times in ~12s then unsubscribed cleanly. Limits: `Client*` systems need the client world; the name table covers 73 of 454 TypeIds; one hook kind per system. | `ghidra/offsets/ECS_SYSTEM_UPDATE_RECON.md` | — |
| `GetTrace` / `ClearTrace` | ~~absent~~ **IMPLEMENTED (Wave 7 A8; row corrected 2026-08-27)** | "Tracing infrastructure not built" was stale — the A8 prototype this row pointed to as its unlock path shipped. Live-verified: `GetTrace()` → `{Enabled, Dropped, Entities}`, `ClearTrace()` → `true`. **The log is `Entities`** — a map keyed by EntityHandle matching Windows' `ECSChangeLog` — not a flat `Events` array; a Tier 2 test and the module docstring both said `Events` until 2026-08-27 and failed against a correct implementation. Full Windows parity (command-buffer + replication-dirty scanning) still gated on the replication foundation. | `src/entity/entity_tracing.c` | Close to Windows' full change tree |

Implemented for contrast: `entity:CreateComponent` (verified ComponentOps
registry at `EntityWorld+0x390`, vptr slot 5), `GetAllEntities`,
`GetAllEntitiesWithComponent`, `GetAllComponents` (server-world archetype
walks). Note: client-world components (`ecl::*`) are not enumerated by the
walk; `esv::Character` is not a registered TypeId — use
`eoc::character::CharacterComponent` for character queries.

## Ext.Stats

| API | Returns | Why deferred | Evidence | Unlock path |
|---|---|---|---|---|
| Passive prototype sync | `false` | **PARTIAL-GO; sync stays false.** `Passives::Parse` needs no loader-private state, but only resolves names to existing prototypes. Existing-entry scalar/description refresh and all three functor-list refreshes are statically safe. `Boosts` remains blocked on an LTO-specialized closure ABI. The map uses `0x220` nodes with an inline `0x210` prototype. | [PASSIVES_PARSE_RECON.md](../ghidra/offsets/PASSIVES_PARSE_RECON.md) | Complete the five-step milestone below before returning success |
| Interrupt prototype sync | `false` | `eoc::InterruptPrototype` (0x1f0 bytes) has a `FixedString` at offset 0, not a vptr; the manager uses a hash table + contiguous 0x1f0-stride array, structurally incompatible with the generic RefMap insert helper. | `ghidra/offsets/COMPONENT_OPS_AND_PROTO_INIT.md` | Map the inlined object move/build path at `0x103063f94` onward |
| `AddAttribute` | `false` | Runtime modifier-list allocation and construction remain unverified. **Re-checked 2026-08-27 on 4.1.1.7398727** (the stub's comment cited the older 7209685): still blocked — `nm` exposes no `RPGStats`/`ModifierList` insert symbols on this build at all, so the machinery is inlined and allocating blind would mix heaps. Rationale holds; only its build reference was stale. | `src/stats/stats_manager.c`, `src/lua/lua_stats.c` | Recover the pre-load modifier allocation path |
| `ExecuteFunctors` | partial | **Rationale corrected 2026-08-27 — the previous one ("full param-block construction unverified") was wrong.** Param blocks already work: `PrepareFunctorParams` builds contexts and eight context layouts ship in `functor_types.h`. The real blocker is that Lua cannot hold a `StatsFunctorList*` — the hooks receive genuine engine lists and forward them, but `events_fire_execute_functor` exposes only a bare integer `FunctorListPtr`, which is unsafe to consume because the pointer dies with the dispatch. | `src/lua/lua_stats.c` (`lua_stats_execute_functors` log line), `src/lua/lua_events.c` | Add a lifetime-scoped `bg3se.FunctorList` userdata (owner+generation per `lua_runtime.h`) and accept it as arg 2 — full design in [2026-08-27-001-feat-ext-stats-executefunctors-plan.md](plans/2026-08-27-001-feat-ext-stats-executefunctors-plan.md) |

Passive sync future milestone:

1. Keep `sync_passive_prototype()` false.
2. Prove the `ParseStaticBoosts` closure ABI or find a safe wrapper.
3. Build an exact-version-gated existing-entry refresher.
4. Live-validate scalar, `Boosts`, and three functor-list updates.
5. Attempt new insertion only with validation and rollback.

Implemented for contrast: `AddEnumerationValue` (Wave 7 B1) calls engine
`ValueList::Insert` at `0x101c44920`. It uses the existing engine-backed
FixedString intern path and verifies both lookup directions plus count growth.

## Ext.Types

| API | Returns | Why deferred | Evidence | Unlock path |
|---|---|---|---|---|
| `Construct(typeName)` | errors or 0 values | **Matched contract, not a gap**: Wave 7 A3 matches Windows Types.inl:286-302 exactly, reaching the shared upstream `// TODO` and returning 0 values. Outside the parity denominator. | `src/lua/lua_ext.c` | Track upstream |

**2026-08-18 correction.** `GetHashSetValueAt`, `AddCustomFunction`, and
`AddCustomProperty` are **implemented**, not stubs — this registry and the
contract manifest were both stale. `GetHashSetValueAt` reads the HashSet keys
array through a guarded proxy with the Windows 1-based index contract
(`lua_ext.c:1100`), and passed Tier 1 `Parity.Types.GetHashSetValueAt` live on
build 4.1.1.7398727. `AddCustomFunction`/`AddCustomProperty` register into a
per-state custom-property registry (`BG3SE_CustomProps`) that the component
proxy `__index` actually consults, including getter invocation
(`component_property.c:707-760`) — the property-map layer this registry
previously recorded as missing exists.

`AddCustomFunction`/`AddCustomProperty` still earn **no parity credit**: the
Tier 1 `Parity.Types.CustomProps` assertion fails at the main menu with
`Type not found: eoc::CharacterComponent`, because component TypeIds are not
discovered until a session loads. Crediting them requires a Tier 2 run with a
save loaded.

## Excluded by scope decision (not deferrals)

These four surfaces sit outside the parity denominator. This list is now the
single authority — ROADMAP.md defers to it (Wave 6 reconciled a silent
disagreement where ROADMAP assumed exclusions this registry never named).

- **Ext.UI** — Windows NsGui/Noesis surface. **Description corrected 2026-08-27: no
  longer "stub layer only".** macOS BG3 links Noesis (~82,000 symbols) and the
  read-only tree surface is now real and live-verified against a loaded session:
  `GetRoot`, `IsReady`, and on elements `Find`, `Child`, `VisualChild`, `GetRoot`,
  `Name`, `ChildrenCount`, `VisualChildrenCount`. Walked
  `CanvasRoot -> ViewboxRoot -> ContentRoot` (21 visual / 23 logical children);
  `Ext.UI.GetRoot():Find("ContentRoot")` — MCM's exact expression — returns a real
  element, retiring the old "ContentRoot not found" failure. Element names come
  from the exported `Noesis::FrameworkElement::GetName`, not a reversed offset.
  **Still stubs:** `Instantiate`, `RegisterType`, `GetValue`, `SetValue` (general
  Noesis property reflection needs `TypeClass` internals that are not exported).
  Whether the working read-only surface earns parity credit is an open scope
  decision; it is recorded here as fact, not as a claim on the denominator.
  **Known latent risk:** `GetName` is a non-virtual `FrameworkElement` method, but
  `VisualChild` returns `Visual*`, which is not guaranteed to be a FrameworkElement.
  Calling it on a non-FE visual is untested. `FrameworkElement::StaticGetClassType`
  (0x4e07e0) and `TypeClass::IsAssignableFrom` (0x33fbe8) are both exported if an
  RTTI guard becomes necessary.
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

- 2026-08-03 (v0.43.0, Wave 7 B-series): AddEnumerationValue left the
  registry after the engine insertion/growth path and readback checks were
  proved. Passive sync became PARTIAL-GO for a future implementation, but
  remains false pending the `Boosts` closure ABI and live validation. ECS
  system-update callbacks became implementation-ready; both APIs remain gaps
  until their per-entry wrappers land.
- 2026-08-02 (v0.43.0, Wave 7 A-series): the five A1 Entity registrations
  and A6 PlayerHasExtender GUID behavior left the registry. A7 FLOAT_ARRAY
  support remains verification-gated. GetEntitiesAroundPosition meets the
  public contract through an archetype-walk fallback while GridStructure RE
  remains deferred. Construct's exact Windows error surface is documented.
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

## Investigation notes (2026-08-20)

**`Ext.StaticData.GetSources` / `GetByModId` — no cheap path.** Windows
implements both as thin accessors over `GuidResourceBank<T>::ResourceGuidsByMod`
(`StaticData.inl:37-45`). A symbol sweep of the 4.1.1.7398727 arm64 image finds:

- no `GuidResourceBank` symbols at all;
- no `ResourceGuidsByMod` symbol;
- no per-mod resource mapping symbol of any kind (the only `ModGUID` symbols are
  `gui::VMModPreview::Get/SetModGUID`, unrelated).

`ls::ResourceBank` exists but is the visual/asset bank, not the GUID resource
bank. The macOS port also models this area differently from Windows: it reaches
managers directly (e.g. `eoc::FeatManager` via `Context.field_0x130`, count at
`+0x7C`, array at `+0x80`) rather than through a `GuidResourceBank<T>` with
`Resources` + `ResourceGuidsByMod` members.

This does **not** prove the field is absent -- a plain struct member needs no
symbol -- but it does mean there is no symbol-driven shortcut, and the accessor
that would reveal its offset does not exist as a distinct function to decompile.
Recovering it means finding a site that populates the mapping during resource
load and reading the destination offset there. Treat as genuine RE, not a quick
win.
