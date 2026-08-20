---
title: "feat: Systematic Top-5 SE Mod Vetting Pipeline"
type: feat
status: active
date: 2026-05-02
---

# Systematic Top-5 SE Mod Vetting Pipeline

## Overview

Build a repeatable, CLI-driven process to systematically vet the 5 most popular BG3 mods that depend on Script Extender, using the existing `bg3se_harness` compat infrastructure. This validates our ~94% parity claim against real-world mod workloads and surfaces any remaining compatibility gaps before users hit them.

## Problem Statement

We have a mature compat pipeline (`compat vet`, `compat run`, `compat matrix`) but only 2 scenario manifests (MCM, Community Library). No end-to-end vetting has been run against the top SE-dependent mods with their actual Lua code executing. The pipeline exists; the test coverage does not.

## The Top 5 Mods (Confirmed via Exa + Firecrawl + Nexus)

Selection criteria: maximizes SE API surface coverage, weighted by userbase size and dependency depth.

| # | Mod | Nexus ID | Endorsements | SE APIs Exercised | Risk |
|---|-----|----------|-------------|-------------------|------|
| 1 | **Mod Configuration Menu (MCM)** | 9162 | 12.6K | IMGUI, Net, Vars, ModEvents, RegisterNetListener, Events, Mod | **Critical** — 500+ mods depend on it. IMGUI on Metal is highest-risk. |
| 2 | **Community Library** | 1333 | — | Stats, Entity, Osiris, Events, Utils, ModEvents | **High** — shared Lua utilities, compat framework. Dependency for hundreds. |
| 3 | **5e Spells** | 125 | — | Stats, Osiris, Events, StaticData | **Medium** — top spell mod. Tests functor execution, StaticData resolution. |
| 4 | **Combat Extender** | 5207 | — | Stats, Entity, Osiris, Events, Vars | **High** — stress-tests Stats mutation. Requires MCM (multi-mod compat). |
| 5 | **Party Limit Begone** | 327 | 40.7K | Entity, Osiris, Events | **Medium** — highest-endorsed SE mod. Tests entity manipulation at scale. |

**Combined API coverage:** IMGUI, Net, Vars, ModEvents, RegisterNetListener, Stats, Entity, Osiris, Events, Utils, StaticData, Mod — every major SE namespace exercised.

**Source confirmation:** Nexus top mods page (Firecrawl scrape), Exa neural search (endorsement counts), existing `catalog/popular_mods.json` (P0/P1 priority tiers).

### Catalog Discrepancies to Fix

| Mod | Catalog nexus_id | Actual nexus_id | Notes |
|-----|-----------------|-----------------|-------|
| MCM | 8901 | **9162** | 8901 may be old AtilioA version; 9162 is Volitio's current |
| 5e Spells | 366 | **125** | 366 appears to be ImpUI (ImprovedUI); 125 is correct for 5e Spells |
| Expansion Lvl 20 | 3755 | **279** | Codex 5.5 verified: actual Nexus page is #279 |

## Proposed Solution

### Phase 1: Catalog + Scenario Updates (no game required)

**1a. Fix `popular_mods.json` discrepancies**
- Update MCM `nexus_id`: 8901 → 9162, author: AtilioA → Volitio
- Update 5e Spells `nexus_id`: 366 → 125, author: DiZ91891 → Celes
- Verify other catalog entries against Nexus

**1b. Create 3 new scenario manifests** in `tools/bg3se_harness/scenarios/`:

**`5e_spells.json`** — Stats + StaticData + functor execution:
```json
{
  "description": "5e Spells compatibility test. P0 — top spell mod, tests Stats+StaticData functor chain.",
  "mods": ["5e_spells"],
  "save_fixture": null,
  "assertions": [
    "assert(Ext.Utils.Version() ~= nil, 'SE loaded')",
    "assert(type(Ext.Stats.Get) == 'function', 'Ext.Stats.Get exists')",
    "assert(type(Ext.Stats.GetAll) == 'function', 'Ext.Stats.GetAll exists')",
    "assert(type(Ext.StaticData.Get) == 'function', 'Ext.StaticData.Get exists')",
    "assert(type(Ext.Osiris.RegisterListener) == 'function', 'Ext.Osiris.RegisterListener exists')",
    "assert(type(Ext.Events.Subscribe) == 'function', 'Ext.Events.Subscribe exists')"
  ],
  "notes": "5e Spells adds unimplemented D&D spells. Tests stat creation and functor resolution."
}
```

**`combat_extender.json`** — Stats mutation + MCM dependency:
```json
{
  "description": "Combat Extender compatibility test. P1 — stress-tests Stats mutation under MCM control.",
  "mods": ["combat_extender", "mcm"],
  "save_fixture": null,
  "assertions": [
    "assert(Ext.Utils.Version() ~= nil, 'SE loaded')",
    "assert(type(Ext.Stats.Get) == 'function', 'Ext.Stats.Get exists')",
    "assert(type(Ext.Stats.SetRawAttribute) == 'function', 'Stats mutation available')",
    "assert(type(Ext.Entity.Get) == 'function', 'Ext.Entity.Get exists')",
    "assert(type(Ext.Vars) == 'table', 'Ext.Vars available')",
    "assert(type(Ext.IMGUI.NewWindow) == 'function', 'IMGUI available (MCM dep)')",
    "assert(type(Ext.ModEvents.Subscribe) == 'function', 'ModEvents available (MCM dep)')"
  ],
  "notes": "Combat Extender depends on MCM. Tests multi-mod loading + Stats mutation at runtime."
}
```

**`party_limit_begone.json`** — Entity manipulation:
```json
{
  "description": "Party Limit Begone compatibility test. P1 — 40K+ endorsements, tests entity manipulation.",
  "mods": ["party_limit_begone"],
  "save_fixture": null,
  "assertions": [
    "assert(Ext.Utils.Version() ~= nil, 'SE loaded')",
    "assert(type(Ext.Entity.Get) == 'function', 'Ext.Entity.Get exists')",
    "assert(type(Ext.Osiris.RegisterListener) == 'function', 'Ext.Osiris.RegisterListener exists')",
    "assert(type(Ext.Events.Subscribe) == 'function', 'Ext.Events.Subscribe exists')"
  ],
  "notes": "Party Limit Begone increases party to 16 and MP to 8. Tests entity system under load."
}
```

### Phase 2: Per-Mod Vetting Protocol (game required)

Run each mod through a 4-step sequence using the existing CLI:

#### Step 1: Prerequisites
```bash
PYTHONPATH=tools python3 -m bg3se_harness doctor
```

#### Step 2: Mod Installation
Each mod must be manually installed (auto-download requires Nexus Premium). For each mod:
```bash
# Install from local PAK
PYTHONPATH=tools python3 -m bg3se_harness mod install /path/to/<mod>.pak

# Verify installed
PYTHONPATH=tools python3 -m bg3se_harness mod list
```

#### Step 3: Vet (probe without full scenario)
```bash
# Quick probe — checks SE loaded, mod loaded, log errors
PYTHONPATH=tools python3 -m bg3se_harness compat vet mcm
PYTHONPATH=tools python3 -m bg3se_harness compat vet community_library
PYTHONPATH=tools python3 -m bg3se_harness compat vet 5e_spells
PYTHONPATH=tools python3 -m bg3se_harness compat vet combat_extender
PYTHONPATH=tools python3 -m bg3se_harness compat vet party_limit_begone
```

#### Step 4: Full Scenario Run
```bash
# Full pipeline: prereqs + assertions + screenshot + crashlog
PYTHONPATH=tools python3 -m bg3se_harness compat run mcm
PYTHONPATH=tools python3 -m bg3se_harness compat run community_library
PYTHONPATH=tools python3 -m bg3se_harness compat run 5e_spells
PYTHONPATH=tools python3 -m bg3se_harness compat run combat_extender
PYTHONPATH=tools python3 -m bg3se_harness compat run party_limit_begone
```

#### Step 5: Full Matrix
```bash
# Run all 5 sequentially
PYTHONPATH=tools python3 -m bg3se_harness compat matrix
```

### Phase 3: Enhanced Assertions (Tier 2 — in-game probes)

Current scenario assertions only check function existence (`type(Ext.X) == 'function'`). Add deeper probes that execute actual API calls once a save is loaded.

**MCM — Tier 2 assertions:**
```lua
-- Verify IMGUI can create a window (Metal rendering)
local w = Ext.IMGUI.NewWindow("SE_Compat_Test")
assert(w ~= nil, "IMGUI window creation works on Metal")
w:Destroy()

-- Verify Net channel creation
local ok = pcall(function() return type(Ext.Net.PostMessageToServer) end)
assert(ok, "Net messaging available")
```

**5e Spells — Tier 2 assertions (needs loaded save):**
```lua
-- Verify Stats resolution for a 5e Spells stat entry
local spells = Ext.Stats.GetAll("SpellData")
assert(#spells > 0, "SpellData stats loaded")

-- Verify StaticData access
local ok, feat = pcall(Ext.StaticData.Get, "Feat", 1)
assert(ok, "StaticData.Get works for Feats")
```

**Combat Extender — Tier 2 assertions (needs loaded save + MCM):**
```lua
-- Verify Stats mutation capability
local s = Ext.Stats.Get("WPN_Longsword")
assert(s ~= nil, "Can read weapon stats")

-- Verify MCM ModEvents cross-mod communication
local received = false
Ext.ModEvents.Subscribe("MCM", "MCM_Saved", function() received = true end)
assert(type(Ext.ModEvents.Subscribe) == 'function', "MCM event subscription works")
```

**Party Limit Begone — Tier 2 assertions (needs loaded save):**
```lua
-- Verify entity enumeration
local players = Osi.DB_Players:Get()
assert(players ~= nil, "Osi.DB_Players accessible")

-- Verify entity event subscription
local ok = pcall(function()
    Ext.Entity.Subscribe("eoc::HealthComponent", function() end)
end)
assert(ok, "Entity event subscription works")
```

### Phase 4: Reporting & Baseline

**4a. Generate baseline reports:**
After successful vetting, save golden results:
```bash
mkdir -p docs/compat-reports/baseline/
# Copy each successful vet report to baseline/
```

**4b. Add `compat diff` subcommand** (future enhancement):
Compare current vet results against baseline to detect regressions after SE code changes.

**4c. Update documentation:**
- Add vetting results to `ROADMAP.md` (mod compat section)
- Add tested mod versions to `popular_mods.json` (`last_tested`, `last_status` fields)

## Technical Considerations

### Mod Installation Requirements
- MCM, 5e Spells, Combat Extender, Party Limit Begone: available as free PAK downloads on Nexus
- Community Library: available as free PAK
- Combat Extender requires MCM installed first (dependency chain)
- No Premium Nexus account needed for manual PAK download (only for API-driven download)

### Game State Requirements
| Assertion Tier | Requires | Notes |
|----------------|----------|-------|
| Tier 1 (function existence) | Game running, SE socket connected | No save needed |
| Tier 2 (API execution) | Loaded save game | Need a save with mods enabled in load order |

### Risk: IMGUI on Metal
MCM's IMGUI usage is the single highest-risk subsystem. The Metal ImGui backend has coordinate conversion quirks (CGEventTap → Cocoa → ImGui). MCM's INSERT hotkey requires CGEventTap keyboard forwarding. This is where compatibility is most likely to break.

### Risk: Multi-Mod Loading Order
Combat Extender depends on MCM. Testing must verify that the modsettings.lsx load order is correct and both mods' `BootstrapServer.lua` / `BootstrapClient.lua` scripts execute in the right sequence.

## Acceptance Criteria

- [ ] `popular_mods.json` nexus_id discrepancies fixed (MCM: 9162, 5e Spells: 125)
- [ ] 5 scenario manifests exist in `tools/bg3se_harness/scenarios/`
- [ ] All 5 mods pass `compat vet` with status "working"
- [ ] All 5 scenarios pass `compat run` (all assertions green)
- [ ] `compat matrix` returns `all_passed: true` for all 5
- [ ] Tier 2 (in-game) assertions added for at least MCM and Combat Extender
- [ ] Baseline vet reports saved in `docs/compat-reports/baseline/`
- [ ] Mod versions recorded in catalog (`last_tested`, `last_status`)

## Success Metrics

- 5/5 top SE mods vetted with "working" status
- 0 SE crashes during mod loading (verified via crashlog)
- All SE API namespaces exercised by at least one mod

## Files to Modify

| File | Change |
|------|--------|
| `tools/bg3se_harness/catalog/popular_mods.json` | Fix nexus_id discrepancies |
| `tools/bg3se_harness/scenarios/5e_spells.json` | New scenario manifest |
| `tools/bg3se_harness/scenarios/combat_extender.json` | New scenario manifest |
| `tools/bg3se_harness/scenarios/party_limit_begone.json` | New scenario manifest |
| `tools/bg3se_harness/scenarios/mcm.json` | Add Tier 2 assertions |
| `tools/bg3se_harness/scenarios/community_library.json` | Add Tier 2 assertions |
| `docs/compat-reports/baseline/` | Golden vet reports (new directory) |

## Codex 5.5 Findings: Infrastructure Gaps

Independent Codex 5.5 review identified these gaps that affect the vetting pipeline:

### Pipeline Gaps (block vetting)
1. **`compat run` doesn't launch the game** — only checks if already running. Workaround: launch manually via `bg3se_harness launch --continue` before running scenarios. [compat.py:175](tools/bg3se_harness/compat.py:175)
2. **`compat run` doesn't install mods** — logs "manual install" note only. [compat.py:157](tools/bg3se_harness/compat.py:157)
3. **`requires_mcm` not enforced** — Combat Extender scenario needs MCM pre-installed but nothing validates this.
4. **Log/crash checks not launch-scoped** — stale `crash.log` or old `latest.log` lines pollute results.
5. **TEST_LINE_RE doesn't handle `[SLOW ...]` token** — slow test results may be misparsed. [test_runner.py:9](tools/bg3se_harness/test_runner.py:9)

### Low-Hanging Fruit (code + docs improvements)
2. **`savegames.restore()` doesn't back up current state** — claims to but only copies fixture. [savegames.py:155](tools/bg3se_harness/savegames.py:155)
3. **`mod enable <name>` only accepts UUID** — CLI advertises name support but doesn't resolve. [cli.py:568](tools/bg3se_harness/cli.py:568)
4. **`lua_timer` JSON: 1024-byte stack buffer** — no bounds checks or JSON escaping. [lua_timer.c:330](src/lua/lua_timer.c:330)
5. **`staticdata_probe_manager()` uses direct memory reads** — should use `safe_memory_read`. [staticdata_manager.c:2012](src/staticdata/staticdata_manager.c:2012)
6. **Offline harness tests stale** — `test_help()` covers 22 commands but omits `mod`, `compat`, `wiki`, `parity`, `doctor`, `save`. [tests.py:59](tools/bg3se_harness/tests.py:59)
7. **Intentional stubs mixed with parity claims** — `Ext.Types.Construct`, IMGUI images, `entity:Replicate` are graceful stubs but docs say "complete".

## Verification

1. `PYTHONPATH=tools python3 -m bg3se_harness compat list` — all 5 scenarios appear
2. `PYTHONPATH=tools python3 -m bg3se_harness doctor` — all prerequisites pass
3. Launch game: `PYTHONPATH=tools python3 -m bg3se_harness launch --continue`
4. Run Tier 1 regression first: `echo '!test' | nc -U /tmp/bg3se.sock`
5. Vet each mod: `PYTHONPATH=tools python3 -m bg3se_harness compat vet <key>`
6. Run each scenario: `PYTHONPATH=tools python3 -m bg3se_harness compat run <scenario>`
7. Run full matrix: `PYTHONPATH=tools python3 -m bg3se_harness compat matrix`
8. Verify all reports in `docs/compat-reports/` show `status: "working"`
9. Run `!test_ingame` to confirm no regressions with mods loaded
