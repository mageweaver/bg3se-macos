# Binary Diff & Address Migration Tool

**Project:** bg3se-macos (`/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/`)
**Date:** 2026-05-13
**Trigger:** BG3 game update 4.1.1.6995620 -> 4.1.1.7209685 broke all hardcoded hook addresses
**Goal:** Automate detection of shifted addresses between game binary versions, producing a migration map that can be applied to the codebase in minutes instead of hours of manual Ghidra RE

---

## Problem Statement

BG3SE-macOS depends on ~60 hardcoded virtual addresses spread across 12 source files, plus ~1,999 TypeId addresses in `generated_typeids.h`. When Larian ships a game update, the binary layout shifts and every address breaks. The current recovery process requires:

1. Full Ghidra import of the new binary (~30 min)
2. Manual re-discovery of each singleton pointer, function address, and TypeId
3. Grepping source files and updating defines one by one

This plan designs a reusable Python tool (`tools/bindiff.py`) that compares the old and new binaries offline, resolves ~95% of address shifts automatically, and outputs a migration manifest.

---

## Address Inventory

### Category A: Singleton Pointer Globals (DATA/BSS segment, ~20 addresses)

These are `m_ptr` globals in `__DATA`/`__DATA_CONST`. Most have surviving mangled symbols in `nm` output. Relocation is trivial via symbol table lookup.

| Address | Symbol | File |
|---------|--------|------|
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

**Resolution strategy:** `nm -gU <binary> | c++filt` -> match mangled symbol -> extract new address. For symbols not in `nm` output, fall back to ADRP+LDR cross-reference from known string references.

### Category B: Function Hook Targets (TEXT segment, ~25 addresses)

These are code addresses where Dobby hooks are installed. Symbols are mostly stripped. Resolution requires byte-pattern matching or string xref tracing.

| Address | Function | File | Resolution Strategy |
|---------|----------|------|---------------------|
| `0x10390b6cc` | `BinkManager::LoadVideo` | video_skip.c | String xref: "Splash_Logo_Larian" or ".bk2" pattern |
| `0x10110f0d0` | `EocServer::Startup` | entity_system.c | String xref: "EocServer" init log messages |
| `0x10124f92c` | `IsInCombat` | entity_system.c | Byte pattern (function prologue + EntityWorld access) |
| `0x101250074` | `GetCombatFromGuid` | entity_system.c | Near IsInCombat, same pattern cluster |
| `0x1010dc924` | `TryGetUuidMappingSingleton` | entity_system.c | Byte pattern (TryGetSingleton template) |
| `0x10636b27c` | `StorageContainer::TryGet` | entity_storage.h, component_templates.h | Byte pattern |
| `0x1063d5998` | `GetMessage` (network) | protocol.h | String xref: message type strings |
| `0x105783a38` | `ExecuteStatsFunctor` | functor_types.h | String xref: "ExecuteFunctors" log strings |
| `0x105787918` | `ExecuteFunctors_AttackTarget` | functor_types.h | Cluster near ExecuteStatsFunctor |
| `0x105787c6c` | `ExecuteFunctors_AttackPosition` | functor_types.h | Cluster |
| `0x10578975c` | `ExecuteFunctors_Move` | functor_types.h | Cluster |
| `0x10578a918` | `ExecuteFunctors_Target` | functor_types.h | Cluster |
| `0x10578e4d8` | `ExecuteFunctors_NearbyAttacked` | functor_types.h | Cluster |
| `0x10578fba8` | `ExecuteFunctors_NearbyAttacking` | functor_types.h | Cluster |
| `0x105790a28` | `ExecuteFunctors_Equip` | functor_types.h | Cluster |
| `0x105792a90` | `ExecuteFunctors_Source` | functor_types.h | Cluster |
| `0x1057965e4` | `ExecuteFunctors_Interrupt` | functor_types.h | Cluster |
| `0x10538f374` | `ProcessDealDamageFunctors` | functor_types.h | String xref: damage-related strings |
| `0x101b752b4` | `FeatManager::GetFeats` | staticdata_manager.c | String xref or vtable pattern |
| `0x10120b3e8` | `GetAllFeats` | staticdata_manager.c | Near FeatManager cluster |
| `0x102994834` | `Get<BackgroundManager>` | staticdata_manager.c | TryGetSingleton pattern |
| `0x10341c42c` | `Get<OriginManager>` | staticdata_manager.c | TryGetSingleton pattern |
| `0x10262f184` | `Get<ClassDescriptions>` | staticdata_manager.c | TryGetSingleton pattern |
| `0x10369700c` | `Get<ProgressionManager>` | staticdata_manager.c | TryGetSingleton pattern |
| `0x1011a4494` | `Get<ActionResourceTypes>` | staticdata_manager.c | TryGetSingleton pattern |
| `0x101f72754` | `SpellPrototype::Init` | prototype_managers.c | String xref: spell init strings |
| `0x105f96304` | `GetTemplateRaw` | template_manager.c | String xref or vtable pattern |
| `0x105d31ce4` | `CacheTemplate` | template_manager.c | Near template cluster |
| `0x1060cc608` | `GetResource` (func) | resource_manager.c | ADRP to ResourceManager ptr |
| `0x10651fb60` | `STDString::ctor` | audio_manager.c | Byte pattern (common ABI) |

### Category C: TypeId Addresses (~1,999 addresses)

All extracted via `nm -gU | c++filt | grep TypeId.*ComponentTypeIdContext`. Fully automated by the existing `tools/extract_typeids.py`. This script already handles re-extraction from any binary.

### Category D: Struct Field Offsets (~15 in-struct offsets)

These are offsets *within* runtime objects (e.g., `OFFSET_ENTITYWORLD_IN_EOCSERVER = 0x288`). They do NOT change between game versions unless Larian adds/removes fields. Typically stable across minor updates but must be spot-checked.

| Offset | Meaning | File |
|--------|---------|------|
| `0x288` | EntityWorld in EocServer | entity_system.c |
| `0x1D0` | EntityWorld in EocClient | entity_system.c |
| `0xA8` | GameServer in EocServer | protocol.h |
| `0x1F8` | MessageFactory in GameServer | protocol.h |
| `0x2D0-0x2E0` | ProtocolList in GameServer | protocol.h |
| `0x650/0x65c` | ActivePeers in GameServer | protocol.h |
| `0x6ac` | SkipSplashScreen in GlobalSwitches | global_switches.c |

**Resolution strategy:** Not address-dependent. Verify with runtime probing after hooks are installed. Flag as "needs manual verification" if Category A/B addresses shift by more than a threshold (e.g., >0x10000 bytes), which might indicate struct layout changes.

---

## Architecture

```
tools/bindiff.py                    # Main CLI tool
    |
    +-- lib/symbol_diff.py          # nm-based symbol resolution (Category A + C)
    +-- lib/pattern_match.py        # ARM64 byte pattern matching (Category B)
    +-- lib/string_xref.py          # String cross-reference via ADRP+ADD (Category B)
    +-- lib/arm64_decode.py         # Minimal ARM64 instruction decoder (ADRP, ADD, LDR, B, BL)
    +-- lib/macho_parse.py          # Fat binary slice extraction, segment enumeration
    |
    +-- data/
    |   +-- hook_registry.json      # Canonical registry of all known hook targets
    |   +-- patterns.json           # Byte patterns for each hook target
    |   +-- migration_YYYYMMDD.json # Output: old->new address mapping
    |
    +-- apply_migration.py          # Apply migration_*.json to source files
```

### `hook_registry.json` — Central Address Database

Single source of truth for every hardcoded address in the project. Each entry specifies:

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
  "pattern_offset": 0,
  "verified_version": "4.1.1.6995620"
}
```

For function targets:
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
  "pattern_offset": 0,
  "verified_version": "4.1.1.6995620"
}
```

### Resolution Pipeline (per hook target)

```
1. SYMBOL LOOKUP (fast, high confidence)
   nm -gU <new_binary> | c++filt -> match entry.symbol
   If found -> resolved, confidence=1.0

2. STRING XREF (medium, high confidence)
   Find string VA in new binary's __cstring section
   Trace ADRP+ADD references to that string in __text
   Walk backward from xref to find function prologue
   If unique match -> resolved, confidence=0.9

3. BYTE PATTERN MATCH (slow, medium confidence)
   Extract N-byte signature from old binary at old_address
   Mask relocatable fields (ADRP immediates, BL targets)
   Scan new binary's __text section for matches
   If exactly 1 match -> resolved, confidence=0.8
   If multiple matches -> flag for manual review

4. DELTA ESTIMATE (fallback, low confidence)
   If nearby addresses in same cluster resolved, compute average delta
   Apply same delta to unresolved address
   Flag as confidence=0.5, needs verification
```

---

## Milestones

### M0: Address Inventory & Hook Registry (2h)

**Files to create:**
- `tools/bindiff/data/hook_registry.json` — Populate from the inventory above

**Approach:**
- Walk through every `#define` with a hardcoded address in the source tree
- Record: address, symbol (if known), resolution strategy, source file locations, string xrefs
- Include the ~164 manually-verified TypeId entries from `component_typeid.c`
- Do NOT include the 1,999 generated TypeIds (handled by existing `extract_typeids.py`)

**Deliverable:** Complete JSON registry of ~60 hand-curated hook targets with metadata

**Verification:** `python3 -c "import json; d=json.load(open('tools/bindiff/data/hook_registry.json')); print(f'{len(d)} entries')"` shows ~60

---

### M1: Mach-O Parser & ARM64 Decoder (3h)

**Files to create:**
- `tools/bindiff/lib/__init__.py`
- `tools/bindiff/lib/macho_parse.py` — Fat binary handling, segment/section enumeration, string table access
- `tools/bindiff/lib/arm64_decode.py` — Decode ADRP, ADD, LDR, STR, B, BL, RET, STP/LDP prologues

**Approach:**
- `macho_parse.py`: Use `lipo -detailed_info` or struct-level parsing to find ARM64 slice offset in the fat binary. Parse LC_SEGMENT_64 commands to enumerate `__TEXT/__text`, `__TEXT/__cstring`, `__DATA_CONST`, `__DATA`. Build a VA-to-file-offset translator. Extract the `__cstring` section for string search.
- `arm64_decode.py`: Minimal decoder for the 6 instruction types we care about. Existing code in `scripts/re/find_adrp_refs.py` already decodes ADRP+ADD+LDR — extract and generalize.

**Key ARM64 patterns:**
```
ADRP Xn, #page        (0x90000000 mask 0x9F000000)  -> page = PC_page + imm21<<12
ADD  Xd, Xn, #imm12   (0x91000000 mask 0xFFC00000)  -> Xd = Xn + imm12
LDR  Xt, [Xn, #off]   (0xF9400000 mask 0xFFC00000)  -> Xt = *(Xn + off*8)
B    label             (0x14000000 mask 0xFC000000)
BL   label             (0x94000000 mask 0xFC000000)
RET                    (0xD65F03C0)
STP  X29, X30, [SP, #-N]!  (function prologue marker)
```

**Deliverable:** Both modules with unit tests. `macho_parse` can extract segments from the real BG3 binary. `arm64_decode` can decode instruction streams.

**Verification:**
```bash
python3 -c "
from tools.bindiff.lib.macho_parse import MachOBinary
m = MachOBinary('<BG3_PATH>')
print(f'TEXT: 0x{m.text_segment.vmaddr:x}, size=0x{m.text_segment.vmsize:x}')
print(f'Strings: {len(m.cstrings)} bytes')
"
```

---

### M2: Symbol Diff (Category A + C resolution) (1h)

**Files to create:**
- `tools/bindiff/lib/symbol_diff.py`

**Approach:**
- Run `nm -gU <binary> | c++filt` on both old and new binaries
- Parse output into `{symbol: address}` dicts
- For each entry in `hook_registry.json` with `resolution: "symbol"`:
  - Look up `entry.symbol` in both dicts
  - Compute delta = new_addr - old_addr
  - Record mapping
- Also diff the full TypeId symbol set (existing `extract_typeids.py` logic, integrated)
- Report: symbols that disappeared, symbols that appeared, address deltas

**Key insight:** Most singleton `m_ptr` globals survive in the symbol table because they are template instantiation artifacts. `nm` extracts them reliably. This resolves Category A (all ~20 singletons) and Category C (all ~1,999 TypeIds) with zero ambiguity.

**Deliverable:** `symbol_diff.resolve(old_binary, new_binary, registry) -> {id: {old, new, delta, confidence}}`

**Verification:** Run against old binary twice (delta should be 0 for all).

---

### M3: String Cross-Reference Engine (Category B partial resolution) (3h)

**Files to create:**
- `tools/bindiff/lib/string_xref.py`

**Approach:**
1. **Find string VA:** Search `__TEXT/__cstring` for the target string bytes. Record its virtual address in the new binary.
2. **Find ADRP+ADD referencing that VA:** Scan `__TEXT/__text` for ADRP instructions whose page matches the string's page, followed by ADD with the page offset. Each hit is a code location that references the string.
3. **Walk backward to function prologue:** From each xref, scan backward (up to 256 bytes) looking for `STP X29, X30, [SP, #-N]!` (standard ARM64 function prologue) or the end of the previous function (`RET`/`B`). The address after the prologue marker is the function entry point.
4. **Validate:** If the hook registry entry specifies the function's approximate size or other strings it references, cross-check.

**String xref catalog** (to populate in `hook_registry.json`):

| Function | Anchor Strings | Notes |
|----------|---------------|-------|
| `BinkManager::LoadVideo` | `".bk2"`, `"Video"` | Likely string arg or log |
| `ExecuteStatsFunctor` | `"ExecuteFunctors"` | Log string nearby |
| `GetMessage` (network) | `"GetMessage"`, protocol name strings | VMT or log |
| `SpellPrototype::Init` | `"SpellPrototype"`, `"SpellData"` | Init log |
| `FeatManager::GetFeats` | `"FeatManager"`, `"GetFeats"` | Log or assert |

**For functor cluster:** The 9 ExecuteFunctors_* functions are typically sequential in the binary (within ~0x3000 bytes of each other). Resolve ExecuteStatsFunctor via string xref, then scan forward for functions with matching prologue patterns. The cluster-relative offsets between them are likely stable.

**Deliverable:** `string_xref.find_function_by_string(binary, string, max_results=5) -> [(func_addr, confidence)]`

**Verification:** Find `BinkManager::LoadVideo` in the old binary and confirm it matches `0x10390b6cc`.

---

### M4: Byte Pattern Matcher (Category B fallback) (2h)

**Files to create:**
- `tools/bindiff/lib/pattern_match.py`

**Approach:**
1. **Extract signature from old binary:** Read N bytes (default 32) at the old address. Identify which bytes are position-dependent (ADRP immediates, BL targets) and mask them as wildcards.
2. **Generate pattern:** Produce a Ghidra-style pattern string like `FD 7B BF A9 FD 03 00 91 ?? ?? ?? ?? ?? ?? ?? 90 ...`
3. **Scan new binary:** Use the existing `find_pattern()` logic from `src/osiris/pattern_scan.c`, reimplemented in Python.
4. **Handle multiple matches:** If >1 match, try extending the pattern length. If still ambiguous, report all candidates.

**Auto-masking rules for ARM64:**
- ADRP: mask bits 5-23 and 29-30 (imm field) — the page target changes with relocation
- BL/B: mask bits 0-25 (branch offset changes)
- ADD/LDR with ADRP: mask the imm12 field (page offset may change)
- STP/LDP prologue bytes: keep (stable across versions)
- Register-only instructions (MOV, CMP, etc.): keep entirely

**Pre-computed patterns** (stored in `data/patterns.json`):
Extract patterns from the old binary for all Category B targets. Store alongside the registry so future runs don't need the old binary.

**Deliverable:** `pattern_match.find_by_pattern(binary, pattern, mask) -> [(addr, match_quality)]`

**Verification:** Extract pattern from old binary at known address, search old binary, confirm exactly 1 match at that address.

---

### M5: Cluster Resolution & Delta Propagation (1h)

**Approach:**
Some functions appear in tight clusters (e.g., the 9 ExecuteFunctors variants). When one is resolved, the relative offsets to its neighbors can be assumed stable.

1. Define clusters in `hook_registry.json`:
   ```json
   "cluster": "execute_functors",
   "cluster_anchor": "execute_stats_functor",
   "cluster_delta": 16096
   ```
2. After M2+M3+M4 resolve the anchor, apply cluster deltas to resolve neighbors.
3. Verify each resolved address by checking that the byte at the resolved address looks like a function prologue.

**Deliverable:** Cluster resolution logic integrated into the main pipeline.

---

### M6: Main CLI Tool & Migration Output (2h)

**Files to create:**
- `tools/bindiff/bindiff.py` — Main entry point
- `tools/bindiff/apply_migration.py` — Source file patcher

**`bindiff.py` usage:**
```bash
# Compare two binaries, produce migration manifest
python3 tools/bindiff/bindiff.py \
  --old "/path/to/Baldur's Gate 3.bg3se-original" \
  --new "/path/to/Baldur's Gate 3" \
  --registry tools/bindiff/data/hook_registry.json \
  --output tools/bindiff/data/migration_20260513.json

# Or auto-detect from Steam paths
python3 tools/bindiff/bindiff.py --auto
```

**Output format (`migration_YYYYMMDD.json`):**
```json
{
  "old_version": "4.1.1.6995620",
  "new_version": "4.1.1.7209685",
  "timestamp": "2026-05-13T14:30:00Z",
  "summary": {
    "total": 60,
    "resolved": 57,
    "unresolved": 3,
    "confidence_high": 45,
    "confidence_medium": 10,
    "confidence_low": 2
  },
  "global_delta": {
    "text_delta": 4096,
    "data_delta": 8192,
    "note": "Average shift across all resolved addresses"
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
    },
    {
      "id": "bink_load_video",
      "old_address": "0x10390b6cc",
      "new_address": "0x10391c6cc",
      "delta": 69632,
      "confidence": 0.9,
      "method": "string_xref",
      "files": ["src/game/video_skip.c"]
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
    "resolved": 1995,
    "missing": 4,
    "new": 12,
    "output_file": "src/entity/generated_typeids.h"
  }
}
```

**Console output:**
```
BG3 Binary Diff Tool
====================
Old: 4.1.1.6995620  New: 4.1.1.7209685

Category A: Singleton Pointers (20/20 resolved)
  [OK]  esv::EocServer::m_ptr        0x10898e8b8 -> 0x10899a8b8  (+0xC000) [symbol]
  [OK]  ecl::EocClient::m_ptr        0x10898c968 -> 0x10899d968  (+0xC000) [symbol]
  ...

Category B: Function Targets (23/25 resolved)
  [OK]  BinkManager::LoadVideo       0x10390b6cc -> 0x10391c6cc  (+0x11000) [string_xref]
  [OK]  ExecuteStatsFunctor          0x105783a38 -> 0x10579aa38  (+0x17000) [string_xref]
  [!!]  TryGetUuidMappingSingleton   0x1010dc924 -> ???           [3 candidates]
  ...

Category C: TypeIds (1995/1999 resolved via nm)
  4 TypeIds removed, 12 new TypeIds added

Written: tools/bindiff/data/migration_20260513.json
```

**`apply_migration.py` usage:**
```bash
# Preview changes (dry run)
python3 tools/bindiff/apply_migration.py \
  --migration tools/bindiff/data/migration_20260513.json \
  --dry-run

# Apply to source files
python3 tools/bindiff/apply_migration.py \
  --migration tools/bindiff/data/migration_20260513.json \
  --apply

# Also regenerate typeids header
python3 tools/extract_typeids.py > src/entity/generated_typeids.h
```

**`apply_migration.py` logic:**
- For each entry, read the specified source file
- Find the `#define` or literal hex address
- Replace old address with new address
- Also update `BG3_KNOWN_VERSION` in `version_detect.h`
- Write a summary of changes

**Deliverable:** Working CLI that produces migration JSON and can apply it to source files.

**Verification:** Round-trip test: extract migration from old->old (all deltas should be 0, all resolved).

---

### M7: Harness Integration (1h)

**Files to modify:**
- `tools/bg3se_harness/cli.py` — Add `bindiff` subcommand

**Usage:**
```bash
# Run from harness
PYTHONPATH=tools python3 -m bg3se_harness bindiff
PYTHONPATH=tools python3 -m bg3se_harness bindiff --apply
PYTHONPATH=tools python3 -m bg3se_harness bindiff --status  # Show current version vs known-good
```

**Auto-detection:**
- Old binary: `<BG3_EXEC>.bg3se-original` (the pre-patch backup)
- New binary: `<BG3_EXEC>` (current Steam binary, after `unpatch` if needed)
- If no backup exists, prompt user to provide old binary path

**Deliverable:** `bg3se_harness bindiff` command that auto-locates binaries and runs the pipeline.

---

### M8: Struct Offset Verification (1h)

**Files to create:**
- `tools/bindiff/lib/struct_verify.py`

**Approach:**
Category D offsets (struct field offsets like `OFFSET_ENTITYWORLD_IN_EOCSERVER = 0x288`) can't be verified statically. Instead:

1. After applying the migration and rebuilding, run the game
2. Use the existing sentinel probe mechanism (`version_detect_addresses_safe()`) to validate
3. Add a `!verify_offsets` console command that exercises key struct accesses and reports pass/fail
4. The bindiff tool flags struct offsets as "stable (assumed)" or "needs verification" based on the magnitude of nearby address shifts

**Heuristic:** If all Category A singletons in the same namespace shifted by the same delta (common for DATA segment growth), struct field offsets are almost certainly unchanged. If deltas vary wildly, struct layouts may have changed.

**Deliverable:** Verification guidance in migration output + runtime probe command.

---

## Milestone Summary

| # | Milestone | Est. | Files | Deps |
|---|-----------|------|-------|------|
| M0 | Hook registry JSON | 2h | `data/hook_registry.json` | -- |
| M1 | Mach-O parser + ARM64 decoder | 3h | `lib/macho_parse.py`, `lib/arm64_decode.py` | -- |
| M2 | Symbol diff (Category A+C) | 1h | `lib/symbol_diff.py` | M0, M1 |
| M3 | String xref engine (Category B) | 3h | `lib/string_xref.py` | M1 |
| M4 | Byte pattern matcher (Category B) | 2h | `lib/pattern_match.py` | M1 |
| M5 | Cluster resolution | 1h | integrated into M6 | M3, M4 |
| M6 | Main CLI + migration output | 2h | `bindiff.py`, `apply_migration.py` | M2-M5 |
| M7 | Harness integration | 1h | `cli.py` modification | M6 |
| M8 | Struct offset verification | 1h | `lib/struct_verify.py`, console cmd | M6 |
| **Total** | | **16h** | | |

## Critical Path

```
M0 (registry) ─┬─> M2 (symbols) ──────────┐
M1 (parser)  ──┼─> M3 (string xref) ──────┤
               └─> M4 (byte patterns) ─────┤
                                            ├─> M5 (clusters) ─> M6 (CLI) ─> M7 (harness)
                                            │                              └─> M8 (verify)
                                            └──────────────────────────────────────────────
```

M0 and M1 are independent and can start in parallel. M2/M3/M4 each depend on M1 for the binary parser but are independent of each other. M6 merges all resolution strategies.

## Risk Analysis

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| Symbols stripped from new binary | Low (TypeId symbols survived 6+ updates) | High for Category A | Fall back to ADRP xref from known strings |
| String content changed | Low (log strings are stable) | Medium for Category B | Multiple anchor strings per target, byte pattern fallback |
| Function body rewritten | Medium (compiler optimizations vary) | Medium for byte patterns | Auto-mask relocatable bytes, extend pattern length |
| Major struct layout change | Low (rare between minor versions) | High for Category D | Runtime verification, flag in migration output |
| Fat binary format change | Very low | High | Tool handles both fat and thin binaries |

## Design Decisions

1. **Python, not C.** The tool runs offline against binary files on disk, not at runtime. Python is faster to develop, has excellent struct-packing support, and the existing RE scripts are already Python.

2. **JSON registry, not scattered `#define`s.** The registry is the single source of truth. Source file `#define`s are derived from it via `apply_migration.py`. This inverts the current pattern where addresses are scattered across 12 files with no central index.

3. **Layered resolution with confidence scores.** Symbol lookup (confidence 1.0) > string xref (0.9) > byte pattern (0.8) > cluster delta (0.7) > global delta estimate (0.5). The migration output clearly communicates what was resolved automatically vs. what needs human review.

4. **Preserve old binary as `.bg3se-original`.** The harness already creates this backup during patching. The bindiff tool leverages it — no additional backup infrastructure needed.

5. **Separate generation from application.** `bindiff.py` produces a migration JSON. `apply_migration.py` consumes it. This separation allows review before applying, and preserves the migration record for future reference.

6. **No Ghidra dependency.** The tool works entirely with `nm`, `lipo`, and raw binary reads. Ghidra MCP is available but not required — the tool should work without it for the 95% case.
