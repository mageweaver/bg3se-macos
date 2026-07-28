---
title: "Metathesis: Binary Diff & Address Migration Tool"
type: feat
status: active
date: 2026-05-13
trigger: "BG3 game update 4.1.1.6995620 → 4.1.1.7209685 shifted all hardcoded hook addresses"
origin: docs/plans/2026-05-13-002-feat-binary-diff-address-migration-tool-plan.md
---

# Metathesis: Binary Diff & Address Migration Tool

## Overview

BG3SE-macOS depends on ~60 hardcoded virtual addresses spread across 12 source files, plus ~1,999 TypeId addresses in `generated_typeids.h`. When Larian ships a game update, the binary layout shifts and every address breaks. Current recovery: full Ghidra import (~30 min), manual re-discovery of each singleton, function, and TypeId, then grepping sources and updating defines one by one.

This plan designs a reusable Python tool (`tools/bindiff/`) that compares old and new binaries offline, resolves ~95% of address shifts automatically via a 4-tier cascade, and outputs a migration manifest that patches source files in seconds.

## Problem Statement

Game updated from 4.1.1.6995620 to 4.1.1.7209685. The existing `version_detect.c` sentinel probes confirmed some addresses still work (VideoSkip hook survived), but systematic verification of all ~60 hooks requires manual Ghidra RE. This process takes hours and must repeat on every update.

## Address Inventory

### Category A: Singleton Pointer Globals (~20 addresses)

DATA/BSS segment `m_ptr` globals. Most have surviving mangled symbols in `nm` output — resolution is trivial via symbol table lookup.

| Address | Symbol | Files |
|---------|--------|-------|
| `0x10898e8b8` | `esv::EocServer::m_ptr` | entity_system.c, version_detect.c |
| `0x10898c968` | `ecl::EocClient::m_ptr` | entity_system.c, version_detect.c |
| `0x1089bac80` | `SpellPrototypeManager::m_ptr` | prototype_managers.c, version_detect.c |
| `0x1089bdb30` | `StatusPrototypeManager::m_ptr` | prototype_managers.c |
| `0x108991528` | `BoostPrototypeManager::m_ptr` | prototype_managers.c |
| `0x108aeccd8` | `PassivePrototypeManager` / GlobalStringTable | prototype_managers.c, fixed_string.c |
| `0x108aecce0` | `InterruptPrototypeManager::m_ptr` | prototype_managers.c |
| `0x1089c5730` | `RPGStats::m_ptr` | stats_manager.c, fixed_string.c |
| `0x108aefa98` | Memory manager | prototype_managers.c |
| `0x108a8f070` | `ResourceManager::m_ptr` | audio_manager.c, resource_manager.c |
| `0x108a88508` | `GlobalTemplateManager::m_ptr` | template_manager.c |
| `0x108a309a8` | `CacheTemplateManager::m_ptr` | template_manager.c |
| `0x108a735d8` | `Level::s_CacheTemplateManager` | template_manager.c |
| `0x108a3be40` | `LevelManager::m_ptr` | level_manager.c, template_manager.c |
| `0x1083c4a68` | `PTR_m_State` (staticdata) | staticdata_manager.c |
| `0x108aed088` | `TranslatedStringRepository::m_ptr` | localization.c |
| `0x108b18f30` | `GlobalSwitches` ptr | global_switches.c |

**Resolution:** `nm -gU <binary> | c++filt` → match mangled symbol → extract new address. Fallback: ADRP+LDR cross-reference from known string references.

### Category B: Function Hook Targets (~25 addresses)

TEXT segment code addresses where Dobby hooks are installed. Symbols mostly stripped. Resolution requires string xref tracing or byte-pattern matching.

| Address | Function | File | Strategy |
|---------|----------|------|----------|
| `0x10390b6cc` | `BinkManager::LoadVideo` | video_skip.c | String xref: `".bk2"` |
| `0x10110f0d0` | `EocServer::Startup` | entity_system.c | String xref: `"EocServer"` logs |
| `0x10124f92c` | `IsInCombat` | entity_system.c | Byte pattern + EntityWorld access |
| `0x101250074` | `GetCombatFromGuid` | entity_system.c | Near IsInCombat cluster |
| `0x1010dc924` | `TryGetUuidMappingSingleton` | entity_system.c | Byte pattern (TryGetSingleton) |
| `0x10636b27c` | `StorageContainer::TryGet` | entity_storage.h | Byte pattern |
| `0x1063d5998` | `GetMessage` (network) | protocol.h | String xref |
| `0x105783a38` | `ExecuteStatsFunctor` | functor_types.h | String xref: `"ExecuteFunctors"` |
| `0x105787918` | `ExecuteFunctors_AttackTarget` | functor_types.h | Cluster (anchor: ExecuteStatsFunctor) |
| `0x105787c6c` | `ExecuteFunctors_AttackPosition` | functor_types.h | Cluster |
| `0x10578975c` | `ExecuteFunctors_Move` | functor_types.h | Cluster |
| `0x10578a918` | `ExecuteFunctors_Target` | functor_types.h | Cluster |
| `0x10578e4d8` | `ExecuteFunctors_NearbyAttacked` | functor_types.h | Cluster |
| `0x10578fba8` | `ExecuteFunctors_NearbyAttacking` | functor_types.h | Cluster |
| `0x105790a28` | `ExecuteFunctors_Equip` | functor_types.h | Cluster |
| `0x105792a90` | `ExecuteFunctors_Source` | functor_types.h | Cluster |
| `0x1057965e4` | `ExecuteFunctors_Interrupt` | functor_types.h | Cluster |
| `0x10538f374` | `ProcessDealDamageFunctors` | functor_types.h | String xref |
| `0x101b752b4` | `FeatManager::GetFeats` | staticdata_manager.c | String xref or vtable |
| `0x10120b3e8` | `GetAllFeats` | staticdata_manager.c | Near FeatManager cluster |
| `0x102994834` | `Get<BackgroundManager>` | staticdata_manager.c | TryGetSingleton pattern |
| `0x10341c42c` | `Get<OriginManager>` | staticdata_manager.c | TryGetSingleton pattern |
| `0x10262f184` | `Get<ClassDescriptions>` | staticdata_manager.c | TryGetSingleton pattern |
| `0x10369700c` | `Get<ProgressionManager>` | staticdata_manager.c | TryGetSingleton pattern |
| `0x1011a4494` | `Get<ActionResourceTypes>` | staticdata_manager.c | TryGetSingleton pattern |
| `0x101f72754` | `SpellPrototype::Init` | prototype_managers.c | String xref |
| `0x105f96304` | `GetTemplateRaw` | template_manager.c | String xref or vtable |
| `0x105d31ce4` | `CacheTemplate` | template_manager.c | Near template cluster |
| `0x1060cc608` | `GetResource` | resource_manager.c | ADRP to ResourceManager ptr |
| `0x10651fb60` | `STDString::ctor` | audio_manager.c | Byte pattern (common ABI) |

### Category C: TypeId Addresses (~1,999 addresses)

All extracted via `nm -gU | c++filt | grep TypeId.*ComponentTypeIdContext`. Fully automated by existing `tools/extract_typeids.py`.

### Category D: Struct Field Offsets (~15 in-struct offsets)

Offsets within runtime objects (e.g., `OFFSET_ENTITYWORLD_IN_EOCSERVER = 0x288`). NOT address-dependent — stable across minor updates unless Larian adds/removes fields. Verified at runtime via sentinel probes.

| Offset | Meaning | File |
|--------|---------|------|
| `0x288` | EntityWorld in EocServer | entity_system.c |
| `0x1D0` | EntityWorld in EocClient | entity_system.c |
| `0xA8` | GameServer in EocServer | protocol.h |
| `0x1F8` | MessageFactory in GameServer | protocol.h |
| `0x2D0-0x2E0` | ProtocolList in GameServer | protocol.h |
| `0x650/0x65c` | ActivePeers in GameServer | protocol.h |
| `0x6ac` | SkipSplashScreen in GlobalSwitches | global_switches.c |

## Proposed Solution

### Resolution Pipeline (4-tier cascade)

```
1. SYMBOL LOOKUP       confidence=1.0   Categories A + C
   nm -gU <new_binary> | c++filt → match entry.symbol
   If found → resolved

2. STRING XREF         confidence=0.9   Category B (partial)
   Find string VA in __cstring → trace ADRP+ADD refs in __text
   Walk backward to function prologue (STP X29, X30)
   If unique match → resolved

3. BYTE PATTERN MATCH  confidence=0.8   Category B (fallback)
   Extract N-byte signature from old binary, mask relocatable fields
   Scan new binary __text for matches
   If exactly 1 match → resolved

4. CLUSTER DELTA       confidence=0.7   Category B (propagation)
   If anchor function in cluster resolved, apply relative offsets
   Verify each target has a valid function prologue
```

### Architecture

```
tools/bindiff/
├── bindiff.py                  # Main CLI entry point
├── apply_migration.py          # Source file patcher (preview + apply)
├── lib/
│   ├── __init__.py
│   ├── macho_parse.py          # Fat binary slice extraction, segment enum
│   ├── arm64_decode.py         # Minimal ARM64 decoder (ADRP, ADD, LDR, B, BL)
│   ├── symbol_diff.py          # nm-based symbol resolution (Cat A + C)
│   ├── string_xref.py          # String cross-ref via ADRP+ADD (Cat B)
│   ├── pattern_match.py        # ARM64 byte pattern matching (Cat B)
│   └── struct_verify.py        # Runtime struct offset verification (Cat D)
└── data/
    ├── hook_registry.json      # Canonical registry of all known targets
    ├── patterns.json           # Pre-extracted byte patterns per target
    └── migration_YYYYMMDD.json # Output: old→new address mapping
```

### Central Hook Registry (`hook_registry.json`)

Single source of truth for every hardcoded address. Each entry specifies:

```json
{
  "id": "eocserver_singleton",
  "category": "singleton",
  "address": "0x10898e8b8",
  "symbol": "__ZN3esv9EocServer5m_ptrE",
  "description": "esv::EocServer::m_ptr",
  "resolution": "symbol",
  "files": [
    {"path": "src/entity/entity_system.c", "define": "OFFSET_EOCSERVER_SINGLETON_PTR"},
    {"path": "src/core/version_detect.c", "line_pattern": "0x10898e8b8"}
  ],
  "string_xrefs": [],
  "byte_pattern": null,
  "cluster": null,
  "verified_version": "4.1.1.6995620"
}
```

For stripped function targets:

```json
{
  "id": "bink_load_video",
  "category": "function",
  "address": "0x10390b6cc",
  "symbol": null,
  "description": "BinkManager::LoadVideo",
  "resolution": "string_xref",
  "files": [
    {"path": "src/game/video_skip.c", "define": "VA_BINK_LOAD_VIDEO"}
  ],
  "string_xrefs": ["Splash_Logo_Larian", ".bk2"],
  "byte_pattern": null,
  "cluster": null,
  "verified_version": "4.1.1.6995620"
}
```

### CLI Interface

```bash
# Compare two binaries, produce migration manifest
python3 tools/bindiff/bindiff.py \
  --old "/path/to/Baldur's Gate 3.bg3se-original" \
  --new "/path/to/Baldur's Gate 3" \
  --registry tools/bindiff/data/hook_registry.json \
  --output tools/bindiff/data/migration_20260513.json

# Auto-detect from Steam paths (uses .bg3se-original backup)
python3 tools/bindiff/bindiff.py --auto

# Preview source file changes (dry run)
python3 tools/bindiff/apply_migration.py \
  --migration tools/bindiff/data/migration_20260513.json \
  --dry-run

# Apply to source files
python3 tools/bindiff/apply_migration.py \
  --migration tools/bindiff/data/migration_20260513.json \
  --apply

# Harness integration
PYTHONPATH=tools python3 -m bg3se_harness bindiff
PYTHONPATH=tools python3 -m bg3se_harness bindiff --apply
PYTHONPATH=tools python3 -m bg3se_harness bindiff --status
```

### Migration Output Format

```json
{
  "old_version": "4.1.1.6995620",
  "new_version": "4.1.1.7209685",
  "timestamp": "2026-05-13T14:30:00Z",
  "summary": {
    "total": 60, "resolved": 57, "unresolved": 3,
    "confidence_high": 45, "confidence_medium": 10, "confidence_low": 2
  },
  "entries": [
    {
      "id": "eocserver_singleton",
      "old_address": "0x10898e8b8",
      "new_address": "0x10899a8b8",
      "delta": 49152,
      "confidence": 1.0,
      "method": "symbol",
      "files": ["src/entity/entity_system.c", "src/core/version_detect.c"]
    }
  ],
  "unresolved": [
    {
      "id": "try_get_uuid_mapping_singleton",
      "old_address": "0x1010dc924",
      "reason": "No symbol, no string xref, pattern had 3 matches",
      "candidates": ["0x1010ec924", "0x1010fc924", "0x10110c924"]
    }
  ],
  "typeids": {
    "resolved": 1995, "missing": 4, "new": 12,
    "output_file": "src/entity/generated_typeids.h"
  }
}
```

## Technical Considerations

### ARM64 Instruction Encoding (for auto-masking)

```
ADRP Xn, #page       (0x90000000 mask 0x9F000000)  → page = PC_page + imm21<<12
ADD  Xd, Xn, #imm12  (0x91000000 mask 0xFFC00000)  → Xd = Xn + imm12
LDR  Xt, [Xn, #off]  (0xF9400000 mask 0xFFC00000)  → Xt = *(Xn + off*8)
B    label            (0x14000000 mask 0xFC000000)
BL   label            (0x94000000 mask 0xFC000000)
RET                   (0xD65F03C0)
STP  X29, X30, [SP, #-N]!  (function prologue marker)
```

Auto-masking rules for byte patterns:
- **ADRP:** mask bits 5-23 and 29-30 (page target changes with relocation)
- **BL/B:** mask bits 0-25 (branch offset changes)
- **ADD/LDR after ADRP:** mask imm12 field
- **Register-only instructions:** keep entirely (MOV, CMP)
- **STP/LDP prologues:** keep (stable across versions)

### Fat Binary Handling

ARM64 slice offset: `0xf534000`. File offset formula: `file_offset = 0xf534000 + (virtual_address - 0x100000000)`. The tool must handle both fat and thin binaries via `lipo -detailed_info` or direct Mach-O header parsing.

### Existing Code to Reuse

- `scripts/re/find_adrp_refs.py` — ADRP+ADD decoder (extract and generalize)
- `tools/extract_typeids.py` — TypeId extraction from `nm` output (integrate for Category C)
- `src/osiris/pattern_scan.c` — Pattern scanning logic (reimplement in Python)

## System-Wide Impact

- **No runtime changes.** The tool operates offline on binary files, never at runtime.
- **Source file modifications:** `apply_migration.py` modifies `#define` values in ~12 source files. All changes are pure address literal replacements — no logic changes.
- **Version detection:** Updates `BG3_KNOWN_VERSION` in `version_detect.h` as part of migration.
- **Existing tooling:** `extract_typeids.py` continues to work independently; Metathesis wraps it for Category C.

## Implementation Phases

### Phase 1: Foundation (M0 + M1) — ~5h

**M0: Hook Registry JSON (2h)**
- Walk every `#define` with a hardcoded address across the source tree
- Record: address, symbol (if known), resolution strategy, source files, string xrefs
- Include ~164 manually-verified TypeId entries from `component_typeid.c`
- Deliverable: `tools/bindiff/data/hook_registry.json` with ~60 curated entries
- Verify: `python3 -c "import json; print(len(json.load(open('...'))))"`

**M1: Mach-O Parser + ARM64 Decoder (3h)**
- `macho_parse.py`: fat binary slice extraction, LC_SEGMENT_64 parsing, VA-to-file-offset, `__cstring` extraction
- `arm64_decode.py`: decode ADRP, ADD, LDR, STR, B, BL, RET, STP/LDP prologues
- Verify: extract segments from real BG3 binary, decode instruction streams

M0 and M1 are independent — can be built in parallel.

### Phase 2: Resolution Engines (M2 + M3 + M4) — ~6h

**M2: Symbol Diff (1h)**
- `nm -gU | c++filt` on both binaries → `{symbol: address}` dicts
- Match `hook_registry.json` entries with `resolution: "symbol"`
- Also diff full TypeId symbol set
- Verify: run against same binary twice (all deltas = 0)

**M3: String Cross-Reference Engine (3h)**
- Find string VA in `__cstring` → scan `__text` for ADRP+ADD referencing that VA
- Walk backward to function prologue (STP X29, X30 or previous RET)
- Verify: find `BinkManager::LoadVideo` in old binary, confirm matches `0x10390b6cc`

**M4: Byte Pattern Matcher (2h)**
- Extract N-byte signature from old address, auto-mask relocatable fields
- Ghidra-style pattern: `FD 7B BF A9 FD 03 00 91 ?? ?? ?? ?? ...`
- Verify: extract pattern from known address, search old binary, confirm unique match

M2, M3, M4 each depend on M1 but are independent of each other — can be built in parallel.

### Phase 3: Integration (M5 + M6) — ~3h

**M5: Cluster Resolution (1h)**
- Define clusters in `hook_registry.json` (e.g., 9 ExecuteFunctors variants)
- After anchor resolves, apply relative offsets to neighbors
- Verify each resolved address has valid function prologue

**M6: Main CLI + Migration Output (2h)**
- `bindiff.py` orchestrates the 4-tier cascade
- Outputs migration JSON + console summary
- `apply_migration.py` reads migration JSON, patches `#define`s in source files
- Verify: round-trip test (old→old, all deltas 0, all resolved)

### Phase 4: Harness + Verification (M7 + M8) — ~2h

**M7: Harness Integration (1h)**
- Add `bindiff` subcommand to `cli.py`
- Auto-detect binaries: old = `<BG3_EXEC>.bg3se-original`, new = `<BG3_EXEC>`

**M8: Struct Offset Verification (1h)**
- Runtime verification via sentinel probes after migration + rebuild
- Heuristic: if all Category A singletons in same namespace shifted by same delta, struct offsets are stable
- `!verify_offsets` console command for manual confirmation

## Critical Path

```
M0 (registry) ─┬─→ M2 (symbols) ──────────┐
M1 (parser)  ──┼─→ M3 (string xref) ──────┤
               └─→ M4 (byte patterns) ─────┤
                                            ├─→ M5 (clusters) → M6 (CLI) → M7 (harness)
                                            │                            └→ M8 (verify)
                                            └─────────────────────────────────────────
```

## Acceptance Criteria

- [ ] `hook_registry.json` contains all ~60 hardcoded addresses with metadata
- [ ] `bindiff.py --auto` resolves ≥95% of addresses (≥57/60) for the 4.1.1.7209685 update
- [ ] Category A (singletons): 100% resolved via symbol lookup
- [ ] Category C (TypeIds): ≥99% resolved via `nm` symbol extraction
- [ ] Category B (functions): ≥80% resolved via string xref + byte pattern + cluster
- [ ] `apply_migration.py --dry-run` shows correct `#define` replacements
- [ ] `apply_migration.py --apply` patches source files, project builds clean
- [ ] Round-trip test (old→old) produces zero-delta migration with 100% resolution
- [ ] Harness integration: `bg3se_harness bindiff` works with auto-detected paths
- [ ] Unresolved addresses clearly flagged with candidates and confidence scores

## Risk Analysis

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Symbols stripped from new binary | Low | High (Cat A) | ADRP xref from known strings |
| String content changed | Low | Medium (Cat B) | Multiple anchor strings per target, byte pattern fallback |
| Function body rewritten | Medium | Medium (byte patterns) | Auto-mask relocatable bytes, extend pattern length |
| Major struct layout change | Low | High (Cat D) | Runtime verification, flag in migration output |
| Fat binary format change | Very low | High | Handle both fat and thin binaries |

## Design Decisions

1. **Python, not C.** Offline tool against binary files on disk. Python has excellent struct-packing support and the existing RE scripts are Python.

2. **JSON registry as single source of truth.** Source file `#define`s are derived from the registry via `apply_migration.py`. Inverts the current pattern where addresses are scattered across 12 files.

3. **Layered resolution with confidence scores.** Symbol (1.0) → string xref (0.9) → byte pattern (0.8) → cluster delta (0.7) → global delta estimate (0.5). Migration output communicates what was resolved automatically vs. what needs review.

4. **`.bg3se-original` as old binary.** The harness already creates this backup during patching. No additional backup infrastructure.

5. **Separate generation from application.** `bindiff.py` produces migration JSON. `apply_migration.py` consumes it. Review before applying, preserve migration record.

6. **No Ghidra dependency.** Works entirely with `nm`, `lipo`, and raw binary reads. Ghidra available for the ~5% that can't be resolved automatically.

## Success Metrics

- Time to migrate from game update: **<5 minutes** (vs. hours of manual Ghidra RE)
- Automated resolution rate: **≥95%** of ~60 hook targets
- False positive rate: **0%** (confidence thresholds prevent bad auto-application)
- Build-after-migration success rate: **100%** (address replacements are type-safe)

## Dependencies & Prerequisites

- Old binary backup (`.bg3se-original`) from harness patching
- New binary from Steam update
- Python 3.12+ (matches project requirements)
- `nm`, `lipo` (ships with Xcode CLI tools)
- No pip dependencies (stdlib only: `struct`, `json`, `subprocess`, `re`)

## Sources & References

- **Origin document:** [docs/plans/2026-05-13-002-feat-binary-diff-address-migration-tool-plan.md](2026-05-13-002-feat-binary-diff-address-migration-tool-plan.md) — Codex planner output, 568 lines, 8 milestones
- **Nomos blueprint:** Daimonic address migration subsystem design (4-phase Extract → Match → Verify → Emit pipeline, converged with Codex plan)
- **Existing RE scripts:** `scripts/re/find_adrp_refs.py`, `tools/extract_typeids.py`
- **ARM64 ABI notes:** `agent_docs/architecture.md`, `ghidra/offsets/`
- **Fat binary offset:** `0xf534000` (ARM64 slice), documented in memory
