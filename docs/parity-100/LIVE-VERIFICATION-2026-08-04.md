# Live Verification Session — 2026-08-04

The culminating live gate for accumulated Wave 7 work (Waves 3–5 of the terminal-parity
campaign), run on game build **4.1.1.7209685** — as it turned out, the final session
possible on that build: Steam delivered **4.1.1.7398727** at 01:36 EDT, minutes after
the game quit.

## Session Identity

| Field | Value |
|-------|-------|
| Game build | 4.1.1.7209685 (arm64 LC_UUID `9A647311-E263-3FF2-AF98-111CEDCB3034`) |
| BG3SE dylib | build of 2026-08-03 11:53:27, contains `_replication_flags_get` (Wave 5 code confirmed loaded) |
| PID / handshake | 2890, `!identity` → `session_init:"complete"`, `stats_ready:true`, `game_state:"Running"` |
| Save | most recent via `-continueGame` (host `b4d01c83-25e9-6156-d91b-84e619b3757d`) |

## Results

- **Tier 1 (`!test`):** 113/114 — sole failure was a stale test (see Triage 1)
- **Tier 2 (`!test_ingame`):** 104/110

### Wave 5 verification targets — all PASS

| Test | Verdict | Dispatch evidence |
|------|---------|-------------------|
| `Wave7.Entity.GetReplicationFlags` | PASS | Version gate provably open (an identically-gated path executed the same session), so the SyncBuffers walk ran without crash or Lua error. Return value (number vs nil) not captured — **live-probe checklist NOT satisfied; stays behavioral_gap** |
| `Wave7.Level.RaycastAny` | PASS | Gate provably open: installed binary LC_UUID matched `9A647311-…` exactly and `version_detect_matches()` held → the pass went through **real VMT-slot-10 dispatch**, returning a boolean with no crash. First rung of the stress ladder only — **full ladder unrun; stays a scored deferral** |
| `Wave7.Entity.OnSystemUpdate` | PASS | — |
| `Parity.Level.GetHeightsAt.{Host,TileRange,OutOfBounds}` | PASS ×3 | Real multi-subgrid walk verified live |
| `Diagnostic.Level.TileRawDebugInfo` | PASS | — |
| `Wave7.Osi.DBDelete` | PASS | Non-destructive path; manual sentinel-row destructive verify still outstanding |
| `Wave7.Entity.Tracing`, `GetRegisteredComponentTypes` | PASS | — |

Note on fail-closed opacity: `level_raycast_any` (src/level/level_manager.c:1996) and
`replication_flags.c` return false/nil **silently** when gated — a bare test pass cannot
distinguish dispatch from fail-closed. The UUID/version evidence above is what makes the
RaycastAny dispatch claim sound. A follow-up should add one-shot gate diagnostics so future
sessions self-certify.

## Failure Triage (6)

1. **`Stats.Goal23.HonestSurface`** (Tier 1) — stale test: asserted `AddEnumerationValue == nil`
   from the pre-B1 allocator-gated era; the real contract is boolean. **Fixed** — now asserts
   unknown-enum fail-closed (`false`), which stays valid even after the registry regression
   (finding 3) is repaired, without poisoning `DamageType`.
2. **`Parity.Level.SweepCylinderAll`** — over-strict: demanded `table` while the `Closest`
   companion tolerates the legitimate no-hit `nil`. **Fixed** — accepts nil-or-table.
3. **`Wave7.Stats.AddEnumerationValue`** — REAL: `find_rpgenumeration_by_name` returned NULL
   for *every* probed enum (`DamageType`, `Ability`, `Skill`, …), and the read-side
   `EnumIndexToLabel`/`EnumLabelToIndex` returned nil across the board, while
   `Parity.Stats.CanonicalCounts`/`GetAllReturnsData` passed with real data. The
   ValueList/enumeration registry resolution is broken live despite the version gate passing —
   contradicts the B1 "live-verified" record; suspect the 2026-07-28 offset migration missed
   the value-list root. Needs live RE (blocked on re-migration below).
4. **`Wave7.Entity.UuidRoundtrip`** + 5. **`Wave7.Entity.GetAllEntitiesWithUuid`** — REAL:
   `HandleToUuid(hostEntity)` returned `757d19b3-84e6-d91b-6156-25e9b4d01c83` for actual GUID
   `b4d01c83-25e9-6156-d91b-84e619b3757d` — same bytes, engine-internal swizzled order,
   unswizzled on output. `UuidToHandle` (the inverse direction) works correctly. One
   formatter fix in the W7-A1 surface; the mapping walk itself is keyed consistently.
6. **`Wave7.Entity.GetEntitiesAroundPosition`** — REAL (cluster): host not found within 5m of
   its own position. Correlates with host component-proxy reads returning nil live
   (`e.Transform.Transform.Translate` → nil, `e.Uuid.EntityUuid` → nil) while
   `HealthLayoutSnapshot`/`ComponentEnumeration` passed. Component-proxy resolution for
   specific layouts needs live investigation.

`Stats.DamageEvents.PairedFiring` failed environmental-conditionally (needs BURNING applied
to the host + one status tick) — known precondition, not a defect.

## Game Update Event — build 7398727

At 01:36 EDT, after the session closed, Steam rewrote the game binary:

| | old | new |
|---|-----|-----|
| Version | 4.1.1.7209685 | **4.1.1.7398727** |
| arm64 LC_UUID | `9A647311-E263-3FF2-AF98-111CEDCB3034` | `0C51CAED-6D60-3DCD-9299-8519C92631B0` |
| insert_dylib patch | present | **wiped** (vanilla binary) |

Consequences, all by design:
- All 68 `tests/harness/test_offset_audit.py` tests now fail — correctly, until offsets are
  re-migrated (see the metathesis migration plan,
  `docs/plans/2026-05-13-003-feat-metathesis-binary-address-migration-plan.md`).
- Every runtime version/UUID gate (ValueList insert, RaycastAny, GetReplicationFlags,
  component writes, …) will fail closed on the new build. No unsafe dispatch is possible.
- The harness will re-patch on next launch; offsets must be re-verified first.

## Credit Decisions

No parity credit moves. `GetReplicationFlags` and `RaycastAny` remain `behavioral_gap` in
`contract.json`: the former's live-probe checklist is unsatisfied, the latter's stress
ladder is unrun — and both are now additionally blocked on the 7398727 re-migration.
