# BG3SE-macOS: 100% Parity Closure + CLI Automation Proof + Top-10 Mod Vetting — Codex-Orchestrator Wave Plan

**Date:** 2026-07-28 | **Project:** `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos` | **Branch:** `main`
**On approval:** copy this plan into `docs/plans/2026-07-28-001-feat-parity-closure-codex-wave-campaign-plan.md` (repo convention).

## Context

Work stopped 2026-05-16 (~2.5 months idle). The project claims ~94% parity at v0.37.1+headless, but three campaigns remain unfinished:

1. **Headless CLI automation** (`bg3se-cli-goal.md` non-negotiable outcome): `PYTHONPATH=tools python3 -m bg3se_harness test --headless --continue --tier 1` has **never completed end-to-end**. Boot reaches a responding socket at main menu in ~3s; two blockers remain: (a) no input method changes BG3 menu state (CGEvent/System Events/LSMTLView all fail; OCR reads zero text on BG3's stylized font) — `docs/bugs/true-headless-feasibility.md` classifies the fix as `possible-with-RE` via direct game-state save-load invocation; (b) post-save-load `gui::HotbarSystem::Update()` SIGSEGV attributed to mod-state mismatch — the fix (`mod reconcile --installed --write`) landed May 16 but was **never live-retested**.
2. **Parity**: honest inventory shows stubs behind the 94% claim — component property **writes** entirely stubbed (`src/entity/component_property.c:418`), damage events (BeforeDealDamage/DealDamage/ExecuteFunctor) unwired, Stats stubs (TreasureTable/Category, AddAttribute, ExecuteFunctors, prototype sync), Ext.Types 60%, Ext.Level 71%, Ext.Audio 76%, plus Math/Loca/Entity gaps. Docs disagree with code (version, counts, percentages) per the Nomos audit in `plans/2026-04-02-windows-parity-execplan.md`.
3. **Mod vetting**: catalog has 17 mods, 5 scenarios, **zero successful vets** (only report: MCM `no_socket`). Pipeline gaps: compat doesn't launch the game or install mods, Tier-2 execution assertions absent, MCM dependency unenforced, no baselines, no `compat diff`.

## Decisions (user-confirmed 2026-07-28)

- **Parity target = scope-corrected 100%**: close all supported-surface gaps; Ext.UI (Noesis) stays excluded, Debugger + entity replication formally deferred with honest docs. Denominator frozen in `tools/bg3se_harness/catalog/windows_parity_baseline.json`.
- **Nexus Premium available** → autonomous batch PAK downloads via existing `nexus.py`.
- **Execution = codex-exec waves**: Claude spawns `~/.claude/skills/codex-orchestrator/scripts/codex-exec.sh <profile> "<goal>"` one-shots (builder/debugger/reviewer/researcher/docs), monitors output (`--json` / `--no-cleanup`), drives all live BG3 launches itself via the harness, evaluates against exit gates, and designs the next wave from results. Codex writes code; Claude verifies against the live game.

## Orchestration protocol (applies to every wave)

- Each goal below becomes one codex-exec prompt (≤3,500 chars, written by Claude with file paths + acceptance criteria + "do not touch" constraints from this plan).
- Concurrency: max 2 codex builders in parallel, never on overlapping files. Reviewer passes run after builders.
- **Offline gate before any live attempt** (from `bg3se-cli-goal.md`): `pytest tests/harness -q` (55) + `build/bin/bg3se_test_tier0` (41) + `tests_nexus` (23) + `tests_wiki` (23) + `cmake --build build`.
- **Live validation is Claude-only** (Accessibility perms, game launches, socket commands, screenshots). Codex never launches BG3.
- Wave exit gate = named artifacts + passing commands, recorded in `docs/bugs/wave-campaign-progress.md` (dated, append-only). A wave that fails its gate gets a debugger-profile diagnosis pass, then either a repair goal or an explicit deferral logged in the progress doc.
- Git: one commit per completed goal (user-requested campaign = durable authorization to commit; **no pushes** without explicit user confirmation).

## Wave 0 — Truth & Baseline (½ day; codex: docs + reviewer)

Cheap, unblocks everything, kills doc drift before new work compounds it.

1. **Housekeeping (Claude)**: commit untracked plan/docs artifacts (`docs/plans/*.md`, `docs/e2e-review/`, `bg3se-cli-goal.md`, `plans/2026-04-02-windows-parity-execplan.md`, `progress.json`); add `gold.*.log`, `network.*.log`, `.screenshots/`, `.subdaimon-output/`, `__pycache__` to `.gitignore`; resolve staged `.pyc` deletion.
2. **Goal 0.1 (docs profile)**: reconcile ROADMAP.md / CLAUDE.md / README.md / `agent_docs/api-status.md` / `windows_parity_baseline.json` to one truthful story — actual version (v0.37.1+headless), honest per-namespace percentages from explorer-2 inventory (Types 60% not 90%, Math 57/59, Entity/Stats stub footnotes), scope-corrected denominator with named exclusions (Ext.UI excluded; Debugger + replication deferred).
3. **Goal 0.2 (reviewer→builder)**: fix the critical Codex finding — `osiris_call_by_id()` unsafe `pfn_InternalCall` fallback (`src/injector/main.c:2633`): fail closed.
4. **Claude**: run full offline gate; record baseline numbers.

**Exit gate**: offline suites green; docs agree on numerator/denominator/exclusions; progress doc created.

## Wave 1 — Boot Unblock: the non-negotiable command (1–3 days; codex: researcher + builder + debugger)

Critical path for everything downstream. Two parallel tracks:

- **Goal 1.1 (debugger) — hotbar crash retest**: the unfinished half of `docs/plans/2026-05-16-001`. Claude runs live `test --headless --continue --tier 1` with reconciled registry (all 14 PAKs). If `HotbarSystem::Update` still crashes → codex debugger designs a mod-set bisection procedure (`mod disable` halves), Claude executes the launches, codex analyzes `.ips` crash attributions between rounds.
- **Goal 1.2 (researcher→builder) — menu bypass via direct game-state invocation**: implement the `possible-with-RE` path from `docs/bugs/true-headless-feasibility.md`: RE the `-continueGame`/`-loadSaveGame` GameStateInit consumption path (Ghidra HTTP bridge at `127.0.0.1:8080`, `bg3se-harness ghidra` wrapper) and expose a dylib-side "request save load" that skips menu UI input entirely (console command or auto-trigger when `-continueGame` was passed). Fallback ladder if blocked: the 5 ranked approaches in `docs/bugs/noesis-input-bypass-re.md`.
  - Note: `-continueGame` is already passed at launch today, yet the game sits at menu — the RE question is why the flag stalls (likely a confirm gate, e.g. Mod Verification dialog / MainMenuConfirm), and how to programmatically satisfy it from the dylib.

**Exit gate** (= `bg3se-cli-goal.md` acceptance): `test --headless --continue --tier 1` completes autonomously — save loads, Tier 1 (93 tests) runs, JSON phase evidence emitted (`process_launched … session_loaded → socket_responded → tests_finished`), graphics restored, no manual input. Evidence JSON committed under `docs/bugs/`.

## Wave 2 — Parity Closure A: high-severity gaps (2–4 days; codex: builder ×2 + reviewer)

Ordered by mod impact (from explorer-2 inventory):

- **Goal 2.1 (builder)**: **component property writes** — implement `component_property_write()` (`src/entity/component_property.c:418`) for verified layouts (169 verified first; generated layouts gated behind size-known check). Include write-safety: bounds checks, blacklist one-frame components, refuse unknown layouts loudly.
- **Goal 2.2 (builder, needs Ghidra)**: **damage events** — rebuild GhidraMCP JAR from `/Users/tomdimino/ghidra/GhidraMCP/` (source already has `create_function`; installed JAR 404s — steps 2–4 of `plans/2026-04-02-windows-parity-execplan.md`), recover `ThrowDamageEvent`/`ApplyDamage` ARM64 addresses, wire BeforeDealDamage/DealDamage in `src/stats/functor_hooks.c` + `src/lua/lua_events.c`; finish ExecuteFunctor's missing `Functors*` plumbing (`src/lua/lua_stats.c:1078`).
- **Goal 2.3 (builder)**: **Stats stubs** — real TreasureTable/TreasureCategory reads, AddAttribute/AddEnumerationValue via game heap alloc, GetStatsLoadedMods, prototype sync RefMap inserts (`src/stats/prototype_managers.c:693-744`).
- **Goal 2.4 (builder)**: **test honesty** — convert presence-based parity tests (flagged by `docs/e2e-review/`) into execution assertions for everything Waves 2–3 implement; extend Tier 1/2 suites.

**Exit gate**: Claude live-validates on a disposable combat save — both damage events fire in combat without crash; component write round-trips on a safe component; new Tier 2 tests pass via the now-working Wave-1 pipeline.

## Wave 3 — Parity Closure B: breadth to 100% (2–3 days; codex: builder ×2)

- **Goal 3.1**: Ext.Level — expose SweepCylinderClosest/All (VMT indices 13/17 already known in `level_manager.c`), GetEntitiesOnTile, GetTileDebugInfo, pathfinding suite (FindPath/BeginPathfinding/ReleasePath/GetPathById/GetActivePathfindingRequests).
- **Goal 3.2**: small-gap sweep — Ext.Audio 4 bank functions; Ext.Math Smoothstep + IsNaN; Ext.Types GetValueType/GetHashSetValueAt/GetFunctionLocation + Serialize/Unserialize/Construct against the layout DB; Ext.Localization GetTranslatedString/UpdateTranslatedString; Ext.Entity GetAllEntities/GetAllEntitiesWithComponent/GetAllComponents (tracing stubs → deferred list if engine support absent).
- **Goal 3.3 (docs)**: deferral formalization — Debugger, replication (Replicate/ReplicationFlags), Ext.UI, Virtual Textures, Input Injection removed from denominator with rationale; parity baseline updated.
- Optional stretch (only if waves run ahead): ecl:: component-size batch extraction via Ghidra (351 missing) using the parallel-agent workflow in `agent_docs/development.md`.

**Exit gate**: `bg3se-harness parity scan` reports 100% against the frozen scope-corrected baseline; `parity missing` returns only the named deferrals; docs updated; full Tier 1+2 green via headless pipeline. **Declare scope-corrected 100% parity** in ROADMAP/README/CHANGELOG.

## Wave 4 — Vetting Infrastructure (1–2 days; codex: builder + reviewer)

- **Goal 4.1 (builder)**: integrated compat pipeline — `compat run --launch` (wires `_launch_until_socket()` + save-fixture restore + launch-scoped log windows), auto `mod install`/`enable` from catalog inside the flow (Premium download via `nexus.py`), `requires_mcm` dependency enforcement, `compat diff` vs `docs/compat-reports/baseline/`.
- **Goal 4.2 (builder)**: scenarios to 10 — add manifests for `expansion_lvl20`, `more_reactive_companions`, `camp_event_notifications`, `auto_send_food`, `always_show_approvals` (top-10 by catalog priority: 5 existing + these 5); upgrade all 10 to Tier-2 execution assertions; define one shared save fixture (`save list --fixtures`).
- **Claude**: batch-download all 10 PAKs (Premium), verify installs, dry-run `compat run --launch mcm`.

**Exit gate**: offline tests extended + green; one full autonomous vet (MCM) produces a `working`-status report with launch-scoped evidence.

## Wave 5 — Top-10 Vetting Campaign (1–2 days; Claude-driven, codex: debugger on failures)

Run the campaign mod-by-mod in dependency order (MCM → Community Library → 5e Spells → Expansion Lvl 20 → More Reactive Companions → Party Limit Begone → Combat Extender → Camp Event Notifications → Auto Send Food → Always Show Approvals):

- Per mod: `compat run --launch <scenario>` → vet report → baseline saved → catalog `last_tested`/`last_status` stamped.
- Failures fork to codex debugger goals (crash attribution from `.ips` + SE log scan); genuine BG3SE bugs found become repair goals inside this wave; genuine mod incompatibilities get documented verdicts, not silent skips.
- Finish with `compat matrix` across all 10.

**Exit gate**: 10/10 vet reports with definitive verdicts (target: `working`; any `partial`/`broken` has a root-cause note), baselines populated, matrix committed to `docs/compat-reports/`.

## Wave 6 — Final Audit & Release (½ day; codex: reviewer + security + docs)

- Codex reviewer over the full campaign diff; security profile over injection/patching surface; fix or log findings.
- CHANGELOG entry, version bump (`src/core/version.h`), ROADMAP/README/CLAUDE.md/api-status final pass (end-of-session checklist in `agent_docs/development.md`).
- Evidence bundle: campaign progress doc finalized with links to JSON proofs (headless run, parity scan, compat matrix).
- User decides on push/tag.

## Verification (end-to-end)

```bash
# The three campaign proofs, in order:
PYTHONPATH=tools python3 -m bg3se_harness test --headless --continue --tier 1   # Wave 1: autonomous boot→save→tests
PYTHONPATH=tools python3 -m bg3se_harness parity scan                            # Wave 3: 100% vs scope-corrected baseline
PYTHONPATH=tools python3 -m bg3se_harness compat matrix                          # Wave 5: 10/10 vetted
# Standing offline gate before every live attempt:
PYTHONPATH=tools python3 -m pytest tests/harness -q && build/bin/bg3se_test_tier0
```

## Risks

- **Wave 1 is the funnel**: if direct save-load RE stalls, fallback is the Noesis bypass ladder; hard floor is documented semi-manual launch (one human click), which still permits Waves 2–5 with degraded autonomy — logged as explicit scope correction, not silently absorbed.
- Hotbar crash may be a real vanilla-interaction bug rather than mod-state: bisection in Goal 1.1 decides; a clean-save fixture (no mods beyond the target) is the containment strategy for Wave 5.
- Codex TTY quirk: always through `codex-exec.sh` (auto `script(1)` wrap), never raw `codex exec` backgrounded.
- Game updates during campaign: sentinel probes tolerate data-read drift, but Dobby hooks are exact-version-gated — if Larian ships a patch mid-campaign, re-run `doctor` + version detect before trusting any live result.

## Sources

- Origin request: this conversation (/ce-plan, 2026-07-28)
- `bg3se-cli-goal.md` — non-negotiable CLI outcome + acceptance criteria (carried into Wave 1 gate)
- `plans/2026-04-02-windows-parity-execplan.md` — parity end-state decision framework, GhidraMCP rebuild steps, Nomos doc audit, Codex review findings (carried into Waves 0/2)
- `docs/plans/2026-05-16-001-fix-mod-state-reconciliation-hotbar-crash-plan.md` — unchecked retest/bisection items (carried into Goal 1.1)
- `docs/plans/2026-05-02-001-feat-systematic-top5-mod-vetting-plan.md` — Phases 2–4 unfinished (carried into Waves 4–5)
- `docs/bugs/true-headless-feasibility.md`, `docs/bugs/noesis-input-bypass-re.md` — Wave 1 technical basis
- `tools/bg3se_harness/catalog/windows_parity_baseline.json` + explorer gap inventory — Waves 2–3 work items
- `~/.claude/skills/codex-orchestrator/SKILL.md` — execution mechanics
