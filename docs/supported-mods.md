# Supported Mods

Tracks mods tested with BG3SE-macOS. Many SE mods work out of the box—this list documents confirmed results.

## Automated Vetting

Use the harness CLI to vet mods against the current build:

```bash
# Vet a mod from the catalog, by Nexus ID, or by local PAK path
PYTHONPATH=tools python3 -m bg3se_harness compat vet mcm
PYTHONPATH=tools python3 -m bg3se_harness compat vet 8901
PYTHONPATH=tools python3 -m bg3se_harness compat vet /path/to/mod.pak

# Run all scenarios
PYTHONPATH=tools python3 -m bg3se_harness compat matrix

# List available scenarios and catalog entries
PYTHONPATH=tools python3 -m bg3se_harness compat list
```

Reports are saved to `docs/compat-reports/` as JSON. The full catalog lives at `tools/bg3se_harness/catalog/popular_mods.json`.

## Status Legend

| Status | Meaning |
|--------|---------|
| Working | Fully functional, all features tested |
| Partial | Core features work, some limitations |
| Not Working | Known incompatibility (see notes) |
| Untested | Not yet verified |

## Vetting Priority

Mods are tiered by community impact. Tier 1 mods gate hundreds of downstream mods and are vetted first.

### Tier 1 — Critical (P0)

| Mod | Nexus | APIs Used | Status | Notes |
|-----|-------|-----------|--------|-------|
| [Mod Configuration Menu](https://www.nexusmods.com/baldursgate3/mods/8901) | 8901 | IMGUI, Net, Vars, ModEvents, Events, Mod | Untested | Gate mod. 500+ mods depend on it. 18K lines Lua. |
| [Community Library](https://www.nexusmods.com/baldursgate3/mods/1333) | 1333 | Stats, Entity, Osiris, Events, Utils, ModEvents | Untested | Shared dependency for hundreds of mods. |
| [5e Spells](https://www.nexusmods.com/baldursgate3/mods/366) | 366 | Stats, Osiris, Events, StaticData | Untested | Most endorsed SE spell mod. Tests Stats + functors. |
| [Expansion - Level 20](https://www.nexusmods.com/baldursgate3/mods/3755) | 3755 | Stats, Entity, Osiris, StaticData | Untested | Levels 13-20. Tests Stats, progression, StaticData. |

### Tier 1 — Confirmed

| Mod | Nexus | APIs Used | BG3SE Version | Status | Notes |
|-----|-------|-----------|---------------|--------|-------|
| [More Reactive Companions](https://www.nexusmods.com/baldursgate3/mods/5447) | 5447 | Osiris, Events | v0.36.17 | Working | Party banter, companion reactions. |

### Tier 2 — Important (P1-P2)

| Mod | Nexus | APIs Used | Status | Notes |
|-----|-------|-----------|--------|-------|
| [Combat Extender](https://www.nexusmods.com/baldursgate3/mods/5207) | 5207 | Stats, Entity, Osiris, Events, Vars | Untested | Requires MCM. Stress-tests Stats. |
| [Party Limit Begone](https://www.nexusmods.com/baldursgate3/mods/327) | 327 | Entity, Osiris, Events | Untested | Party to 16, MP to 8. Entity manipulation. |
| [Camp Event Notifications](https://www.nexusmods.com/baldursgate3/mods/1879) | 1879 | Events, Osiris, IMGUI | Untested | Requires MCM. Tests IMGUI notifications. |
| [Auto Send Food To Camp](https://www.nexusmods.com/baldursgate3/mods/6086) | 6086 | Osiris, Events, Vars | Untested | Requires MCM. Minimal API surface. |
| [Always Show Approvals](https://www.nexusmods.com/baldursgate3/mods/4675) | 4675 | Events, Entity, Osiris | Untested | Tests UI hooks and Events system. |
| [AI Allies](https://www.nexusmods.com/baldursgate3/mods/7780) | 7780 | Entity, Osiris, Events, Stats | Untested | Tests entity creation, Osiris integration. |
| [Configurable Enemies](https://www.nexusmods.com/baldursgate3/mods/5765) | 5765 | Stats, Entity, Osiris, Events | Untested | Requires MCM. Stats mutation stress test. |
| [Smart Autosaving](https://www.nexusmods.com/baldursgate3/mods/7358) | 7358 | Timer, Events, Vars | Untested | Tests Timer persistence across save/load. |

### Tier 3 — Edge Cases (P2-P3)

| Mod | Nexus | APIs Used | Status | Notes |
|-----|-------|-----------|--------|-------|
| [Spell Points 5e](https://www.nexusmods.com/baldursgate3/mods/11959) | 11959 | Stats, StaticData, Events | Untested | SE plugin. StaticData + Stats interaction. |
| [Randomised Equipment Loot](https://www.nexusmods.com/baldursgate3/mods/9262) | 9262 | Stats, Entity, Events | Untested | Tests Stats, loot table access. |
| [ImprovedUI](https://www.nexusmods.com/baldursgate3/mods/4688) | 4688 | (none) | Untested | Pure UI XML mod. Negative control (no SE APIs). |
| [BG3 Mod Fixer](https://www.nexusmods.com/baldursgate3/mods/4284) | 4284 | (none) | Untested | Story recompile. Unnecessary on Patch 7+. Negative test. |

## Known Incompatibilities

| Mod | Issue | Reason | Workaround |
|-----|-------|--------|------------|
| *None documented yet* | — | — | — |

## Vet Report Format

Each `compat vet` run produces a JSON report in `docs/compat-reports/`:

```json
{
  "mod_name": "Mod Configuration Menu",
  "nexus_id": 8901,
  "bg3se_version": "v0.36.50",
  "timestamp": "2026-04-29T...",
  "status": "working|partial|broken|not_loaded|needs_launch|no_socket",
  "se_required": true,
  "load_success": true,
  "bootstrap_executed": true,
  "apis_used": ["Ext.IMGUI", "Ext.Events", "Ext.Net"],
  "errors": [],
  "warnings": []
}
```

**Failure categories:**
- **Missing API** — Mod calls an `Ext.*` function that isn't implemented. Log shows `attempt to call a nil value`.
- **Lua error** — Runtime error with stack trace. Captured from `latest.log`.
- **Crash** — Process dies. Crash ring buffer (`~/Library/Application Support/BG3SE/crash_ring_*.bin`) and `crash.log` contain signal, fault address, and breadcrumbs.
- **Silent failure** — Mod loads without errors but features don't work. Requires manual in-game verification.

## Reporting

### If a Mod Works

Open an issue with the `mod-compatibility` label, or submit a PR adding the mod to this list. Include: mod name, Nexus link, version tested, BG3SE-macOS version, features tested.

### If a Mod Doesn't Work

1. Check logs: `~/Library/Application Support/BG3SE/logs/latest.log`
2. Run: `PYTHONPATH=tools python3 -m bg3se_harness compat vet <mod>`
3. Open an issue with the JSON report and any additional context.

## API Coverage

BG3SE-macOS implements ~94% of the Windows BG3SE API. Mods using these namespaces should work:

| Namespace | Status | Coverage |
|-----------|--------|----------|
| Ext.Osiris | Full | 40+ functions, RegisterListener, NewCall/NewQuery/NewEvent |
| Ext.Entity | Full | GUID lookup, 1,999 components, CreateComponent/RemoveComponent |
| Ext.Stats | Full | 52 functions, 100% Windows parity |
| Ext.Events | Full | 33 events with priority ordering |
| Ext.Timer | Full | 20 functions incl. persistent timers |
| Ext.Vars | Full | PersistentVars, User/Mod variables |
| Ext.StaticData | Full | All 9 data types |
| Ext.Resource | Full | 34 resource types |
| Ext.Template | Full | 14 functions, 10 properties |
| Ext.Types | Full | 9 functions incl. GenerateIdeHelpers |
| Ext.IMGUI | Full | 40 widget types |
| Ext.Net | Full | RakNet backend, Request/Reply callbacks |
| Ext.ModEvents | Full | Cross-mod event system (MCM compat) |
| Ext.RegisterNetListener | Full | Per-channel network message listener |
| Ext.Level | Full | 15 functions incl. 6 Sweep + RaycastAll |
| Ext.Audio | Full | 13 functions + PlayExternalSound |
| Ext.Localization | Full | GetLanguage, CreateHandle |
| Ext.Math | Full | Random, Fract |
| Ext.Mod | Full | 5 functions |
| Ext.Debug | Full | Memory introspection, mod diagnostics |
| Ext.Utils | Full | Print, Version, MonotonicTime, GetGameState |

See [ROADMAP.md](../ROADMAP.md) for implementation details and the full parity matrix.
