# Wave 2 Post-Closure Review — Consolidated Findings (2026-07-29)

Four parallel review subagents over `91313f0..ecdd4da` (the six unpushed Wave 2 commits).
Tracked as tasks #19 (tests), #20 (silent failures), #21 (correctness), #14-18 (docs).

## Verdict summary

The crash class that dominated the day is closed: **both code reviewers independently
verified the hidden `result_out` ABI fix end-to-end** — all 9 ExecuteStatsFunctors
wrappers + Interrupt forward it correctly (zero-subscriber fast paths included),
ProcessDealDamageFunctors correctly omits it, typedefs match, and the
FUNCTOR_ADDRS_VERIFIED_BUILD gate is independent of the global version match.
Also verified clean: fire_damage_event lua_gate acquisition + pointer lifetimes,
treasure-manager fail-closed reads, component-write error propagation, and the
ecdd4da stats_sync honesty fix.

## Code findings (fix pass: this commit)

| # | Sev | File | Finding |
|---|-----|------|---------|
| 1 | CRITICAL | `prototype_managers.c:796-814` | `sync_interrupt_prototype` logs TODO and returns **true** — same honesty bug class as the fixed stats_sync, missed on the InterruptData branch. Fix: passive-path pattern (one-shot WARN + false). |
| 2 | MAJOR | `prototype_managers.c:758-773` | `sync_status_prototype` calls the game's `StatusPrototype::Init` on a calloc'd prototype with a **NULL vtable**; the spell path copies the VMT from a template prototype first (lines 688-696), the status path doesn't. Latent NULL-vtable dispatch window. |
| 3 | MINOR | `component_property.c:462-483` | `component_property_bounds_valid` returns true unconditionally when `componentSize==0` on non-generated layouts — writes bypass all bounds checks. Fix: refuse writes at size 0 regardless of `generated`; reads stay permissive. |
| 4 | MEDIUM | `functor_hooks.c:393` | `g_HooksInstalled = (success_count > 0)` — partial install reports as fully installed; a failed ProcessDealDamageFunctors hook leaves BeforeDealDamage/DealDamage subscriptions silently dead. Fix: surface the install count via Ext.Debug.GetHookStatus. |
| 5 | LOW | `lua_stats.c:1188-1196` | GetStatsLoadedBefore one-shot WARN suppresses warnings for *different* missing-mod UUIDs. Fix: warn every time (WARN-level should fire). |
| 6 | LOW | `lua_stats.c:397-424` | AddAttribute/AddEnumerationValue one-shot warnings suppress distinct arguments. **Accepted as-is**: return values are honest; the gate is intentional log-spam prevention. |

Non-finding noted: `g_EventCount` increment is non-atomic — diagnostic only, never use
for correctness decisions.

## Test coverage gaps (task #19 — Wave 3 input)

Ranked (rev-tests): (1) HIGH no registered end-to-end damage-event firing test — the
51/51 live verification was manual; (2) HIGH DYNAMIC_ARRAY write-refusal untested;
(3) MED generated-unknown-size refusal untested; (4) MED `Sync` on nonexistent stat;
(5) MED treasure table with 0 subtables; (6) MED **`fn_status_proto_init` missing from
`tools/offset_manifest.json`** — next binary migration silently zeroes it, disabling
status sync with no CI signal; (7) LOW UUID-pattern assertion in ModuleLoadOrder;
(8) LOW Parity.Events.BeforeDealDamage/DealDamage are presence checks with behavior
names; (9) LOW PrototypeSyncHonesty asserts weaker than its name; (10) LOW the ABI
guard test pins signatures, not forwarding (functional risk low — non-forwarding
crashes immediately). Pre-existing fixture-fragile tier-2 tests that silently no-op
behind guards: RegistryCounts, HandleRoundtrip, RaycastShape.

## Documentation punch-list (tasks #14-18)

Ground truth verified by rev-docs against source: writes real for
INT32/UINT8/BOOL/FLOAT/INT32_ARRAY; treasure reads real; GetStatsLoadedMods real;
status sync real / passive honestly false; damage events fire live; tests
55 C + 191 pytest + 109 T1 + 74 T2 = 429.

- **Footnotes `[^stats-stubs]`/`[^entity-stubs]` duplicated in FOUR files** —
  CLAUDE.md:173-174, api-status.md:36-37, README.md:143-144, ROADMAP.md:67-68 —
  all claim treasure stubs, empty GetStatsLoadedMods, and stubbed component writes.
  Entity-stub line ref drifted (main.c:1134-1136, not 955-958).
- CLAUDE.md:204 test counts (67→74 tier-2; 209→246 offline); README.md:140 (385→429);
  ROADMAP.md:1552 (385→429); docs/testing.md:510 (125→183 registered definitions).
- api-status.md:1 version header v0.37.1 → v0.39.0 (or the Wave 2 bump);
  api-status.md:33 + CLAUDE.md Ext.Events line: "exist as event objects" →
  "fire live (build 7209685, verified 51/51)".
- "TreasureTable/TreasureCategory stubs" → drop "stubs": api-status.md:11,
  README.md:114, ROADMAP.md:42.
- ROADMAP.md:483-487 remove ExecuteFunctor/BeforeDealDamage/DealDamage from
  "Missing Events (~12)" (→ ~9); ROADMAP.md:710 Section 3.2 "❌ Not Started" →
  "✅ Complete (Wave 2)"; ROADMAP.md:292-293 check both component read/write boxes.
- Ext.Types stale refs (CLAUDE.md:171, api-status.md:18): stubs are at
  main.c:1108-1120 + lua_ext.c:1004-1012; GetValueType/GetHashSetValueAt/
  GetFunctionLocation are implemented (lua_ext.c:978-1040) — possibly 11/15 (73%),
  maintainer call.
- CHANGELOG: no Wave 2 entry; version.h 0.39.0 already consumed by the community-PR
  release at origin → Wave 2 needs its own bump (v0.40.0 recommended).
- Architecture docs (pre-existing): agent_docs/architecture.md:4 and
  docs/architecture.md:7-44 still describe DYLD_INSERT_LIBRARIES as the injection
  method; CLAUDE.md ground truth is insert_dylib static Mach-O patching. Module
  trees stale in both (docs/architecture.md lists 12 of 30 src/ dirs).

## Maintainer decisions pending

1. Wave 2 version number (v0.40.0 recommended; three new API surfaces).
2. Ext.Types 9/15 vs 11/15 recount (does GetHashSetValueAt's graceful nil count?).
3. Overall parity % recalculation — deferred to Wave 3 exit per campaign plan.
