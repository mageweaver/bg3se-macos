I’m using the `bg3se-macos-ghidra` skill because this review hinges on the project’s reverse-engineering evidence and parity conventions. I’ll first read its instructions, then compare the draft directly against the synthesis, all four reports, the canonical deferral registry, the roadmap matrix, and the new component-ops finding.

---

The skill confirms two constraints that materially affect the critique: main-executable `__TEXT` cannot be patched, and ARM64 calling conventions—including hidden/result and promoted arguments—must be treated as first-class validation concerns. I’m now inventorying the evidence set and extracting the exact roadmap rows below 100%, rather than relying on the synthesis totals alone.

---

The first material mismatch is already clear: the owner’s mandate is a full campaign, but Phase E is still framed as “scope decision required,” and several Phase A items are only partial substitutes for the Windows contracts. I’m drilling into the component-removal/prototype report next, because D2’s generated table has hidden return-value, coverage, and update-maintenance dependencies that the draft currently compresses into a single work item.

---

The architectural rebuilds are more coupled than the draft implies. Savegame parity is not just `PersistentVars`: the Windows serializer packages PersistentVars, user variables, mod variables, and persistent timers together; dual VMs also change ownership for callbacks, timers, events, IMGUI/input, per-mod `_ENV` registries, and the network message bus. I’m mapping those ownership boundaries so the proposed go/no-go gates are executable rather than aspirational.

---

The draft is substantially stronger than the original: it now catches Types/Entity function substitution, makes Phase E mandatory, and acknowledges the four save-persisted data families. It is still not execution-safe, chiefly because the contract manifest arrives too late, dual-VM ownership arrives after features that depend on it, and several “closure” items are only partial implementations.

Also, the current matrix has nine scored rows below 100%, not six or seven: Osi, Entity, Stats, Types, Net, Level, Console, PersistentVars, and Client Lua State. See the [matrix](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ROADMAP.md:37).

## 1. Sequencing errors and missed dependencies

### 1.1 Build the contract manifest before Phase A

The manifest is currently a closeout artifact at [plan line 97](/Users/tomdimino/.claude/plans/encapsulated-sauteeing-grove.md:97). It must be Phase 0’s main output.

Without it:

- Phase A cannot know whether its Entity work closes 4, 6, or more contracts.
- `Ext.Net` still lacks a denominator.
- Proxy methods such as `entity:SetReplicationFlags`, `Replicate`, and `RemoveComponent` can disappear from an inventory derived only from module registration blocks.
- Optional-argument behavior such as `Ext.Stats.Get(name, level)` remains invisible to function-count accounting.

The manifest must inventory module functions, userdata/proxy methods, events, optional arguments, return values, side effects, and failure behavior—not merely registration names.

Phase 0 must therefore end with:

```text
Every Windows contract → implemented / behavioral gap / matched upstream TODO / excluded
```

No “unclassified” entries should be allowed before implementation begins.

### 1.2 Dual-VM ownership must move ahead of several Phase A items

Current Phase E2 starts by creating a second `lua_State` [at line 83](/Users/tomdimino/.claude/plans/encapsulated-sauteeing-grove.md:83). That is too late and in the wrong internal order.

The current runtime has:

- One global `lua_State *L`.
- One global `g_current_context`.
- Global/static state owners in console, input, IMGUI, entity events, log events, functor hooks, timers, mod environments, callbacks, and networking.
- `lua_net_process_messages(L, L)`, explicitly using the same state as both endpoints.

Before creating the second VM, introduce explicit `LuaRuntime`/state ownership and replace global context with context derived from the calling `lua_State`. Otherwise both VMs will report whichever context was most recently written globally.

This refactor is a dependency of:

- A2 custom-property registries, which must be per-state and generation-safe.
- A4 tracing, which Windows stores per Lua state.
- A6 Net callback and request ownership.
- Console client/server selection.
- Client/server variable synchronization.
- Event and timer isolation.

Recommended order:

```text
Phase 0 contract manifest
→ E2 ownership refactor and minimal two-state vertical slice
→ portable Phase A work
→ RE phases B/C/D
→ E1 savegame integration
```

The E1 save-hook feasibility spike can run early, but state migration should follow the dual-VM server-state abstraction.

### 1.3 A4 tracing depends on Phase C replication

The proposed callback-only tracing MVP cannot close the Windows contract. Windows tracing scans both entity command-buffer changes and replication dirties; construct/destroy callbacks observe only a subset. The research report prices callback-only tracing at 3–5 days, but faithful tracing at 2–3 weeks and partially dependent on replication.

Move tracing closure after C1/C2. A callback-only backend can be a prototype, but it must not earn full credit for `EnableTracing`, `GetTrace`, and `ClearTrace`.

### 1.4 A3 is not “pure engineering”

`GetHashSetValueAt` requires:

- A typed SetProxy userdata.
- Proven `ls::HashSet<T>` key/bucket/occupancy layout.
- Element conversion metadata.
- Lifetime handling.
- One-based indexing.

The evidence report estimates 1–2 weeks, not 1–2 days. Move it to Phase B as a read-only RE-gated item.

### 1.5 Phase C starts from the wrong subsystem

The proposed entry point is GameServer peer bookkeeping [at line 58](/Users/tomdimino/.claude/plans/encapsulated-sauteeing-grove.md:58). Windows does not implement `Replicate` by directly queuing peer traffic. It modifies:

```text
EntityWorld::Replication
  → SyncBuffers::ComponentPools[replicationType]
  → HashMap<EntityHandle, BitSet>
  → SyncBuffers::Dirty
```

The game’s normal serializer consumes that state later. GameServer peer arrays may help with end-to-end validation, but they are not the primary RE target.

Correct C ordering:

1. Recover ordinary-component name ↔ replicated-type index mappings.
2. Recover `EntityWorld::Replication`.
3. Recover read-only ComponentPools/HashMap/BitSet lookup.
4. Implement `GetReplicationFlags`.
5. Recover existing-entry bitset OR.
6. Recover native map insertion and BitSet growth.
7. Implement `SetReplicationFlags`.
8. Implement `Replicate(component)` as qword 0/all bits.
9. Prove downstream remote-client observation.

Do not make all of C2/C3 contingent on one coarse C1 gate. Read-only lookup may be shippable even if insertion remains blocked.

### 1.6 E1 must follow server-state ownership, but its hookability spike should start early

True save embedding has a platform-specific prerequisite the draft does not surface: the port cannot assume it can patch the main executable’s `__TEXT`. Before pricing visitor layouts, prove a usable interception point—such as an exported callback, writable dispatch/vtable entry, registration API, or an already-hookable library boundary—for both save and load.

If no safe write hook exists, knowing the `ObjectVisitor` ABI does not unlock embedding.

### 1.7 Optional/skippable D items conflict with the mandate

The owner requested a full campaign. These clauses contradict that:

- ExecuteFunctors only “if ahead of schedule.”
- Interrupt sync “never attempted.”
- A three-context ExecuteFunctors MVP treated as closure.
- One-session BeginPathfinding “settlement.”

A permanent deferral may be the result of a bounded evidence-gathering milestone. Lack of mod demand alone is insufficient under this mandate.

### 1.8 A permanent deferral is not 100%

The definition of done currently allows a scored row to say “permanent deferral” while supporting a 100% declaration. That is internally inconsistent. A proved-impossible item is closed as a planning item, but still behaviorally missing.

Final reporting should be:

```text
X% supported-surface behavioral parity,
with N permanent scored deferrals and four scope exclusions
```

Only report 100% if no scored contract remains deferred.

## 2. Omissions and incomplete coverage

The registry contains 17 table rows; after removing `Construct` as an upstream TODO, 16 remain open. The draft touches most, but several are omitted or only partially addressed.

### Entirely omitted

| Missing item | Evidence/problem |
|---|---|
| `entity:SetReplicationFlags` | Present in Windows and explicitly pending in ROADMAP; Phase C covers Get and Replicate only. |
| EntityHandle component-property writes | Explicit Tier-2 item in the synthesis; semantic ownership/reverse-index risks require an allowlist. |
| Interrupt prototype-sync reconnaissance | Listed as “never attempted,” contrary to the full-campaign mandate. At minimum, map the post-`0x103063f94` population/container transaction and set a defensible gate. |
| `Construct` validation parity | macOS currently accepts any string and returns nil. Windows errors for unknown, non-object, and non-constructible types before its TODO return. The current implementation does not yet match that contract. |
| `Ext.Stats.Get(name, level)` scaling | Still explicitly ignored in `lua_stats.c` and unchecked in ROADMAP. Function presence hides this behavioral gap. |
| User/mod variable client-server synchronization | ROADMAP says it remains unimplemented; dual VM and Net must include it. |
| Roadmap “partial” adjudication | Ext.Mod, StaticData verification, IMGUI fonts/textures/style, component coverage, debug inspector/profiler, and technical-debt checklists remain unresolved or stale. The plan must either include them or explicitly mark them stale/non-parity with owner approval. |

### Present, but insufficient to close the contract

| Draft item | Required strengthening |
|---|---|
| A2 custom properties | Component-only support is partial. Windows uses the shared property-map system across object proxies. Either generalize it or score component-only support as partial. |
| A3 HashSet | A raw known layout is insufficient; implement typed, lifetime-aware SetProxy behavior. |
| A4 tracing | Callback create/destroy logging misses ECB changes, updates, and replication dirties. |
| A7 float arrays | No real component field currently uses the type, so it cannot be behaviorally accepted until a verified field exists. |
| B2 passive sync | One-session `Parse` inspection is a discriminator, not an implementation milestone. Define existing-entry refresh versus new-entry insertion separately. |
| B3 tile debug | A raw diagnostic is useful but is not Windows `GetTileDebugInfo` parity. Full min-height and public flag conversion remain required. |
| B4 raycasts | Sweep emulation is not raycast parity. Record it as a fallback extension, not an implementation. |
| D2 removal | Covers only the 734 emitted specializations, not roughly 1,999 registered types. Publish exact supported coverage. |
| D5 ExecuteFunctors | Three contexts are an MVP; full parity requires every Windows-supported context and native result/context lifecycle. |
| D6 BeginPathfinding | A callable address alone is insufficient; callbacks, cancellation, path ownership, unload, and release must all be proven. |
| E1 migration fallback | Continually falling back to global JSON preserves cross-save leakage. File data should be a one-time import into a specific save, followed by save-owned authority. |

### Roadmap reconciliation required

Several unchecked ROADMAP entries are already implemented or contradicted elsewhere:

- Stats type filtering.
- Stats Create and Sync.
- StaticData auto-capture claims.
- Regression/unit testing checkboxes despite the 509-test suite.

Add a Phase 0 “stale pending-item adjudication” pass: every unchecked entry becomes verified-complete, an active Wave 7 item, an explicit exclusion, or removed as obsolete. Nothing should remain silently stale.

## 3. Risk and effort corrections

### Replication

The 2–4 week technical estimate is plausible only for a successful, symbol-rich path. A release-quality campaign including a multiplayer harness is more credibly 4–8 weeks.

Underpriced risks:

- Native HashMap insertion and BitSet growth are the corruption boundary.
- The dirty byte’s consumption/clearing lifecycle must be observed.
- Replication-type mapping is distinct from ordinary component TypeIds.
- Client-side use must fail without changing server memory.
- Local single-player transport proves almost nothing.
- Two BG3 processes require identity, save isolation, deterministic connection, and remote-state observation—not simply “both launched.”

Overpriced/misdirected element:

- Recovering a peer sync queue is probably unnecessary for the API. Let the engine consume `SyncBuffers::Dirty`; observe the downstream packet only as validation.

Acceptance must cover the eight demanded component families identified by the report: ActionResources, God, CombatParticipant, AvailableLevel, EocLevel, Classes, GameObjectVisual, and Stats.

### D2 RemoveComponent table

The current 1–2 week first-build estimate is reasonable for generating addresses, but not for proving correct Lua behavior.

Hidden work:

- Emit `(canonical name, TypeId-global RVA, remover RVA)`, not fixed numeric indices.
- Read the live TypeId at startup and cross-check the component registry.
- Validate every remover lies in executable text.
- Audit representative bodies and shared-tail clusters; `nm` proves an address, not an ABI.
- Use `EntityWorld+0x3f0` for `ImmediateWorldCache`; `+0x390` is the ComponentOps registry.
- Recover change-map membership. Native removers return `void`, and `GetChange()` returning null is ambiguous because null also represents a deletion marker.
- Distinguish missing component, new removal, already-pending removal, and pending-add cancellation.
- Test destruction callbacks and post-flush state, not only the immediate call.
- Publish `734/N` coverage; unmapped types remain a real parity deficit.

“About one day per update” is optimistic. Automated extraction may take one day, but ABI sampling, manifest audit, and live destructive tests make 2–5 days per build more credible.

### Other corrections

| Item | Draft | Better price/risk |
|---|---:|---|
| Full custom-property layer | 3–8 days | 2–4 weeks; medium Lua lifetime/reset/reentrancy risk |
| HashSet proxy | 1–2 days | 1–2 weeks; medium read/lifetime risk |
| Faithful tracing | 3–5 days | 2–3 weeks after replication; medium performance/reentrancy risk |
| Raycast chain | 1–2 weeks phase total | Roughly 5–7 focused sessions plus stress testing; closest remains critical |
| Full ExecuteFunctors | Optional 3–5-session MVP | 7–12 sessions; high/critical during constructor/destructor recovery |
| BeginPathfinding | One recon session | 3–4 sessions if high-level ABI is callable; 5–8 for credible lifecycle parity |
| Dual VM | “Engineering-heavy, low RE” | High engineering/concurrency risk; likely 4–8 weeks |
| Savegame embedding | One architectural item | Critical hook/ABI/data-integrity risk; likely 4–8+ weeks |
| Entire Phase E | 1–2 months | 2–4 months is safer, depending on save-hook availability |

The new `Passives::Get` finding is a project-wide warning: local symbol mangling cannot be trusted as the callable ABI. Its `const&` became a FixedString index passed by value [in the actual body](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/COMPONENT_OPS_AND_PROTO_INIT.md:535). Every new local symbol—including both Stats insertion leads and pathfinding helpers—needs body-level argument tracking and representative caller analysis for every supported build.

Also split the B4 gates: failure of RaycastAny’s optional ABI does not logically prove the empty-function RaycastClosest path impossible, and failure of Closest must not invalidate a working Any/All implementation.

## 4. Enforceable acceptance criteria by phase

The baseline gate before and after every phase should be:

```bash
cd build
cmake --build . --target bg3se_test_tier0
./bin/bg3se_test_tier0                    # 55/55 baseline

cd ..
PYTHONPATH=tools pytest tests/harness/ -v # 245/245 baseline

echo '!identity' | nc -U /tmp/bg3se.sock
echo '!test' | nc -U /tmp/bg3se.sock      # 113/113 baseline
echo '!test_ingame' | nc -U /tmp/bg3se.sock # 96/96 baseline
```

After new tests land, require the new total to pass with zero skip/xfail; do not use “≥530 tests” as the quality gate.

### Phase A

Tests must enforce:

- Contract scanner reports all nine discounted rows and zero unclassified Windows contracts.
- Osi test inserts at least three sentinel tuples, retrieves with partial filters/nils, deletes one exact/partial match, and proves unrelated rows remain.
- UUID/handle conversions round-trip a live entity and reject stale/invalid handles.
- Entity enumeration returns the same canonical handle set across UUID and component filters.
- `GetEntitiesAroundPosition` tests inside, boundary, and outside radii.
- Registered-component filtering tests one-frame and mapped flags.
- Custom property tests collisions, getter/setter behavior, read-only errors, recursion protection, state reset cleanup, and server/client isolation.
- `Construct` tests all Windows validation errors plus valid constructible-type/no-result behavior.
- Any float-array writer test uses a verified real field, round-trips exact values, restores the original, and rejects wrong length, NaN, and infinity.

### Phase B

Tests must enforce:

- Every new address has an offset-manifest recipe and unknown-build fail-closed test.
- Local-symbol wrappers have stored disassembly/ABI assertions, not only `nm` matches.
- Enumeration insertion grows exactly once, resolves label↔index both ways, rejects duplicates, and survives stats reload.
- Passive sync, if enabled, independently verifies scalar, Boosts, and functor-list population without changing the nested vptr.
- HashSet access tests index 1, final index, zero, and out-of-range, plus element conversion and expired-proxy behavior.
- Tile debug min/max values agree with `GetHeightsAt` across flat terrain, slopes, and a surface/cloud tile.
- Raycast tests include no hit, one hit, multiple hits, mask exclusion, ordering, thousands of repeated calls, and level unload/reload without memory growth.

A raw tile result or sweep emulation must be tested under a differently named diagnostic API and must not flip the parity contract to implemented.

### Phase C

Offline tests:

- Component name ↔ replication index mapping is deterministic.
- Pool bounds, qword bounds, BitSet growth, dirty-state transitions, and unsupported components fail safely.
- Unknown builds expose no write-capable API.
- `GetReplicationFlags` and `SetReplicationFlags` are registered on the entity proxy.

Multiplayer test:

1. Launch host and remote client with distinct verified identities.
2. Record host flags and a remotely observable component value.
3. Mutate on the host.
4. Call `SetReplicationFlags` and `Replicate`.
5. Assert flags changed only when new bits were added.
6. Assert the remote client observes the change within a bounded timeout.
7. Repeat for existing and newly inserted flag-map entries.
8. Repeat rapid mutations, reconnect, save/load, and level transition.
9. Assert a client-side replication attempt fails closed.
10. Run all 11 compatibility scenarios afterward.

### Phase D

Tests must enforce:

- `AddAttribute` succeeds only during `StatsStructureLoaded`, permits parsing a stat using the new field, and refuses once `RPGStats::Objects` is nonempty.
- Remove-table offline audit validates all 734 rows, unique canonical names, TypeId globals, executable remover targets, and exact build gating.
- Runtime removal covers tag, inline, proxy, non-trivially destructible, transient, missing, pending-add, already-pending-remove, and callback cases. Verify after ECB flush and restore the disposable save.
- FixedString writes return strings, transfer ownership transactionally, restore originals, and survive a high-iteration replacement loop without refcount drift or crash.
- EntityHandle writes accept only live same-world handles on a per-field allowlist and reject stale/cross-world/relationship-sensitive fields.
- ExecuteFunctors uses native constructors/destructors and proves every supported context, result lifetime, pre/post hook pairing, and repeated invocation.
- BeginPathfinding proves callback-once behavior, cancellation, unload cleanup, path removal, invalid entities, and no double release.
- Interrupt sync either passes existing-entry and growth/reallocation tests or produces instruction-level evidence supporting permanent deferral.

### Phase E

Acceptance is milestone-specific below, but the phase-level gate is:

- Server and client globals are isolated.
- Both contexts pass their relevant Tier-1/Tier-2 subsets.
- Cross-VM Net request/reply works in both directions.
- Resetting one VM leaves the other alive.
- Save A and Save B restore different values with no global-file bleed.
- PersistentVars, user vars, mod vars, and persistent timers all round-trip.
- Vanilla BG3 can load an extender-bearing save.
- Full 509-baseline-plus-new-tests and all 11 mods pass after repeated reset/save/load cycles.

## 5. Tier-3 milestone decompositions

### E1: savegame-owned persistence

#### E1.0 — Contract and fixture definition

Define the serialized region, version field, per-mod identifier, size limits, future-version behavior, and the four payload families. Create two disposable save fixtures with deliberately different values.

Gate: no implementation until the expected A/B slot semantics and migration behavior are written as tests.

#### E1.1 — Hook-surface feasibility

Locate both save-write and load-read visitor boundaries and prove they are interceptable without unsafe main-`__TEXT` patching. Capture the visitor pointer, direction, owning thread, and lifecycle.

Go: repeatable observation of both directions on disposable saves.

No-go: no writable hook/registration/vtable boundary. A per-save sidecar may then be a fallback improvement, but cannot be called save-embedded parity.

#### E1.2 — ObjectVisitor ABI proof

Recover and version-gate `EnterRegion`, `ExitRegion`, `EnterNode`, `ExitNode`, count, integer, FixedString, and STDString operations. Build an offline mock visitor so schema tests do not require BG3.

Gate: mock round-trip, nested-region balance, malformed counts, unknown versions, and size caps all pass.

#### E1.3 — Read-only extender-region reader

Read a known extender-bearing save without mutating it. Validate region discovery, version parsing, per-mod payload boundaries, and ignorable unknown fields.

Gate: repeated reads are deterministic and do not alter the save.

#### E1.4 — Minimal write proof

Write a small versioned sentinel region into a copied save, then verify:

- BG3 loads it with the extender.
- BG3 loads it without the extender.
- Re-saving preserves base-game integrity.
- Repeated saves replace/update rather than duplicate the region.

Failure to satisfy vanilla loadability is a hard no-go.

#### E1.5 — Server snapshot and restore layer

Create a Lua-independent snapshot while holding the correct server-state lock/gate. Serialize PersistentVars, user vars, mod vars, and persistent timers. Restore before `SessionLoaded`.

Gate: no Lua calls occur from an unsafe serializer thread, and restore ordering is deterministic.

#### E1.6 — Migration

Import existing global JSON once into the currently loaded save, record completion in the embedded schema, then make the save authoritative. Do not continue global fallback indefinitely.

Gate: Save A and Save B remain independent after migration and after deleting the old JSON files.

#### E1.7 — Hardening

Test autosave, manual save, quicksave, overwrite, corrupted extender region, future version, missing mods, removed mods, very large payloads, save/load loops, and crash interruption.

### E2: true dual client/server Lua VMs

#### E2.0 — State-ownership audit

Inventory every raw `lua_State *`, registry reference, callback queue, timer, subscription, module cache, `_ENV`, and global context variable. Introduce a state/generation owner abstraction.

Gate: static tests reject new unowned global Lua references.

#### E2.1 — Context refactor

Make `Ext.GetContext`, `IsServer`, and `IsClient` derive from the calling VM, not global mutable state. Separate server/client lifecycle and generation IDs.

Gate: two synthetic states simultaneously report different contexts.

#### E2.2 — Two registered but script-empty VMs

Create both states, register common APIs, and expose only role-appropriate capabilities. Keep bootstraps disabled initially.

Gate: both states initialize and shut down repeatedly with balanced registries and no stale published pointers.

#### E2.3 — Bootstrap and module isolation

Load only `BootstrapServer.lua` into server and only `BootstrapClient.lua` into client. Maintain separate `Mods`, `ModuleUUID`, `_ENV`, `require()` cache, loaded-file set, and custom-property registry.

Gate: sentinels and loaded modules in one VM are invisible in the other.

#### E2.4 — Event and thread routing

Route:

- Osiris, authoritative Stats, and server entity events to server.
- Input, IMGUI, and client game-state events to client.
- Shared events/timers according to the Windows contract.
- Deferred callbacks only to their owning VM/generation.

Gate: every event test asserts both “received by correct VM” and “not received by wrong VM.” No VM may be entered from an unapproved thread.

#### E2.5 — Cross-VM Net and variable sync

Use the existing `message_bus_process(server_L, client_L)` split for real. Make request callbacks state-owned, transfer only serialized payloads, and implement user/mod variable synchronization flags.

Gate: server→client, client→server, request/reply, timeout, reset-with-pending-request, and variable conflict cases pass.

#### E2.6 — Console and independent reset

Implement `server`, `client`, `reset server`, `reset client`, and `reset` with state-safe command queues.

Gate: console-created globals appear only in the selected VM; resetting one VM invalidates only its references and leaves the other responsive.

#### E2.7 — Lifecycle and compatibility hardening

Exercise module load, session load, level transitions, reconnect, save/load, repeated independent resets, shutdown, and a long-running memory soak.

Gate: all tests plus the 11-mod matrix pass with no stale-state callback, cross-state registry access, or monotonic memory leak.

In short: move the contract manifest and dual-state ownership to the front, aim Phase C at ECS `SyncBuffers` rather than peer queues, expand D2’s return/coverage validation, and make every “MVP” explicitly partial until the full Windows behavior passes tests. No files were modified.