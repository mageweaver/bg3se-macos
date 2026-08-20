# Implementation Plan: Expand Component Coverage (Issue #33)

## Current State

| Metric | Count |
|--------|-------|
| Total component layouts | 158 |
| Components with properties | 43 |
| Tag components (zero-field) | 115 |
| TypeIds in binary | ~1,999 |
| Current coverage | ~8% |

**All high-priority components from Issue #33 are already defined:**
- ✅ ActionResources, SpellBook, StatusContainer, Inventory, Experience, Resistances, Passive

## Problem Statement

The acceptance criteria in Issue #33 require **runtime verification** that components work correctly:
```lua
entity.ActionResources.ActionPoints  -- Does this return correct value?
entity.SpellBook.Spells              -- Does array access work?
pairs(entity.Health)                 -- Does iteration work?
```

**Key gap:** Components are defined but may have incorrect ARM64 offsets. Need verification pass.

## Implementation Strategy

### Phase 1: Verification Pass (~1 hour)

Test all 43 property-bearing components in-game to confirm offsets are correct.

**Test script:**
```lua
-- Test high-priority components
local player = Ext.Entity.Get(GetHostCharacter())

-- 1. Health (baseline - known working)
_P("Health.Hp = " .. tostring(player.Health.Hp))

-- 2. ActionResources
_P("ActionResources test:")
for k,v in pairs(player.ActionResources) do _P("  " .. k .. " = " .. tostring(v)) end

-- 3. SpellBook
_P("SpellBook test:")
for k,v in pairs(player.SpellBook) do _P("  " .. k .. " = " .. tostring(v)) end

-- 4. Stats
_P("Stats.Abilities[1] = " .. tostring(player.Stats.Abilities[1]))

-- 5. Experience
_P("Experience.CurrentLevelExperience = " .. tostring(player.Experience.CurrentLevelExperience))
```

**Document failures:** Any component returning nil/0 when it shouldn't needs offset correction.

### Phase 2: Fix Offset Mismatches (variable)

For each failing component:

1. **Runtime probe** - Use `Ext.Debug.ProbeStruct()` to find actual values
2. **Ghidra verify** - Decompile accessor functions if needed
3. **Fix offset** - Update `component_offsets.h`
4. **Rebuild & retest**

### Phase 3: Add Missing High-Value Components

Check Windows BG3SE for components we don't have yet:

```bash
# Find components in Windows headers not in our codebase
grep "DEFINE_COMPONENT" ~/bg3se/BG3Extender/GameDefinitions/Components/*.h | \
  grep -v TAG | \
  awk -F'[,(]' '{print $2}' | \
  sort -u > /tmp/windows_components.txt

grep "shortName" src/entity/component_offsets.h | \
  awk -F'"' '{print $2}' | \
  sort -u > /tmp/macos_components.txt

comm -23 /tmp/windows_components.txt /tmp/macos_components.txt
```

**Priority targets for mods:**
- Summon-related (SummonOwner, SummonContainer)
- Spell targeting (SpellCastState, SpellTargets)
- Item-specific (UseSocket, EquippedItems)

### Phase 4: Documentation

Update `docs/api-reference.md` with:
- Full component list
- Property types for each
- Example usage patterns

## Acceptance Criteria Verification

| Criterion | Status | Action |
|-----------|--------|--------|
| ActionResources accessible | Defined | Verify in-game |
| SpellBook accessible | Defined | Verify in-game |
| Inventory/Container accessible | Defined | Verify in-game |
| StatusContainer accessible | Defined | Verify in-game |
| Experience/Leveling accessible | Defined | Verify in-game |
| pairs() iteration works | Implemented | Verify in-game |
| Documentation updated | Pending | Phase 4 |

## Quick Win: Update Stale Documentation

The `acceleration.md` file says "52 components" but we have 158. Update:
- acceleration.md (outdated)
- README.md (check)
- ROADMAP.md (check)

## Files to Modify

1. `src/entity/component_offsets.h` - Fix any broken offsets
2. `agent_docs/acceleration.md` - Update component count
3. `docs/api-reference.md` - Add component documentation
4. `ROADMAP.md` - Update Issue #33 status

## Test Commands

```bash
# Build
cd build && cmake --build .

# In-game verification (after launching game)
echo 'local p = Ext.Entity.Get(GetHostCharacter()); for k,v in pairs(p.Health) do _P(k.." = "..tostring(v)) end' | nc -U /tmp/bg3se.sock
```
