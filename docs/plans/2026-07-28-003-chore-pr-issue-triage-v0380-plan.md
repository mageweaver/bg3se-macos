# PR & Issue Triage vs v0.38.0 (commit 11f9d6d)

**Project:** /Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos
**Final plan file (at execution):** `docs/plans/2026-07-28-003-chore-pr-issue-triage-v0380-plan.md`
**Objective (user-clarified):** decide merge-vs-respond for each open PR, and
respond / close / fix for each open issue, against current main.

## Context

Tonight's commit `11f9d6d` (dylib v0.38.0) migrated every hardcoded game-binary offset to
BG3 Hotfix 36 (4.1.1.7209685), made the version contract coherent (sentinels +
BG3_KNOWN_VERSION in lockstep), added an nm-based offset audit
(`tests/harness/test_offset_audit.py`), gated the 11 un-auditable functor code patches on
`FUNCTOR_ADDRS_VERIFIED_BUILD`, and serialized all native-to-Lua entries behind `lua_gate`.

The open community PRs and issues largely predate this: the dominant crash class
(staticdata/template Dobby hooks patching stale offsets on Hotfix 36 → SIGBUS in
`esv::hotbar::System::FinalizeAddSlot`) was independently diagnosed by mikowals in PR #91
and reported as issues #94, #92, #87 — all against pre-migration builds (v0.39.0 release /
commit 48d1c97). This plan verifies each PR/issue against current main and defines the
disposition + follow-up work.

## Established facts (verified before planning)

- **PR #91 (mikowals)** — gates staticdata/template code patches on exact version match
  (same policy we applied to functor hooks tonight) AND introduces an offset-table
  architecture: `src/core/offset_table.c/h` + `tools/offset_manifest.json` +
  `tools/port_offsets.py` spanning ~20 source files. Overlaps heavily with files we
  modified tonight.
- **PR #93 (marcus-sa)** — 31 commits stacked on #91. Real `Osi.DB_*` database reads
  (walks name index → `CReteDBase` Facts list), DIV query dispatch from the game's
  authoritative param definitions, per-mod Lua `_ENV` sandbox, `Ext.IO`, JSON stringifier
  crash fix, IMGUI render-thread + Metal-layer fixes. Verified against real mods
  (Sit This One Out 2). This substantially overlaps our Wave 2/3 parity goals.
- **Issue #94 (johnackley)** — deterministic hotbar SIGBUS on new game, zero mods,
  Hotfix 36, repro'd on 48d1c97 + v0.39.0 — the exact crash class our migration fixed.
  Frame 0 unmapped, libbg3se not on faulting stack (delayed corruption signature of a
  stale Dobby patch).
- Issues #92 and #87 report the same Hotfix 36 crash class (bodies pending detailed read).

## Pending exploration (agents in flight)

1. PR diff analysis: what in #91/#93 is superseded by 11f9d6d vs still valuable;
   conflict severity; disposition recommendation.
2. Issue triage for #92, #87, #90, #89, #88, #86, #84, #82, #81, #80: root cause,
   fixed-by-11f9d6d?, action, effort.

## Issue Dispositions (triage complete)

| Issue | Root cause | Fixed by 11f9d6d? | Action |
|-------|-----------|-------------------|--------|
| #94 hotbar SIGBUS | Stale staticdata/template patch offsets on Hotfix 36 | **Yes** | Respond: fixed in v0.38.0, ask to retest, close on confirm |
| #92 Hotfix 36 launch crash | Same class; sentinel false-positive documented in log | **Yes** | Respond + close on confirm |
| #89 CTD, M4/15.7.7 | No logs; almost certainly same class | Probably | Respond: request SE/BG3 versions, point to v0.38.0 |
| #88 `<tuple>` not found | CommandLineTools-only, SDK sysroot; CMakeLists 6-34 auto-detection likely postdates reporter | Partial | Respond: rebuild from main, manual `-DCMAKE_OSX_SYSROOT` fallback |
| #87 crash + MCM (3 sub-bugs) | (1) stale offsets **fixed**; (2) PAK dir ≠ modsettings display name in `mod_pak_has_script_extender()` (`src/mod/mod_loader.c:149-150`) **not fixed**; (3) MCM render blocked by (2), Metal swizzling itself version-independent | Partial | Fix sub-issue 2 (reporter supplied a working patch), respond |
| #81 Trials of Tav undetected | Same PAK-dir-vs-display-name bug + single-name assumption | No | Same fix as #87.2: PAK filename fallback + enumerate `Mods/*` dirs inside PAK |
| #90 external drive | Hardcoded Steam path (`tools/bg3se_harness/config.py:4`, `src/core/version_detect.c:122` `find_bg3_app_path`, `scripts/launch_bg3.sh:12`) | No | Fix: parse `libraryfolders.vdf` + `BG3SE_GAME_PATH` env override |
| #86 alternate source dir | Same hardcoded paths + `deploy.sh` POST_BUILD | No | Same fix as #90, batched |
| #84 injection marker | `launch_bg3.sh:73` checks `/tmp/bg3se_loaded.txt` which the dylib never writes; reporter's mods are cosmetic (non-SE) anyway | No | Small fix: sentinel check → real log dir; respond explaining non-SE mods |
| #82 arm64e claim | AI misdiagnosis — user-space BG3 is arm64, not arm64e; real cause likely codesign/SIP | No (never code) | Respond + troubleshooting FAQ entry |
| #80 overlay console (7 bugs) | NSTextField/TSM zombie (`__retain_OA`), `dispatch_sync` deadlock in `overlay_is_visible` (overlay.m:936), 5 reporter-proven fixes unmerged | No | Large: step 1 merge reporter's 5 fixes, step 2 rearchitect input away from NSTextField (or fold console into IMGUI backend). Own issue, not this pass |
| #83 DOS2 | Feature request, out of scope | — | Respond: out of scope, close |
| #70/#42/#35/#8 | Own tracking issues | — | Leave open |

### Issue work batches
1. **Respond-and-close wave** (no code): #94, #92, #89, #88, #83, #82 — v0.38.0 retest asks + FAQ.
2. **Quick win**: #84 launch_bg3.sh sentinel check (small).
3. **Batch A — mod detection** (#87.2 + #81): PAK filename fallback + `Mods/*` enumeration in
   `src/mod/mod_loader.c` (`mod_pak_has_script_extender`, `check_mod_has_script_extender`,
   `mod_detect_enabled`). Reporter patch exists in #87 as a starting point. Medium.
4. **Batch B — path discovery** (#90 + #86): Steam `libraryfolders.vdf` parsing +
   `BG3SE_GAME_PATH` override across `config.py`, `version_detect.c` (`find_bg3_app_path`),
   `launch_bg3.sh`, `deploy.sh`. Medium.
5. **Deferred project**: #80 overlay rework (large; separate plan).

## PR Dispositions (analysis complete; user approved "we integrate, with credit")

### PR #91 (mikowals) — offset-table architecture + version gating
**Superseded by 11f9d6d:** BG3_KNOWN_VERSION bump, sentinel updates, entity/prototype/
resource offset values, the arm64_hook.c JIT write-protect fix (we shipped the same fix),
and the exact-version gating policy (we went further with FUNCTOR_ADDRS_VERIFIED_BUILD).
**Still valuable:** `src/core/offset_table.c/h` (centralized multi-build VersionOffsets
table with runtime remap + graceful degradation), `tools/port_offsets.py` +
`tools/offset_manifest.json` + `docs/PORTING.md` (nm-driven offset generation for future
game builds — complements our audit test, which validates but cannot generate), and the
FeatManager/Get<T> TypeContext-traversal capture (more robust than prologue hooks).
**Conflicts:** HIGH across ~12 files we rewrote tonight; main.c EXTREME.
**Disposition:** integrate ourselves on a branch, cherry-picking mikowals' commits where
clean (authorship preserved), dropping the now-redundant value updates. Respond on the PR
with the triage + credit; close as integrated once the branch merges.

### PR #93 (marcus-sa) — Osiris DB/query dispatch, MCM, IMGUI (31 commits atop #91)
**Highest value, no equivalent in main:**
1. Osiris name-index walk (`osi_func_enumerate_by_name`) + `CReteDBase` Facts reader
   (`osi_db_read_facts`) + DIV param defs (`osi_read_param_defs`) — real `Osi.DB_*` reads
   and correct typed query outputs. Our `lua_osi_db_get` can't read databases at all
   (OsiFunctionId==0 invisible to id-probe).
2. Per-mod Lua `_ENV` sandbox (`Mods.<ModTable>`, `mod_env_set/apply`, chunk-env hook) —
   required for MCM; mods currently share one `_G`.
3. `Ext.IO` PAK reads + UserProfile persistence — required for MCM settings.
4. `lua_json.c` stringifier rewrite (fixes a real SIGSEGV; clean cherry-pick).
5. GameState↔ecl mapping, KeyInput SDL-name shape, ModVersion array, widget API additions.
**Reconcile manually:** IMGUI render-thread model. #93's event queue (render thread
enqueues, main thread drains) is architecturally superior to firing Lua from the render
thread under our lua_gate — adopt the queue for IMGUI events, KEEP lua_gate for every
other cross-thread entry (console, Osiris dispatch, hotkeys, shutdown). Textual+semantic
conflict in `lua_imgui.c`; main.c EXTREME.
**Outstanding review point:** mikowals' 2026-07-26 comment on the test-query return path
(fix belongs in the param-list walk, not the zero-out branch) — address during integration.
**Disposition:** integrate after #91's pieces land, staged by the priority above, with
credit; respond on the PR.

## Execution Plan

### Phase 0 — Save plan into repo
Copy this plan to `docs/plans/2026-07-28-003-chore-pr-issue-triage-v0380-plan.md`.

### Phase 1 — Responses (no code, post via gh)
Draft + post comments (Tom-voice, technical, crediting reporters):
- **#94, #92:** fixed in v0.38.0 (11f9d6d) — stale-offset class, sentinels re-derived,
  nm audit added; ask retest, close on confirmation.
- **#89:** probably same class; request SE/BG3 versions + logs, point at v0.38.0.
- **#88:** rebuild from current main (SDK auto-detection in CMakeLists 6-34); manual
  `-DCMAKE_OSX_SYSROOT` fallback.
- **#82:** correct the arm64e misdiagnosis; point at FAQ entry (Phase 2).
- **#83:** DOS2 out of scope; close.
- **PR #91 + #93:** triage summary, integration path, credit, note what 11f9d6d already
  covers; on #93 endorse mikowals' param-list-walk point as the integration approach.

### Phase 2 — Quick fixes (small)
- **#84:** `scripts/launch_bg3.sh:73` — replace `/tmp/bg3se_loaded.txt` sentinel check
  with a check against the real log (`~/Library/Application Support/BG3SE/logs/latest.log`
  mtime > launch time), or have the dylib write the sentinel from `bg3se_init`. Prefer the
  script-side fix (no dylib rebuild). Also reply re: cosmetic (non-SE) mods.
- **#82 FAQ:** add a Troubleshooting section to README.md (arm64 vs arm64e, codesign,
  SIP, submodules).

### Phase 3 — Batch A: mod detection (#87.2 + #81, unblocks MCM)
In `src/mod/mod_loader.c` (`mod_pak_has_script_extender` ~:149, `check_mod_has_script_extender`,
`mod_detect_enabled` ~:330):
- Fallback lookup: when `Mods/<modsettings display name>/ScriptExtender/Config.json`
  misses, try `Mods/<pak filename minus .pak>/…`, then enumerate every directory under the
  PAK's `Mods/` prefix (reporter's patch in #87 is the starting point; extend per #81).
- Tests: pytest fixtures with synthetic PAK listings covering display-name match,
  filename fallback, and multi-dir enumeration.

### Phase 4 — Batch B: Steam path discovery (#90 + #86)
- Parse `~/Library/Application Support/Steam/steamapps/libraryfolders.vdf` for all library
  roots; accept `BG3SE_GAME_PATH` env override everywhere.
- Touch points: `tools/bg3se_harness/config.py:4` (BG3_APP_BUNDLE — make it a resolver
  function), `src/core/version_detect.c:105-122` (`find_bg3_app_path`),
  `scripts/launch_bg3.sh:12`, `scripts/deploy.sh` (POST_BUILD target).
- Tests: pytest for the vdf parser + env override precedence.

### Phase 5 — PR integration branch (`integration/community-prs`)
1. Cherry-pick mikowals' offset_table + tools commits; reconcile against our 7209685
   #defines (table becomes the source of truth; keep `test_offset_audit.py` validating
   the table's active-build entries; wire `FUNCTOR_ADDRS_VERIFIED_BUILD` policy into it).
2. Cherry-pick marcus-sa's pieces in priority order (Osiris DB → _ENV sandbox → Ext.IO →
   JSON fix → events/mod-info → IMGUI event queue reconciliation with lua_gate).
   Fix the test-query return path per mikowals' review during the Osiris pick.
3. Gate: full offline suite (tier-0 + pytest) + build + tier-1/tier-2 live run + MCM
   compat scenario (`compat run mcm`) — MCM is the acid test for #93's work.
4. Merge to main, respond on both PRs (integrated + credit), close PRs, close #87/#81
   if MCM detection verified live.

### Phase 6 — Deferred (own issues, filed not fixed)
- **#80 overlay console:** comment acknowledging shailaric's 7-bug analysis; step 1
  (merge their 5 proven fixes) + step 2 (input rearchitecture away from NSTextField/TSM,
  or fold console into the IMGUI backend) as a separate plan. Note the socket console as
  the supported interface meanwhile.
- Keep #70/#42/#35/#8 open (own tracking).

## Verification
- Offline: `PYTHONPATH=tools python3 -m pytest tests/harness -q` (grows past 144),
  `./build/bin/bg3se_test_tier0`, clean `cmake --build`.
- Live: `bg3se-harness launch --headless` → `!identity` → `!test` / `!test_ingame`;
  `compat run mcm` for Batch A + #93 integration; external-path resolver exercised with
  `BG3SE_GAME_PATH` pointed at a temp copy.
- GitHub: every open PR/issue has a maintainer response; crash-class issues closed on
  reporter confirmation or after a retest window.

## Ordering & risk
Phases 1-2 are same-day and independent. Batch A before Batch B (MCM unblocks Wave 5).
Phase 5 is the long pole (main.c EXTREME conflicts) — do it on the branch, never directly
on main, and land it before Wave 2 parity work to avoid re-conflicting with #93's Osiris
surface. Credit mikowals and marcus-sa in CHANGELOG + next release notes.
