# Wave 7 Phase 0 Closeout — Evidence Record

**Date:** 2026-08-02 | **Release:** v0.42.0 | **Plan:** [docs/plans/2026-08-01-001-feat-wave-7-terminal-parity-plan.md](../plans/2026-08-01-001-feat-wave-7-terminal-parity-plan.md)

Phase 0 corrected the last two inflation mechanisms in the parity accounting (function substitution and name-presence credit), produced the contract manifest that becomes the Wave 7 scoring denominator, and adjudicated every stale ROADMAP pending item.

## Numbers, before → after

| Metric | Wave 6 published | Phase 0 corrected | Why it moved |
|---|---|---|---|
| Overall (matrix row-mean) | 96.7% | **94.8%** | Entity + Types rows re-scored from per-function contract diffs |
| Ext.Entity row | 84.6% (22/26) | **53.8% (14/26)** | Per-function diff vs `Entity.inl:291-325`: 11 registrations missing outright (HandleToUuid, UuidToHandle, GetAllEntitiesWithUuid, GetEntitiesAroundPosition, Create, Destroy, OnSystemUpdate, OnSystemPostUpdate, GetTrace, ClearTrace, GetRegisteredComponentTypes) + EnableTracing stub; the old 22/26 assumed only the four admitted stubs were absent |
| Ext.Types row | 92.9% (13/14) | **76.9% (10/13)** | AddCustomFunction/AddCustomProperty are *functional* on Windows (Lua-side registry, `Types.inl:328,347`) — the deferral registry's "outside the 15-function baseline" claim was wrong; GetComponentLayout/GetAllLayouts/GenerateIdeHelpers are macOS extras earning no credit |
| Function-level behavioral parity | — (not measured) | **72.4%** (202/279 scored) | New contract manifest measures every contract, including proxy methods and per-context modules the matrix rows hid |
| Test suite | 509 | **516** | +7 pytest guards pinning the manifest counts |

## Contract manifest

`CONTRACT.md` + `contract.json` (schema v1): 293 contracts across 27 namespaces — 202 implemented, 77 behavioral gaps, 1 matched upstream TODO (`Ext.Types.Construct`), 13 excluded (Ext.ClientUI, Ext.ClientInput — the published scope exclusions). Zero unclassified. Scored offline by `bg3se-harness parity scan --contract`; pinned by `tests/harness/test_contract_manifest.py`.

**Adjudication corrections applied to the generated manifest** (the extraction agent's output was cross-checked mechanically against `src/` registrations, then each contested cluster verified by reading the registration site):

- **Reclassified absent → implemented (10):** all 8 `Ext.Vars` schema functions (RegisterUserVariable, RegisterModVariable, GetModVariables, SyncModVariables, GetEntitiesWithVariable, SyncUserVariables, DirtyUserVariables, DirtyModVariables — `src/vars/user_variables.c:754-1485`) and `Ext.Debug.IsDeveloperMode`/`Reset` (`src/lua/lua_debug.c:942,945`).
- **Reclassified implemented → gap (1):** `Ext.Net.PlayerHasExtender` — the Windows contract is GUID-only (`ServerNet.inl:78` resolves `esv::Character->UserID`); the macOS GUID path returns nil unconditionally (`src/lua/lua_net.c:259`). The userId overload is a macOS extra. Wave 7 Phase A6 closes the GUID path.
- **Notes corrected, class kept (6):** the Client/ServerTemplate lookup functions are ported on macOS but under the substituted module name `Ext.Template` (`src/lua/lua_template.c:366-371`) — Windows registers only per-context modules, so mods calling the Windows placement hit nil. Closable by aliasing (Phase A).
- **Confirmed correct despite name-collision alarms (11):** the entity-proxy OnCreate/OnDestroy cluster — Windows genuinely exposes per-entity subscription methods on the proxy (`LuaEntityProxy.inl:340-386`); macOS registers those names only at module level, so the proxy contracts stay gaps.

**Known best-effort areas** (declared, not silent): the Windows event surface beyond the 35 inventoried events (input/visual/camera/UI events gated on excluded subsystems) and per-contract enumeration of the dynamic `Osi.*`/`Ext.Osiris` dispatch.

## ROADMAP stale-item adjudication

Every unchecked/pending/partial marker classified: **17 verified complete** (evidence cited inline — e.g. Stats type filtering `lua_stats.c:492`, Create/Sync, offset signature database, crash recovery, the four-tier test suite), **4 annotated as active Wave 7 items** (DB:Delete → A5, Replicate → C8-9, Set/GetReplicationFlags → C4/C7, Stats.Get level → A4), **8 marked obsolete** (superseded phase plans, stale percentage headers). Three stale "13/15 (86.7%)" Types entries corrected to 10/13.

## Gates at closeout

- Tier 0: 55/55. Tier H: 252/252 (incl. 7 new contract guards). `parity scan --contract`: 72.4%, zero unclassified.
- Tier 1 (113) and Tier 2 (96, with the two documented environment-dependent failures) were live-verified during the Wave 6 pass on build 4.1.1.7209685. Phase 0 touched only documentation, harness Python, and offline tests — no native code changed after that live verification.

## What Wave 7 works next

Phase 1 (dual-VM state ownership E2.0–E2.2) unblocks Phase A; the manifest's 77 gaps are the work queue. See the plan for the full phase ladder and the planner critique (`docs/plans/2026-08-01-001-feat-wave-7-terminal-parity-plan-critique.md`) for the sequencing rationale.
