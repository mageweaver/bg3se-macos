# What 100% Parity Would Take

Synthesis of four parallel Codex (gpt-5.6-sol, high reasoning) research passes run
2026-07-30 against v0.41.0 / game build 4.1.1.7209685. Full reports in this
directory: [parity-adjudication.md](parity-adjudication.md),
[level-deferrals.md](level-deferrals.md), [stats-deferrals.md](stats-deferrals.md),
[entity-types-deferrals.md](entity-types-deferrals.md).

## The headline

**97.3% is arithmetically correct but not behaviorally honest.** The matrix mean
over 25 scored rows reproduces exactly (18×100 + 7 discounted rows = 97.2667%),
but several "100%" rows carry admitted stubs, and the parity scanner
(`tools/bg3se_harness/parity.py`) awards credit for name presence, not behavior.
Re-scoring only the *admitted* stubs (Entity 25/30, Stats 49/52, Types 13/17)
gives **~96.0% as the honest upper bound**. A matched-contract audit would land
lower — Entity function substitution, input injection, Vars sync, and IMGUI
residuals are unpriced.

## The one gap with proven mod demand

**`entity:Replicate()` is a no-op, and four of our ten vetted mods call it**:
Community Library (`EntityUtils.lua:63,86,116,196`), 5e Spells
(`S5E_Additions.lua:172`), Expansion (`BootstrapServer.lua:1429-31,2523,2538`),
Combat Extender (`BootstrapServer.lua:1108,1534-35`). All four pass vetting
because the no-op is masked in single-player — multiplayer correctness is
unproven. Every other deferral has **zero callers** in the vetted-ten corpus
(verified at source level, including ExecuteFunctors, RemoveComponent, tracing,
and all four Types stubs).

## New evidence the reports surfaced

1. **Zeroed-aggregate raycast hypothesis** — direct disassembly shows
   `RaycastAny`'s worker (`0x105c58adc`) takes an internal scene-lock path when
   the by-value `ls::Optional` is zeroed, and `RaycastClosest`'s wrapper
   (`0x105c4e784` → worker `0x105c573cc`) branches on a null leading
   method-table field before copying the 0x40-byte `ls::Function`. A zeroed
   16-byte optional / 0x40-byte function object may legally mean "no lock" /
   "no filter" — unlocking all three raycasts without constructing real C++
   callables. Needs a compile-and-disassemble ABI shim proof.
2. **`CRPGStats_Modifier_ValueList::Insert(FixedString const&, int)` at
   `0x101c44920`** — likely collapses `AddEnumerationValue` to one session at
   medium risk (native transaction instead of manual container surgery).
3. **`eoc::Passives::Parse` at `0x101c0f37c`** — one bounded recon session
   decides whether passive prototype sync gets a narrow callable helper or
   stays deferred.
4. **Windows `Ext.Types.Construct` is itself `// TODO; return 0`**
   (`Types.inl:286`) — we have been charging ourselves for an API the reference
   never implemented. Matching its validate-then-return-nothing contract is
   <1 day; it should leave the parity denominator.
5. **Latent bug in our code**: the interrupt cached lookup routes through the
   generic `refmap_lookup` (`prototype_managers.c:432`) although interrupt
   storage is a proven hash-table + 0x1f0-stride array, not a RefMap. Native
   `InterruptPrototypeManager::GetPrototype` is at `0x101b7adcc`, and
   `eoc::Passives::Get` at `0x101c0f27c` for the passive side.
6. **Stale diagnostic**: `component_property.c:527` claims FixedString interning
   is unavailable, but `fixed_string_intern()` exists and works — the real
   blocker is DecRef/ownership transfer, not interning.
7. **Contract fixes**: `GetHashSetValueAt` is 1-based on Windows (our stub
   comment says 0-based); `GetReplicationFlags` lives on the entity proxy in
   current Windows, not `Ext.Entity`; `DisableTracing` doesn't exist upstream
   (it's `EnableTracing(bool)`).
8. **Scope-doctrine conflict**: `docs/deferrals.md` names two exclusions
   (Ext.UI, DAP); ROADMAP front matter silently excludes three more
   (replication, Virtual Textures, input injection). Replication can't be
   excluded — it has mod demand (above). The other two need explicit scope
   decisions, not silence.

## Tiered path to 100%

### Tier 0 — Truth pass (days, no RE)

- Re-baseline the matrix with behavioral accounting (~96.0%), or publish the
  matched-contract manifest the adjudicator describes.
- Fix the interrupt/passive cached lookups to use native getters.
- Fix the stale FixedString diagnostic, the GetHashSetValueAt index contract,
  GetReplicationFlags placement, and the Construct denominator.
- Reconcile the scope-exclusion registry (2 vs 5) with explicit decisions.

### Tier 1 — Pure engineering, no new instruction-level RE

| Item | Effort | Risk | Demand |
|---|---|---|---|
| Custom-property layer (`AddCustomFunction`/`AddCustomProperty`, component-only) | 3–8 days | Lua lifetime only | none found |
| `Construct` Windows-contract shim | <1 day | none | none |
| Entity tracing (callback-backed change log MVP) | 3–5 days | perf/reentrancy | none (diagnostic) |
| Osi.* filtered retrieval/deletion verification | days | low | — |
| Float-array writer (dormant — no real field uses it yet) | 0.5–1 day | low | none |

### Tier 2 — RE-gated, ranked by value

1. **Replication foundation → `GetReplicationFlags` (read-only) →
   `entity:Replicate`** — 2–4 weeks total; the only gap four vetted mods
   actually exercise. Validate in multiplayer, not the single-player pipeline.
2. **`AddEnumerationValue` via `0x101c44920`** — 1 session, medium risk.
3. **`AddAttribute`** (pre-load-gated via `CNamedElementManager::Insert`
   `0x102105d2c`) — 2–3 sessions, high risk after stats load.
4. **`RemoveComponent` dispatch table** (734 specializations, build-gated,
   fail-closed) — 1–2 weeks first build, ~1 day per game update. The generic
   rewrite (3–6 weeks, critical risk) only later, with the table as oracle.
5. **Raw `GetTileDebugInfo`** — 1–2 sessions, low risk.
6. **Raycast chain**: `RaycastAny` ABI proof (1 session) → `RaycastAll` +
   `PhysicsHitAll` lifetime (2) → `RaycastClosest` empty-function semantics
   (2–3). High/critical risk; sweep-emulation fallback exists.
7. **`ExecuteFunctors` MVP** — game-owned parsed lists, AttackTarget/Target/
   AttackPosition contexts only — 3–5 sessions (full: 7–12). No vetted mod
   calls it; 5e Spells' functor usage is stats-text, not API.
8. **FixedString component writes** — recover DecRef, transactional swap,
   allowlist — 3–5 days.
9. **Passives::Parse recon** — 1 session; proceed only if a callable helper
   emerges (else 5–8 sessions, not justified).
10. **EntityHandle writes** — deliberately late; semantic corruption risk
    exceeds observed demand.

### Tier 3 — Big architectural items inside "true 100%"

- PersistentVars savegame semantics (replace file-global JSON) — extensive RE.
- True dual client/server Lua VM + console context switching — engineering-heavy.
- `Ext.Net` residual 5% — undefined; must first name its denominator.
- Input injection, Virtual Textures — implement or make explicit scope
  exclusions.

### Keep deferred (permanent, documented)

- Interrupt prototype sync (6–10 sessions, critical risk, zero mod demand).
- `BeginPathfinding` via manual `AiPath` population (only reconsider if the
  engine's `CreatePathForCharacter` — client `0x102de3d78`, server
  `0x104815680` — proves callable end-to-end).
- Generic `RemoveComponent` reimplementation; arbitrary `Construct`.
- Ext.UI/Noesis, Lua Debugger/DAP (scope exclusions, publish as such).

## Definition of done

Publish "100% supported-surface parity; Ext.UI and DAP excluded" — never
unqualified "100% Windows parity" — backed by a versioned contract manifest,
conformance tests that invoke behavior (presence-only tests award no credit),
and a matrix where every scored row is 100% and exclusions read "permanent
scope exclusion."

Nominal ladder under the current matrix: +2.733 points across 7 rows.
Honest ladder: rebaseline to ~96.0, then ~+4.0 points, dominated by
replication, Level, Types, PersistentVars, and dual-VM work.
