I’m using the BG3SE macOS Ghidra research skill because this needs coordinated static disassembly, runtime-probe planning, and comparison with the Windows reference. I’ll keep the analysis read-only and cite exact source lines and binary addresses.
The skill confirms the key platform constraint: the loader code in the hardened main executable can be decompiled and probed, but not safely detoured. I’m now tracing each deferred path against the current macOS implementations and Windows APIs, then I’ll verify real-mod call patterns rather than infer their value from API names.
A useful distinction is emerging: `ExecuteFunctors` is not chiefly blocked by the nine dispatcher ABIs—the hook code has already recovered those, including the hidden result object. It is blocked one layer earlier: valid game-owned `Functors`/`Functor` objects and correctly constructed polymorphic context objects. I’m separating that from the more tractable modifier-manager insertion work.
I’ve narrowed this to two especially useful native choke points: a direct enum-value insertion method and a previously undocumented `Passives::Parse` helper. I’m checking their exact symbols/addresses now, because they materially change whether those gaps require unsafe container reconstruction.
## Answer

Closing the Ext.Stats gap splits into two tractable native-container bindings and two much harder loader/ABI reconstructions:

| Gap | Focused sessions | Corruption risk | Recommendation |
|---|---:|---|---|
| `AddEnumerationValue` | 1 | Medium | Implement first |
| `AddAttribute` | 2–3 | High; Critical after stats load | Implement second, pre-load only |
| `ExecuteFunctors` | 3–5 MVP; 7–12 full | High/Critical | Build a narrow AttackTarget/Target MVP |
| Passive sync | 2–3 if `Passives::Parse` helps; otherwise 5–8 | High/Critical | One RE discriminator session, then defer if still inlined |
| Interrupt sync | 6–10 | Critical | Keep as an honest deferral |

The strongest new leads are:

- `CRPGStats_Modifier_ValueList::Insert(FixedString const&, int)` at `0x101c44920`, which may eliminate all manual container work for `AddEnumerationValue`.
- `eoc::Passives::Parse(eoc::Passives const&, ls::STDString const&)` at `0x101c0f37c`. This deserves decompilation, but its signature does not prove that it is the missing `PassivePrototype::Init`.
- Native cached lookups exist at `eoc::Passives::Get` `0x101c0f27c` and `InterruptPrototypeManager::GetPrototype` `0x101b7adcc`. The current macOS cached accessors incorrectly route both through the generic `refmap_lookup` helper at [prototype_managers.c:432](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/stats/prototype_managers.c:432), even though interrupt storage is definitely not a RefMap.

All addresses below are build-specific to `4.1.1.7209685` and should remain exact-version gated.

## Evidence

- The repository correctly records the four deferrals at [docs/deferrals.md:47](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/docs/deferrals.md:47).
- Passive allocation, `0x210` size, `Clean()` call, and nested vptr at `+0xb8` are established at [COMPONENT_OPS_AND_PROTO_INIT.md:389](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/COMPONENT_OPS_AND_PROTO_INIT.md:389).
- Interrupt lookup returns `array + index * 0x1f0`, proving contiguous value storage at [COMPONENT_OPS_AND_PROTO_INIT.md:455](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/COMPONENT_OPS_AND_PROTO_INIT.md:455).
- Both paths intentionally fail closed at [prototype_managers.c:797](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/stats/prototype_managers.c:797).
- Modifier layout reads are runtime-verified, but construction is not, at [stats_manager.c:204](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/stats/stats_manager.c:204).
- The active attribute APIs are explicit allocator/container stubs at [lua_stats.c:782](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/lua/lua_stats.c:782).
- `PrepareFunctorParams` currently zeroes approximate C structures without invoking their constructors at [lua_stats.c:1049](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/lua/lua_stats.c:1049). The layouts themselves contain placeholders at [functor_types.h:120](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/stats/functor_types.h:120).
- The nine dispatch ABIs and hidden `Result` output parameter are documented at [functor_types.h:373](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/stats/functor_types.h:373).
- Windows constructs real contexts, assigns `PropertyContext` and `ClassResources`, and dispatches through nine overloads at [StatFunctors.inl:36](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Libs/StatFunctors.inl:36).

## Details

### 1. Passive prototype sync

Precise blocker:

`PassivePrototype` construction is understood, but semantic population from `StatsObject` is not. Calling the constructor at `0x101c0d6c8`, inserting the node, and calling `Clean()` at `0x101c0d964` would leave an empty prototype. Copying a status-style vptr would corrupt the first field because only the nested `StatsFunctorList` at `+0xb8` is polymorphic.

Windows explicitly clears dynamic fields and calls a mapped `PassivePrototype::Init(proto, object)` at [Stats.cpp:107](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/GameDefinitions/Stats/Stats.cpp:107); no equivalent callable macOS symbol is known.

Concrete RE path:

1. Through the Ghidra bridge, decompile in this order:

   - `0x101c0f37c` — `eoc::Passives::Parse`
   - `0x101c0f27c` — `eoc::Passives::Get`
   - `0x101c0f2bc` — passive RefMap `operator[]`
   - `0x101c0d6c8`, `0x101c0d964`, `0x101c0dc0c` — ctor/Clean/dtor
   - `0x103061fbc` — complete `DoLoadStats` lambda

2. For `0x103061fbc`, disassemble and follow all branches from the `Clean()` call at `0x103062d00` to the passive-loop latch. Rename the registers holding `StatsObject*`, `PassivePrototype*`, and manager. Inventory:

   - Every store through the prototype pointer.
   - Every called parser/helper.
   - Dynamic-array and `StatsFunctorList` ownership operations.
   - Whether existing and newly inserted entries share a common population tail.

3. Query xrefs to `Passives::Parse`. If it accepts a serialized passive list rather than a `StatsObject`, reject it as the missing initializer. If the loader calls it with material derived from the stat object, it may provide a narrow reusable population route.

Runtime probes:

- First replace diagnostic lookup with native `Passives::Get`; do not trust the current generic lookup.
- Use `Ext.Stats.GetPrototypeManagerPtrs()`, `Ext.Debug.HexDump(proto, 0x210)`, `ReadPtr`, and `ReadFixedString` on a known vanilla passive.
- Mutate one scalar property, one `Boosts` property, and one functor-list property separately. Dump before/after the proposed population call and confirm that the `+0xb8` nested vptr and unrelated arrays remain valid.
- Test existing-prototype refresh first. Only test new insertion after load/unload cycles and duplicate-key behavior are clean.

Effort and risk:

- One session to decide whether `Passives::Parse` is useful.
- Two to three sessions if it is the narrow helper.
- Five to eight sessions if the post-`Clean()` loader region must be reconstructed.
- Existing-entry refresh: High risk.
- New-entry insertion plus nested ownership: Critical risk.

Judgment: the one-session `Parse` investigation is worthwhile. Full manual replay of the inlined loader is not justified unless a reproducible mod failure specifically requires passive synchronization.

### 2. Interrupt prototype sync

Precise blocker:

This is both a population and a container-transaction problem. The manager maintains buckets at `+0x8`, capacity at `+0x10`, collision links at `+0x18`, keys at `+0x28`, and contiguous `0x1f0` values at `+0x38`; lookup performs `base + index * 0x1f0` at [COMPONENT_OPS_AND_PROTO_INIT.md:460](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/COMPONENT_OPS_AND_PROTO_INIT.md:460). Reallocation moves every prototype and invalidates all previous element pointers.

The current generic cached lookup at [prototype_managers.c:441](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/stats/prototype_managers.c:441) is structurally incorrect for this manager. Windows uses `try_get`/`add_key`, clears `Costs`, then calls `Init` at [Stats.cpp:139](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/GameDefinitions/Stats/Stats.cpp:139).

Concrete RE path:

1. Decompile:

   - `0x101b7adcc` — native `GetPrototype`
   - `0x101b7a054` — prototype destructor
   - `0x103061fbc`, concentrating on `0x103063f08` through the end of interrupt construction
   - All calls and branch targets reached after the first object move at `0x103063f94`
   - All xrefs that write manager `+0x38`, `+0x28`, `+0x18`, `+0x10`, or call the destructor in a stride loop

2. Recover separately:

   - Hash growth/reserve.
   - Key insertion and collision-chain update.
   - Construction of a new trailing element.
   - Move/destruction during reallocation.
   - Per-stat population and the boundary between that population and container maintenance.

3. Do not implement insertion until a single native helper performs the reserve/add-key transaction. Reproducing only the visible arrays would be exceptionally fragile.

Runtime probes:

```lua
local m = Ext.Stats.GetPrototypeManagerPtrs().Interrupt
local cap = Ext.Debug.ReadU32(m + 0x10)
local buckets = Ext.Debug.ReadPtr(m + 0x08)
local links = Ext.Debug.ReadPtr(m + 0x18)
local keys = Ext.Debug.ReadPtr(m + 0x28)
local values = Ext.Debug.ReadPtr(m + 0x38)
```

- Bind `GetPrototype` at `0x101b7adcc`; verify its result equals `values + index * 0x1f0`.
- Dump several adjacent elements and confirm `FixedString` at `+0` and `TranslatedString` at `+8`.
- For an existing interrupt, mutate one condition/cost field and compare the exact changed regions after population.
- New-key testing must check every old key after forced growth, not merely the inserted key.

Effort and risk:

- Existing-entry population only, if cleanly separable: three to five sessions.
- Complete new-entry support: six to ten sessions.
- Corruption risk: Critical, especially across reallocation and unload.

Judgment: keep it deferred. 5e Spells edits many `InterruptData` conditions but does not call `Ext.Stats.Sync()` for them; Combat Extender does not call this API either. That is not enough demand to justify reconstructing a contiguous owning container.

### 3. `AddAttribute` / `AddEnumerationValue`

#### `AddEnumerationValue`

The blocker is now narrower than the source comments suggest. Static symbol inspection found:

```text
0x101c44920 CRPGStats_Modifier_ValueList::Insert(FixedString const&, int)
```

This likely encapsulates the value-list allocation, name map, and insertion needed by the Windows implementation at [Stats.inl:739](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Libs/Stats.inl:739).

RE path:

1. Decompile `0x101c44920` and all callers.
2. Establish duplicate behavior, returned value/void ABI, allocation ownership, and whether it updates both the value array and name map.
3. Identify the native `GetPropertyType` call used by callers so non-enumerations remain rejected.
4. Compute the proposed numeric value from the verified native size immediately before insertion, then verify lookup in both directions.

Runtime probe during `StatsStructureLoaded`:

- Record value-list count, buffer, capacity, and lookup result.
- Insert a sentinel label with the native method.
- Confirm count increased exactly once, existing labels still resolve, duplicate insertion fails cleanly, and index-to-label/label-to-index agree.
- Repeat after a stats reload before enabling it generally.

Estimate: one focused session. Risk: Medium if routed through `0x101c44920`; High if raw arrays or hash fields are written.

#### `AddAttribute`

The exact blocker is native object construction and lifetime, not the already-probed field offsets. A modifier appears to be a packed `0x10`-byte object with `EnumerationIndex +0`, `LevelMapIndex +4`, zero field `+8`, and `FixedString +0xc`, but the allocator, default initialization, and manager ownership still need proof.

Useful symbols:

```text
0x102105d2c CNamedElementManager<CRPGStats_Modifier>::Insert(Modifier*)
0x101c44a10 CRPGStats_Modifier::GetPosition(char const*)
0x102106470 RPGStats::ParseStructureFolder(...)
```

RE path:

1. Decompile `0x102105d2c` to recover manager growth, duplicate-name behavior, assigned handle, and ownership.
2. Decompile `0x101c44a10` to confirm packed name access.
3. Trace modifier allocations from `RPGStats::ParseStructureFolder` and `DoLoadStats` to establish allocation size, allocator, `LevelMapIndex == -1`, and the `+8` default.
4. Verify destruction on manager teardown. Use the same native allocation family as the loader.
5. Enforce the Windows timing rule: refuse once `RPGStats::Objects` is nonempty. Windows explicitly requires `StatsStructureLoaded` at [Stats.inl:707](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Libs/Stats.inl:707).

Runtime probe:

- At `StatsStructureLoaded`, assert Objects size is zero.
- Snapshot the target `ModifierList.Attributes` count/buffer/capacity.
- Insert one sentinel modifier through native `Insert`.
- Verify name lookup, type handle, all defaults, and existing attributes.
- Parse a test stat containing the new property and verify its indexed-property width and value.
- Refuse any test after objects exist: changing schema width then can invalidate every already-created stats object.

Estimate: two to three sessions. Risk: High when properly pre-load gated; Critical after stats objects exist.

### 4. `ExecuteFunctors`

Precise blocker:

There are three missing ownership/ABI pieces, not just “most functor parameter blocks”:

1. No real `StatsFunctorList`/`StatsFunctorBase` Lua object is supplied to the overloads.
2. Contexts are raw zeroed approximate structures without native vptrs, nested objects, entity maps, or `ClassResources`.
3. Active invocation must construct and destroy the hidden `esv::functor::Result`. The hooks merely forward an existing `result_out`; they do not teach the caller its size or lifetime.

The existing dispatcher at `0x10577399c` only accepts `AttackTargetContextData`, so it cannot safely substitute for the nine overloads; this is already recognized at [lua_stats.c:1091](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/lua/lua_stats.c:1091).

Concrete RE path:

1. Decompile list sourcing and ownership:

   - `0x101fada8c` — `StatsFunctorListManager::CreateStatsFunctorList`
   - `0x101b79ad4` — functor-list manager `Insert`
   - `0x101b7944c` — `StatsFunctorList` destructor
   - Xrefs to these functions and to individual `Clone()` virtuals

2. Decompile call sites of the three highest-value overloads:

   - AttackTarget `0x10577787c`
   - Target `0x10577a87c`
   - AttackPosition `0x105777bd0`

   Recover the stack allocation, constructor, and destructor for `Result`, plus actual context sizes and constructor calls.

3. Decompile native context constructors, beginning with:

   - AttackTarget: `0x104ce5c20`, `0x1010c3028`
   - AttackPosition: `0x1010c3dd8`
   - Target: `0x104cafc18`
   - Move: `0x1055d05a4`
   - Source: `0x1055bca0c`
   - Equip: `0x1051d5af8`

4. Mirror Windows initialization: invoke placement construction, set the exact `Type`, set `PropertyContext = TARGET | AOE`, and populate `ClassResources`; see [StatFunctors.inl:70](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Libs/StatFunctors.inl:70).

5. MVP: execute an existing game-parsed functor list. Do not start with hand-created functors. Add `CreateFunctor` only after recovering the native constructor registry or `RPGStats::ConstructFunctor`.

Runtime probes:

- Use the existing functor hooks to capture live vanilla AttackTarget, Target, and AttackPosition contexts. Hex-dump them and compare vptrs/defaults against freshly native-constructed contexts.
- Replay a harmless game-owned list against a controlled target.
- Validate pre/post hook counts, result destruction, entity ownership, and repeated invocation.
- Test malformed/nil entities before exposing writable context fields.

Effort and risk:

- Existing parsed list plus AttackTarget/Target/AttackPosition: three to five sessions.
- All nine contexts, `CreateFunctor`, cloning, and full ownership: seven to twelve sessions.
- Risk: Critical with zeroed structures; High during native-constructor bring-up; potentially Medium after every constructor/destructor and result lifetime is proven.

Real-mod value:

- Combat Extender contains no `Ext.Stats`, `ExecuteFunctor(s)`, `PrepareFunctorParams`, or `CreateFunctor` calls.
- 5e Spells also contains no direct functor API calls. Its `S5E_BootstrapShared.lua` modifies `Target_Help` and calls `Sync()` around archive lines 581–587, but uses stats text—not active `ExecuteFunctors`—for its effects.
- Across 5e Spells generated stats, the dominant action tokens are:

  1. `ApplyStatus` — 1817
  2. `DealDamage` — 1572
  3. `Summon` — 889
  4. `CreateExplosion` — 508
  5. `RemoveStatus` — 442
  6. `SpawnExtraProjectiles` — 301
  7. `RegainHitPoints` — 243

These counts include generated/upcast copies, so they are a relative signal rather than unique behaviors. Constructors already visible for this high-value set include `StatsFunctorStatus` `0x101fbb234`, `RemoveStatus` `0x101fbb354`, `Summon` `0x101fbb760`, `DealDamage` `0x101fbc08c`, `RegainHitPoints` `0x101fbc3dc`, `CreateExplosion` `0x101fbc80c`, and `SpawnExtraProjectiles` `0x101fbd86c`.

AttackTarget and Target contexts should therefore come first, followed by AttackPosition. Interrupt context is much lower priority unless a mod explicitly needs roll-adjustment functors.

## Related

Ranked attack order:

1. Bind and validate `AddEnumerationValue` through `0x101c44920`.
2. Recover modifier allocation and implement pre-load-only `AddAttribute`.
3. Implement an `ExecuteFunctors` MVP using game-owned parsed lists and native AttackTarget/Target/AttackPosition contexts.
4. Spend one bounded session decompiling `Passives::Parse`, `Passives::Get`, and their xrefs. Proceed only if a callable population helper emerges.
5. Leave interrupt sync deferred until a concrete incompatibility pays for the six-to-ten-session, critical-risk loader/container mapping.

Final judgment: passive sync is worth reconnaissance, but not a full inlined-loader reconstruction absent a usable helper. Interrupt sync should remain an honest deferral. Calling either “100% parity” after approximate struct writes would be substantially worse than reporting the capability gap accurately.

Confidence is high on the blockers and container shapes, medium on effort estimates, and intentionally low on the usefulness of `Passives::Parse` until decompiled. The Ghidra HTTP bridge was not reachable during this pass, so the new symbol leads above come from the installed binary’s local symbol table rather than fresh decompiler output.

## Address refresh for build 4.1.1.7398727 (2026-08-20)

Every address previously listed in this document is for **4.1.1.7209685** and is
wrong for the build now under test. Re-resolved from the shipped symbol table:

| Symbol | 7398727 |
|---|---|
| `CRPGStats_Modifier_ValueList::Insert(ls::FixedString const&, int)` | `0x101c42014` |
| `CNamedElementManager<CRPGStats_Modifier, int, 0, -1>::Insert(CRPGStats_Modifier*)` | `0x102103420` |
| `CRPGStats_Modifier::GetPosition(char const*)` | `0x101c42104` |
| `RPGStats::ParseStructureFolder(ls::Path const&)` | `0x102103b64` |
| `eoc::Passives::Parse(eoc::Passives const&, ls::STDString const&)` | `0x101c0ca70` |
| `eoc::Passives::Get(ls::FixedString const&) const` | `0x101c0c970` |
| `eoc::InterruptPrototypeManager::GetPrototype(ls::FixedString const&) const` | `0x101b784c0` |
| `esv::aigrid::CreatePathForCharacter(esv::Character const*, eoc::aigrid::ProcessedSettings const&)` | `0x0000000104820334` |
| `esv::aigrid::ProcessSettingsForCharacter(esv::Character const&, eoc::aigrid::UnprocessedSettings const&)` | `0x` |

### `AddAttribute`: blocker confirmed, not removed

Disassembling `CNamedElementManager<CRPGStats_Modifier>::Insert` confirms the
document's field guess and the shape of the remaining work:

    x21 = modifier + 0xC          ; FixedString name, as documented
    x8  = [[this] + 0x28]         ; virtual name-lookup
    blr x8                        ; -> existing index, or -1
    (index != -1) -> duplicate path, releases the incoming node

So `Insert` registers a modifier that the **caller has already constructed and
owns**. It does not allocate. The blocker is therefore exactly as recorded —
allocator, default initialization (`LevelMapIndex == -1`, the zero field at +8)
and manager ownership — and none of that is settled by having the address.
Shipping it on a guess risks the stats schema, which is Critical once
`RPGStats::Objects` is non-empty. Still deferred.

### `BeginPathfinding`: full ABI recovered, construction still unproven

    esv::aigrid::ProcessSettingsForCharacter(esv::Character const&,
                                             eoc::aigrid::UnprocessedSettings const&)
    esv::aigrid::CreatePathForCharacter(esv::Character const*,
                                        eoc::aigrid::ProcessedSettings const&)

This is the higher-level entry point the deferral recommended finding, and it
removes the need to populate `AiPath` by hand. It does **not** remove the need to
construct `UnprocessedSettings` correctly, which simply moves the
field-population risk to a different struct — and a malformed settings block
feeds invalid bounds into pathfinding. The request lifecycle (callback storage,
game-thread delivery, cancellation, release) is also still unaddressed. Still
deferred.

## `AddEnumerationValue` unblocked and verified (2026-08-20)

This was recorded as implemented but was gated to `4.1.1.7209685` — a build
nobody runs — so it failed closed on every runnable build. The pin's own comment
said "no live insertion has ever succeeded" and asked for a round trip on
7398727 before moving. That round trip has now been done.

Verified live on 4.1.1.7398727 with the real gate and no opt-in:

    insert 'Damage Type' / 'BG3SE_GateProof_7398727' -> 14 (integer)
    EnumLabelToIndex  -> 14        (nil before the insert)
    EnumIndexToLabel  -> the label (round trips)
    duplicate insert  -> false
    unknown enum      -> false
    Slashing/Piercing/Bludgeoning/Fire/Cold/Acid still resolve both ways
    StatusData count and Ext.Stats.Get('BLESS') unaffected

The call shape is independently corroborated by the game's own loader.
`CRPGStats_Modifier_ValueList::Insert` has exactly three callers
(`RPGStats::FinishAndCleanCurrentParsedType` and two in
`RPGStats::ParseStructureFolder`), and each calls
`Insert(list, &FixedString, index)` with the index loaded from the list's item
count. Their field accesses — item count at `+0x18`, bucket count at `+0x08`,
buckets at `+0x10` — match the offsets the port already used.

Return value corrected: Windows returns `std::optional<int32_t>` holding the new
value (`Stats.inl:633`); the port pushed a boolean, so callers using the
returned index received `true`.

## `AddAttribute` — the documented RE path does not exist

This document proposed decompiling
`CNamedElementManager<CRPGStats_Modifier>::Insert` (0x102103420 on 7398727) to
recover manager growth, duplicate-name behaviour, assigned handle and ownership.

A `BL` scan of `__text` finds **zero direct callers** of that function. The stats
loader creates modifiers through inlined code and never calls it. So there are
no call sites to learn ownership semantics from, and invoking it would exercise
a code path the shipping game never runs.

The blocker is therefore worse than recorded, not better: allocation, default
initialization and ownership cannot be inferred from observed behaviour, and the
consequence of guessing is a corrupted stats schema (Critical once
`RPGStats::Objects` is non-empty). Still deferred.

## `StaticData.GetSources` / `GetByModId`: three theories, all wrong — stopping

Windows implements both as a direct read of `bank_->ResourceGuidsByMod`
(StaticData.inl:120/125), so this reduces to one field offset. Three attempts:

1. **`+0x10`**, predicted by `GuidResourceBankBase` (vptr + 2 FixedStrings).
   Fails validation outright.
2. **`+0x00`**, inferred from the bank header dump — `Resources` appeared to sit
   at ptr+0x40, leaving 0x00..0x40 for the map. Produced a 13-entry map whose
   keys were not GUIDs and whose value arrays were all empty.
3. **Shape scan** across the first 0x400 bytes for a HashMap with equal-length
   key and value arrays. Found 14 candidates — and dumping their contents shows
   why they are all wrong:

       cand bank+0x0   n=13  raw: f2 ff ff ff f5 ff ff ff 00 00 00 00 01 00 00 00
       cand bank+0x40  n=41  raw: d1 ff ff ff e8 ff ff ff bb ff ff ff fc ff ff ff
       cand bank+0xE0  n=24  raw: e0 ff ff ff ea ff ff ff fd ff ff ff ed ff ff ff

   Those are negative int32 sentinels — hash bucket heads — not GUIDs. The scan
   was matching `StaticArray<int32_t>` arrays, so the assumed HashMap layout
   (`Keys` at map+0x20, as used elsewhere in the port) does not describe these
   banks.

Note candidate `bank+0x40` has exactly 41 entries and Feat has 41 resources, so
the arithmetic looked confirmatory while the contents disproved it. That is the
same trap as the 0x180 stride: a number agreeing does not make the
interpretation right.

**Stopping here deliberately.** Each theory has been plausible and wrong, and
the correct technique is the one that worked for TransformComponent and the
Osiris hooks: find a function that reads this field and take the offset from its
instructions, rather than deriving it from a struct that evidently does not
match. Until then both functions fail closed and return nil rather than serving
fabricated GUIDs.

### Resolved 2026-08-20: it was the container layout, not the offset

Windows' predicted `+0x10` was correct the whole time. What was wrong was the
HashMap layout applied to it.

`ls::ModdableFilesLoader` (what BG3SE calls `GuidResourceBank`) stores its
Resources table at `+0x50` — `AddLoadedObject` does `add x0, x21, #0x50` before
calling Ensure — and `vptr + 2 FixedStrings + a 0x40-byte map` lands exactly
there, confirming ResourceGuidsByMod occupies `0x10..0x50`.

The container is `ls::HashTable`, whose layout was read from
`ls::HashTable<ls::Guid, HashTableOpsDefault>::Ensure` (0x100c0217c):

    +0x00  bucket heads (int32*)
    +0x08  bucket count
    +0x20  keys buffer -- a RAW pointer to 16-byte Guids, not an Array header

Three earlier attempts applied the ECS HashMap layout (Keys as an Array header
at +0x20), so every scan matched int32 bucket-head arrays and concluded the map
was not there.

Verified: per-mod resource lists sum exactly to each bank's total — Feat 41/41,
Background 22/22, ActionResource 87/87, Race 156/156, Class 70/70 — across 13
modules. A wrong offset cannot produce that agreement.

Lesson: the failure was assuming one engine container layout applies to all of
them. Reading the container's own accessor settled in minutes what three
offset theories could not.

## `StaticData.Create`: mechanism fully recovered, not shipped

Windows' `Create` is `bank_->Resources.add_key(guid)` plus copying the VMT from
an existing resource (StaticData.inl:76). The insert is the whole problem, and
it is now understood rather than guessed.

### `ls::HashTable<K, Ops>` layout

Read from `ls::HashTable<ls::Guid, HashTableOpsDefault>::Ensure` @ `0x100c0217c`:

    +0x00  bucket heads (int32*)
    +0x08  bucket count
    +0x10  chain/next indices buffer (int32*)
    +0x18  chain capacity
    +0x1c  chain count
    +0x20  keys buffer (raw, 16-byte Guid elements)
    +0x28  key capacity
    +0x2c  key count

### The insert algorithm

    grow keys via DynamicArray<Guid, TaggedAllocator>::Reserve if full
    keys[n] = guid; keyCount++
    grow chain via DynamicArray<int, TaggedAllocator>::Reallocate if full
    next[n] = -1; chainCount++
    if (bucketCount < keyCount * 1.5) Rehash()      ; 0x100c022dc
    else link n into buckets[hash % bucketCount]

The hash is `guid.lo ^ guid.hi` (`eor x11, x9, x8`) reduced by `udiv`/`msub`.

### Why it is not implemented here

`Ensure` is exported, so the correct approach is to **call it** rather than
reimplement hashing and rehashing — which is also how `AddLoadedObject` does it:
`add x0, x21, #0x50` then `bl Ensure`, followed by separate management of the
values array at bank +0x78/+0x80/+0x88.

What stops it is not knowledge but verifiability. This mutates a live resource
bank, and a rehash rewrites every bucket in it. Windows itself refuses to expand
by default and gates growth behind `allowUnsafeExpansion` with the comment
"This leaks memory!" and a warning that it may break existing resource
references. A wrong value-slot write would corrupt a bank that the running save
depends on, and there is no cheap way to observe corruption before it matters.

Shipping a live-mutation path that cannot be verified is the pattern that caused
this session's crashes. The recovery above removes the unknowns, so an
implementation attempt can start from measurements rather than guesses whenever
it can be tested against a disposable profile.
