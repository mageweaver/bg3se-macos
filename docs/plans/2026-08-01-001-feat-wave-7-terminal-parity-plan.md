# Wave 7: Terminal Parity — Close Every Deferral or Prove It Closed

**Project:** /Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos
**On approval, persist as:** `docs/plans/2026-08-01-001-feat-wave-7-terminal-parity-plan.md`, with the full planner critique as `...-plan-critique.md` alongside it (plan-mode restricts writes now; critique currently at scratchpad `planner-critique.md`).
**Scope decision (Tom, 2026-08-01):** full campaign including the Tier-3 architectural rebuilds.
**Validation:** draft critiqued by a Codex planner (gpt-5.6-sol, high reasoning, 221k tokens); all material findings folded in below. Registration-block claims verified directly against the Windows reference source during planning.

## Context

Wave 6 re-baselined parity with behavioral accounting and made `docs/deferrals.md` the scope authority. Planning-phase verification against the Windows registration blocks then exposed the last inflation mechanism: **function substitution** — macOS rows earn credit for macOS-only extras while genuinely-registered Windows functions are absent. Two rows verified during planning:

- **Ext.Types**: Windows registers exactly 14 functions (`Types.inl` RegisterTypesLib), and `AddCustomFunction`/`AddCustomProperty` are among them — *functional*, implemented as Lua-side custom-property registration (`GetCustomProperties().RegisterProperty`), not engine calls. The deferral registry's claim that they sit "outside the 15-function Windows baseline" is wrong (the 15 was manufactured; GetComponentLayout/GetAllLayouts/GenerateIdeHelpers are macOS extras Windows never registers). Honest Types today: **10/13** (denominator 14 − Construct, upstream `// TODO`); missing GetHashSetValueAt, AddCustomFunction, AddCustomProperty.
- **Ext.Entity**: Windows' 26 registrations include `HandleToUuid`, `UuidToHandle`, `GetAllEntitiesWithUuid`, `GetEntitiesAroundPosition`, `Create`, `Destroy`, `OnSystemUpdate`, `OnSystemPostUpdate`, `GetTrace`, `ClearTrace`, `GetRegisteredComponentTypes`, and an `OnChange` alias — most absent on macOS. The Wave 6 "22/26" assumed only the four admitted stubs were missing; the true behavioral score must come from a per-function diff.

Nine scored matrix rows sit below 100%: Osi, Entity, Stats, Types, Net, Level, Console, PersistentVars, Client Lua State.

Wave 7's mandate: address **all** deferrals and roadmap pending items, as close to 100% as possible. Every item ends in one of two states — **implemented and live-verified**, or **converted to a documented permanent deferral with instruction-level evidence**. Nothing stays "pending," and nothing scores as closed while only partially matching the Windows behavior.

**Definition of done (honest form):** publish "**X% supported-surface behavioral parity, N permanent scored deferrals, 4 scope exclusions** (Ext.UI, DAP, Virtual Textures, Input Injection)" — backed by a versioned contract manifest, conformance tests that invoke behavior, and a matrix where every scored row is 100% or reads "permanent deferral (evidence: …)". A proved-impossible item closes as a *planning* item but remains behaviorally missing — "100%" is claimed only if no scored contract stays deferred.

## Phase 0 — Wave 6 closeout + contract manifest (~2–3 days)

The Wave 6 working tree is complete but uncommitted. Phase 0's primary output is the **contract manifest** — moved here from closeout, because Phase A cannot know what it closes without a denominator.

1. **Correct the Types row** to 10/13 (76.9%) across the five doc surfaces; fix the wrong deferral-registry claim (AddCustomFunction/AddCustomProperty are inside the Windows surface — scored gaps Phase A closes for real points).
2. **Entity contract diff** — enumerate the 26 Windows registrations (`Entity.inl:291-325`) against macOS; re-score behaviorally; add registry entries for each missing function (verify each individually; some may exist under other names).
3. **Contract manifest v1** (`docs/parity-100/CONTRACT.md` + machine-readable JSON): inventory of *every* Windows contract — module functions, **userdata/proxy methods** (entity:SetReplicationFlags, Replicate, RemoveComponent live here, invisible to registration-block scans), events, **optional arguments** (e.g. `Ext.Stats.Get(name, level)`), return values, side effects, failure behavior. Every entry classified: `implemented / behavioral gap / matched upstream TODO / excluded`. **Zero unclassified entries before implementation begins.** `parity scan` gains `--contract` mode scoring against it.
4. **Stale pending-item adjudication** — every unchecked ROADMAP entry becomes verified-complete, an active Wave 7 item, an explicit exclusion, or removed as obsolete (several are already implemented: stats type filtering, Create/Sync, StaticData auto-capture, the testing checkboxes vs the 509-test suite). Nothing remains silently stale.
5. **Recompute overall parity** (~96.0% or below) consistently in ROADMAP/CLAUDE.md/README/api-status/supported-mods.
6. Four-lens review over the full Wave 6 diff, then commit v0.42.0 including `docs/parity-100/` (never `lib/Dobby`, `tools/vendor/insert_dylib`). Tom decides push/tag.

## Phase 1 — Dual-VM state ownership (E2.0–E2.2, pulled forward; ~1–2 weeks)

The planner's central sequencing finding: A2 custom properties, A8 tracing, A6 net callbacks, console switching, and variable sync all depend on **per-state ownership**. Today the runtime has one global `lua_State *L`, one global `g_current_context`, global/static owners in console, input, IMGUI, entity events, log events, functor hooks, timers, mod environments, callbacks, networking — and `lua_net_process_messages(L, L)` using the same state as both endpoints. Building Phase A against that guarantees rework.

- **E2.0 State-ownership audit**: inventory every raw `lua_State*`, registry ref, callback queue, timer, subscription, module cache, `_ENV`, and global context variable. Introduce a `LuaRuntime` owner abstraction with generation IDs. Gate: static tests reject new unowned global Lua references.
- **E2.1 Context refactor**: `Ext.GetContext`/`IsServer`/`IsClient` derive from the calling VM, not global mutable state. Gate: two synthetic states simultaneously report different contexts.
- **E2.2 Two registered, script-empty VMs**: create both states behind lua_gate, register role-appropriate APIs, bootstraps disabled. Gate: repeated init/shutdown with balanced registries, no stale published pointers, and **no regression of the 11-mod compat matrix** (single-VM behavior preserved until E2.3+).

E2.3–E2.7 (bootstrap isolation, routing, cross-VM net, console, hardening) land later as Phase E — see below.

## Phase A — Portable engineering on the manifest (~2–3 weeks)

| Item | Closes | Approach | Effort |
|---|---|---|---|
| A1. Entity cheap-contract closure | 4–6 Entity registrations | `HandleToUuid`/`UuidToHandle` (Uuid↔Handle mapping component already captured — `src/entity/guid_lookup.h`, `entity_system.c:827`); `GetAllEntitiesWithUuid` (filter over archetype walk); `GetRegisteredComponentTypes`; `OnChange` alias; `GetEntitiesAroundPosition` (position filter or Level delegate) | 2–3 days |
| A2. Custom-property layer | Types AddCustomFunction/AddCustomProperty | Mirror the Windows design: property registry consulted by proxy `__index`/`__newindex` (`Types.inl:328,347`) — but **per-state and generation-safe** (built on Phase 1 ownership). Windows applies it across object proxies, not just components: component-only support scores as *partial* until generalized. Tests: collisions, read-only errors, recursion protection, state-reset cleanup, server/client isolation | 2–4 weeks |
| A3. Construct validation parity | Construct contract match | macOS currently accepts any string and returns nil; Windows errors for unknown, non-object, and non-constructible types *before* its TODO return. Match that error surface | 1 day |
| A4. `Ext.Stats.Get(name, level)` scaling | Stats behavioral gap | The level argument is explicitly ignored in `lua_stats.c`; function presence hides the gap. Implement leveled lookup or document as scored gap with evidence | 1–3 days |
| A5. Osi.* residual | Osi row settled | Finish and live-verify `Osi.DB_*` filtered retrieval and deletion (partial-tuple `DB:Delete`). Test: insert ≥3 sentinel tuples, retrieve with partial filters/nils, delete one exact/partial match, prove unrelated rows remain | 1–2 days |
| A6. `PlayerHasExtender` | Net module functions → 100 | Windows client {PostMessageToServer, IsHost, Version} + server {BroadcastMessage, PostMessageToClient, PostMessageToUser, PlayerHasExtender, IsHost, Version} (`ClientNet.inl:37`, `ServerNet.inl:101`); macOS lacks only PlayerHasExtender. Implement over the PeerVersion handshake; callbacks become state-owned per Phase 1. Note per-context split for E2 routing | 0.5–1 day |
| A7. Float-array component writer | component-writes deferral shrinks | Mirror the INT32_ARRAY writer (`src/entity/component_property.c`) — but **only behind a verified real field** that uses the type; no field, no credit. Test: round-trip exact values, restore original, reject wrong length/NaN/infinity | 0.5–1 day + field discovery |
| A8. Tracing **prototype** | groundwork for C-gated closure | Callback-backed change log over EntityEvents salted-pool infra. Explicitly *partial*: Windows tracing scans command-buffer changes and replication dirties, which callbacks can't see. Full EnableTracing/GetTrace/ClearTrace credit lands **after C steps 3–4** | 3–5 days |

Moved out of A: GetHashSetValueAt → Phase B (it needs a typed SetProxy, proven `ls::HashSet<T>` layout, element conversion, lifetime handling — 1–2 weeks of RE-gated work, not 1–2 days).

## Phase B — Cheap RE unlocks, bounded sessions with go/no-go (~2–3 weeks)

Discipline per item (Wave 6 native-getter template): disassemble first (fat-slice `0xf558000`; **body-level argument tracking on every local symbol** — the `Passives::Get` LTO arg-promotion lesson applies to both Stats insertion leads and pathfinding helpers), bind through the offset table with nm-audited manifest recipes, fail closed on unknown versions, live-verify with a new Tier-2 test.

| Item | Evidence in hand | Go / No-go |
|---|---|---|
| B1. `AddEnumerationValue` | `CRPGStats_Modifier_ValueList::Insert(FixedString const&, int)` at `0x101c44920` | Go if Insert handles growth internally. Test: grows exactly once, label↔index both ways, rejects duplicates, survives stats reload. No-go → permanent deferral with Insert disassembly |
| B2. `Passives::Parse` recon → passive sync | `eoc::Passives::Parse` `0x101c0f37c`; ctor/Clean/dtor verified; DoLoadStats lambda mapped | One session **discriminates**; implementation is a separate milestone. Define existing-entry refresh vs new-entry insertion separately; verify scalar, Boosts, and functor-list population without touching the nested vptr. Parse takes loader-internal material → no-go, sync stays honestly false |
| B3. `GetTileDebugInfo` | Raw flags, ground/cloud masks, max height recovered (`AIGRID_PATHFINDING.md`); MinHeight +0x0a OPEN | Raw variant ships as a **differently-named diagnostic API** (not parity credit). Parity requires MinHeight confirmation + public flag conversion; validate min/max agree with `GetHeightsAt` across flat terrain, slopes, surface/cloud tiles |
| B4. Raycast chain | Zeroed-aggregate hypothesis: `RaycastAny` worker `0x105c58adc` internal scene-lock path when the by-value `ls::Optional` is zeroed; `RaycastClosest` wrapper `0x105c4e784` branches on a null method-table field | **Split gates**: B4a Any ABI-shim proof → B4b All + PhysicsHitAll lifetime → B4c Closest empty-function semantics. Failure of one leg does **not** condemn the others — a working Any/All ships even if Closest stays deferred. ~5–7 focused sessions + stress tests (no-hit, one, multiple, mask exclusion, ordering, thousands of calls, level unload/reload without memory growth). Sweep emulation, if used, is a fallback *extension*, never raycast parity |
| B5. GetHashSetValueAt (from A) | Windows contract is 1-based | Typed, lifetime-aware SetProxy over proven `ls::HashSet<T>` layout. Tests: index 1, final index, zero, out-of-range, element conversion, expired-proxy behavior. 1–2 weeks |
| B6. `OnSystemUpdate`/`OnSystemPostUpdate` recon | Windows subscribes to ECS system-update phases; macOS has tick/functor hook infra | 1 recon session: locate the dispatch. Callable → implement; not → registry entry with evidence |

## Phase C — Replication (~4–8 weeks, release-quality incl. multiplayer harness)

5 of 11 vetted mods call `entity:Replicate()`. Non-excludable. **Retargeted per the critique**: Windows does not queue peer traffic — it marks state the engine's own serializer consumes:

```
EntityWorld::Replication → SyncBuffers::ComponentPools[replicationType]
  → HashMap<EntityHandle, BitSet> → SyncBuffers::Dirty
```

GameServer peer arrays (+0x650/+0x65c, `ghidra/offsets/NETWORKING.md`) are end-to-end *validation* aids, not the RE target. Replication-type indices are **distinct from ordinary component TypeIds** — the mapping is its own recovery item.

Ordered steps, each with its own gate (not one coarse C1 gate — read-only lookup may ship even if insertion stays blocked):

1. Recover component-name ↔ replicated-type-index mapping.
2. Recover `EntityWorld::Replication` on macOS.
3. Recover read-only ComponentPools/HashMap/BitSet lookup.
4. **Ship `GetReplicationFlags`** (entity proxy, read-only — safe alone).
5. Recover existing-entry BitSet OR.
6. Recover native map insertion + BitSet growth (**the corruption boundary** — native paths only, no manual container surgery).
7. **Ship `SetReplicationFlags`** (previously omitted from the plan; present on Windows and pending in ROADMAP).
8. **Ship `entity:Replicate(component)`** as qword 0/all-bits.
9. Prove downstream remote-client observation; observe the dirty byte's consumption/clearing lifecycle.

**Multiplayer harness built before step 8**: two BG3 instances with distinct verified identities (`!identity`), isolated saves, deterministic localhost connection, remote-state observation. Acceptance: mutate on host → SetReplicationFlags/Replicate → remote client observes within bounded timeout; existing and newly-inserted flag-map entries; rapid mutations, reconnect, save/load, level transition; **client-side replication attempts fail closed without touching server memory**; all 11 compat scenarios re-run after. Coverage spans the eight demanded component families: ActionResources, God, CombatParticipant, AvailableLevel, EocLevel, Classes, GameObjectVisual, Stats.

**C-gated closure of A8**: after steps 3–4, wire replication dirties into the tracing backend and score EnableTracing/GetTrace/ClearTrace for full credit.

## Phase D — High-risk ladder, evidence-gated (no optional skips) (~3–5 weeks)

Full-campaign mandate: nothing here is "if ahead of schedule." Each item gets, at minimum, a **bounded evidence-gathering milestone** whose output is either an implementation or instruction-level permanent-deferral evidence.

1. **D1. `AddAttribute`** — pre-load-gated via `CNamedElementManager<CRPGStats_Modifier>::Insert` `0x102105d2c`. Tests: succeeds only during `StatsStructureLoaded`, a stat parses using the new field, refuses once `RPGStats::Objects` is nonempty. 2–3 sessions.
2. **D2. `RemoveComponent` dispatch table** — per-build table over the 734 `ImmediateWorldCache::RemoveComponent<T>` instantiations. Hardened per critique: emit `(canonical name, TypeId-global RVA, remover RVA)` — not fixed indices; read live TypeIds at startup and cross-check the registry; validate removers lie in executable text; audit representative bodies and shared-tail clusters (`nm` proves an address, not an ABI); use `EntityWorld+0x3f0` for ImmediateWorldCache (+0x390 is ComponentOps); recover change-map membership — native removers return `void` and a null `GetChange()` is ambiguous with a deletion marker, so distinguish missing / new removal / already-pending / pending-add-cancellation; test destruction callbacks and post-ECB-flush state on a disposable save. **Publish `734/N` coverage** — unmapped types remain a real deficit. First build 1–2 weeks; **2–5 days per game update** (not "~1 day"). Generic rewrite stays OUT.
3. **D3. FixedString + EntityHandle component writes** — FixedString: recover DecRef, transactional ownership swap, per-field allowlist; high-iteration replacement loop without refcount drift. EntityHandle (previously omitted; explicit Tier-2 item): accept only live same-world handles on a per-field allowlist, reject stale/cross-world/relationship-sensitive fields. 1–2 weeks.
4. **D4. `Ext.Entity.Create`/`Destroy`** — engine entity lifecycle RE. Recon first (likely same EntityWorld machinery as CreateComponent); go/no-go on allocator/handle-mint evidence.
5. **D5. `ExecuteFunctors`** — an AttackTarget/Target/AttackPosition MVP is explicitly *partial*; parity requires every Windows-supported context with native constructors/destructors, result/context lifetime, pre/post hook pairing, repeated invocation. 7–12 sessions, high risk during ctor/dtor recovery. Bounded evidence milestone first; permanent deferral requires that evidence, not just the demand analysis.
6. **D6. `BeginPathfinding` settlement** — `CreatePathForCharacter` (client `0x102de3d78`, server `0x104815680`). A callable address is insufficient: prove callback-once behavior, cancellation, unload cleanup, path removal, invalid entities, no double release. 3–4 sessions if the high-level ABI is callable; 5–8 for credible lifecycle parity.
7. **D7. Interrupt prototype-sync recon** (previously "never attempted" — contradicts the mandate): one bounded session mapping the post-`0x103063f94` population/container transaction. Output: a defensible go/no-go gate or instruction-level permanent-deferral evidence.

## Phase E — Tier-3 architectural rebuilds (~2–4 months)

### E1. Savegame-owned persistence (PersistentVars + user vars + mod vars + persistent timers — all four families travel together, `SavegameSerializer.inl:31,51`)

The **E1.1 hook-surface feasibility spike runs early** (can overlap Phase B): the port cannot patch main-executable `__TEXT`, so before pricing visitor layouts, prove an interception point for both save-write and load-read (exported callback, writable dispatch/vtable entry, registration API, or hookable library boundary). No safe write hook → knowing the ObjectVisitor ABI unlocks nothing; a per-save sidecar becomes the fallback *improvement* but is never called save-embedded parity.

- **E1.0 Contract & fixtures**: serialized region schema, version field, per-mod identifier, size limits, future-version behavior, all four payload families; two disposable save fixtures with deliberately different values. No implementation until A/B slot semantics and migration behavior exist as tests.
- **E1.1 Hook feasibility** (early): repeatable observation of both directions on disposable saves = go.
- **E1.2 ObjectVisitor ABI proof**: EnterRegion/ExitRegion/EnterNode/ExitNode, count, integer, FixedString, STDString ops — version-gated, with an **offline mock visitor** so schema tests don't require BG3. Gate: mock round-trip, nested-region balance, malformed counts, unknown versions, size caps.
- **E1.3 Read-only reader**: parse an extender-bearing save without mutating it; deterministic repeated reads.
- **E1.4 Minimal write proof**: sentinel region into a *copied* save. Hard gates: BG3 loads it with the extender, **without** the extender (vanilla-loadable is a hard no-go if violated), re-save preserves integrity, repeated saves replace rather than duplicate.
- **E1.5 Server snapshot/restore layer**: Lua-independent snapshot under the correct lock/gate; restore before SessionLoaded; no Lua calls from unsafe serializer threads.
- **E1.6 Migration**: **one-time import** of the global JSON into the currently-loaded save, recorded in the embedded schema; the save then becomes authoritative. No indefinite JSON fallback (it preserves the cross-save-leakage bug). Gate: Save A and Save B independent after migration and after deleting the old JSON.
- **E1.7 Hardening**: autosave, quicksave, overwrite, corrupted region, future version, missing/removed mods, large payloads, save/load loops, crash interruption.

### E2. Dual-VM completion (E2.3–E2.7; E2.0–E2.2 landed in Phase 1)

- **E2.3 Bootstrap/module isolation**: BootstrapServer.lua → server VM only, BootstrapClient.lua → client only; separate Mods/ModuleUUID/`_ENV`/require-cache/loaded-file set/custom-property registry. Gate: sentinels in one VM invisible in the other.
- **E2.4 Event & thread routing**: Osiris + authoritative Stats + server entity events → server; input/IMGUI/client game-state → client; shared events/timers per Windows contract; deferred callbacks only to owning VM/generation. Gate: every event test asserts "received by correct VM" **and** "not received by wrong VM"; no VM entered from unapproved threads. **Every mod re-vetted before E2.4 proceeds past E2.3.**
- **E2.5 Cross-VM Net + variable sync**: `message_bus_process(server_L, client_L)` for real; state-owned request callbacks; serialized payloads only; **user/mod variable synchronization flags** (previously omitted; ROADMAP names it unimplemented). Gate: both directions, request/reply, timeout, reset-with-pending-request, variable conflicts.
- **E2.6 Console context switching**: `server`/`client`/`reset server`/`reset client`/`reset` (Windows `Console.cpp:36` shape), state-safe command queues. Closes the Console row. Gate: console globals appear only in the selected VM; resetting one leaves the other responsive.
- **E2.7 Lifecycle & compat hardening**: module load, session load, level transitions, reconnect, save/load, repeated independent resets, shutdown, memory soak. Gate: full test baseline + 11-mod matrix, no stale-state callbacks, no cross-state registry access, no monotonic leak.

## Permanent deferrals (candidates — each now requires its evidence milestone first)

- Generic `RemoveComponent` reimplementation (D2's table is the shipped form); arbitrary `Construct` beyond the validation contract (upstream is `// TODO`).
- Ext.UI/Noesis, Lua Debugger/DAP, Virtual Textures, Input Injection (published scope exclusions).
- Interrupt sync / ExecuteFunctors-full / raycast legs — only *after* their D7/D5/B4 evidence milestones conclude no-go.

## Verification strategy (every phase)

- Baseline gate before and after every phase: tier0 55/55, pytest 245/245 (+new), `!identity` then `!test` 113/113, `!test_ingame` full. After new tests land, **the new total passes with zero skip/xfail** — "≥530 tests" is not itself a quality gate.
- Presence-only tests award no credit; every closed contract gets a behavioral Tier-2 test.
- RE discipline: body-level disassembly before binding (LTO arg-promotion check), offset-table + manifest recipes, fail-closed unknown-version paths, `ghidra/offsets/` evidence doc per unlock.
- Partial implementations (A8 tracing prototype, B3 raw tile info, sweep-emulation fallback) ship under **distinct diagnostic names** and never flip a contract row to "implemented."
- Replication (C steps 8–9) and cross-VM net (E2.5) verified in the two-instance multiplayer harness, never the single-player pipeline.
- Compat matrix (11 scenarios, `--launch`) re-run after Phases C, D, and each E milestone that touches state ownership.

## Estimated shape (full campaign, planner-corrected)

| Milestone | Cumulative | Lands |
|---|---|---|
| Phase 0 | ~2–3 days | Contract manifest, honest baseline (~96.0%), Wave 6 committed |
| + Phase 1 | ~2 weeks | State ownership + two-VM skeleton, zero mod regressions |
| + A + B | ~6–8 weeks | Types closed or scored, Entity contracts mostly closed, Level settled, Osi/Net settled |
| + C | ~2.5–4 months | Replication triad (Get/Set/Replicate) + multiplayer harness; tracing full credit |
| + D | ~3–4.5 months | Stats ~98–100, Entity ~100 (with D2/D4), all evidence milestones concluded |
| + E | ~4.5–6 months | Save-owned persistence + dual VM; every scored row 100% or documented permanent |

Timeline is honest, not padded: the planner priced Phase E alone at 2–4 months depending on save-hook availability. If E1.1's spike no-goes, the sidecar fallback shortens E1 substantially at the cost of one permanent scored deferral.
