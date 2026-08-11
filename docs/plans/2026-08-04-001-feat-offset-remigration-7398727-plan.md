---
title: "Re-migrate BG3SE-macOS offsets for game build 4.1.1.7398727"
type: feat
status: active
date: 2026-08-04
---

# Re-migrate BG3SE-macOS offsets for game build 4.1.1.7398727

This ExecPlan is a living document. Sections Progress, Surprises &
Discoveries, Decision Log, and Outcomes & Retrospective must be kept up to
date as work proceeds.

## Purpose / Big Picture

Steam replaced Baldur's Gate 3 build `4.1.1.7209685` with
`4.1.1.7398727` at 2026-08-04 01:36 EDT. The new arm64 Mach-O UUID is
`0C51CAED-6D60-3DCD-9299-8519C92631B0`; the previous UUID was
`9A647311-E263-3FF2-AF98-111CEDCB3034`. Steam also restored the executable to
a vanilla binary, removing the `insert_dylib` load command. BG3SE-macOS is
currently safe because its version, UUID, and subsystem gates reject the new
build, but all address-dependent behavior is unavailable.

After this plan is executed, BG3SE-macOS will identify build 7398727, resolve
every active game address by a reproducible method, build and pass the complete
offline test ladder, inject only after those gates are green, and complete a
new live session on 7398727. The session must also settle the three defects
found in the final 7209685 run: UUID output byte order, the stats enumeration
registry returning no names, and nil reads from host component proxies. The
result is visible when `!identity` reports build 7398727 and a complete session,
the existing 68 address-sensitive audit checks are green, `!test` and
`!test_ingame` pass, and the deferred replication, raycast, and savegame probes
have evidence recorded without prematurely changing parity credit.

## Progress

- [x] (2026-08-04 13:17Z) Confirmed the installed build, arm64 UUID, vanilla injection state, preserved 7209685 backup, current address tooling, and sampled symbol coverage.
- [x] (2026-08-04 13:17Z) Measured the current TypeId population and proved that the 7209685-to-7398727 TypeId movement is not a uniform segment delta.
- [x] (2026-08-04) Preserved both binaries in `build/migration-binaries/{4.1.1.7209685,4.1.1.7398727}/` with verified UUIDs (`9A647311-…` / `0C51CAED-…`), SHA-256 prefixes `fa0e4183…` / `d0109029…`, and arm64 slice offsets `0xf558000` / `0xf5c0000` (otool -f, decimal 257261568 / 257687552).
- [x] (2026-08-04, Wave 1C + lead) GUID formatter is now the exact inverse of guid_parse; tier0 suite 68/68 including live-host roundtrip, pre-fix regression case, zero/ff/mixed-case, and 16-byte preservation.
- [x] (2026-08-04, Wave 1A + lead) GameFunctionId interface with per-version game_functions[] replaces the two-column FnRemap; 7398727 row populated from exact arm64 nm (anonymous fields zero, expected-manual); Bink promoted, ValueList Insert added; audit refactored to version-scalable selection — 74 passed + 2 xfailed (the Wave 2A fields), 68-check baseline preserved.
- [x] (2026-08-04, Wave 1B + lead) TypeIds regenerated per exact mangled symbol from the frozen 7398727 binary: 2,004 components, 6 ugc additions, 1 removal (comment-reported); nine ReplicatedTypeContext globals generated and consumed by replication_flags.c; component_data_shift declared invalid for 7398727 (component_data_shift_valid=false); generator determinism tests 3/3.
- [x] (2026-08-04, Wave 2A + lead) Anonymous globals re-derived by ADRP+LDR metathesis with old-build self-test (`0x108af4f30`/`0x108a86128` reproduced): `global_switches_ptr=0x108b25f40` (unique candidate, writer/reader corroborated), `osiris_interface_ptr=0x108ab68f8` (five-way corroboration: OsirisCall/OsirisQuery/ErrorMessage loads, InitStory/ShutdownStory writes). Lead inserted both into the 7398727 row; the audit's live disasm check confirms the Osiris slot against the installed binary. Evidence: `ghidra/offsets/ADDRESS_MIGRATION_7398727.md`.
- [x] (2026-08-04, Wave 2B + lead) ValueList registry root re-proved on both builds via otool (no Ghidra import needed): manager base is `RPGStats+0x00`, exposing the July regression as a double-counted vtable at the old `+0x08` root. Read path fixed with bounded validation (count≤capacity≤4096), read-only `Ext.Stats.GetValueListRegistryDiagnostic()` added, Insert resolved via `GAME_FN_VALUELIST_INSERT`, mutation now behind dedicated `VALUELIST_INSERT_VERIFIED_BUILD` (pinned 7209685 until the Phase 5 live round trip). Wave 2C ABI review: all six subsystems PASS statically (`ghidra/offsets/ABI_REVIEW_7398727.md`).
- [x] (2026-08-04, Wave 2 lead integration) Anonymous values inserted; sentinels moved to 7398727 (`version_detect.c`); `BG3_KNOWN_VERSION` bumped to 7398727. Gates moved WITH evidence: `FUNCTOR_ADDRS_VERIFIED_BUILD` (2C §1, dispatcher-typedef caveat annotated in functor_types.h), `COMPONENT_OPS_VERIFIED_BUILD` (2C §2 + Wave 1B TypeIds; audit pin updated with citation). Gates kept CLOSED: ECS system update (system-TypeId table still 7209685), savegame hook (awaits E1.1 breadcrumb), RaycastAny UUID (awaits Phase 6 stress ladder), ValueList Insert (new dedicated gate). Replication read-only path opens on matched build id + 2C §6 struct math, still no parity credit.
- [x] (2026-08-04, lead post-bump hazard sweep) The BG3_KNOWN_VERSION bump would have re-armed three consumers still hardcoding 7209685 VAs behind version guards: `prototype_managers.c` (five singleton m_ptrs via the retired `ghidra_to_runtime` + component_data_shift) and `focus_hack.c` (BaseApp slot — it WRITES the +0x142 flag through the pointer read there). Fixed by adding `status_proto_mgr_ptr`, `passives_ptr`, `interrupt_proto_mgr_ptr`, `boost_proto_mgr_ptr`, `baseapp_instance_ptr` to the offset table (values: manifest source records, exact nm on both frozen binaries; 6995620 rows get the previously-computed −0x8000 values, baseapp 0 = fail closed), moving the five manifest `source_addresses` entries into `data_singletons`, and extending the audit's ADDITIONAL_TABLE_FIELDS. The two LEGACY combat defines in entity_system.c are dead (no consumers) and stay as documented anchors; fixed_string.c's hardcoded fallback is probe-validated and unreachable while the table path works.
- [x] (2026-08-04) Offline ladder green: build clean, Tier 0 68/68, full harness pytest **288 passed, 0 xfailed** (both Wave 2A xfails converted to passing checks against the installed binary; five new per-version singleton fields nm-audited every run). Patch + `!identity` + `!test` + `!test_ingame` remain for the Phase 5 controlled live session.
- [ ] Complete the component-proxy diagnosis and the deferred GetReplicationFlags, RaycastAny, and E1.1 savegame probes on build 7398727.
- [ ] Update this document's discoveries, decisions, progress, and retrospective with final commands and evidence.

## Surprises & Discoveries

- Observation: the May “Metathesis” plans are mostly design documents, but a narrower and useful resolver was implemented later.
  Evidence: `tools/port_offsets.py`, `tools/offset_manifest.json`, and `docs/PORTING.md` exist; no `tools/bindiff/`, `apply_migration.py`, Mach-O parser module, string-xref engine, or pattern database from the May design exists.

- Observation: the implemented resolver handles most of the curated core without Ghidra, but it is not yet safe to apply unchanged to 7398727.
  Evidence: `python3 tools/port_offsets.py resolve --binary <installed> --version 4.1.1.7398727` indexed 726,413 symbols, resolved 50 addresses, and carried nine struct offsets. It correctly reported that `BinkManager::LoadVideo` moved from `0x10390b6cc` to `0x103916380` and must be promoted out of the “constant” category. It also left `global_switches_ptr` and `osiris_interface_ptr` manual.

- Observation: “not exported” usually does not mean “pattern-only” on macOS BG3. Plain `nm` sees local symbols that `nm -gU` and `dlsym` do not.
  Evidence: the new binary resolves local `t` symbols such as `CRPGStats_Modifier_ValueList::Insert` at `0x101c42014` and `phx::PhysXScene::RaycastAny` at `0x105c598b4`, as well as local BSS singletons.

- Observation: the existing audit has 73 collected pytest cases, of which exactly 68 are address-sensitive and currently fail against the new binary by design.
  Evidence: 35 direct-source symbol cases plus 30 offset-table symbol cases make 65 individually parameterized symbol checks; the remap-column test adds one symbol-backed case covering 18 named functions; the `m_State` GOT-slot test adds one; and the Osiris disassembly-slot test adds one. Thus 66 of the 68 address-sensitive test cases are symbol-backed, one is GOT-backed, and one is disassembly/pattern-backed. Deduplicating overlap among direct constants, the offset table, and remap rows yields 57 distinct `nm`-audited preferred virtual addresses. `global_switches_ptr` is a second pattern-only core address but is currently a documented audit exclusion with a runtime read-back guard rather than its own failing test.

- Observation: representative singleton migration is mechanical and non-uniform.
  Evidence: arm64 `nm | c++filt` on 7398727 produced `esv::EocServer::m_ptr = 0x1089c6f58`, `ecl::EocClient::m_ptr = 0x1089c4fc0`, `eoc::SpellPrototypeManager::m_ptr = 0x1089f3320`, `eoc::Passives::m_ptr = 0x1089ec8c8`, `RPGStats::m_ptr = 0x1089fddd0`, and `ls::ResourceManager::m_ptr = 0x108ac8080`. Their 7209685 values were respectively `0x1089968b8`, `0x108994968`, `0x1089c2c80`, `0x1089bc228`, `0x1089cd730`, and `0x108a97070`.

- Observation: the current 1,999-entry generated TypeId header cannot be migrated by one `component_data_shift`.
  Evidence: `tools/extract_typeids.py` finds 2,004 unique component TypeIds in 7398727. Compared by generated identifier, 1,998 names overlap the current header, six names were added, and one was removed. The 1,998 shared entries have six address deltas: `0x2ff30` for 1,689 entries, `0x2ffd8` for 162, `0x30668` for 86, `0x30278` for 42, `0x30660` for 17, and `0x30010` for two. A single anchor would silently misaddress 309 shared components.

- Observation: the six new generated names are UGC cache/request singleton components, and the removed name is `ecl::mod::RequestItemInfoSingletonComponent`.
  Evidence: the generated identifier diff adds `ls::ugc::{CacheModDependencies,CacheModInfo,CacheModList,PendingModDependencyRequest,PendingModInfoRequest,PendingModListRequest}SingletonComponent` and removes `ecl::mod::RequestItemInfoSingletonComponent`.

- Observation: all nine replication-context globals in `src/entity/replication_flags.c` remain symbol-backed and should not be hand-migrated.
  Evidence: plain arm64 `nm` resolves the 7398727 `ecs::sync::ReplicatedTypeContext` TypeIds, including God `0x1089329b8`, GameObjectVisual `0x108935b60`, AvailableLevel `0x1089415b0`, DisplayName `0x108944d10`, ActionResources `0x10894a8c0`, Stats `0x10894abc0`, Classes `0x10894abd0`, EocLevel `0x10894abf0`, and CombatParticipant `0x10894c700`.

- Observation: the PhysX vtable slot mapping survived the update, but that is not a complete runtime ABI proof.
  Evidence: `test_installed_arm64_physxscene_vtable_dispatch` passes against 7398727 and still maps slot 10 to `phx::PhysXScene::RaycastAny`. The zeroed optional aggregate, level lifecycle, masks, and repeated dispatch still require live verification before changing the UUID gate.

- Observation: the next harness patch would destroy the only immediately available old binary for binary-diff work.
  Evidence: the harness-managed `Baldur's Gate 3.bg3se-original` still has arm64 UUID `9A647311-E263-3FF2-AF98-111CEDCB3034`, but `tools/bg3se_harness/patch.py` deliberately unlinks an existing backup when it sees a vanilla updated executable, then replaces it with the new build.

- Observation: the current function-remap implementation is structurally limited to two builds.
  Evidence: `src/core/offset_table.c` defines `FnRemap { addr_6995620, addr_7209685 }` and hard-codes the active version to column 0 or 1. `tools/port_offsets.py` explicitly warns that a third version needs a schema change.

## Decision Log

- Decision: use repository-local source, audit tests, and the installed binaries as authoritative; treat stale skill and historical-plan addresses only as leads.
  Rationale: the sampled skill table predates the July migration, while current `nm` output and `tests/harness/test_offset_audit.py` encode the actual 7209685 state.
  Date/Author: 2026-08-04 / Codex

- Decision: use `tools/port_offsets.py` as the primary migration path and implement only the targeted Metathesis fallback needed for anonymous or disappeared symbols.
  Rationale: the current binary retains names for nearly every active function and singleton. Building the unimplemented May `tools/bindiff/` architecture before restoring compatibility would add risk and delay. Old/new masked comparison remains mandatory for the two anonymous globals and as a fallback when an exact symbol disappears.
  Date/Author: 2026-08-04 / Codex

- Decision: replace raw-address remapping with a version-scalable game-function identifier interface.
  Rationale: adding a third fixed column repeats the same migration hazard. Callers should request a named function ID; the active version row supplies its address. Bink and ValueList Insert then become ordinary manifest entries rather than exceptional constants.
  Date/Author: 2026-08-04 / Codex

- Decision: stop using `VersionOffsets.component_data_shift` for generated and replication TypeIds.
  Rationale: six observed delta families disprove the uniform-shift premise for 7398727. Generate symbol records and resolve each TypeId independently, retaining `component_data_shift` only for explicitly documented legacy consumers until they too are migrated per symbol.
  Date/Author: 2026-08-04 / Codex

- Decision: land the HandleToUuid formatter correction before address migration and prove `guid_to_string(guid_parse(text)) == text` natively.
  Rationale: the defect is offset-independent and is an inverse-conversion bug, not a mapping-address bug.
  Date/Author: 2026-08-04 / Codex

- Decision: treat ValueList registry recovery as a migration deliverable, not as unrelated feature work.
  Rationale: every enum name resolved to NULL during the final 7209685 session, strongly suggesting that the July migration preserved the Insert function but missed or misread the registry root/layout.
  Date/Author: 2026-08-04 / Codex

- Decision: keep every subsystem-specific build/UUID gate closed until that subsystem's address and ABI are independently proved.
  Rationale: changing `BG3_KNOWN_VERSION` proves neither a Dobby wrapper ABI nor a virtual-call aggregate ABI. `FUNCTOR_ADDRS_VERIFIED_BUILD`, `COMPONENT_OPS_VERIFIED_BUILD`, `ECS_SYSTEM_UPDATE_VERIFIED_BUILD`, `SAVEGAME_HOOK_VERIFIED_BUILD`, and the RaycastAny UUID must move separately or remain on 7209685.
  Date/Author: 2026-08-04 / Codex

- Decision: no parity credit changes occur in the migration waves.
  Rationale: GetReplicationFlags has not satisfied its live value checklist, RaycastAny has not completed its stress ladder, and both must remain `behavioral_gap` until new 7398727 evidence exists.
  Date/Author: 2026-08-04 / Codex

## Outcomes & Retrospective

(Pending completion. Record the final address-bucket counts, TypeId additions and removals, offline test totals, live Tier 1/Tier 2 totals, the three bug verdicts, deferred-probe verdicts, and any gates intentionally left closed.)

## Context and Orientation

The repository root is
`/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos`.
The installed executable is
`/Users/tomdimino/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3`.
The current executable is a universal x86_64/arm64 Mach-O whose plist reports
`4.1.1.7398727`; `dwarfdump --uuid` reports the arm64 UUID stated above. Its
`otool -L` output contains no `libbg3se` load command. The harness-managed old
binary is the same path with `.bg3se-original` appended and still identifies as
the 7209685 arm64 binary.

A preferred virtual address, abbreviated VA, is the address recorded in the
Mach-O before address-space randomization. BG3's preferred base is
`0x100000000`; a relative virtual address, abbreviated RVA, is `VA -
0x100000000`. A symbol-backed address is recovered by exact demangled name with
plain `nm`, including lowercase local symbols. A GOT-backed address is an
indirect pointer slot recovered with `otool -Iv`. Pattern migration compares
the old and new arm64 instruction/data neighborhoods while masking relocation
fields such as ADRP pages and branch targets. A struct offset is an offset
inside an allocated object, such as `EocServer + 0x288`; it is not an absolute
game address and must be carried only after static or live layout evidence.

The core address authority is split among `src/core/offset_table.c`,
`src/core/offset_table.h`, `tools/offset_manifest.json`, and
`tools/port_offsets.py`. `src/core/version_detect.c` contains three sentinel
VAs; `src/core/version_detect.h` contains `BG3_KNOWN_VERSION`. The current
audit is `tests/harness/test_offset_audit.py`. The full list of direct source
consumers includes `src/entity/entity_system.c`,
`src/entity/entity_storage.h`, `src/stats/prototype_managers.c`,
`src/stats/functor_types.h`, `src/stats/stats_manager.c`,
`src/strings/fixed_string.c`, `src/network/protocol.h`,
`src/game/video_skip.c`, `src/game/focus_hack.c`,
`src/resource/resource_manager.c`, `src/localization/localization.c`, and
`src/audio/audio_manager.c`.

The generated component family is defined by `tools/extract_typeids.py`,
`src/entity/generated_typeids.h`,
`src/entity/generated_component_registry.c`, and the 163-entry curated overlay
in `src/entity/component_typeid.c`. The curated entries carry metadata such as
proxy and one-frame behavior and must not remain a second independent address
authority. The nine replication indices in
`src/entity/replication_flags.c` use a different template context,
`ecs::sync::ReplicatedTypeContext`, and therefore need a separate generated
symbol family.

The enum regression is rooted in `src/stats/stats_manager.c`:
`RPGStats::m_ptr` is symbol-backed, the source assumes
`ModifierValueLists == RPGStats + 0x08`, and Insert is still hardcoded at
`VALUELIST_INSERT_ADDRESS`. The migration must independently re-prove the
RPGStats-to-manager root, the manager's element array, the enumeration name at
`+0x00`, and the ValueList bucket layout. Resolving Insert alone does not fix a
NULL result from `find_rpgenumeration_by_name()`.

The GUID defect is in `src/entity/guid_lookup.c`. `guid_parse()` constructs the
engine's byte-swizzled representation, but `guid_to_string()` currently treats
the two 64-bit words as already arranged textual UUID fields. Fix the formatter
as the exact inverse of the parser and use it for HandleToUuid and map output.

The RaycastAny production gate is in `src/level/level_manager.c`. It requires
both `version_detect_matches()` and a hardcoded arm64 LC_UUID byte array before
dispatching `PHYSICS_VMT_RAYCAST_ANY`, slot 10. The offline vtable audit proves
the slot name, not the zeroed optional aggregate under all live conditions.
The replication reader in `src/entity/replication_flags.c` similarly returns
false silently when `version_detect_matches()` is false, so tests must capture
an actual numeric result rather than merely “no crash.”

### Measured migration buckets

| Bucket | Measured size and boundary | Required method |
|---|---|---|
| Offline symbols and Mach-O metadata | 57 distinct VAs in the current `nm` audit; the current resolver resolves 50 core addresses, with Bink, ValueList Insert, nine replication TypeIds, and 2,004 generated component TypeIds also demonstrably named. These groups overlap and must not be summed as independent source sites. | Plain arm64 `nm` plus `c++filt`; `otool -Iv` for the one `m_State` GOT slot; `dwarfdump`/load-command parsing for UUID. Exact symbol and signature, never name substring alone. |
| Binary diff / Metathesis fallback | Two confirmed anonymous core globals: `global_switches_ptr` and `osiris_interface_ptr`. Any exact symbol that disappears joins this bucket. Non-exported local functions do not join merely because `dlsym` cannot see them. | Preserve both binaries; compare masked ARM64 neighborhoods and reference roles; require a unique candidate and corroborating ADRP+LDR or named-caller evidence. Never auto-apply a cluster delta by itself. |
| Fresh Ghidra or equivalent focused disassembly | Mandatory: ValueList registry root/layout. Conditional: any of the nine carried struct offsets that fail named-function disassembly, the two anonymous globals if targeted decoding is ambiguous, component-proxy layouts after TypeId repair, and subsystem ABIs for functors/component ops/system updates/savegame. RaycastAny's worker/optional ABI gets a fresh static diff even though its vtable slot passed. | Import a separately named 7398727 arm64 program; use string/XREF and named-symbol anchors; record instructions and data-flow in versioned `ghidra/offsets/` evidence. |
| Live runtime probes | ValueList manager contents, component proxy reads, replication values and SyncBuffers ownership, RaycastAny stress/lifecycle behavior, and the E1.1 write/read savegame breadcrumb. | Guarded diagnostics on a loaded disposable save, with `!identity` captured first and exact returned values logged. |

## Plan of Work

### Phase 0: freeze evidence and preserve recovery inputs

Before any harness command that can patch the game, copy the old
`.bg3se-original` into ignored `build/migration-binaries/4.1.1.7209685/` and
copy the new vanilla executable into the corresponding 7398727 directory, or
record an equally durable explicit path. Record SHA-256 hashes, architectures,
versions, UUIDs, and arm64 slice offsets. Do not let the harness refresh its
backup until this is complete. Create the 7398727 Ghidra import as a separate
program or project; never replace the 7209685 analysis needed for comparison.

The phase gate is two immutable inputs whose hashes and arm64 UUIDs can be
rechecked after every tool run. No source address is changed in this phase.

### Phase 1: land offset-independent correctness and mechanical symbol migration

Fix `guid_to_string()` so it reverses every transformation performed by
`guid_parse()`. Add native tests using the exact live host example
`b4d01c83-25e9-6156-d91b-84e619b3757d`, the incorrect observed output
`757d19b3-84e6-d91b-6156-25e9b4d01c83`, all-zero and all-`ff` UUIDs, and mixed
case input. The observable contract is a canonical lowercase UUID and exact
round trip.

In parallel, extend the manifest/resolver so every active absolute address has
one named recipe. Introduce a `GameFunctionId` (or equivalently typed stable
identifier) and a per-version address array. Callers resolve by ID rather than
passing a hardcoded address from either old column. Migrate all active
`offset_table_remap_fn()` consumers, promote Bink to an ordinary function ID,
and add ValueList Insert. Keep the old entry points temporarily only if a test
proves they fail closed; remove active dependence on the two-column schema by
the end of the phase.

Add the 7398727 `VersionOffsets` row from exact symbol output, but leave the two
anonymous fields zero until Phase 2 evidence exists. Add a resolver validation
that rejects a claimed constant when its symbol moved and rejects a scalar
TypeId shift when more than one shared-entry delta is observed. The emitted C
must be deterministic and must name every unresolved item.

### Phase 2: bulk TypeIds and targeted Metathesis fallback

Change `tools/extract_typeids.py` to parse and preserve each raw mangled symbol,
component name, context, preferred VA, and build identity. Generate component
records that resolve each exported `ComponentTypeIdContext` symbol independently
at runtime, with the exact generated VA retained for offline audit. This avoids
applying one `component_data_shift` to six distinct delta families. Generate a
separate `ReplicatedTypeContext` table for the nine supported replication
components and consume it from `replication_flags.c` instead of embedding nine
VAs by hand.

Regenerate both the header and registration source from 7398727. Require an
explicit added/removed report: 2,004 current components, 1,998 shared names, six
additions, and one removal are the present expected observation. Merge the 163
curated records with the generated address authority by name; their size,
proxy, and one-frame metadata remains curated, but their ordinary TypeId VAs do
not. Investigate the two curated one-frame pointer entries separately because
their old symbols were already exceptional and may have been removed.

For `global_switches_ptr` and `osiris_interface_ptr`, run the targeted
Metathesis fallback against the preserved old and new arm64 slices. Resolve
Osiris from the named `osi::OsirisInterface::OsirisQuery` prologue's relevant
ADRP+LDR target. Resolve global switches from the high-frequency anonymous slot
written by `App::CreateGlobalSwitches`, not by applying a data delta. Add the
result and derivation to the manifest and audit. If either candidate is not
unique, leave its field zero and escalate only that item to Ghidra.

### Phase 3: fresh RE and the ValueList migration deliverable

Re-import or analyze 7398727 and re-derive the complete path from
`RPGStats::m_ptr` to `ModifierValueLists`, including manager count, element
buffer, per-element name, bucket array, item count, and Insert ABI. The named
Insert address is already known as `0x101c42014`, but its call remains disabled
until the object pointer and `x1` reference ABI are confirmed. Update
`stats_manager.c` to consume the typed function ID from Phase 1 rather than a
literal VA.

Add a read-only diagnostic that reports the manager address, count, and a
bounded sample of resolved names before enabling mutation. Offline tests must
cover malformed counts and NULL buffers. The first live proof must resolve at
least `DamageType`, `Ability`, and `Skill` in both directions. Only then run one
unique-label insertion, verify count growth by exactly one, duplicate rejection,
label-to-index and index-to-label identity, and stats-reload survival. This is
the acceptance gate for the live-found enum-registry defect.

During the same static pass, compare the named 7398727 functions that underlie
the carried struct offsets and subsystem-specific build gates. Address equality
is insufficient for functor, component-ops, system-update, and savegame hooks;
record the wrapper argument registers, hidden result storage, entry bytes, and
the exact build whose ABI was proved. Any unproved gate remains on 7209685 and
closed.

### Phase 4: lead integration, version gates, and offline validation

The lead integrates completed units and owns `src/injector/main.c` and
`src/lua/lua_ext.c` throughout. Update the three version-detect sentinel VAs and
add/select the 7398727 offset row. Bump `BG3_KNOWN_VERSION` only after every
active unguarded address is resolved and the audit has no stale target.

Do not mechanically bump the independent gates. Move
`FUNCTOR_ADDRS_VERIFIED_BUILD` only after every wrapper signature is rechecked;
move `COMPONENT_OPS_VERIFIED_BUILD` and `ECS_SYSTEM_UPDATE_VERIFIED_BUILD` only
after their structures and calls are re-proved; leave
`SAVEGAME_HOOK_VERIFIED_BUILD` at 7209685 until the later E1.1 runtime spike.
Keep the old RaycastAny UUID byte array during offline integration and the first
general live session. Add one-shot gate diagnostics so tests distinguish a
closed gate from a real dispatch.

Refactor `tests/harness/test_offset_audit.py` to select the installed version's
row and the version-scalable function table rather than parse a hardcoded
7209685 column. Preserve the existing 68 address-sensitive checks as a named
baseline, add coverage for Bink, ValueList, replication TypeIds, generated
TypeIds, global switches, and all new table fields, and keep the five structural
tests. The phase ends only when the existing 68 are green, the expanded audit
is green, Tier 0 and all pytest tests pass, and the dylib builds.

### Phase 5: controlled live session and the component-proxy defect

Only now allow the harness to replace its managed backup and patch the vanilla
7398727 executable. Restart the game after the new dylib build. Capture
`!identity` before any other result and require the detected version, expected
version, UUID/gate diagnostics, session initialization, stats readiness, and
game state to identify one coherent process.

Run `!test` before loading or mutating a save, then load a known disposable save
and run `!test_ingame`. Explicitly re-run the UUID roundtrip, all-UUID map,
ValueList read/insertion tests, and host position lookup. For the component
proxy failure, probe the same host through direct component lookup and through
the proxy for `ls::TransformComponent` and `ls::uuid::Component`. Compare the
resolved TypeIndex, component address, selected property definition source,
field offset, proxy lifetime, and returned value. Repair TypeId selection first;
only change `component_offsets.h` or generated property definitions if a direct
memory read plus new static evidence proves a layout change. Acceptance is a
non-nil `Transform.Translate`, the canonical `EntityUuid`, and the host found
within five metres of its own XZ position.

### Phase 6: deferred live verification in strict order

First complete the GetReplicationFlags checklist from
`ghidra/offsets/REPLICATION_SYNCBUFFERS.md`. Capture numeric return values, all
nine replication indices relative to pool size, a known entity's hash-chain
match, inline and heap bitset modes, qword bounds, the dirty transition, and the
subsequent clear. A no-error nil result is not a pass.

Second re-prove RaycastAny. The offline vtable test already shows slot 10 still
names RaycastAny; compare the 7398727 wrapper/worker ABI and use a temporary,
explicitly diagnostic-only build gate to exercise dispatch without changing
the production UUID constant. Run no-hit, hit, include/exclude-mask, long and
short ray, repeated-call, level unload/reload, and memory-growth rungs. Only
after those rungs pass may the lead replace `s_raycast_any_verified_uuid` with
the 16 bytes of `0C51CAED-6D60-3DCD-9299-8519C92631B0` and rerun the full ladder
through the production API.

Third run the E1.1 savegame feasibility spike on disposable saves. Re-resolve
`esv::OsirisVariableHelper::SavegameVisit` by its local symbol, verify the first
16 entry bytes for 7398727, leave `SAVEGAME_HOOK_VERIFIED_BUILD` closed in the
release path, and install only a breadcrumb diagnostic. Observe one write and
one read callback with correct direction and caller evidence. Do not enter a
visitor region or serialize payload in this spike. Promote E1.1 only if both
directions are repeatable without save mutation or instability.

GetReplicationFlags and RaycastAny remain `behavioral_gap` in
`docs/parity-100/contract.json` throughout migration. A later evidence update
may reconsider credit only after the corresponding 7398727 checklist is
complete. The E1.1 result changes feasibility documentation, not parity by
itself.

### Parallel implementation waves and file ownership

No two concurrent units may edit the same file. Each wave ends with a lead
review and integration gate before ownership changes. `src/injector/main.c` and
`src/lua/lua_ext.c` are always lead-owned, even when a builder supplies a patch
suggestion or test string.

| Wave / unit | Files | Approach | Verification |
|---|---|---|---|
| Wave 0, lead: evidence freeze | `build/migration-binaries/**` only; this plan's living sections | Preserve both binaries and identities before the harness patch refreshes its backup. Create a separately named 7398727 Ghidra program. | Hashes and UUIDs reproduce; old remains `9A647311-...`, new remains `0C51CAED-...`; `otool -L` on new is vanilla. |
| Wave 1A, sol builder: core resolver and scalable function IDs | `tools/port_offsets.py`, `tools/offset_manifest.json`, `src/core/offset_table.c`, `src/core/offset_table.h`, `tests/harness/test_offset_audit.py`, `docs/PORTING.md`, and the active remap consumers in `src/strings/fixed_string.c`, `src/resource/resource_manager.c`, `src/localization/localization.c`, `src/audio/audio_manager.c`, `src/stats/functor_hooks.c`, `src/network/net_hooks.c`, and `src/entity/component_registry.c` | Replace the two-column address remap with typed function IDs and per-version addresses; add 7398727; promote Bink; add ValueList; preserve fail-closed behavior. | Resolver has no unclassified active address, rejects multi-delta TypeId assumptions, emits deterministic 7398727 data, and the expanded offset audit passes after anonymous fields are supplied. |
| Wave 1B, sol builder: TypeId and replication generation | `tools/extract_typeids.py`, `src/entity/generated_typeids.h`, `src/entity/generated_component_registry.c`, `src/entity/component_typeid.c`, `src/entity/component_typeid.h`, `src/entity/replication_flags.c`, `src/entity/replication_flags.h`, plus new narrowly named generator tests | Generate mangled-symbol-backed per-entry records for ordinary and replication contexts; retain curated metadata by name; remove the uniform-shift dependency for these families. | Exact extractor/header/registry set equality; expected 2,004 current names and 6-add/1-remove report; all nine replication symbols match `nm`; no generated record is resolved by a single family delta. |
| Wave 1C, sol builder: GUID inverse | `src/entity/guid_lookup.c`, `src/entity/guid_lookup.h`, new `tests/tier0/test_guid_lookup.c` | Implement the formatter as the inverse of the existing parser and add table-driven canonicalization/roundtrip cases. Do not touch entity binding registration. | Native tests reproduce the old bad host result before the fix and the canonical host GUID after it; parse-format-parse preserves all 16 bytes. |
| Wave 1, lead integration | `CMakeLists.txt`, `tests/tier0/test_main.c`, `src/injector/main.c`, `src/lua/lua_ext.c` only | Wire the new native test and reconcile interfaces. Do not bump version or subsystem gates yet. | Tier 0 builds/runs; no builder-owned file is edited concurrently; source still fails closed on 7398727. |
| Wave 2A, sol builder: anonymous-address Metathesis | new `scripts/re/migrate_anonymous_globals.py`, new `ghidra/offsets/ADDRESS_MIGRATION_7398727.md` | Compare old/new arm64 slices, decode targeted ADRP+LDR references, and report unique candidates for global switches and Osiris. Never write source automatically. | Old binary self-test reproduces `0x108af4f30` and `0x108a86128`; new candidates have corroborating writers/callers and unique roles. |
| Wave 2B, sol builder: ValueList RE and fix | `src/stats/stats_manager.c`, `src/stats/stats_manager.h`, `src/lua/lua_stats.c`, new versioned ValueList evidence under `ghidra/offsets/`, and new stats-focused harness tests outside `lua_ext.c` | Re-derive the registry root and manager/value-list layouts, consume the typed Insert ID, expose bounded diagnostics, and restore read-before-write behavior. | Offline malformed-layout tests pass; Ghidra/otool evidence identifies every pointer step and ABI; live phase later resolves three canonical enums before one guarded insertion. |
| Wave 2C, sol builder: build-specific ABI review | Versioned evidence files under `ghidra/offsets/` and new read-only audit tests; no production gate constants | Diff functor, component-ops, ECS-system-update, RaycastAny, and savegame entry signatures/bytes. Report pass, changed, or unresolved per subsystem. | Each recommendation cites exact 7398727 symbol/instructions and wrapper registers; unresolved means “keep gate closed,” never an inferred bump. |
| Wave 2, lead integration and gates | `src/core/version_detect.c`, `src/core/version_detect.h`, `src/stats/functor_types.h`, `src/entity/entity_system.c`, `src/entity/ecs_system_update.c`, `src/game/savegame_hook.c`, `src/level/level_manager.c`, `src/injector/main.c`, `src/lua/lua_ext.c`, `CMakeLists.txt`, `tests/tier0/test_main.c` | Insert anonymous values, update sentinels and known version after audits, selectively move only proved subsystem gates, and add self-certifying live tests. Keep Raycast UUID and savegame release gate old. | Existing 68 address-sensitive checks plus expanded checks pass; Tier 0, pytest, and build pass; grep confirms no unreviewed 7398727 gate promotion. |
| Wave 3, lead: controlled launch | Harness-managed game binary and ignored session artifacts only | Patch only after offline success, restart, capture identity, run Tier 1 and Tier 2, and dispatch focused probes. | One PID/build identity; `!test` and `!test_ingame` green; three live-found defects each have value-level evidence. |
| Wave 4A, sol builder after first live evidence: component proxy | `src/entity/component_property.c`, `src/entity/component_property.h`, and only the proved generated/property-layout files; never `main.c` or `lua_ext.c` | Trace direct component pointer versus proxy lookup and repair the smallest proved layer. | Host Transform and UUID proxy reads match direct memory and position query finds host within five metres. |
| Wave 4B, sol builder after first live evidence: replication probe | `src/entity/replication_flags.c`, `src/entity/replication_flags.h`, and a dedicated diagnostic/test module | Add value-capturing diagnostics and complete the documented SyncBuffers checklist without writes. | Numeric flags, pool/index bounds, hash key, bitset modes, dirty lifecycle, and nil-versus-zero semantics are captured. |
| Wave 4C, sol builder after first live evidence: Raycast/save breadcrumbs | Dedicated diagnostic source/tests and versioned evidence docs; production `src/level/level_manager.c` and `src/game/savegame_hook.c` remain lead-owned | Supply temporary diagnostic-only gates and evidence; lead alone changes production UUID or savegame gate after proof. | Raycast stress ladder completes; save breadcrumb sees repeatable write and read on disposable saves without payload mutation. |

## Concrete Steps

All commands below run from
`/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos` unless a
different working directory is shown. Paths are quoted because the game path
contains spaces and an apostrophe.

First preserve the binaries before any `bg3se_harness patch`, `launch`, or
`test` command:

```bash
export BG3_NEW_BIN="/Users/tomdimino/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3"
export BG3_OLD_BIN="${BG3_NEW_BIN}.bg3se-original"
export MIGRATION_BIN_DIR="$PWD/build/migration-binaries"
mkdir -p "$MIGRATION_BIN_DIR/4.1.1.7209685" "$MIGRATION_BIN_DIR/4.1.1.7398727"
cp -p "$BG3_OLD_BIN" "$MIGRATION_BIN_DIR/4.1.1.7209685/BaldursGate3"
cp -p "$BG3_NEW_BIN" "$MIGRATION_BIN_DIR/4.1.1.7398727/BaldursGate3"
shasum -a 256 "$MIGRATION_BIN_DIR/4.1.1.7209685/BaldursGate3" "$MIGRATION_BIN_DIR/4.1.1.7398727/BaldursGate3"
dwarfdump --uuid "$MIGRATION_BIN_DIR/4.1.1.7209685/BaldursGate3"
dwarfdump --uuid "$MIGRATION_BIN_DIR/4.1.1.7398727/BaldursGate3"
```

Expected UUID lines contain `9A647311-E263-3FF2-AF98-111CEDCB3034` for the old
arm64 slice and `0C51CAED-6D60-3DCD-9299-8519C92631B0` for the new arm64
slice. Record the hashes in Progress before continuing.

Reproduce the mechanical core resolution without applying it:

```bash
python3 tools/port_offsets.py resolve \
  --binary "$MIGRATION_BIN_DIR/4.1.1.7398727/BaldursGate3" \
  --version 4.1.1.7398727 --emit \
  > build/offset-migration-7398727.txt
```

Before implementation this exits nonzero with one Bink “constant changed”
error, two manual anonymous-slot warnings, `component_data_shift = 0x2ff30`,
and “resolved 50 addresses; 9 struct offsets carried.” After implementation it
must exit zero, contain no manual active address, and must not claim that one
TypeId shift applies to every generated record.

Sample exact named addresses and signatures directly:

```bash
nm -arch arm64 "$BG3_NEW_BIN" | c++filt | \
  rg 'EocServer::m_ptr|EocClient::m_ptr|SpellPrototypeManager::m_ptr|Passives::m_ptr|RPGStats::m_ptr|ResourceManager::m_ptr|CRPGStats_Modifier_ValueList::Insert|BinkManager::LoadVideo'
```

Generate the TypeId comparison report into ignored build output before changing
source:

```bash
python3 tools/extract_typeids.py "$BG3_NEW_BIN" \
  > build/generated_typeids-7398727.h
python3 tools/extract_typeids.py "$BG3_NEW_BIN" --registry \
  > build/generated_component_registry-7398727.c
```

The pre-refactor script reports 2,004 components. The implemented generator
must also emit a machine-checkable set/delta report and the separate
`ReplicatedTypeContext` records. Diff names and symbols, not only line counts.

Run the targeted old/new anonymous resolver after it exists:

```bash
python3 scripts/re/migrate_anonymous_globals.py \
  --old "$MIGRATION_BIN_DIR/4.1.1.7209685/BaldursGate3" \
  --new "$MIGRATION_BIN_DIR/4.1.1.7398727/BaldursGate3" \
  --target global_switches_ptr --target osiris_interface_ptr
```

Expected output is one new preferred VA per target, the old self-test VAs,
candidate counts, named anchor/caller evidence, and no source write.

Run the installed-binary audits before changing a production gate:

```bash
XONSH_HISTORY_BACKEND=dummy PYTHONPATH=tools pytest -q \
  tests/harness/test_offset_audit.py
XONSH_HISTORY_BACKEND=dummy PYTHONPATH=tools pytest -q \
  tests/harness/test_physics_vmt_audit.py
```

The first command must report the 68 address-sensitive cases green and all 73
existing file cases green before counting any newly added cases. The second
must map every audited 7398727 vtable slot to the expected demangled function.

Build and run the full offline ladder:

```bash
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build
./build/bin/bg3se_test_tier0
XONSH_HISTORY_BACKEND=dummy PYTHONPATH=tools pytest -q tests/harness
```

Expected output is a successful universal dylib build, all Tier 0 tests passing,
and no pytest failure or unexpected skip when the installed game is present.
Do not accept a green build with a resolver error, a zero active field, or an
ABI-specific gate opened by the global version bump alone.

Only after all offline commands pass, patch and launch through the harness:

```bash
PYTHONPATH=tools python3 -m bg3se_harness patch
PYTHONPATH=tools python3 -m bg3se_harness launch --continue
```

The patch step is intentionally delayed because it refreshes
`.bg3se-original`. Verify injection and identity from a new process:

```bash
otool -L "$BG3_NEW_BIN" | rg libbg3se
echo '!identity' | nc -U /tmp/bg3se.sock
echo '!test' | nc -U /tmp/bg3se.sock
echo '!test_ingame' | nc -U /tmp/bg3se.sock
```

Expected identity includes `session_init:"complete"`, `stats_ready:true`,
`game_state:"Running"`, detected and expected version `4.1.1.7398727`, and the
new UUID/gate diagnostic. Record full Tier 1 and Tier 2 totals rather than only
the last line.

Run focused probes from the same identity-confirmed session. The exact Lua
wrappers may be packaged as named Tier 2 tests, but their required observations
are: canonical HandleToUuid output; all-UUID map contains the canonical host
key; enum manager count and names; bidirectional enum lookups; host Transform
and EntityUuid proxy values; numeric replication flags; and explicit RaycastAny
dispatch evidence. Finish with the disposable-save breadcrumb only after the
general suite is stable.

## Validation and Acceptance

The migration is accepted only if every gate below is observable and recorded.

| Gate | Acceptance |
|---|---|
| Binary identity | Archived old/new hashes remain stable; installed plist is `4.1.1.7398727`; arm64 UUID is `0C51CAED-6D60-3DCD-9299-8519C92631B0`; injection occurs only after offline validation. |
| Address inventory | Every active absolute game address is in the manifest or a generated symbol table with a named resolution method. `port_offsets.py` exits zero. Bink is no longer classified constant. ValueList Insert and all nine replication globals are audited. Anonymous globals have disassembly evidence and tests. |
| Existing offset audit | All 68 address-sensitive checks pass against the installed 7398727 arm64 slice. The whole pre-existing file reports 73/73, and newly added checks also pass. No row or remap parser is hardcoded to 7209685. |
| TypeIds | Generated source exactly matches the 2,004 symbols in the installed build; the current 6-add/1-remove diff is reviewed; each address is resolved independently; curated metadata joins by name; exceptional one-frame entries have explicit verdicts. A test fails if a future build has more than one delta but tooling tries to emit one scalar shift. |
| ABI gates | Global version bump does not open functor, component-ops, ECS-update, savegame, or Raycast UUID gates. Each moved gate cites 7398727 instruction evidence. Unproved gates remain closed and log why. |
| Offset-independent bug | `guid_to_string(guid_parse("b4d01c83-25e9-6156-d91b-84e619b3757d"))` returns the same canonical string, and live HandleToUuid/UuidToHandle roundtrip succeeds. |
| Enum migration bug | Before insertion, `DamageType`, `Ability`, and `Skill` resolve by name and both lookup directions return real values. One unique label grows a mutable enum once, rejects a duplicate, survives stats reload, and never mutates a primitive/flag list. |
| Component proxy bug | The loaded host returns non-nil Transform translation and canonical EntityUuid through the proxy, values match direct component reads, and `GetEntitiesAroundPosition` finds the host within five metres of its own XZ position. |
| Offline regression | CMake build succeeds; every Tier 0 test passes; the complete harness pytest suite passes with no address audit skipped merely because the installed binary could not be parsed. |
| General live regression | One `!identity` precedes and identifies the process used for results. `!test` and `!test_ingame` pass; conditional DamageEvents testing is run with its BURNING precondition rather than waived as a code failure. |
| GetReplicationFlags deferral | The full documented probe captures a number, verifies pool/hash/bitset and dirty lifecycle, and distinguishes absent zero from gate-closed nil. Until then its contract status remains `behavioral_gap`. |
| RaycastAny deferral | Static slot and ABI checks plus the full no-hit/hit/mask/repetition/unload-reload/memory ladder pass on 7398727. Only then is the production UUID byte array changed and the ladder repeated. Until then its contract status remains `behavioral_gap`. |
| E1.1 savegame spike | On disposable saves, a diagnostic-only hook observes repeatable write and read calls with correct direction and unchanged save behavior. No payload serialization is added in this migration. |

## Idempotence and Recovery

All offline resolution commands are read-only with respect to the installed
binary. Generated output should first go under ignored `build/`; regenerating
it must be deterministic. The 6995620 and 7209685 offset rows stay intact, so a
7398727 failure can be isolated by selecting or removing only the new row and
its gate entries rather than rewriting history.

Archive the old binary before patching because the harness intentionally
deletes and recreates `.bg3se-original` after a Steam update. Once archived,
the patch operation is recoverable with `PYTHONPATH=tools python3 -m
bg3se_harness unpatch`, which restores the harness's new-build vanilla backup.
It will not restore the old 7209685 build; that is why the separately hashed
archive is mandatory.

Pattern migration never writes source. A result with zero or multiple
candidates leaves the relevant field at zero, which keeps the feature closed.
Ghidra imports use a new versioned program so both sides remain available. Do
not reuse a stale hardcoded fat-slice offset; derive the arm64 slice from the
universal header every run.

Live enum insertion uses one build-specific unique label and verifies duplicate
rejection, making a rerun safe within the same process. Use a fresh process for
a clean first-insert assertion. Raycast and replication read probes are
non-mutating. The savegame spike uses disposable copies and a breadcrumb-only
hook; if write/read observation is not repeatable, leave the gate closed and
restore the disposable save from its copy.

If a live crash or corrupt read occurs, stop the session, unpatch to the vanilla
7398727 backup, inspect the session log and crash report, and close only the
suspect subsystem gate. Do not force addresses with
`BG3SE_FORCE_ADDRESSES=1` as acceptance evidence. Do not run `git add`,
`commit`, `push`, or `stash`; do not modify `lib/`, `tools/vendor/`, or
`AGENTS.md` while executing this plan.

## Interfaces and Dependencies

At completion, the core must expose a version-scalable function lookup similar
to the following contract; exact names may vary, but raw mixed-vintage VAs must
not remain the caller interface:

```c
typedef enum GameFunctionId {
    GAME_FN_FIXED_STRING_CREATE,
    GAME_FN_RESOURCE_GET,
    GAME_FN_BINK_LOAD_VIDEO,
    GAME_FN_VALUELIST_INSERT,
    /* existing functor, localization, networking, and audio functions */
    GAME_FN_COUNT
} GameFunctionId;

void *offset_table_game_fn(GameFunctionId id);
```

`offset_table_game_fn()` returns NULL for an unknown version, an out-of-range
ID, or a zero/unverified address. It returns a runtime callable pointer only
from the selected version row. Function ABI gates remain independent of this
address lookup.

The TypeId generator must retain enough information to resolve an exact raw
symbol and audit its preferred VA:

```c
typedef struct GeneratedTypeIdEntry {
    const char *component_name;
    const char *mangled_symbol;
    uint64_t audited_preferred_va;
} GeneratedTypeIdEntry;
```

Ordinary component entries use `ecs::ComponentTypeIdContext`; replication
entries use `ecs::sync::ReplicatedTypeContext`. Runtime initialization resolves
each symbol independently and validates the resulting index range. Curated
metadata is keyed by component name and does not carry a duplicate ordinary
TypeId VA. Any address fallback is explicitly per build and per entry.

The ValueList Insert ABI on 7209685 was:

```c
typedef void (*StatsValueListInsertFn)(
    void *value_list,
    const uint32_t *fixed_string_index,
    int32_t value);
```

Fresh 7398727 disassembly must confirm that `x1` is still dereferenced and that
the map fields remain bucket count `+0x08`, buckets `+0x10`, and item count
`+0x18` before the function ID is enabled. The manager root and name traversal
have their own evidence gate.

`guid_to_string()` must be the exact inverse of `guid_parse()` over all valid
36-character UUIDs and emit 36 lowercase characters plus NUL. No caller-specific
swizzle may be duplicated in `entity_system.c`.

The implementation depends only on repository code, Python 3, Xcode command
line tools (`nm`, `c++filt`, `otool`, `lipo`, `dwarfdump`, `codesign`), CMake,
pytest, the existing Ghidra installation, the preserved old binary, and the
installed new binary. It adds no third-party library and changes nothing under
`lib/` or `tools/vendor/`.
