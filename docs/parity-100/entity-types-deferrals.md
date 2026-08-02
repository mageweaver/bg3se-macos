I’m using the BG3SE macOS Ghidra skill because this is a reverse-engineering parity assessment. I’ll trace the macOS implementation and Windows reference, then verify real-mod demand from repository evidence and, only where needed, targeted web research.
The skill’s main constraint is important here: macOS cannot patch the hardened main executable, so “we found the specialization” is not yet the same as “we can safely dispatch to it.” I’m treating callable-address stability and ARM64 ABI correctness as part of each unlock assessment.
So far, the code confirms three distinct risk classes: removal/replication are engine-invariant problems, FixedString is an ownership/interning problem, and EntityHandle/float-array writes are mostly representation and validation problems. I’m now checking the Windows implementation to avoid overestimating what these APIs actually promise.
The local vetting evidence has one concrete production caller: Community Library invokes `entity:Replicate("ActionResources")`; the other nine top-vetted mods still pass with all of these gaps deferred. I’m checking source-level evidence next, because namespace-level “uses Entity” is too coarse to claim a specific API dependency.
The Exa skill is now guiding a narrow source search: exact API identifiers on GitHub first, then selective extraction. This will separate “popular mod is compatible” from “popular mod actually calls the missing function.”
The Exa package’s documented script path is stale in this environment, so that skill cannot run cleanly. I’m falling back to read-only GitHub/web search while keeping the same exact-identifier methodology.
I’ve finished the broad code and installed-mod audit. One important finding changes the prioritization: four vetted mods explicitly call `Replicate`, while none of the top-ten set calls `RemoveComponent` or the missing `Ext.Types` APIs. I’m tightening the ownership/lifetime analysis for the new writable types before giving you the ranked plan.
The ownership check confirms that `FixedString` is not “just another 32-bit scalar”: the port already has a verified interning call, but replacement must transfer the new string-table reference and decrement the old one. The current “interning unavailable” refusal is stale; the real blocker is recovering and version-gating the release operation.
## Answer

The sound path for `entity:RemoveComponent` is the per-build specialization dispatch table. It delegates the dangerous work—pending-change storage, callbacks, allocator use, and destruction—to Larian’s exact `RemoveComponent<T>` implementation. A generic rewrite should only be attempted later, using the 734 native specializations as a differential oracle.

The highest-value gap is replication. Four of the ten vetted popular mods call `Replicate`; none of the audited ten call `RemoveComponent`, tracing, or the missing `Ext.Types` functions. However, the passing compatibility tests show source-level demand, not proven multiplayer correctness: the current no-op is often masked in single-player.

Two scope corrections matter:

- FixedString interning is already recovered. Its real blocker is reference-count ownership, not string creation.
- Windows `Ext.Types.Construct` is itself still a TODO and returns no object. It should not count as a functional macOS parity gap.

Effort estimates below are engineering time including offline RE and targeted live validation, not just code entry. Confidence is high on architecture/demand and medium on estimates until multiplayer testing is performed.

## Evidence

The macOS deferral table accurately summarizes the visible stubs at [docs/deferrals.md:31](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/docs/deferrals.md:31) and [docs/deferrals.md:56](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/docs/deferrals.md:56). The implementation confirms:

- `RemoveComponent` always returns false: [entity_system.c:1642](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/entity/entity_system.c:1642)
- `Replicate` is a no-op: [entity_system.c:1862](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/entity/entity_system.c:1862)
- tracing/flag APIs are Lua stubs: [main.c:1111](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/injector/main.c:1111)
- Types stubs occupy [lua_ext.c:1033](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/lua/lua_ext.c:1033) through [lua_ext.c:1111](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/lua/lua_ext.c:1111).

The installed-mod audit used the ten-mod vetted set documented at [README.md:93](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/README.md:93) and [supported-mods.md:39](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/docs/supported-mods.md:39). Active calls were found in:

- Community Library: `EntityUtils.lua:63,86,116,196`
- 5e Spells: `S5E_Additions.lua:172`
- Expansion: `BootstrapServer.lua:1429-1431,2523,2538`
- Combat Extender: `BootstrapServer.lua:1108,1534-1535`

The repository already records Community Library’s multiplayer intent at [CHANGELOG.md:872](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/docs/CHANGELOG.md:872).

## Details

### 1. `entity:RemoveComponent`

**Blocker.** There are 734 `ImmediateWorldCache::RemoveComponent<T>` functions and no runtime-TypeId overload. Each specialization loads its own `TypeId<T>::m_TypeIndex`, so calling the wrong function removes the wrong component. The ABI is also `void(cache, entity)`, not the Windows helper’s Boolean result: [COMPONENT_OPS_AND_PROTO_INIT.md:179](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/COMPONENT_OPS_AND_PROTO_INIT.md:179).

The body touches substantially more than the component pointer: per-type change storage, the current or pending component, destroy callbacks, and a null deletion change [COMPONENT_OPS_AND_PROTO_INIT.md:247](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/COMPONENT_OPS_AND_PROTO_INIT.md:247). Windows’ generic implementation confirms these responsibilities at [EntitySystem.cpp:291](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/GameDefinitions/EntitySystem.cpp:291) and [EntitySystem.cpp:313](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/GameDefinitions/EntitySystem.cpp:313).

**Sound unlock: generated dispatch table.**

1. Enumerate every `RemoveComponent<T>` symbol.
2. For each specialization, extract both its function RVA and the referenced `TypeId<T>::m_TypeIndex` global RVA.
3. Emit `(canonical type name, TypeId-global RVA, remover RVA)` rather than hard-coding a numeric type index.
4. At startup, read the live TypeId, cross-check it against the component registry, and populate `typeIndex → function`.
5. Require an exact build/version or binary hash; validate that every target lies in executable `__TEXT`; fail closed for missing or ambiguous mappings.
6. Obtain the cache through the verified `EntityWorld+0x3f0` offset [COMPONENT_OPS_AND_PROTO_INIT.md:264](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/COMPONENT_OPS_AND_PROTO_INIT.md:264).
7. Test inline, proxy, non-trivially destructible, transient, missing-component, pending-add, and already-pending-remove cases.

Return-value parity needs one additional recovery: the native specialization returns `void`, and `GetChange()` returns the change pointer, which is null for a deletion marker. Correctly distinguishing “new removal” from “already pending removal” therefore requires reading the per-type change map’s membership, not merely calling `GetChange()`.

**Alternative: generic rewrite.** Recover the component-registry entry layout, atomic availability bitset, placement construction of `ComponentChanges`, hash-map insertion, allocator contract, destructor proc, proxy-specific paths, and callback ABI. Cluster and compare many specializations before implementing it. Only adopt it after differential tests against native removers produce identical pending-change and callback behavior.

**Verdict.** Dispatch is much sounder. It preserves game-owned behavior and confines mistakes to mapping/version validation. It safely covers the emitted 734 types; literal coverage beyond those types would eventually require the generic path.

**Effort/risk/demand.**

- Dispatch generator and first build: **1–2 weeks**; subsequent builds: roughly **0.5–1 day** if symbol structure remains stable.
- Generic implementation: **3–6 weeks**.
- Risk: **medium** for fail-closed dispatch; **critical** for the generic rewrite.
- Popular-mod demand: **none found** in the vetted ten.

### 2. `EnableTracing` / `DisableTracing`

**Blocker.** Tracing is not simply replication bookkeeping. Windows maintains an extender-owned change log, scanning both replication pools and ECB command buffers [EntitySystem.cpp:1002](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/GameDefinitions/EntitySystem.cpp:1002) and [EntitySystem.cpp:1042](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/GameDefinitions/EntitySystem.cpp:1042).

The macOS port already has a useful partial foundation: verified component `OnConstruct`/`OnDestroy` callback arrays [entity_events.c:42](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/entity/entity_events.c:42) and signal injection [entity_events.c:571](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/entity/entity_events.c:571). Those can record post-fact component creation/destruction, but not all entity flags, pending ECB state, or replication dirties.

**Unlock path.**

1. Add per-Lua-state tracing enablement and a bounded change log.
2. Reuse component callbacks for an initial create/destroy backend.
3. Recover `EntityWorld::CommandBuffers`, ECB entity-change maps, component-change stores, and their flush boundary.
4. After replication RE, scan dirty replication pools at that same boundary.
5. Expose `GetTrace` and `ClearTrace`; otherwise enabling tracing has no useful Windows-equivalent consumer.
6. Keep the current `DisableTracing()` only as a compatibility wrapper. Current Windows exposes `EnableTracing(bool)`, not a separate Disable API [Entity.inl:243](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Libs/Entity.inl:243).

**Effort/risk/demand.**

- Callback-only partial implementation: **3–5 days**.
- Faithful ECB plus replication tracing: **2–3 weeks**, partly shared with replication.
- Risk: **low native-corruption risk**, but **medium performance/reentrancy risk**.
- Popular-mod demand: **none found**; this is primarily a developer diagnostic.

### 3. `GetReplicationFlags`

**Blocker.** It needs a component-name → replication-index mapping, `EntityWorld::Replication`, the `ComponentPools` array layout, and read-only lookup in `HashMap<EntityHandle, BitSet>`. Windows performs exactly that at [EntitySystem.cpp:610](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/GameDefinitions/EntitySystem.cpp:610).

There is also an API placement mismatch: current Windows exposes `entity:GetReplicationFlags(...)` [LuaEntityProxy.inl:295](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Shared/Proxies/LuaEntityProxy.inl:295), while macOS currently stubs `Ext.Entity.GetReplicationFlags`.

**Unlock path.**

1. Enumerate the symbol-rich `TypeContext<ecs::sync::ReplicatedTypeContext>::RegisterType<T>` corpus.
2. Pair those types with ordinary component TypeIds by canonical name, following Windows’ mapping algorithm [EntitySystem.cpp:728](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/GameDefinitions/EntitySystem.cpp:728).
3. Recover and validate `EntityWorld::Replication`.
4. Map the `SyncBuffers` array header and pool/hash-map read layout.
5. Implement read-only lookup and qword bounds checks first.
6. Register it on entity proxies; optionally retain the table-level function as a shim.

**Effort/risk/demand.**

- Shared replication RE foundation: **1–2 weeks**.
- Incremental implementation after that: **1–2 days**.
- Risk: **medium-low**, because this path can remain strictly read-only and fail closed.
- Popular-mod demand: **none found**; the Windows repository uses it in ECS tests.

### 4. `entity:Replicate`

**Blocker.** This is the mutable half of the preceding work. Windows finds or inserts the entity’s bitset, expands it, ORs the requested flags, then sets `SyncBuffers::Dirty` [LuaEntityProxy.inl:308](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Shared/Proxies/LuaEntityProxy.inl:308). `Replicate()` marks the first qword completely dirty [LuaEntityProxy.inl:335](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Shared/Proxies/LuaEntityProxy.inl:335). The conceptual Windows structure is an array of entity→bitset maps plus a dirty flag [EntitySystem.h:388](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/GameDefinitions/EntitySystem.h:388).

**Unlock path.**

1. Complete the replication-index and read-only pool recovery above.
2. Locate native pool lookup/insertion and dynamic-bitset growth helpers through representative `SyncedData<T>` constructors. Prefer callable game helpers over reimplementing hash-table allocation.
3. Recover the dirty-byte offset and verify when it is cleared/consumed.
4. Enforce server-world-only use.
5. Implement `GetReplicationFlags` first, then insertion, then full-qword replication.
6. Validate in single-player, multiplayer host, remote client, reconnect, save/load, and rapid repeated mutation scenarios.
7. Test the exact demanded components: `ActionResources`, `God`, `CombatParticipant`, `AvailableLevel`, `EocLevel`, `Classes`, `GameObjectVisual`, and `Stats`.

This behavior agrees with the [official BG3SE API documentation](https://github.com/Norbyte/bg3se/blob/main/Docs/API.md).

**Effort/risk/demand.**

- Complete shared foundation plus mutation: **2–4 weeks**.
- Incremental write implementation after read-only flags work: **3–7 days**.
- Risk: **high** until native insertion/growth helpers and dirty lifecycle are verified; malformed pool state can crash the network serializer.
- Popular-mod demand: **high**—Community Library, 5e Spells, Expansion, and Combat Extender all call it.
- Demonstrated failure demand: **not yet proven**. All four currently pass the predominantly single-player vetting pipeline despite the no-op, so multiplayer assertions are required.

### 5. FixedString property writes

**Blocker.** The current diagnostic claims interning is unavailable [component_property.c:527](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/entity/component_property.c:527), but this is stale. The port already exposes `fixed_string_intern()` [fixed_string.h:99](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/strings/fixed_string.h:99) through a version-remapped `ls::FixedString::Create` call [fixed_string.c:1479](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/strings/fixed_string.c:1479).

The real problem is ownership. Windows `FixedString` copy/assignment/destruction increments and decrements string-table references [BaseString.h:157](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/CoreLib/Base/BaseString.h:157), with native release helpers at [BaseString.inl:133](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/CoreLib/Base/BaseString.inl:133).

**Unlock path.**

1. Recover and version-gate `FixedString::DecRef` or the global-table `DecRef`.
2. Accept Lua strings, not raw numeric IDs.
3. Read the old ID and intern the replacement, acquiring one owned reference.
4. Write the new ID atomically through the safe-memory layer.
5. On success, decrement the old ID; on failure, decrement the newly created ID.
6. Permit nil/null only with an explicit documented null-index convention.
7. Enable only audited component/field pairs and remove the corresponding Unserialize exclusion at [component_property.c:1192](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/entity/component_property.c:1192).
8. Fix reads to return strings rather than the current raw index [component_property.c:404](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/entity/component_property.c:404).

**Effort/risk/demand.**

- **3–5 days**, plus field-by-field layout validation.
- Risk: **high** for raw assignment; **medium-low** with correct transfer/release semantics and an allowlist.
- Popular-mod demand: **none found** in active vetted-mod code.

### 6. EntityHandle property writes

**Blocker.** EntityHandle is declared as a 64-bit field [component_property.h:35](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/entity/component_property.h:35), but `component_property_field_size()` does not admit it [component_property.c:443](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/entity/component_property.c:443). Reads currently expose debugging hex strings rather than the normal entity proxy/handle convention [component_property.c:391](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/entity/component_property.c:391).

The larger blocker is semantic: many such fields represent ownership or indexed relationships—`TopOwner`, `Trader`, `Buyer`, `Looter`, and similar examples are presently marked writable in the imported layouts [component_offsets.h:4108](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/entity/component_offsets.h:4108). Blind replacement may leave reverse indexes or inventory systems inconsistent.

**Unlock path.**

1. Add the verified eight-byte size.
2. Normalize inputs from BG3 entity userdata, Lua integer, canonical hex string, and optional nil/null.
3. Validate handle encoding, generation, liveness, and server/client world compatibility.
4. Build a per-field allowlist from observed native mutation behavior; do not enable all imported `readOnly=false` entries globally.
5. Test destroyed/stale handles and cross-level entities.
6. Return or accept entity proxies consistently instead of debug-only strings.

**Effort/risk/demand.**

- Parsing/framework: **2–3 days**.
- Meaningful per-field audit: **3–7 additional days**, potentially longer for relationship fields.
- Risk: **high semantic corruption risk**, even though the physical write is only eight bytes.
- Popular-mod demand: **none found**.

### 7. Fixed float-array writes

**Blocker.** Reads already exist [component_property.c:355](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/entity/component_property.c:355), but field sizing and the writer omit `FIELD_TYPE_FLOAT_ARRAY`. More importantly, a repository-wide search found no actual component property definition using that type, so this is currently dormant infrastructure rather than a blocked real field.

**Unlock path.**

1. Add `arraySize * sizeof(float)` to the size validator.
2. Require an exact-length Lua table.
3. Reject non-numeric, non-finite, and out-of-range values.
4. Convert into a temporary fixed buffer.
5. Perform one checked memory write.
6. Only classify fields as float arrays after validating their offset, count, and component size on macOS.

**Effort/risk/demand.**

- Writer: **0.5–1 day**.
- Finding and validating useful layouts: **1–3 days per component family**.
- Risk: **low** for verified layouts; **high** if `arraySize` is guessed.
- Popular-mod demand: **none found**.

### 8. `Ext.Types.Construct`

**Blocker.** True arbitrary construction needs size/alignment, allocator, constructor, destructor, ownership, and Lua `__gc` behavior per type.

However, upstream Windows checks constructibility and then stops at `// TODO; return 0` [Types.inl:286](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Libs/Types.inl:286). It does not currently construct an object.

**Unlock path.**

- For actual Windows parity: mirror its validation/error contract and return no object. This is less than **one day**.
- For a genuinely functional extension: introduce per-type construction descriptors and only register types with recovered allocator/ctor/dtor triples. Never infer “constructible” from layout data alone.

**Effort/risk/demand.**

- Behavioral Windows parity: **<1 day**.
- General functional construction: **6+ weeks/ongoing per-type RE**.
- Risk: **critical** for generic construction; low for a semantics-only parity shim.
- Popular-mod demand: **none found**.
- Recommendation: exclude it from the “functional parity” denominator until Windows implements it.

### 9. `Ext.Types.GetHashSetValueAt`

**Blocker.** The function is only the last few lines of a larger missing facility: macOS has no typed set proxy carrying container pointer, concrete element type, push conversion, and lifetime metadata. Windows delegates to such a proxy [LuaSetProxy.h:5](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Shared/Proxies/LuaSetProxy.h:5).

There is also a contract bug in the macOS comment: it says zero-based [lua_ext.c:1051](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/lua/lua_ext.c:1051), while Windows accepts indices `1..size` and accesses `keys[index-1]` [LuaSetProxy.h:46](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Shared/Proxies/LuaSetProxy.h:46).

**Unlock path.**

1. Recover the relevant `ls::HashSet<T>` layouts and distinguish logical key storage from buckets/occupancy metadata.
2. Add typed, lifetime-aware SetProxy userdata.
3. Register element converters for each reflected `T`.
4. Implement length, iteration, and indexed read before considering mutation.
5. Make `GetHashSetValueAt` a 1-based checked delegation returning nil out of range.

**Effort/risk/demand.**

- Read-only proxy and common element types: **1–2 weeks**.
- Risk: **medium** for reads; high if mutation is added before allocator/hash invariants are recovered.
- Popular-mod demand: **none found**.

### 10. `Ext.Types.AddCustomFunction`

**Blocker.** No engine RE is inherently required. The missing piece is a common extensible property-map layer. Today the component proxy directly checks native fields and then returns nil [component_property.c:660](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/entity/component_property.c:660).

Windows maintains persistent Lua registry references keyed by property-map registry index and rejects collisions with native/custom properties [LuaPropertyMap.inl:225](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Shared/Proxies/LuaPropertyMap.inl:225).

**Unlock path.**

1. Add a per-Lua-state registry keyed by type identity and property name.
2. Persist and release Lua function references during state resets.
3. Reject collisions with real and prior custom properties.
4. Consult the registry from `__index` after special/native properties.
5. Initially support `ComponentProxy`; extend the common hook to every object-proxy family for full parity.
6. Preserve lifetime information when handing the object to custom getters/functions.

**Effort/risk/demand.**

- Component-only implementation: **3–5 days**.
- Full cross-proxy property-map layer: **2–3 weeks**.
- Risk: **low native-corruption risk**, **medium Lua lifetime/reentrancy risk**.
- Popular-mod demand: **none found**.

### 11. `Ext.Types.AddCustomProperty`

**Blocker.** It needs the same registry plus getter invocation and optional setter dispatch. The current component `__newindex` always attempts a native memory write and errors on failure [component_property.c:692](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/entity/component_property.c:692). It has no custom-setter branch.

Windows invokes a registered getter with a lifetime-aware proxy, and invokes setters with `(object, value)` [LuaPropertyMap.inl:279](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Shared/Proxies/LuaPropertyMap.inl:279).

**Unlock path.**

1. Build the shared custom-property registry described above.
2. Invoke getters under protected Lua calls with balanced stacks.
3. Check custom setters before falling through to native `component_property_write`.
4. Return a read-only error when no setter exists.
5. Add recursion guards and state-reset cleanup.
6. Generalize the hook to all property-map-backed proxies for real parity.

**Effort/risk/demand.**

- Component-only implementation: **5–8 days**.
- Full proxy-surface implementation: **3–4 weeks**, overlapping substantially with `AddCustomFunction`.
- Risk: **low native-corruption risk**, **medium Lua lifetime/recursion risk**.
- Popular-mod demand: **none found**.

## Related

Several parity records should be corrected before measuring progress:

- FixedString’s blocker is reference ownership, not missing interning.
- `Construct` is not functional on current Windows.
- `GetHashSetValueAt` is 1-based.
- `GetReplicationFlags` belongs on the entity proxy in the current Windows API.
- `DisableTracing` is a compatibility wrapper; current Windows uses `EnableTracing(bool)`.
- A 734-entry removal dispatcher gives safe emitted-type coverage, not theoretical removal of every one of the roughly 2,000 registered component types.

## Ranked attack order

1. **Replication foundation:** component↔replication mapping, `SyncBuffers`, then read-only `entity:GetReplicationFlags`.
2. **`entity:Replicate`:** native map insertion/bitset growth and multiplayer validation against the four demanding mods.
3. **`RemoveComponent` specialization dispatcher:** build-gated and generated; use it later as the oracle for any generic implementation.
4. **FixedString writes:** recover DecRef and implement transactional ownership transfer.
5. **Float-array writer:** cheap, but only activate it when a verified field appears.
6. **Shared custom-property layer:** `AddCustomFunction` first, then `AddCustomProperty`.
7. **HashSet proxy and `GetHashSetValueAt`.**
8. **Full tracing:** leverage the replication work and existing component callbacks.
9. **Audited EntityHandle writes:** deliberately late because relationship corruption risk exceeds observed demand.
10. **`Construct`:** align the Windows no-result contract, but exclude genuine arbitrary construction from the present parity target.
