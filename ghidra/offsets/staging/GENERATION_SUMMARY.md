# Component Layout Generation Summary

Generated on: 2025-12-24

## Overview

Successfully generated **176 component layouts** for the eoc:: namespace by cross-referencing:
- Windows BG3SE C++ headers
- Ghidra ARM64 component size measurements
- Automated field offset calculation

## Statistics

- **Total layouts generated**: 176
- **Boost components**: 85
- **Core components**: 91
- **Total lines of code**: 2,587
- **Components skipped**: 507 (missing headers or too complex)

## Component Categories

### Boost Components (85)
Includes damage, ability, resistance, and other gameplay modifier components:
- AC/Armor boosts (ACOverrideFormulaBoostComponent, ArmorClassBoostComponent)
- Ability score boosts (AbilityBoostComponent, AbilityOverrideMinimumBoostComponent)
- Action resource modifiers (ActionResourceBlockBoostComponent, ActionResourceValueBoostComponent)
- Damage type boosts (DamageBonusBoostComponent, DamageReductionBoostComponent)
- Movement/vision boosts (DarkvisionRangeBoostComponent, MovementSpeedLimitBoostComponent)
- Status effect boosts (StatusImmunityBoostComponent, DownedStatusBoostComponent)

### Core Components (91)
Base gameplay components:
- Character stats (ArmorComponent, HealthComponent, StatsComponent, BaseStatsComponent)
- Inventory (inventory::MemberComponent, inventory::OwnerComponent, inventory::WeightComponent)
- Items (item::DyeComponent, item::PortalComponent, item::MapMarkerStyleComponent)
- Spells (spell::AddedSpellsComponent, spell::BookComponent, spell::ContainerComponent)
- Status effects (status::CauseComponent, status::ContainerComponent, status::LifetimeComponent)
- Combat (TurnBasedComponent, TurnOrderComponent, combat::ParticipantComponent)
- Character creation (character_creation::LevelUpComponent, character_creation::StateComponent)

## Sample Generated Components

### Simple Component (BaseHpComponent - 8 bytes)
```c
static const ComponentPropertyDef g_eoc_BaseHpComponent_Properties[] = {
    { "Vitality", 0x00, FIELD_TYPE_INT32, 0, false },
    { "VitalityBoost", 0x04, FIELD_TYPE_INT32, 0, false },
};
```

### Boost Component (AbilityBoostComponent - 16 bytes)
```c
static const ComponentPropertyDef g_eoc_AbilityBoostComponent_Properties[] = {
    { "Ability", 0x00, FIELD_TYPE_INT32, 0, false },
    { "Value", 0x04, FIELD_TYPE_INT32, 0, false },
};
```

### GUID Component (BackgroundComponent - 16 bytes)
```c
static const ComponentPropertyDef g_eoc_BackgroundComponent_Properties[] = {
    { "Background", 0x00, FIELD_TYPE_GUID, 0, false },
};
```

## Field Type Mappings

The generator maps C++ types to BG3SE field types:
- int32_t, int → FIELD_TYPE_INT32 (4 bytes)
- uint32_t → FIELD_TYPE_UINT32 (4 bytes)
- uint8_t → FIELD_TYPE_UINT8 (1 byte)
- uint16_t → FIELD_TYPE_UINT32 (2 bytes)
- int8_t → FIELD_TYPE_INT32 (1 byte)
- bool → FIELD_TYPE_BOOL (1 byte)
- float → FIELD_TYPE_FLOAT (4 bytes)
- double → FIELD_TYPE_FLOAT (8 bytes)
- FixedString → FIELD_TYPE_FIXEDSTRING (4 bytes)
- Guid → FIELD_TYPE_GUID (16 bytes)
- EntityHandle → FIELD_TYPE_ENTITYHANDLE (8 bytes)
- Enums (AbilityId, SkillId, DamageType, etc.) → FIELD_TYPE_INT32 (4 bytes)
- Array<T> → FIELD_TYPE_ARRAY (16 bytes)

## Offset Calculation

Offsets are calculated using:
1. Sequential field placement
2. 4-byte alignment for fields >= 4 bytes
3. Natural packing for smaller types

**Note**: These are ESTIMATED offsets based on Windows x64 layout. ARM64 verification recommended via:
- Runtime probing with Ext.Debug.ProbeStruct()
- Ghidra accessor function analysis
- In-game testing with known component values

## Size Constraints

- Core components: max 128 bytes (simpler structures)
- Boost components: max 256 bytes (can be more complex)
- Larger components skipped as too complex for auto-generation

Components exceeding these limits should be manually analyzed with Ghidra or runtime probing.

## Parsing Strategy

The generator uses two parsing strategies:

1. **Struct-based** (for regular components):
   - Matches `struct ComponentName : public BaseComponent { ... }`
   - Extracts DEFINE_COMPONENT for full namespace name
   - Parses field declarations

2. **DEFN_BOOST macro** (for boost components):
   - Matches `DEFN_BOOST(Name, ShortName, { fields })`
   - Constructs component name as `eoc::NameBoostComponent`
   - Extracts fields from macro body

## Output Format

Each component includes:
```c
// Component name and size comment
// eoc::ComponentName - SIZE bytes (0xHEX)
// Source: StructName from Windows BG3SE

// Property definitions array
static const ComponentPropertyDef g_eoc_ComponentName_Properties[] = {
    { "FieldName", 0xOFFSET, FIELD_TYPE, 0, is_array },
    ...
};

// Layout definition
static const ComponentLayoutDef g_eoc_ComponentName_Layout = {
    .componentName = "eoc::ComponentName",
    .shortName = "ComponentName",
    .componentTypeIndex = 0,
    .componentSize = 0xSIZE,
    .properties = g_eoc_ComponentName_Properties,
    .propertyCount = sizeof(...) / sizeof(...),
};
```

## Verification Workflow

To verify a generated layout in-game:

```lua
-- Get an entity with the component
local entity = Ext.Entity.Get("PLAYER_GUID")

-- Check if component exists and has expected fields
local comp = entity.ComponentName
if comp then
    _D(comp)  -- Dump component data

    -- Verify specific fields match expected types
    _P("Field1:", comp.Field1)
    _P("Field2:", comp.Field2)
end

-- Runtime offset probing (advanced)
local typeId = 0x... -- Component TypeId from component_typeid.c
local rawPtr = Ext.Entity.GetComponentRawPtr(entity, typeId)
if rawPtr then
    local results = Ext.Debug.ProbeStruct(rawPtr, 0, 64, 4)
    _D(results)  -- Compare with generated offsets
end
```

## Next Steps

1. **Verification**: Test 10-20 high-priority layouts against live game data
2. **Integration**: Merge verified layouts into `src/entity/component_offsets.h`
3. **Expansion**: Generate esv::, ecl::, and ls:: namespace components using same workflow
5. **Testing**: Create automated test suite using in-game Lua console

## Known Limitations

1. **Complex types skipped**: variant, std::function, complex nested structures
2. **Padding not explicit**: May need manual adjustment for ARM64 struct padding rules
3. **Bitfields unsupported**: Bitfield members are skipped
4. **Virtual tables**: Components with virtual methods not handled
5. **Union types**: Cannot represent union members accurately

## Files Generated

- **Generator script**: `ghidra/offsets/staging/generate_layouts.py`
- **Output**: `ghidra/offsets/staging/generated_eoc_layouts.c` (2,587 lines, 176 components)
- **Summary**: `ghidra/offsets/staging/GENERATION_SUMMARY.md` (this file)

## Source Data

**Windows BG3SE Headers**:
- `/Users/tomdimino/Desktop/Programming/bg3se/BG3Extender/GameDefinitions/Components/Stats.h`
- `/Users/tomdimino/Desktop/Programming/bg3se/BG3Extender/GameDefinitions/Components/Boosts.h`
- `/Users/tomdimino/Desktop/Programming/bg3se/BG3Extender/GameDefinitions/Components/Item.h`
- `/Users/tomdimino/Desktop/Programming/bg3se/BG3Extender/GameDefinitions/Components/Combat.h`
- `/Users/tomdimino/Desktop/Programming/bg3se/BG3Extender/GameDefinitions/Components/Data.h`
- And 4 more header files

**Ghidra ARM64 Sizes**:
- `/Users/tomdimino/Desktop/Programming/bg3se-macos/ghidra/offsets/components/COMPONENT_SIZES_EOC_CORE.md`
- `/Users/tomdimino/Desktop/Programming/bg3se-macos/ghidra/offsets/components/COMPONENT_SIZES_EOC_BOOST.md`
- `/Users/tomdimino/Desktop/Programming/bg3se-macos/ghidra/offsets/components/COMPONENT_SIZES_EOC_ITEM.md`
- And 13 more size documentation files

## Component Name Examples

**Top 20 generated components**:
1. eoc::ACOverrideFormulaBoostComponent (24 bytes)
2. eoc::AbilityBoostComponent (16 bytes)
3. eoc::AbilityFailedSavingThrowBoostComponent (1 byte)
4. eoc::AbilityOverrideMinimumBoostComponent (12 bytes)
5. eoc::ActionResourceBlockBoostComponent (24 bytes)
6. eoc::ActionResourceConsumeMultiplierBoostComponent (32 bytes)
7. eoc::ActionResourceMultiplierBoostComponent (32 bytes)
8. eoc::ActionResourcePreventReductionBoostComponent (24 bytes)
9. eoc::ActionResourceReplenishTypeOverrideBoostComponent (24 bytes)
10. eoc::ActionResourceValueBoostComponent (40 bytes)
11. eoc::ActiveCharacterLightBoostComponent (4 bytes)
12. eoc::AddTagBoostComponent (16 bytes)
13. eoc::AdvantageBoostComponent (24 bytes)
14. eoc::AiArchetypeOverrideBoostComponent (8 bytes)
15. eoc::ArmorAbilityModifierCapOverrideBoostComponent (8 bytes)
16. eoc::ArmorClassBoostComponent (4 bytes)
17. eoc::ArmorComponent (16 bytes)
18. eoc::AttackSpellOverrideBoostComponent (8 bytes)
19. eoc::AttributeBoostComponent (4 bytes)
20. eoc::AttributeFlagsComponent (4 bytes)

## Size Distribution

- **1-4 bytes**: 42 components (tags, flags, simple enums)
- **5-16 bytes**: 68 components (basic data structures)
- **17-32 bytes**: 38 components (GUID-based, small arrays)
- **33-64 bytes**: 19 components (medium complexity)
- **65-128 bytes**: 8 components (high complexity)
- **129-256 bytes**: 1 component (DamageReductionBoostComponent)

## Success Metrics

- **Generation rate**: 176 generated / 683 total with sizes = 25.8%
- **Boost coverage**: 85 / 98 boost components with sizes = 86.7%
- **Header matching**: 176 / 143 parsed from Windows headers = 123% (DEFN_BOOST expanded more)
- **Code generation**: 100% success (no syntax errors in output)
