/**
 * BG3SE-macOS - Component Property Access Implementation
 *
 * Provides safe, data-driven property access for ECS components.
 */

#include "component_property.h"
#include "generated_enums.h"

// Generated layout records intentionally rely on zero-initialization for the
// optional array metadata fields and use empty arrays for tag components.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wzero-length-array"
#endif
#include "component_offsets.h"
#include "generated_property_defs.h"  // 504 generated component layouts
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "../core/safe_memory.h"
#include "../core/logging.h"
#include "../core/game_memory.h"
#include "../lifetime/lifetime.h"
#include "../strings/fixed_string.h"
#include "guid_lookup.h"
#include "../core/guid_format.h"
#include "component_registry.h"

#include <limits.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

// Lua headers
#include "../../lib/lua/src/lua.h"
#include "../../lib/lua/src/lauxlib.h"
#include "../../lib/lua/src/lualib.h"

// ============================================================================
// Constants
// ============================================================================

/*
 * Room for a layout per known component, with headroom. The previous value was
 * 1024 with a comment claiming it was "enough for all 1,999 components", which
 * it plainly was not -- 533 layouts ship today, so nothing was being dropped
 * yet, but the next few hundred would have vanished into a DEBUG line.
 */
#define MAX_COMPONENT_LAYOUTS 4096
#define COMPONENT_PROXY_METATABLE "bg3se.ComponentProxy"
#define ARRAY_PROXY_METATABLE "bg3se.ArrayProxy"

// Array<T> memory layout on ARM64
#define ARRAY_BUF_OFFSET    0x00   // T* buf_
#define ARRAY_CAP_OFFSET    0x08   // uint32_t capacity_
#define ARRAY_SIZE_OFFSET   0x0C   // uint32_t size_

// ============================================================================
// Global State
// ============================================================================

static ComponentLayoutDef g_Layouts[MAX_COMPONENT_LAYOUTS];
static int g_LayoutCount = 0;
static bool g_Initialized = false;

static bool component_property_register_layout_internal(
    const ComponentLayoutDef *layout, bool generated);

// ============================================================================
// Initialization
// ============================================================================

bool component_property_init(void) {
    if (g_Initialized) return true;

    g_LayoutCount = 0;

    // Register built-in layouts from component_offsets.h (hand-verified)
    int verified_count = 0;
    for (int i = 0; g_AllComponentLayouts[i] != NULL; i++) {
        if (component_property_register_layout(g_AllComponentLayouts[i])) {
            verified_count++;
        }
    }
    LOG_ENTITY_DEBUG("Registered %d verified component layouts", verified_count);

    // Register generated layouts from Windows BG3SE headers (unverified offsets)
    int generated_count = 0;
    for (int i = 0; i < GENERATED_COMPONENT_COUNT; i++) {
        const ComponentLayoutDef* layout = g_GeneratedComponentLayouts[i];
        if (!layout) continue;

        // Skip if already registered (from g_AllComponentLayouts)
        if (component_property_get_layout(layout->componentName)) continue;

        if (component_property_register_layout_internal(layout, true)) {
            generated_count++;
        }
    }
    LOG_ENTITY_DEBUG("Registered %d generated component layouts (Windows offsets)", generated_count);

    g_Initialized = true;
    LOG_ENTITY_DEBUG("Component property system initialized with %d total layouts", g_LayoutCount);
    return true;
}

// ============================================================================
// Layout Registration & Lookup
// ============================================================================

static bool component_property_register_layout_internal(
    const ComponentLayoutDef *layout, bool generated) {
    if (!layout || !layout->componentName) return false;
    if (g_LayoutCount >= MAX_COMPONENT_LAYOUTS) {
        // WARN, not DEBUG: silently dropping layouts makes components read as
        // absent from Lua with nothing to explain why, which is exactly how the
        // uuid component hid.
        LOG_ENTITY_WARN("Component layout registry full at %d; '%s' and any "
                        "layouts after it are unreachable",
                        MAX_COMPONENT_LAYOUTS, layout->componentName);
        return false;
    }

    // Copy layout
    g_Layouts[g_LayoutCount] = *layout;
    g_Layouts[g_LayoutCount].generated = generated;
    g_LayoutCount++;

    LOG_ENTITY_DEBUG("Registered component layout: %s (%s) with %d properties",
                   layout->componentName, layout->shortName, layout->propertyCount);
    return true;
}

bool component_property_register_layout(const ComponentLayoutDef *layout) {
    return component_property_register_layout_internal(layout, false);
}

const ComponentLayoutDef *component_property_get_layout(const char *componentName) {
    if (!componentName) return NULL;

    for (int i = 0; i < g_LayoutCount; i++) {
        if (strcmp(g_Layouts[i].componentName, componentName) == 0) {
            return &g_Layouts[i];
        }
    }
    return NULL;
}

const ComponentLayoutDef *component_property_get_layout_by_short_name(const char *shortName) {
    if (!shortName) return NULL;

    // Upstream's Lua-facing name wins: layout shortNames were derived
    // mechanically from the class name ("TreeState"), while mods use the
    // DEFINE_COMPONENT name ("TadpoleTreeState"). This also makes ambiguous
    // short names deterministic ("Data" is eoc::DataComponent upstream, but
    // interrupt/sight/spatial_grid all have a DataComponent too).
    const char *cls = component_upstream_name_to_class(shortName);
    if (cls) {
        const ComponentLayoutDef *layout = component_property_get_layout(cls);
        if (layout) return layout;
    }

    for (int i = 0; i < g_LayoutCount; i++) {
        if (g_Layouts[i].shortName &&
            strcmp(g_Layouts[i].shortName, shortName) == 0) {
            return &g_Layouts[i];
        }
    }
    return NULL;
}

const ComponentLayoutDef *component_property_get_layout_by_index(uint16_t typeIndex) {
    if (typeIndex == 0) return NULL;

    for (int i = 0; i < g_LayoutCount; i++) {
        if (g_Layouts[i].componentTypeIndex == typeIndex) {
            return &g_Layouts[i];
        }
    }
    return NULL;
}

void component_property_set_type_index(const char *componentName, uint16_t typeIndex) {
    if (!componentName) return;

    for (int i = 0; i < g_LayoutCount; i++) {
        if (strcmp(g_Layouts[i].componentName, componentName) == 0) {
            g_Layouts[i].componentTypeIndex = typeIndex;
            LOG_ENTITY_DEBUG("Set TypeIndex for %s: %u",
                           componentName, typeIndex);
            return;
        }
    }
}

// ============================================================================
// Property Reading - Helper Functions
// ============================================================================

static const ComponentPropertyDef *find_property(const ComponentLayoutDef *layout,
                                                  const char *name) {
    if (!layout || !name) return NULL;

    for (int i = 0; i < layout->propertyCount; i++) {
        if (strcmp(layout->properties[i].name, name) == 0) {
            return &layout->properties[i];
        }
    }
    return NULL;
}

// ============================================================================
// Property Reading
// ============================================================================

// Enum-labelled integer fields push the upstream label string, matching what
// Windows SE mods observe (their patched Lua compares enum objects against
// label strings; in vanilla Lua the string itself is the only representation
// that keeps `Slot == "Boots"` working). Unknown values fall back to the
// integer so nothing is hidden.
static bool enum_read_underlying(mach_vm_address_t addr, FieldType type,
                                 uint64_t *out) {
    switch (type) {
        case FIELD_TYPE_INT8:
        case FIELD_TYPE_UINT8: {
            uint8_t v = 0;
            if (!safe_memory_read(addr, &v, sizeof(v))) return false;
            *out = v;
            return true;
        }
        case FIELD_TYPE_INT16:
        case FIELD_TYPE_UINT16: {
            uint16_t v = 0;
            if (!safe_memory_read(addr, &v, sizeof(v))) return false;
            *out = v;
            return true;
        }
        case FIELD_TYPE_INT32:
        case FIELD_TYPE_UINT32: {
            uint32_t v = 0;
            if (!safe_memory_read_u32(addr, &v)) return false;
            *out = v;
            return true;
        }
        default:
            return false;
    }
}

static const char *enum_label_for(const ComponentEnumDef *def, uint64_t value) {
    for (int i = 0; i < def->count; i++) {
        if (def->labels[i].value == value) return def->labels[i].label;
    }
    return NULL;
}

static bool enum_value_for(const ComponentEnumDef *def, const char *label,
                           uint64_t *out) {
    for (int i = 0; i < def->count; i++) {
        if (strcmp(def->labels[i].label, label) == 0) {
            *out = def->labels[i].value;
            return true;
        }
    }
    return false;
}

// ============================================================================
// LegacyRefMap<DamageType, Array<RollDefinition>> (Weapon.Rolls)
// ============================================================================
//
// Upstream CoreLib/Base/LegacyMap.h:
//   RefMapInternals { uint32_t ItemCount; uint32_t HashSize; MapNode **HashTable; }
//   MapNode         { MapNode *Next; TKey Key; TValue Value; }
// with TKey = DamageType (uint8) and TValue = Array<RollDefinition>
// (buf @0x00, capacity @0x08, size @0x0C). Bucket = Hash(key) % HashSize,
// Hash(uint8) = value; new keys append at the chain tail; no rehash.
//
// Base/ExposedTypes.h RollDefinition:
//   { DiceSizeId DiceValue; uint8 AmountOfDices; int32 DiceAdditionalValue;
//     bool DiceNegative; }  -> 12 bytes.
//
// Reads produce { [DamageTypeLabel] = { {DiceValue=.., AmountOfDices=..,
// DiceAdditionalValue=.., DiceNegative=..}, ... } }. Writes follow upstream
// Unserialize for maps (clear(), then get_or_insert + Array push_back), so
// the buffers/nodes come from and go back to ls::MemoryManager.

#define ROLLMAP_ITEMCOUNT_OFF 0x00
#define ROLLMAP_HASHSIZE_OFF  0x04
#define ROLLMAP_TABLE_OFF     0x08
#define ROLLMAP_SIZE          0x10

#define ROLLNODE_NEXT_OFF  0x00
#define ROLLNODE_KEY_OFF   0x08
#define ROLLNODE_ARRAY_OFF 0x10
#define ROLLNODE_SIZE      0x20

#define ROLLDEF_DICEVALUE_OFF  0x00
#define ROLLDEF_AMOUNT_OFF     0x01
#define ROLLDEF_ADDITIONAL_OFF 0x04
#define ROLLDEF_NEGATIVE_OFF   0x08
#define ROLLDEF_SIZE           12

// Sanity bounds; a weapon map has a handful of buckets and 1-3 rolls per type.
#define ROLLMAP_MAX_BUCKETS 4096
#define ROLLMAP_MAX_CHAIN   256
#define ROLLMAP_MAX_ROLLS   64
#define ROLLMAP_MAX_ENTRIES 32

typedef struct {
    uint8_t diceValue;
    uint8_t amountOfDices;
    int32_t diceAdditionalValue;
    bool diceNegative;
} RollDefinitionStaged;

typedef struct {
    uint8_t key;
    uint32_t count;
    RollDefinitionStaged rolls[ROLLMAP_MAX_ROLLS];
} RollMapEntryStaged;

static bool roll_map_read_header(mach_vm_address_t addr, uint32_t *itemCount,
                                 uint32_t *hashSize, uint64_t *table) {
    uint8_t header[ROLLMAP_SIZE];
    if (!safe_memory_read(addr, header, sizeof(header))) return false;
    memcpy(itemCount, header + ROLLMAP_ITEMCOUNT_OFF, sizeof(*itemCount));
    memcpy(hashSize, header + ROLLMAP_HASHSIZE_OFF, sizeof(*hashSize));
    memcpy(table, header + ROLLMAP_TABLE_OFF, sizeof(*table));
    return true;
}

static void roll_map_push_definition(lua_State *L, const uint8_t *def) {
    int32_t additional = 0;
    memcpy(&additional, def + ROLLDEF_ADDITIONAL_OFF, sizeof(additional));

    lua_createtable(L, 0, 4);
    const char *dice = enum_label_for(&g_enum_DiceSizeId, def[ROLLDEF_DICEVALUE_OFF]);
    if (dice) lua_pushstring(L, dice);
    else lua_pushinteger(L, def[ROLLDEF_DICEVALUE_OFF]);
    lua_setfield(L, -2, "DiceValue");
    lua_pushinteger(L, def[ROLLDEF_AMOUNT_OFF]);
    lua_setfield(L, -2, "AmountOfDices");
    lua_pushinteger(L, additional);
    lua_setfield(L, -2, "DiceAdditionalValue");
    lua_pushboolean(L, def[ROLLDEF_NEGATIVE_OFF] != 0);
    lua_setfield(L, -2, "DiceNegative");
}

// Push the map as a plain Lua table, or nil when the memory does not look
// like a RefMap. Never allocates or writes game memory.
static void roll_map_push(lua_State *L, mach_vm_address_t addr) {
    uint32_t itemCount = 0, hashSize = 0;
    uint64_t table = 0;
    if (!roll_map_read_header(addr, &itemCount, &hashSize, &table)) {
        lua_pushnil(L);
        return;
    }
    if (hashSize > ROLLMAP_MAX_BUCKETS || (hashSize != 0 && table == 0)) {
        LOG_ENTITY_DEBUG("roll map at %p: implausible header (count=%u buckets=%u table=0x%llx)",
                         (void *)(uintptr_t)addr, itemCount, hashSize,
                         (unsigned long long)table);
        lua_pushnil(L);
        return;
    }

    lua_createtable(L, 0, (int)itemCount);
    for (uint32_t b = 0; b < hashSize; b++) {
        uint64_t node = 0;
        if (!safe_memory_read(table + b * sizeof(uint64_t), &node, sizeof(node))) break;
        for (uint32_t depth = 0; node != 0 && depth < ROLLMAP_MAX_CHAIN; depth++) {
            uint8_t raw[ROLLNODE_SIZE];
            if (!safe_memory_read(node, raw, sizeof(raw))) { node = 0; break; }
            uint64_t next = 0, buf = 0;
            uint32_t size = 0;
            memcpy(&next, raw + ROLLNODE_NEXT_OFF, sizeof(next));
            memcpy(&buf, raw + ROLLNODE_ARRAY_OFF + ARRAY_BUF_OFFSET, sizeof(buf));
            memcpy(&size, raw + ROLLNODE_ARRAY_OFF + ARRAY_SIZE_OFFSET, sizeof(size));
            uint8_t key = raw[ROLLNODE_KEY_OFF];

            if (size > ROLLMAP_MAX_ROLLS || (size != 0 && buf == 0)) {
                LOG_ENTITY_DEBUG("roll map node %p: implausible array (size=%u buf=0x%llx)",
                                 (void *)(uintptr_t)node, size, (unsigned long long)buf);
                size = 0;
            }

            lua_createtable(L, (int)size, 0);
            for (uint32_t i = 0; i < size; i++) {
                uint8_t def[ROLLDEF_SIZE];
                if (!safe_memory_read(buf + i * ROLLDEF_SIZE, def, sizeof(def))) break;
                roll_map_push_definition(L, def);
                lua_rawseti(L, -2, (lua_Integer)i + 1);
            }

            const char *label = enum_label_for(&g_enum_DamageType, key);
            if (label) {
                lua_setfield(L, -2, label);
            } else {
                lua_rawseti(L, -2, key);
            }
            node = next;
        }
    }
}

static bool roll_map_stage_key(lua_State *L, int keyIndex, uint8_t *out) {
    if (lua_type(L, keyIndex) == LUA_TSTRING) {
        uint64_t v = 0;
        if (!enum_value_for(&g_enum_DamageType, lua_tostring(L, keyIndex), &v)) return false;
        *out = (uint8_t)v;
        return true;
    }
    if (lua_isinteger(L, keyIndex)) {
        lua_Integer v = lua_tointeger(L, keyIndex);
        if (v < 0 || v > UINT8_MAX) return false;
        *out = (uint8_t)v;
        return true;
    }
    return false;
}

static bool roll_map_stage_u8_field(lua_State *L, int tableIndex, const char *field,
                                    const ComponentEnumDef *enumDef, uint8_t *out) {
    lua_getfield(L, tableIndex, field);
    bool ok = false;
    if (enumDef && lua_type(L, -1) == LUA_TSTRING) {
        uint64_t v = 0;
        ok = enum_value_for(enumDef, lua_tostring(L, -1), &v) && v <= UINT8_MAX;
        if (ok) *out = (uint8_t)v;
    } else if (lua_isinteger(L, -1)) {
        lua_Integer v = lua_tointeger(L, -1);
        ok = v >= 0 && v <= UINT8_MAX;
        if (ok) *out = (uint8_t)v;
    } else if (lua_isnil(L, -1)) {
        *out = 0;
        ok = true;
    }
    lua_pop(L, 1);
    return ok;
}

// Convert the Lua table at tableIndex into staged entries. Raises a Lua
// error on malformed input so nothing has touched game memory yet.
static int roll_map_stage(lua_State *L, int tableIndex, RollMapEntryStaged *entries,
                          const char *what) {
    int count = 0;
    lua_pushnil(L);
    while (lua_next(L, tableIndex) != 0) {
        if (count >= ROLLMAP_MAX_ENTRIES) {
            return luaL_error(L, "%s: too many damage types (max %d)", what,
                              ROLLMAP_MAX_ENTRIES);
        }
        RollMapEntryStaged *e = &entries[count];
        memset(e, 0, sizeof(*e));
        if (!roll_map_stage_key(L, -2, &e->key)) {
            return luaL_error(L, "%s: key '%s' is not a DamageType", what,
                              luaL_tolstring(L, -2, NULL));
        }
        if (lua_type(L, -1) != LUA_TTABLE) {
            return luaL_error(L, "%s: value for damage type %d must be a table", what,
                              (int)e->key);
        }
        int arr = lua_absindex(L, -1);
        size_t n = lua_rawlen(L, arr);
        if (n > ROLLMAP_MAX_ROLLS) {
            return luaL_error(L, "%s: too many rolls for damage type %d (max %d)",
                              what, (int)e->key, ROLLMAP_MAX_ROLLS);
        }
        for (size_t i = 0; i < n; i++) {
            lua_rawgeti(L, arr, (lua_Integer)i + 1);
            if (lua_type(L, -1) != LUA_TTABLE) {
                return luaL_error(L, "%s: roll %zu for damage type %d must be a table",
                                  what, i + 1, (int)e->key);
            }
            int roll = lua_absindex(L, -1);
            RollDefinitionStaged *r = &e->rolls[i];
            if (!roll_map_stage_u8_field(L, roll, "DiceValue", &g_enum_DiceSizeId, &r->diceValue)
                || !roll_map_stage_u8_field(L, roll, "AmountOfDices", NULL, &r->amountOfDices)) {
                return luaL_error(L, "%s: roll %zu for damage type %d has an invalid "
                                  "DiceValue/AmountOfDices", what, i + 1, (int)e->key);
            }
            lua_getfield(L, roll, "DiceAdditionalValue");
            if (lua_isnil(L, -1)) {
                r->diceAdditionalValue = 0;
            } else if (lua_isinteger(L, -1)) {
                lua_Integer v = lua_tointeger(L, -1);
                if (v < INT32_MIN || v > INT32_MAX) {
                    return luaL_error(L, "%s: DiceAdditionalValue out of int32 range", what);
                }
                r->diceAdditionalValue = (int32_t)v;
            } else {
                return luaL_error(L, "%s: DiceAdditionalValue must be an integer", what);
            }
            lua_pop(L, 1);
            lua_getfield(L, roll, "DiceNegative");
            r->diceNegative = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            lua_pop(L, 1);  // roll table
        }
        e->count = (uint32_t)n;
        count++;
        lua_pop(L, 1);  // value; keep key for lua_next
    }
    return count;
}

// Upstream LegacyRefMap::clear(): free every node (and its Array buffer),
// null the buckets, ItemCount = 0. HashTable itself is kept.
static bool roll_map_clear(mach_vm_address_t addr, uint32_t hashSize, uint64_t table) {
    for (uint32_t b = 0; b < hashSize; b++) {
        uint64_t node = 0;
        mach_vm_address_t slot = table + b * sizeof(uint64_t);
        if (!safe_memory_read(slot, &node, sizeof(node))) return false;
        for (uint32_t depth = 0; node != 0 && depth < ROLLMAP_MAX_CHAIN; depth++) {
            uint8_t raw[ROLLNODE_SIZE];
            if (!safe_memory_read(node, raw, sizeof(raw))) return false;
            uint64_t next = 0, buf = 0;
            memcpy(&next, raw + ROLLNODE_NEXT_OFF, sizeof(next));
            memcpy(&buf, raw + ROLLNODE_ARRAY_OFF + ARRAY_BUF_OFFSET, sizeof(buf));
            game_memory_free((void *)(uintptr_t)buf);
            game_memory_free((void *)(uintptr_t)node);
            node = next;
        }
        uint64_t zero = 0;
        if (!safe_memory_write(slot, &zero, sizeof(zero))) return false;
    }
    uint32_t zeroCount = 0;
    return safe_memory_write(addr + ROLLMAP_ITEMCOUNT_OFF, &zeroCount, sizeof(zeroCount));
}

// Upstream get_or_insert(): walk the bucket chain, append a new node at the
// tail when the key is absent, ItemCount++. Returns the node address.
static uint64_t roll_map_get_or_insert(mach_vm_address_t addr, uint32_t hashSize,
                                       uint64_t table, uint8_t key) {
    uint32_t bucket = key % hashSize;
    mach_vm_address_t slot = table + bucket * sizeof(uint64_t);
    uint64_t node = 0;
    if (!safe_memory_read(slot, &node, sizeof(node))) return 0;
    for (uint32_t depth = 0; node != 0 && depth < ROLLMAP_MAX_CHAIN; depth++) {
        uint8_t raw[ROLLNODE_SIZE];
        if (!safe_memory_read(node, raw, sizeof(raw))) return 0;
        if (raw[ROLLNODE_KEY_OFF] == key) return node;
        uint64_t next = 0;
        memcpy(&next, raw + ROLLNODE_NEXT_OFF, sizeof(next));
        if (next == 0) {
            slot = node + ROLLNODE_NEXT_OFF;
            break;
        }
        node = next;
    }

    uint8_t *fresh = (uint8_t *)game_memory_alloc(ROLLNODE_SIZE);
    if (!fresh) return 0;
    memset(fresh, 0, ROLLNODE_SIZE);
    fresh[ROLLNODE_KEY_OFF] = key;
    uint64_t freshAddr = (uint64_t)(uintptr_t)fresh;
    if (!safe_memory_write(slot, &freshAddr, sizeof(freshAddr))) {
        game_memory_free(fresh);
        return 0;
    }
    uint32_t itemCount = 0;
    if (safe_memory_read_u32(addr + ROLLMAP_ITEMCOUNT_OFF, &itemCount)) {
        itemCount++;
        safe_memory_write(addr + ROLLMAP_ITEMCOUNT_OFF, &itemCount, sizeof(itemCount));
    }
    return freshAddr;
}

// Replace the node's Array<RollDefinition> contents (clear + push_back).
static bool roll_map_fill_node(uint64_t node, const RollMapEntryStaged *e) {
    mach_vm_address_t array = node + ROLLNODE_ARRAY_OFF;
    uint64_t oldBuf = 0;
    if (!safe_memory_read(array + ARRAY_BUF_OFFSET, &oldBuf, sizeof(oldBuf))) return false;

    uint8_t *buf = NULL;
    if (e->count > 0) {
        buf = (uint8_t *)game_memory_alloc((size_t)e->count * ROLLDEF_SIZE);
        if (!buf) return false;
        memset(buf, 0, (size_t)e->count * ROLLDEF_SIZE);
        for (uint32_t i = 0; i < e->count; i++) {
            uint8_t *def = buf + i * ROLLDEF_SIZE;
            def[ROLLDEF_DICEVALUE_OFF] = e->rolls[i].diceValue;
            def[ROLLDEF_AMOUNT_OFF] = e->rolls[i].amountOfDices;
            memcpy(def + ROLLDEF_ADDITIONAL_OFF, &e->rolls[i].diceAdditionalValue,
                   sizeof(int32_t));
            def[ROLLDEF_NEGATIVE_OFF] = e->rolls[i].diceNegative ? 1 : 0;
        }
    }

    uint8_t header[16] = {0};
    uint64_t bufAddr = (uint64_t)(uintptr_t)buf;
    memcpy(header + ARRAY_BUF_OFFSET, &bufAddr, sizeof(bufAddr));
    memcpy(header + ARRAY_CAP_OFFSET, &e->count, sizeof(uint32_t));
    memcpy(header + ARRAY_SIZE_OFFSET, &e->count, sizeof(uint32_t));
    if (!safe_memory_write(array, header, sizeof(header))) {
        game_memory_free(buf);
        return false;
    }
    game_memory_free((void *)(uintptr_t)oldBuf);
    return true;
}

// Rebuild the map at addr from the Lua table at tableIndex. Mirrors upstream
// Unserialize for LegacyRefMap: clear(), then get_or_insert + Array fill.
static bool roll_map_write(lua_State *L, mach_vm_address_t addr, int tableIndex,
                           const char *what) {
    luaL_checktype(L, tableIndex, LUA_TTABLE);
    if (!game_memory_available()) {
        LOG_ENTITY_DEBUG("%s: game allocator unavailable; refusing roll map write", what);
        return false;
    }

    uint32_t itemCount = 0, hashSize = 0;
    uint64_t table = 0;
    if (!roll_map_read_header(addr, &itemCount, &hashSize, &table)) return false;
    // A map that has never allocated its bucket table would need
    // LegacyRefMap's resize path; every WeaponComponent we have seen has one.
    if (hashSize == 0 || hashSize > ROLLMAP_MAX_BUCKETS || table == 0) {
        LOG_ENTITY_DEBUG("%s: map has no bucket table (count=%u buckets=%u table=0x%llx); "
                         "refusing write", what, itemCount, hashSize,
                         (unsigned long long)table);
        return false;
    }

    RollMapEntryStaged *entries = calloc(ROLLMAP_MAX_ENTRIES, sizeof(*entries));
    if (!entries) return false;
    // roll_map_stage longjmps out on bad input; entries leaks in that case,
    // which is acceptable for a mod-authoring error.
    int count = roll_map_stage(L, lua_absindex(L, tableIndex), entries, what);

    bool ok = roll_map_clear(addr, hashSize, table);
    for (int i = 0; ok && i < count; i++) {
        uint64_t node = roll_map_get_or_insert(addr, hashSize, table, entries[i].key);
        ok = node != 0 && roll_map_fill_node(node, &entries[i]);
    }
    if (ok) {
        LOG_ENTITY_DEBUG("%s: rebuilt roll map with %d damage type(s)", what, count);
    } else {
        LOG_ENTITY_DEBUG("%s: roll map rebuild failed partway", what);
    }
    free(entries);
    return ok;
}

// ============================================================================
// Array<T> of plain values (upstream CoreLib/Base/BaseArray.h)
//
// Unserialize for Array<T> upstream is clear() + push_back() per element,
// with the buffer owned by GameAllocRaw/GameFree — the same
// ls::MemoryManager pair game_memory wraps. We rebuild the buffer in one
// allocation and swap the header {buf_, capacity_, size_}.
// ============================================================================

#define PLAIN_ARRAY_HEADER_SIZE 0x10
#define PLAIN_ARRAY_MAX_ELEMENTS 4096

static bool plain_array_elem_writable(const ComponentPropertyDef *prop) {
    if (!prop || prop->elemSize == 0) return false;
    switch (prop->elemType) {
        case ELEM_TYPE_GUID:          return prop->elemSize == sizeof(Guid);
        case ELEM_TYPE_ENTITY_HANDLE: return prop->elemSize == sizeof(uint64_t);
        case ELEM_TYPE_FIXED_STRING:  return prop->elemSize == sizeof(uint32_t);
        default:                      return false;
    }
}

// Convert the Lua value at valueIndex into one element at dst. Raises a Lua
// error on malformed input; the caller must not hold heap memory across it.
static void plain_array_stage_element(lua_State *L, int valueIndex,
                                      const ComponentPropertyDef *prop,
                                      uint8_t *dst, const char *what,
                                      lua_Integer position) {
    switch (prop->elemType) {
        case ELEM_TYPE_GUID: {
            // Same tolerance as FIELD_TYPE_GUID: canonical form or a
            // "Name_<uuid>" tail; empty string is the null guid.
            size_t slen = 0;
            const char *s = luaL_checklstring(L, valueIndex, &slen);
            Guid value = {0, 0};
            if (slen > 0) {
                const char *tail = slen > 36 ? s + slen - 36 : s;
                if (!guid_parse(tail, &value)) {
                    luaL_error(L, "'%s' is not a valid GUID for %s[%lld]", s, what,
                               (long long)position);
                }
            }
            memcpy(dst, &value, sizeof(value));
            return;
        }
        case ELEM_TYPE_ENTITY_HANDLE: {
            uint64_t value = 0;
            if (lua_type(L, valueIndex) == LUA_TSTRING) {
                const char *s = lua_tostring(L, valueIndex);
                char *end = NULL;
                value = strtoull(s, &end, 0);
                if (end == s || *end != '\0') {
                    luaL_error(L, "'%s' is not a valid entity handle for %s[%lld]", s,
                               what, (long long)position);
                }
            } else {
                value = (uint64_t)luaL_checkinteger(L, valueIndex);
            }
            memcpy(dst, &value, sizeof(value));
            return;
        }
        case ELEM_TYPE_FIXED_STRING: {
            uint32_t fs = FS_NULL_INDEX;
            if (lua_type(L, valueIndex) == LUA_TNUMBER) {
                fs = (uint32_t)lua_tointeger(L, valueIndex);
            } else {
                size_t slen = 0;
                const char *s = luaL_checklstring(L, valueIndex, &slen);
                if (slen > 0) {
                    fs = fixed_string_intern(s, (int)slen);
                    if (fs == FS_NULL_INDEX) {
                        luaL_error(L, "Could not intern FixedString for %s[%lld]", what,
                                   (long long)position);
                    }
                }
            }
            memcpy(dst, &fs, sizeof(fs));
            return;
        }
        default:
            luaL_error(L, "%s: unsupported array element type", what);
    }
}

// Replace the Array<T> at addr with the sequence in the Lua table at
// tableIndex. Elements are staged onto the Lua stack as a userdata scratch
// buffer so a luaL_error mid-stage cannot leak heap memory.
static bool plain_array_write(lua_State *L, mach_vm_address_t addr, int tableIndex,
                              const ComponentPropertyDef *prop, const char *what) {
    int absTable = lua_absindex(L, tableIndex);
    luaL_checktype(L, absTable, LUA_TTABLE);
    if (!game_memory_available()) {
        LOG_ENTITY_DEBUG("%s: game allocator unavailable; refusing array write", what);
        return false;
    }

    size_t count = lua_rawlen(L, absTable);
    if (count > PLAIN_ARRAY_MAX_ELEMENTS) {
        luaL_error(L, "%s: %zu elements exceeds the %d element limit", what, count,
                   PLAIN_ARRAY_MAX_ELEMENTS);
        return false;
    }

    size_t bytes = count * prop->elemSize;
    uint8_t *staged = (uint8_t *)lua_newuserdatauv(L, bytes ? bytes : 1, 0);
    int stagedIdx = lua_gettop(L);
    memset(staged, 0, bytes ? bytes : 1);
    for (size_t i = 0; i < count; i++) {
        lua_rawgeti(L, absTable, (lua_Integer)i + 1);
        plain_array_stage_element(L, -1, prop, staged + i * prop->elemSize, what,
                                  (lua_Integer)i + 1);
        lua_pop(L, 1);
    }

    uint64_t oldBuf = 0;
    if (!safe_memory_read(addr + ARRAY_BUF_OFFSET, &oldBuf, sizeof(oldBuf))) {
        lua_remove(L, stagedIdx);
        return false;
    }

    uint8_t *buf = NULL;
    if (count > 0) {
        buf = (uint8_t *)game_memory_alloc(bytes);
        if (!buf) {
            lua_remove(L, stagedIdx);
            return false;
        }
        memcpy(buf, staged, bytes);
    }
    lua_remove(L, stagedIdx);

    uint32_t count32 = (uint32_t)count;
    uint8_t header[PLAIN_ARRAY_HEADER_SIZE] = {0};
    uint64_t bufAddr = (uint64_t)(uintptr_t)buf;
    memcpy(header + ARRAY_BUF_OFFSET, &bufAddr, sizeof(bufAddr));
    memcpy(header + ARRAY_CAP_OFFSET, &count32, sizeof(count32));
    memcpy(header + ARRAY_SIZE_OFFSET, &count32, sizeof(count32));
    if (!safe_memory_write(addr, header, sizeof(header))) {
        game_memory_free(buf);
        return false;
    }
    game_memory_free((void *)(uintptr_t)oldBuf);
    LOG_ENTITY_DEBUG("%s: rebuilt array with %zu element(s)", what, count);
    return true;
}

int component_property_read_def(lua_State *L, void *componentPtr,
                                const ComponentPropertyDef *prop) {
    if (!L || !componentPtr || !prop) {
        lua_pushnil(L);
        return 1;
    }

    uintptr_t addr = (uintptr_t)componentPtr + prop->offset;

    if (prop->enumDef) {
        uint64_t value = 0;
        if (!enum_read_underlying((mach_vm_address_t)addr, prop->type, &value)) {
            lua_pushnil(L);
            return 1;
        }
        const char *label = enum_label_for(prop->enumDef, value);
        if (label) {
            lua_pushstring(L, label);
        } else {
            lua_pushinteger(L, (lua_Integer)value);
        }
        return 1;
    }

    switch (prop->type) {
        case FIELD_TYPE_INT8: {
            int8_t val = 0;
            if (safe_memory_read((mach_vm_address_t)addr, &val, sizeof(val))) {
                lua_pushinteger(L, val);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        case FIELD_TYPE_UINT8: {
            uint8_t val = 0;
            if (safe_memory_read((mach_vm_address_t)addr, &val, sizeof(val))) {
                lua_pushinteger(L, val);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        case FIELD_TYPE_INT16: {
            int16_t val = 0;
            if (safe_memory_read((mach_vm_address_t)addr, &val, sizeof(val))) {
                lua_pushinteger(L, val);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        case FIELD_TYPE_UINT16: {
            uint16_t val = 0;
            if (safe_memory_read((mach_vm_address_t)addr, &val, sizeof(val))) {
                lua_pushinteger(L, val);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        case FIELD_TYPE_INT32: {
            int32_t val = 0;
            if (safe_memory_read_i32((mach_vm_address_t)addr, &val)) {
                lua_pushinteger(L, val);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        case FIELD_TYPE_UINT32: {
            uint32_t val = 0;
            if (safe_memory_read_u32((mach_vm_address_t)addr, &val)) {
                lua_pushinteger(L, val);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        case FIELD_TYPE_INT64: {
            int64_t val = 0;
            if (safe_memory_read((mach_vm_address_t)addr, &val, sizeof(val))) {
                lua_pushinteger(L, val);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        case FIELD_TYPE_UINT64: {
            uint64_t val = 0;
            if (safe_memory_read((mach_vm_address_t)addr, &val, sizeof(val))) {
                lua_pushinteger(L, (lua_Integer)val);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        case FIELD_TYPE_BOOL: {
            uint8_t val = 0;
            if (safe_memory_read((mach_vm_address_t)addr, &val, sizeof(val))) {
                lua_pushboolean(L, val != 0);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        case FIELD_TYPE_FLOAT: {
            float val = 0.0f;
            if (safe_memory_read((mach_vm_address_t)addr, &val, sizeof(val))) {
                lua_pushnumber(L, val);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        case FIELD_TYPE_DOUBLE: {
            double val = 0.0;
            if (safe_memory_read((mach_vm_address_t)addr, &val, sizeof(val))) {
                lua_pushnumber(L, val);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        case FIELD_TYPE_VEC3: {
            float vals[3] = {0};
            if (safe_memory_read((mach_vm_address_t)addr, vals, sizeof(vals))) {
                lua_createtable(L, 0, 3);
                lua_pushnumber(L, vals[0]); lua_setfield(L, -2, "x");
                lua_pushnumber(L, vals[1]); lua_setfield(L, -2, "y");
                lua_pushnumber(L, vals[2]); lua_setfield(L, -2, "z");
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        case FIELD_TYPE_VEC4: {
            float vals[4] = {0};
            if (safe_memory_read((mach_vm_address_t)addr, vals, sizeof(vals))) {
                lua_createtable(L, 0, 4);
                lua_pushnumber(L, vals[0]); lua_setfield(L, -2, "x");
                lua_pushnumber(L, vals[1]); lua_setfield(L, -2, "y");
                lua_pushnumber(L, vals[2]); lua_setfield(L, -2, "z");
                lua_pushnumber(L, vals[3]); lua_setfield(L, -2, "w");
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        case FIELD_TYPE_INT32_ARRAY: {
            if (prop->arraySize == 0) {
                lua_pushnil(L);
                return 1;
            }
            lua_createtable(L, prop->arraySize, 0);
            for (int i = 0; i < prop->arraySize; i++) {
                int32_t val = 0;
                if (safe_memory_read_i32((mach_vm_address_t)(addr + i * sizeof(int32_t)), &val)) {
                    lua_pushinteger(L, val);
                } else {
                    lua_pushnil(L);
                }
                lua_rawseti(L, -2, i + 1);  // 1-indexed
            }
            return 1;
        }

        case FIELD_TYPE_FLOAT_ARRAY: {
            if (prop->arraySize == 0) {
                lua_pushnil(L);
                return 1;
            }
            lua_createtable(L, prop->arraySize, 0);
            for (int i = 0; i < prop->arraySize; i++) {
                float val = 0.0f;
                if (safe_memory_read((mach_vm_address_t)(addr + i * sizeof(float)), &val, sizeof(val))) {
                    lua_pushnumber(L, val);
                } else {
                    lua_pushnil(L);
                }
                lua_rawseti(L, -2, i + 1);
            }
            return 1;
        }

        case FIELD_TYPE_GUID: {
            // ls::Guid {uint64 lo, hi}; upstream Guid::ToString byte order,
            // so the string matches what mods compare against (and what
            // guid_parse accepts on the write side).
            Guid guid = {0, 0};
            if (safe_memory_read((mach_vm_address_t)addr, &guid, sizeof(guid))) {
                char buf[37];
                guid_to_string(&guid, buf);
                lua_pushstring(L, buf);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        case FIELD_TYPE_ROLL_MAP: {
            roll_map_push(L, (mach_vm_address_t)addr);
            return 1;
        }

        case FIELD_TYPE_ENTITY_HANDLE: {
            uint64_t val = 0;
            if (safe_memory_read((mach_vm_address_t)addr, &val, sizeof(val))) {
                // Return as hex string for debugging
                char buf[32];
                snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)val);
                lua_pushstring(L, buf);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        case FIELD_TYPE_FIXEDSTRING: {
            // FixedString is a uint32_t index into the GlobalStringTable.
            // Resolve to the string itself — upstream returns the string, and
            // mods (and the Serialize/Unserialize round trip) depend on it.
            uint32_t val = 0;
            if (!safe_memory_read_u32((mach_vm_address_t)addr, &val)) {
                lua_pushnil(L);
                return 1;
            }
            if (val == FS_NULL_INDEX) {
                lua_pushstring(L, "");
                return 1;
            }
            const char *resolved = fixed_string_resolve(val);
            if (resolved) {
                lua_pushstring(L, resolved);
            } else {
                lua_pushinteger(L, val);  // unresolvable: surface the index
            }
            return 1;
        }

        case FIELD_TYPE_DYNAMIC_ARRAY: {
            // Dynamic Array<T> - return an array proxy
            component_property_push_array_proxy(L, (void *)addr, prop);
            return 1;
        }

        case FIELD_TYPE_STRUCT_PTR: {
            // T* member: proxy the pointee with its own layout (nil when NULL)
            void *target = NULL;
            if (!prop->structLayout
                || !safe_memory_read((mach_vm_address_t)addr, &target, sizeof(target))
                || !target) {
                lua_pushnil(L);
                return 1;
            }
            component_property_push_proxy(L, target, prop->structLayout);
            return 1;
        }

        default:
            LOG_ENTITY_DEBUG("Unsupported field type: %d", prop->type);
            lua_pushnil(L);
            return 1;
    }
}

// ============================================================================
// Runtime check level (Ext.Debug.SetEntityRuntimeCheckLevel)
// ============================================================================
//
// Windows uses ecs::RuntimeCheckLevel to control how aggressively property maps
// are validated (None=0, Once=1, Always=2, FullECS=3). The macOS port always
// validates on the write path -- that is a safety invariant and lowering the
// level must never weaken it. What the level controls here is the *read* path,
// which by default performs no per-access range check: Always/FullECS turn on
// a per-read bounds check against the layout size. The default (Once) therefore
// reproduces today's behavior exactly, and raising the level only adds checking.

static int g_runtime_check_level = 1;  // RuntimeCheckLevel::Once

void component_property_set_check_level(int level) {
    g_runtime_check_level = level;
    LOG_ENTITY_DEBUG("Entity runtime check level set to %d", level);
}

int component_property_get_check_level(void) {
    return g_runtime_check_level;
}

static size_t component_property_field_size(const ComponentPropertyDef *prop);

int component_property_read(lua_State *L, void *componentPtr,
                            const ComponentLayoutDef *layout,
                            const char *propertyName) {
    const ComponentPropertyDef *prop = find_property(layout, propertyName);
    if (!prop) {
        return 0;  // Property not found
    }

    // Always(2) and FullECS(3) validate every read against the layout bounds.
    if (g_runtime_check_level >= 2 && layout->componentSize != 0) {
        size_t fieldSize = component_property_field_size(prop);
        if (fieldSize != 0
            && ((size_t)prop->offset > layout->componentSize
                || fieldSize > (size_t)layout->componentSize - prop->offset)) {
            LOG_ENTITY_DEBUG(
                "Refusing component read: %s.%s range [0x%x, 0x%zx) exceeds "
                "layout size 0x%x",
                layout->componentName, prop->name, prop->offset,
                (size_t)prop->offset + fieldSize, layout->componentSize);
            return 0;
        }
    }

    return component_property_read_def(L, componentPtr, prop);
}

// ============================================================================
// Property Writing
// ============================================================================

static size_t component_property_field_size(const ComponentPropertyDef *prop) {
    if (!prop) return 0;

    switch (prop->type) {
        case FIELD_TYPE_INT8:
        case FIELD_TYPE_UINT8:
        case FIELD_TYPE_BOOL:
            return sizeof(uint8_t);

        case FIELD_TYPE_INT16:
        case FIELD_TYPE_UINT16:
            return sizeof(uint16_t);

        case FIELD_TYPE_INT32:
        case FIELD_TYPE_UINT32:
        case FIELD_TYPE_FLOAT:
        case FIELD_TYPE_FIXEDSTRING:
            return sizeof(uint32_t);

        case FIELD_TYPE_INT64:
        case FIELD_TYPE_UINT64:
        case FIELD_TYPE_DOUBLE:
        case FIELD_TYPE_ENTITY_HANDLE:
            return sizeof(uint64_t);

        case FIELD_TYPE_GUID:
            return sizeof(Guid);

        case FIELD_TYPE_ROLL_MAP:
            // RefMapInternals header; the nodes hang off HashTable.
            return ROLLMAP_SIZE;

        case FIELD_TYPE_DYNAMIC_ARRAY:
            // Array<T> header {buf_, capacity_, size_}; the buffer is
            // rebuilt through the game allocator (plain_array_write).
            return plain_array_elem_writable(prop) ? PLAIN_ARRAY_HEADER_SIZE : 0;

        case FIELD_TYPE_INT32_ARRAY:
            if (prop->arraySize == 0) return 0;
            return (size_t)prop->arraySize * sizeof(int32_t);

        case FIELD_TYPE_FLOAT_ARRAY:
            if (prop->arraySize == 0) return 0;
            return (size_t)prop->arraySize * sizeof(float);

        default:
            return 0;
    }
}

static bool component_property_is_pointer_typed(const ComponentPropertyDef *prop) {
    /*
     * Dynamic Array<T> embeds a game-owned buffer pointer. Arrays of plain
     * values (Guid / EntityHandle / FixedString) are rebuilt through the game
     * allocator the way upstream Unserialize does (clear + push_back); arrays
     * of structs additionally require ownership/lifetime operations that this
     * layer cannot provide, so those stay refused.
     */
    if (prop && prop->type == FIELD_TYPE_STRUCT_PTR) return true;
    return prop && prop->type == FIELD_TYPE_DYNAMIC_ARRAY
        && !plain_array_elem_writable(prop);
}

static bool component_property_bounds_valid(const ComponentLayoutDef *layout,
                                            const ComponentPropertyDef *prop,
                                            size_t fieldSize) {
    /* Unknown component size means no boundary to validate against — refuse
     * writes for verified layouts too, not just generated ones, or a size-0
     * verified layout would let writes past the component silently corrupt
     * adjacent ECS memory. */
    if (layout->componentSize == 0) {
        LOG_ENTITY_DEBUG(
            "Refusing component write: layout %s has unknown size%s",
            layout->componentName, layout->generated ? " (generated)" : "");
        return false;
    }

    if (layout->componentSize != 0
        && ((size_t)prop->offset > layout->componentSize
            || fieldSize > (size_t)layout->componentSize - prop->offset)) {
        LOG_ENTITY_DEBUG(
            "Refusing component write: %s.%s range [0x%x, 0x%zx) exceeds layout size 0x%x%s",
            layout->componentName, prop->name, prop->offset,
            (size_t)prop->offset + fieldSize, layout->componentSize,
            layout->generated ? " (generated)" : "");
        return false;
    }

    return true;
}

bool component_property_write(lua_State *L, void *componentPtr,
                              const ComponentLayoutDef *layout,
                              const char *propertyName, int valueIndex) {
    if (!L || !componentPtr || !layout || !layout->componentName || !propertyName) {
        LOG_ENTITY_DEBUG(
            "Refusing component write: invalid arguments (L=%p component=%p layout=%p property=%s)",
            (void *)L, componentPtr, (const void *)layout,
            propertyName ? propertyName : "<null>");
        return false;
    }

    const ComponentPropertyDef *prop = find_property(layout, propertyName);
    if (!prop) {
        LOG_ENTITY_DEBUG("Refusing component write: unknown property %s.%s",
                         layout->componentName, propertyName);
        return false;
    }

    if (strstr(layout->componentName, "OneFrame") != NULL
        || strstr(layout->componentName, "Request") != NULL) {
        LOG_ENTITY_DEBUG("Refusing component write: transient component %s is blacklisted",
                         layout->componentName);
        return false;
    }

    // FixedString writes intern via the engine's ls::FixedString::Create
    // (find-or-add + IncRef), so the slot takes correct ownership of the new
    // value. The old value's DecRef entry point is not yet recovered on
    // macOS, so the previous entry leaks one refcount per write — bounded by
    // how often mods assign FS fields, the same accepted tradeoff as the
    // loca overwrite buffers. Upstream unserializes FS fields (assignment
    // IncRefs new / DecRefs old); refusing here silently broke stat cloning
    // (TransmogEnhanced Data.StatsId).
    // The write itself happens in the switch below.

    if (component_property_is_pointer_typed(prop)) {
        LOG_ENTITY_DEBUG("Refusing component write: %s.%s is pointer-typed",
                         layout->componentName, propertyName);
        return false;
    }

    size_t fieldSize = component_property_field_size(prop);
    if (fieldSize == 0) {
        LOG_ENTITY_DEBUG("Refusing component write: unsupported field type %d for %s.%s",
                         prop->type, layout->componentName, propertyName);
        return false;
    }

    if (!component_property_bounds_valid(layout, prop, fieldSize)) {
        return false;
    }

    if (prop->readOnly) {
        LOG_ENTITY_DEBUG("Refusing component write: %s.%s is read-only",
                         layout->componentName, propertyName);
        return false;
    }

    // See property_can_unserialize: unverified generated layouts never write.
    if (layout->generated) {
        LOG_ENTITY_DEBUG("Refusing component write: %s.%s layout is "
                         "generated/unverified", layout->componentName,
                         propertyName);
        return false;
    }

    uintptr_t componentAddress = (uintptr_t)componentPtr;
    if (componentAddress > UINTPTR_MAX - prop->offset) {
        LOG_ENTITY_DEBUG("Refusing component write: address overflow for %s.%s",
                         layout->componentName, propertyName);
        return false;
    }

    mach_vm_address_t address =
        (mach_vm_address_t)(componentAddress + prop->offset);
    bool wrote = false;

    // Enum-labelled fields accept the upstream label string (or an integer,
    // which falls through to the normal typed write below).
    if (prop->enumDef && lua_type(L, valueIndex) == LUA_TSTRING) {
        const char *label = lua_tostring(L, valueIndex);
        uint64_t value = 0;
        if (!enum_value_for(prop->enumDef, label, &value)) {
            luaL_error(L, "'%s' is not a valid %s enum label for %s.%s",
                       label, prop->enumDef->name, layout->componentName,
                       propertyName);
            return false;
        }
        int absIdx = lua_absindex(L, valueIndex);
        lua_pushinteger(L, (lua_Integer)value);
        lua_replace(L, absIdx);
    }

    switch (prop->type) {
        case FIELD_TYPE_FIXEDSTRING: {
            uint32_t fs = FS_NULL_INDEX;
            if (lua_type(L, valueIndex) == LUA_TNUMBER) {
                // Raw index passthrough (e.g. round-tripping an unresolvable
                // value that serialize surfaced as an integer).
                fs = (uint32_t)lua_tointeger(L, valueIndex);
            } else {
                size_t slen = 0;
                const char *s = luaL_checklstring(L, valueIndex, &slen);
                if (slen > 0) {
                    fs = fixed_string_intern(s, (int)slen);
                    if (fs == FS_NULL_INDEX) {
                        luaL_error(L, "Could not intern FixedString for %s.%s",
                                   layout->componentName, propertyName);
                        return false;
                    }
                }
            }
            wrote = safe_memory_write(address, &fs, sizeof(fs));
            break;
        }

        case FIELD_TYPE_INT32: {
            lua_Integer raw = luaL_checkinteger(L, valueIndex);
            if (raw < INT32_MIN || raw > INT32_MAX) {
                luaL_error(L, "Value for %s.%s is outside int32 range",
                           layout->componentName, propertyName);
                return false;
            }
            int32_t value = (int32_t)raw;
            wrote = safe_memory_write(address, &value, sizeof(value));
            break;
        }

        case FIELD_TYPE_UINT8: {
            lua_Integer raw = luaL_checkinteger(L, valueIndex);
            if (raw < 0 || raw > UINT8_MAX) {
                luaL_error(L, "Value for %s.%s is outside uint8 range",
                           layout->componentName, propertyName);
                return false;
            }
            uint8_t value = (uint8_t)raw;
            wrote = safe_memory_write(address, &value, sizeof(value));
            break;
        }

        case FIELD_TYPE_INT8: {
            lua_Integer raw = luaL_checkinteger(L, valueIndex);
            if (raw < INT8_MIN || raw > INT8_MAX) {
                luaL_error(L, "Value for %s.%s is outside int8 range",
                           layout->componentName, propertyName);
                return false;
            }
            int8_t value = (int8_t)raw;
            wrote = safe_memory_write(address, &value, sizeof(value));
            break;
        }

        case FIELD_TYPE_INT16: {
            lua_Integer raw = luaL_checkinteger(L, valueIndex);
            if (raw < INT16_MIN || raw > INT16_MAX) {
                luaL_error(L, "Value for %s.%s is outside int16 range",
                           layout->componentName, propertyName);
                return false;
            }
            int16_t value = (int16_t)raw;
            wrote = safe_memory_write(address, &value, sizeof(value));
            break;
        }

        case FIELD_TYPE_UINT16: {
            lua_Integer raw = luaL_checkinteger(L, valueIndex);
            if (raw < 0 || raw > UINT16_MAX) {
                luaL_error(L, "Value for %s.%s is outside uint16 range",
                           layout->componentName, propertyName);
                return false;
            }
            uint16_t value = (uint16_t)raw;
            wrote = safe_memory_write(address, &value, sizeof(value));
            break;
        }

        case FIELD_TYPE_UINT32: {
            lua_Integer raw = luaL_checkinteger(L, valueIndex);
            if (raw < 0 || raw > UINT32_MAX) {
                luaL_error(L, "Value for %s.%s is outside uint32 range",
                           layout->componentName, propertyName);
                return false;
            }
            uint32_t value = (uint32_t)raw;
            wrote = safe_memory_write(address, &value, sizeof(value));
            break;
        }

        case FIELD_TYPE_INT64: {
            int64_t value = (int64_t)luaL_checkinteger(L, valueIndex);
            wrote = safe_memory_write(address, &value, sizeof(value));
            break;
        }

        case FIELD_TYPE_UINT64: {
            // lua_Integer is int64; the bit pattern is what the engine keeps.
            uint64_t value = (uint64_t)luaL_checkinteger(L, valueIndex);
            wrote = safe_memory_write(address, &value, sizeof(value));
            break;
        }

        case FIELD_TYPE_DOUBLE: {
            double value = (double)luaL_checknumber(L, valueIndex);
            wrote = safe_memory_write(address, &value, sizeof(value));
            break;
        }

        case FIELD_TYPE_GUID: {
            // Accepts the canonical 36-char form, or anything ending in one
            // (upstream tolerates "Name_<uuid>" template ids).
            size_t slen = 0;
            const char *s = luaL_checklstring(L, valueIndex, &slen);
            Guid value = {0, 0};
            if (slen > 0) {
                const char *tail = slen > 36 ? s + slen - 36 : s;
                if (!guid_parse(tail, &value)) {
                    luaL_error(L, "'%s' is not a valid GUID for %s.%s", s,
                               layout->componentName, propertyName);
                    return false;
                }
            }
            wrote = safe_memory_write(address, &value, sizeof(value));
            break;
        }

        case FIELD_TYPE_ENTITY_HANDLE: {
            // Reads surface handles as "0x..." strings; accept those and
            // plain integers (upstream EntityHandle is a uint64).
            uint64_t value = 0;
            if (lua_type(L, valueIndex) == LUA_TSTRING) {
                const char *s = lua_tostring(L, valueIndex);
                char *end = NULL;
                value = strtoull(s, &end, 0);
                if (end == s || *end != '\0') {
                    luaL_error(L, "'%s' is not a valid entity handle for %s.%s", s,
                               layout->componentName, propertyName);
                    return false;
                }
            } else {
                value = (uint64_t)luaL_checkinteger(L, valueIndex);
            }
            wrote = safe_memory_write(address, &value, sizeof(value));
            break;
        }

        case FIELD_TYPE_ROLL_MAP: {
            char what[160];
            snprintf(what, sizeof(what), "%s.%s", layout->componentName, propertyName);
            wrote = roll_map_write(L, address, valueIndex, what);
            break;
        }

        case FIELD_TYPE_DYNAMIC_ARRAY: {
            char what[160];
            snprintf(what, sizeof(what), "%s.%s", layout->componentName, propertyName);
            wrote = plain_array_write(L, address, valueIndex, prop, what);
            break;
        }

        case FIELD_TYPE_BOOL: {
            luaL_checktype(L, valueIndex, LUA_TBOOLEAN);
            uint8_t value = lua_toboolean(L, valueIndex) ? 1 : 0;
            wrote = safe_memory_write(address, &value, sizeof(value));
            break;
        }

        case FIELD_TYPE_FLOAT: {
            float value = (float)luaL_checknumber(L, valueIndex);
            wrote = safe_memory_write(address, &value, sizeof(value));
            break;
        }

        case FIELD_TYPE_INT32_ARRAY: {
            int absoluteIndex = lua_absindex(L, valueIndex);
            luaL_checktype(L, absoluteIndex, LUA_TTABLE);
            size_t suppliedSize = lua_rawlen(L, absoluteIndex);
            if (suppliedSize != prop->arraySize) {
                luaL_error(L, "Value for %s.%s must contain exactly %u elements",
                           layout->componentName, propertyName, prop->arraySize);
                return false;
            }

            int32_t values[UINT8_MAX];
            for (uint8_t i = 0; i < prop->arraySize; i++) {
                lua_rawgeti(L, absoluteIndex, (lua_Integer)i + 1);
                lua_Integer raw = luaL_checkinteger(L, -1);
                if (raw < INT32_MIN || raw > INT32_MAX) {
                    luaL_error(L, "Element %u for %s.%s is outside int32 range",
                               (unsigned)i + 1, layout->componentName, propertyName);
                    return false;
                }
                values[i] = (int32_t)raw;
                lua_pop(L, 1);
            }

            wrote = safe_memory_write(address, values, fieldSize);
            break;
        }

        case FIELD_TYPE_FLOAT_ARRAY: {
            // Mirrors the INT32_ARRAY contract: exact length, per-element
            // validation (NaN/infinity refused — the engine treats both as
            // corrupt data), staged buffer, one atomic write. Wave 7 A7:
            // no verified layout carries this type yet, so the path is
            // exercised only once a real field lands (ls::EffectComponent::
            // OverrideFadeCapacity is the verification candidate).
            int absoluteIndex = lua_absindex(L, valueIndex);
            luaL_checktype(L, absoluteIndex, LUA_TTABLE);
            size_t suppliedSize = lua_rawlen(L, absoluteIndex);
            if (suppliedSize != prop->arraySize) {
                luaL_error(L, "Value for %s.%s must contain exactly %u elements",
                           layout->componentName, propertyName, prop->arraySize);
                return false;
            }

            float values[UINT8_MAX];
            for (uint8_t i = 0; i < prop->arraySize; i++) {
                lua_rawgeti(L, absoluteIndex, (lua_Integer)i + 1);
                double raw = (double)luaL_checknumber(L, -1);
                if (isnan(raw) || isinf(raw)) {
                    luaL_error(L, "Element %u for %s.%s is NaN or infinity",
                               (unsigned)i + 1, layout->componentName, propertyName);
                    return false;
                }
                values[i] = (float)raw;
                lua_pop(L, 1);
            }

            wrote = safe_memory_write(address, values, fieldSize);
            break;
        }

        default:
            /* component_property_field_size() rejects every other type. */
            break;
    }

    if (!wrote) {
        LOG_ENTITY_DEBUG("Component write failed safely: %s.%s at %p (%zu bytes)",
                         layout->componentName, propertyName, (void *)(uintptr_t)address,
                         fieldSize);
        return false;
    }

    LOG_ENTITY_DEBUG("Component write succeeded: %s.%s (%zu bytes)",
                     layout->componentName, propertyName, fieldSize);
    return true;
}

// ============================================================================
// Component Proxy Userdata
// ============================================================================

typedef struct {
    void *componentPtr;
    const ComponentLayoutDef *layout;
    LifetimeHandle lifetime;
} ComponentProxy;

// Custom properties currently extend component proxies only; StatsObject and
// other userdata keep their existing metatable behavior.
static bool component_proxy_push_custom_type(lua_State *L,
                                             const char *component_name) {
    int base = lua_gettop(L);
    lua_getfield(L, LUA_REGISTRYINDEX, BG3SE_CUSTOM_PROPS_REGISTRY_KEY);
    if (!lua_istable(L, -1)) {
        lua_settop(L, base);
        return false;
    }

    lua_getfield(L, -1, component_name);
    if (!lua_istable(L, -1)) {
        lua_settop(L, base);
        return false;
    }

    lua_remove(L, base + 1);
    return true;
}

static int component_proxy_custom_index(lua_State *L,
                                        const char *component_name,
                                        const char *key) {
    int base = lua_gettop(L);
    if (!component_proxy_push_custom_type(L, component_name)) {
        return 0;
    }

    lua_getfield(L, -1, "functions");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, key);
        if (lua_isfunction(L, -1)) {
            lua_replace(L, base + 1);
            lua_settop(L, base + 1);
            return 1;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    lua_getfield(L, -1, "properties");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, key);
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "getter");
            if (lua_isfunction(L, -1)) {
                lua_replace(L, base + 1);
                lua_settop(L, base + 1);
                lua_pushvalue(L, 1);
                lua_call(L, 1, 1);
                return 1;
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }

    lua_settop(L, base);
    return 0;
}

static int component_proxy_custom_newindex(lua_State *L,
                                           const char *component_name,
                                           const char *key) {
    int base = lua_gettop(L);
    if (!component_proxy_push_custom_type(L, component_name)) {
        return 0;
    }

    lua_getfield(L, -1, "properties");
    if (!lua_istable(L, -1)) {
        lua_settop(L, base);
        return 0;
    }

    lua_getfield(L, -1, key);
    if (!lua_istable(L, -1)) {
        lua_settop(L, base);
        return 0;
    }

    lua_getfield(L, -1, "setter");
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, base);
        return luaL_error(L, "Property '%s' is read-only", key);
    }

    lua_replace(L, base + 1);
    lua_settop(L, base + 1);
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 3);
    lua_call(L, 2, 0);
    return 1;
}

static int component_proxy_index(lua_State *L) {
    ComponentProxy *proxy = (ComponentProxy *)luaL_checkudata(L, 1, COMPONENT_PROXY_METATABLE);
    if (!lifetime_lua_is_valid(L, proxy->lifetime)) {
        return lifetime_lua_expired_error(L, "Component");
    }
    const char *key = luaL_checkstring(L, 2);

    // Special properties
    if (strcmp(key, "__type") == 0) {
        lua_pushstring(L, proxy->layout->componentName);
        return 1;
    }
    if (strcmp(key, "__shortname") == 0) {
        lua_pushstring(L, proxy->layout->shortName);
        return 1;
    }
    if (strcmp(key, "__ptr") == 0) {
        lua_pushlightuserdata(L, proxy->componentPtr);
        return 1;
    }

    // Look up property
    int result = component_property_read(L, proxy->componentPtr, proxy->layout, key);
    if (result > 0) {
        return result;
    }

    result = component_proxy_custom_index(
        L, proxy->layout->componentName, key);
    if (result > 0) {
        return result;
    }

    // Property not found
    lua_pushnil(L);
    return 1;
}

static int component_proxy_newindex(lua_State *L) {
    ComponentProxy *proxy = (ComponentProxy *)luaL_checkudata(L, 1, COMPONENT_PROXY_METATABLE);
    if (!lifetime_lua_is_valid(L, proxy->lifetime)) {
        return lifetime_lua_expired_error(L, "Component");
    }
    const char *key = luaL_checkstring(L, 2);

    const ComponentPropertyDef *property = find_property(proxy->layout, key);
    if (!property) {
        int result = component_proxy_custom_newindex(
            L, proxy->layout->componentName, key);
        if (result > 0) {
            return 0;
        }
    }

    /*
     * Norbyte's Windows LightObjectProxyMetatable::NewIndex translates every
     * non-Success property-map result into luaL_error (including read-only and
     * unsupported types).  Keep the same UX: never silently ignore a refused
     * game-memory write.
     */
    if (!property || !component_property_write(
            L, proxy->componentPtr, proxy->layout, key, 3)) {
        return luaL_error(L, "Cannot set component property %s.%s",
                          proxy->layout->componentName, key);
    }

    return 0;
}

static int component_proxy_tostring(lua_State *L) {
    ComponentProxy *proxy = (ComponentProxy *)luaL_checkudata(L, 1, COMPONENT_PROXY_METATABLE);
    // tostring works even on expired components (for debugging)
    bool valid = lifetime_lua_is_valid(L, proxy->lifetime);
    if (valid) {
        lua_pushfstring(L, "Component<%s>(%p)",
                       proxy->layout->shortName ? proxy->layout->shortName : proxy->layout->componentName,
                       proxy->componentPtr);
    } else {
        lua_pushfstring(L, "Component<%s>(%p) [EXPIRED]",
                       proxy->layout->shortName ? proxy->layout->shortName : proxy->layout->componentName,
                       proxy->componentPtr);
    }
    return 1;
}

static int component_proxy_pairs_iter(lua_State *L) {
    ComponentProxy *proxy = (ComponentProxy *)lua_touserdata(L, lua_upvalueindex(1));
    int *index = (int *)lua_touserdata(L, lua_upvalueindex(2));

    // Validate lifetime on each iteration
    if (!lifetime_lua_is_valid(L, proxy->lifetime)) {
        return lifetime_lua_expired_error(L, "Component");
    }

    if (*index >= proxy->layout->propertyCount) {
        return 0;  // End of iteration
    }

    const ComponentPropertyDef *prop = &proxy->layout->properties[*index];
    lua_pushstring(L, prop->name);
    component_property_read_def(L, proxy->componentPtr, prop);

    (*index)++;
    return 2;
}

static int component_proxy_pairs(lua_State *L) {
    ComponentProxy *proxy = (ComponentProxy *)luaL_checkudata(L, 1, COMPONENT_PROXY_METATABLE);
    if (!lifetime_lua_is_valid(L, proxy->lifetime)) {
        return lifetime_lua_expired_error(L, "Component");
    }

    // Create upvalues: proxy and index
    lua_pushlightuserdata(L, proxy);
    int *index = (int *)lua_newuserdata(L, sizeof(int));
    *index = 0;

    lua_pushcclosure(L, component_proxy_pairs_iter, 2);
    lua_pushvalue(L, 1);  // table (proxy)
    lua_pushnil(L);       // initial key
    return 3;
}

void component_property_push_proxy(lua_State *L, void *componentPtr,
                                   const ComponentLayoutDef *layout) {
    if (!componentPtr || !layout) {
        lua_pushnil(L);
        return;
    }

    ComponentProxy *proxy = (ComponentProxy *)lua_newuserdata(L, sizeof(ComponentProxy));
    proxy->componentPtr = componentPtr;
    proxy->layout = layout;
    proxy->lifetime = lifetime_lua_get_current(L);

    luaL_getmetatable(L, COMPONENT_PROXY_METATABLE);
    lua_setmetatable(L, -2);
}

const ComponentLayoutDef *component_property_check_proxy(lua_State *L, int index) {
    void *ud = luaL_testudata(L, index, COMPONENT_PROXY_METATABLE);
    if (ud) {
        ComponentProxy *proxy = (ComponentProxy *)ud;
        return proxy->layout;
    }
    return NULL;
}

// ============================================================================
// Array Proxy Userdata
// ============================================================================

typedef struct {
    void *arrayPtr;             // Pointer to Array<T> struct (buf_/capacity_/size_)
    ArrayElementType elemType;  // Element type for formatting
    uint16_t elemSize;          // Element size in bytes
    const ComponentLayoutDef *structLayout;  // Pointee layout for ELEM_TYPE_STRUCT_PTR
    LifetimeHandle lifetime;    // For validity checking
} ArrayProxy;

// Read array metadata from memory
static bool array_proxy_read_metadata(ArrayProxy *proxy, void **buf_out, uint32_t *size_out) {
    if (!proxy || !proxy->arrayPtr) return false;

    uintptr_t base = (uintptr_t)proxy->arrayPtr;

    // Read buf_ pointer
    void *buf = NULL;
    if (!safe_memory_read((mach_vm_address_t)(base + ARRAY_BUF_OFFSET), &buf, sizeof(buf))) {
        return false;
    }

    // Read size_
    uint32_t size = 0;
    if (!safe_memory_read_u32((mach_vm_address_t)(base + ARRAY_SIZE_OFFSET), &size)) {
        return false;
    }

    if (buf_out) *buf_out = buf;
    if (size_out) *size_out = size;
    return true;
}

// Push a single array element to Lua stack
static int array_proxy_push_element(lua_State *L, ArrayProxy *proxy, void *buf, uint32_t index) {
    if (!buf || proxy->elemSize == 0) {
        lua_pushnil(L);
        return 1;
    }

    uintptr_t elemAddr = (uintptr_t)buf + (index * proxy->elemSize);

    switch (proxy->elemType) {
        case ELEM_TYPE_GUID: {
            // Upstream Guid::ToString byte order (see FIELD_TYPE_GUID).
            Guid guid = {0, 0};
            if (safe_memory_read((mach_vm_address_t)elemAddr, &guid, sizeof(guid))) {
                char buf[37];
                guid_to_string(&guid, buf);
                lua_pushstring(L, buf);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        case ELEM_TYPE_FIXED_STRING: {
            uint32_t val = 0;
            if (safe_memory_read_u32((mach_vm_address_t)elemAddr, &val)) {
                lua_pushinteger(L, val);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        case ELEM_TYPE_ENTITY_HANDLE: {
            uint64_t val = 0;
            if (safe_memory_read((mach_vm_address_t)elemAddr, &val, sizeof(val))) {
                char buf[32];
                snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)val);
                lua_pushstring(L, buf);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        case ELEM_TYPE_CLASS_INFO: {
            // ClassInfo: ClassUUID(16) + SubClassUUID(16) + Level(4)
            lua_createtable(L, 0, 5);

            // ClassUUID at offset 0
            uint8_t classGuid[16] = {0};
            if (safe_memory_read((mach_vm_address_t)elemAddr, classGuid, 16)) {
                char guidBuf[GUID_STRING_SIZE];
                guid_bytes_to_string(classGuid, guidBuf, sizeof(guidBuf));
                lua_pushstring(L, guidBuf);
                lua_setfield(L, -2, "ClassUUID");
            }

            // SubClassUUID at offset 16
            uint8_t subclassGuid[16] = {0};
            if (safe_memory_read((mach_vm_address_t)(elemAddr + 16), subclassGuid, 16)) {
                char guidBuf[GUID_STRING_SIZE];
                guid_bytes_to_string(subclassGuid, guidBuf, sizeof(guidBuf));
                lua_pushstring(L, guidBuf);
                lua_setfield(L, -2, "SubClassUUID");
            }

            // Level at offset 32
            int32_t level = 0;
            if (safe_memory_read((mach_vm_address_t)(elemAddr + 32), &level, sizeof(level))) {
                lua_pushinteger(L, level);
                lua_setfield(L, -2, "Level");
            }

            // Debug info
            lua_pushinteger(L, index + 1);
            lua_setfield(L, -2, "__index");

            return 1;
        }

        case ELEM_TYPE_BOOST_ENTRY: {
            // BoostEntry: BoostType(4) + padding(4) + Array<EntityHandle>(buf:8 + cap:4 + size:4)
            lua_createtable(L, 0, 4);

            // BoostType at offset 0
            uint32_t boostType = 0;
            if (safe_memory_read_u32((mach_vm_address_t)elemAddr, &boostType)) {
                lua_pushinteger(L, boostType);
                lua_setfield(L, -2, "Type");
            }

            // Array<EntityHandle> at offset 8 - size is at offset 8+8+4 = 20
            uint32_t boostCount = 0;
            if (safe_memory_read_u32((mach_vm_address_t)(elemAddr + 20), &boostCount)) {
                lua_pushinteger(L, boostCount);
                lua_setfield(L, -2, "BoostCount");
            }

            // Boosts: the boost entity handles (upstream BoostEntry.Boosts),
            // surfaced as "0x..." strings like ELEM_TYPE_ENTITY_HANDLE.
            uint64_t boostBuf = 0;
            if (safe_memory_read((mach_vm_address_t)(elemAddr + 8), &boostBuf, sizeof(boostBuf))
                && boostBuf != 0 && boostCount <= 256) {
                lua_createtable(L, (int)boostCount, 0);
                for (uint32_t i = 0; i < boostCount; i++) {
                    uint64_t handle = 0;
                    if (!safe_memory_read((mach_vm_address_t)(boostBuf + (uint64_t)i * 8),
                                          &handle, sizeof(handle))) {
                        break;
                    }
                    char handleBuf[32];
                    snprintf(handleBuf, sizeof(handleBuf), "0x%llx", (unsigned long long)handle);
                    lua_pushstring(L, handleBuf);
                    lua_rawseti(L, -2, (lua_Integer)i + 1);
                }
                lua_setfield(L, -2, "Boosts");
            }

            // Debug info
            lua_pushinteger(L, index + 1);
            lua_setfield(L, -2, "__index");

            char addrBuf[32];
            snprintf(addrBuf, sizeof(addrBuf), "0x%llx", (unsigned long long)elemAddr);
            lua_pushstring(L, addrBuf);
            lua_setfield(L, -2, "__ptr");

            return 1;
        }

        case ELEM_TYPE_STRUCT_PTR: {
            // Array<T*>: proxy the pointee (e.g. esv::StatusMachine::Statuses)
            void *target = NULL;
            if (!proxy->structLayout || proxy->elemSize != sizeof(void *)
                || !safe_memory_read((mach_vm_address_t)elemAddr, &target, sizeof(target))
                || !target) {
                lua_pushnil(L);
                return 1;
            }
            component_property_push_proxy(L, target, proxy->structLayout);
            return 1;
        }

        case ELEM_TYPE_SPELL_DATA:
        case ELEM_TYPE_SPELL_META:
        case ELEM_TYPE_STATUS_INFO:
        case ELEM_TYPE_UNKNOWN:
        default: {
            // For complex types, return a table with the element address and basic info
            // This allows further introspection
            lua_createtable(L, 0, 3);

            // __ptr: raw address for debugging
            char addrBuf[32];
            snprintf(addrBuf, sizeof(addrBuf), "0x%llx", (unsigned long long)elemAddr);
            lua_pushstring(L, addrBuf);
            lua_setfield(L, -2, "__ptr");

            // __index: 1-based index
            lua_pushinteger(L, index + 1);
            lua_setfield(L, -2, "__index");

            // __size: element size
            lua_pushinteger(L, proxy->elemSize);
            lua_setfield(L, -2, "__size");

            // For SpellData, try to extract the SpellId (first field is SpellId struct)
            if (proxy->elemType == ELEM_TYPE_SPELL_DATA) {
                // SpellId is at offset 0, contains FixedString at 0x00
                uint32_t spellId = 0;
                if (safe_memory_read_u32((mach_vm_address_t)elemAddr, &spellId)) {
                    lua_pushinteger(L, spellId);
                    lua_setfield(L, -2, "SpellId");
                }
            }

            return 1;
        }
    }
}

static int array_proxy_index(lua_State *L) {
    ArrayProxy *proxy = (ArrayProxy *)luaL_checkudata(L, 1, ARRAY_PROXY_METATABLE);
    if (!lifetime_lua_is_valid(L, proxy->lifetime)) {
        return lifetime_lua_expired_error(L, "Array");
    }

    // Get index (1-based in Lua)
    if (!lua_isinteger(L, 2)) {
        lua_pushnil(L);
        return 1;
    }

    lua_Integer luaIndex = lua_tointeger(L, 2);
    if (luaIndex < 1) {
        lua_pushnil(L);
        return 1;
    }

    // Read array metadata
    void *buf = NULL;
    uint32_t size = 0;
    if (!array_proxy_read_metadata(proxy, &buf, &size)) {
        lua_pushnil(L);
        return 1;
    }

    // Convert to 0-based index and check bounds
    uint32_t index = (uint32_t)(luaIndex - 1);
    if (index >= size) {
        lua_pushnil(L);
        return 1;
    }

    return array_proxy_push_element(L, proxy, buf, index);
}

static int array_proxy_len(lua_State *L) {
    ArrayProxy *proxy = (ArrayProxy *)luaL_checkudata(L, 1, ARRAY_PROXY_METATABLE);
    if (!lifetime_lua_is_valid(L, proxy->lifetime)) {
        return lifetime_lua_expired_error(L, "Array");
    }

    uint32_t size = 0;
    if (array_proxy_read_metadata(proxy, NULL, &size)) {
        lua_pushinteger(L, size);
    } else {
        lua_pushinteger(L, 0);
    }
    return 1;
}

static int array_proxy_tostring(lua_State *L) {
    ArrayProxy *proxy = (ArrayProxy *)luaL_checkudata(L, 1, ARRAY_PROXY_METATABLE);
    bool valid = lifetime_lua_is_valid(L, proxy->lifetime);

    if (valid) {
        uint32_t size = 0;
        array_proxy_read_metadata(proxy, NULL, &size);
        lua_pushfstring(L, "Array[%d](%p)", (int)size, proxy->arrayPtr);
    } else {
        lua_pushfstring(L, "Array(%p) [EXPIRED]", proxy->arrayPtr);
    }
    return 1;
}

static int array_proxy_pairs_iter(lua_State *L) {
    ArrayProxy *proxy = (ArrayProxy *)lua_touserdata(L, lua_upvalueindex(1));
    int *index = (int *)lua_touserdata(L, lua_upvalueindex(2));

    if (!lifetime_lua_is_valid(L, proxy->lifetime)) {
        return lifetime_lua_expired_error(L, "Array");
    }

    void *buf = NULL;
    uint32_t size = 0;
    if (!array_proxy_read_metadata(proxy, &buf, &size)) {
        return 0;
    }

    if (*index >= (int)size) {
        return 0;  // End of iteration
    }

    // Push 1-based key
    lua_pushinteger(L, *index + 1);

    // Push value
    array_proxy_push_element(L, proxy, buf, *index);

    (*index)++;
    return 2;
}

static int array_proxy_pairs(lua_State *L) {
    ArrayProxy *proxy = (ArrayProxy *)luaL_checkudata(L, 1, ARRAY_PROXY_METATABLE);
    if (!lifetime_lua_is_valid(L, proxy->lifetime)) {
        return lifetime_lua_expired_error(L, "Array");
    }

    // Create upvalues: proxy and index
    lua_pushlightuserdata(L, proxy);
    int *index = (int *)lua_newuserdata(L, sizeof(int));
    *index = 0;

    lua_pushcclosure(L, array_proxy_pairs_iter, 2);
    lua_pushvalue(L, 1);  // table (proxy)
    lua_pushnil(L);       // initial key
    return 3;
}

void component_property_push_array_proxy(lua_State *L, void *arrayPtr,
                                         const ComponentPropertyDef *prop) {
    if (!arrayPtr || !prop) {
        lua_pushnil(L);
        return;
    }

    ArrayProxy *proxy = (ArrayProxy *)lua_newuserdata(L, sizeof(ArrayProxy));
    proxy->arrayPtr = arrayPtr;
    proxy->elemType = prop->elemType;
    proxy->elemSize = prop->elemSize;
    proxy->structLayout = prop->structLayout;
    proxy->lifetime = lifetime_lua_get_current(L);

    luaL_getmetatable(L, ARRAY_PROXY_METATABLE);
    lua_setmetatable(L, -2);
}

static void serialize_array_proxy(lua_State *L, ArrayProxy *proxy) {
    if (!proxy || proxy->elemSize == 0) {
        lua_pushnil(L);
        return;
    }

    void *buf = NULL;
    uint32_t size = 0;
    if (!array_proxy_read_metadata(proxy, &buf, &size) || (size > 0 && !buf)) {
        lua_pushnil(L);
        return;
    }
    if (size > (uint32_t)INT_MAX) {
        LOG_ENTITY_DEBUG("Refusing to serialize implausibly large array (%u elements)",
                         size);
        lua_pushnil(L);
        return;
    }

    lua_createtable(L, (int)size, 0);
    for (uint32_t i = 0; i < size; i++) {
        array_proxy_push_element(L, proxy, buf, i);
        // Struct-pointer elements come back as proxies; flatten them so the
        // serialized table holds no game references.
        if (proxy->elemType == ELEM_TYPE_STRUCT_PTR && lua_isuserdata(L, -1)
            && component_property_serialize_proxy(L, -1)) {
            lua_remove(L, -2);
        }
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
}

bool component_property_serialize_proxy(lua_State *L, int index) {
    int absoluteIndex = lua_absindex(L, index);
    ComponentProxy *component = (ComponentProxy *)luaL_testudata(
        L, absoluteIndex, COMPONENT_PROXY_METATABLE);
    if (component) {
        if (!lifetime_lua_is_valid(L, component->lifetime)) {
            lifetime_lua_expired_error(L, "Component");
            return true;
        }

        lua_createtable(L, 0, component->layout->propertyCount);
        for (int i = 0; i < component->layout->propertyCount; i++) {
            const ComponentPropertyDef *prop = &component->layout->properties[i];
            if (prop->type == FIELD_TYPE_DYNAMIC_ARRAY) {
                ArrayProxy array = {
                    .arrayPtr = (char *)component->componentPtr + prop->offset,
                    .elemType = prop->elemType,
                    .elemSize = prop->elemSize,
                    .structLayout = prop->structLayout,
                    .lifetime = component->lifetime
                };
                serialize_array_proxy(L, &array);
            } else {
                component_property_read_def(
                    L, component->componentPtr, prop);
                if (prop->type == FIELD_TYPE_STRUCT_PTR && lua_isuserdata(L, -1)
                    && component_property_serialize_proxy(L, -1)) {
                    lua_remove(L, -2);
                }
            }

            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
            } else {
                lua_setfield(L, -2, prop->name);
            }
        }
        return true;
    }

    ArrayProxy *array = (ArrayProxy *)luaL_testudata(
        L, absoluteIndex, ARRAY_PROXY_METATABLE);
    if (array) {
        if (!lifetime_lua_is_valid(L, array->lifetime)) {
            lifetime_lua_expired_error(L, "Array");
            return true;
        }
        serialize_array_proxy(L, array);
        return true;
    }

    return false;
}

static bool property_can_unserialize(const ComponentLayoutDef *layout,
                                     const ComponentPropertyDef *prop) {
    if (!layout || !prop || prop->readOnly) return false;
    // Generated layouts are unverified against this binary; writing through
    // them corrupts engine memory (a transmog stat-clone crashed the server
    // status system this way). Reads stay allowed; writes require a
    // hand-verified layout.
    if (layout->generated) return false;
    if (strstr(layout->componentName, "OneFrame") != NULL
        || strstr(layout->componentName, "Request") != NULL) {
        return false;
    }
    if (component_property_is_pointer_typed(prop)) {
        return false;
    }
    return component_property_field_size(prop) > 0;
}

bool component_property_unserialize_proxy(lua_State *L, int proxyIndex,
                                          int tableIndex) {
    int absoluteProxy = lua_absindex(L, proxyIndex);
    int absoluteTable = lua_absindex(L, tableIndex);
    ComponentProxy *component = (ComponentProxy *)luaL_testudata(
        L, absoluteProxy, COMPONENT_PROXY_METATABLE);
    if (!component) {
        return false;
    }

    if (!lifetime_lua_is_valid(L, component->lifetime)) {
        lifetime_lua_expired_error(L, "Component");
        return true;
    }

    luaL_checktype(L, absoluteTable, LUA_TTABLE);
    for (int i = 0; i < component->layout->propertyCount; i++) {
        const ComponentPropertyDef *prop = &component->layout->properties[i];
        if (!property_can_unserialize(component->layout, prop)) {
            continue;
        }

        lua_getfield(L, absoluteTable, prop->name);
        if (!lua_isnil(L, -1)) {
            LOG_ENTITY_DEBUG("unserialize write %s.%s",
                             component->layout->componentName, prop->name);
        }
        if (!lua_isnil(L, -1)
            && !component_property_write(
                L, component->componentPtr, component->layout, prop->name, -1)) {
            return luaL_error(
                L, "Cannot unserialize component property %s.%s",
                component->layout->componentName, prop->name);
        }
        lua_pop(L, 1);
    }

    return true;
}

// ============================================================================
// Lua Registration
// ============================================================================

void component_property_register_lua(lua_State *L) {
    // Create ComponentProxy metatable
    luaL_newmetatable(L, COMPONENT_PROXY_METATABLE);

    lua_pushcfunction(L, component_proxy_index);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, component_proxy_newindex);
    lua_setfield(L, -2, "__newindex");

    lua_pushcfunction(L, component_proxy_tostring);
    lua_setfield(L, -2, "__tostring");

    lua_pushcfunction(L, component_proxy_pairs);
    lua_setfield(L, -2, "__pairs");

    lua_pop(L, 1);

    LOG_ENTITY_DEBUG("Registered ComponentProxy metatable");

    // Create ArrayProxy metatable
    luaL_newmetatable(L, ARRAY_PROXY_METATABLE);

    lua_pushcfunction(L, array_proxy_index);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, array_proxy_len);
    lua_setfield(L, -2, "__len");

    lua_pushcfunction(L, array_proxy_tostring);
    lua_setfield(L, -2, "__tostring");

    lua_pushcfunction(L, array_proxy_pairs);
    lua_setfield(L, -2, "__pairs");

    // __ipairs uses same iterator as __pairs (1-based keys)
    lua_pushcfunction(L, array_proxy_pairs);
    lua_setfield(L, -2, "__ipairs");

    lua_pop(L, 1);

    LOG_ENTITY_DEBUG("Registered ArrayProxy metatable");
}

// ============================================================================
// Debugging
// ============================================================================

int component_property_get_layout_count(void) {
    return g_LayoutCount;
}

const ComponentLayoutDef *component_property_get_layout_at(int index) {
    if (index < 0 || index >= g_LayoutCount) {
        return NULL;
    }
    return &g_Layouts[index];
}

void component_property_iterate_layouts(ComponentLayoutIteratorFn callback, void *userdata) {
    if (!callback) return;

    for (int i = 0; i < g_LayoutCount; i++) {
        if (!callback(&g_Layouts[i], userdata)) {
            break;  // Callback returned false, stop iteration
        }
    }
}

void component_property_dump_layouts(void) {
    LOG_ENTITY_DEBUG("=== Component Property Layouts (%d total) ===", g_LayoutCount);
    for (int i = 0; i < g_LayoutCount; i++) {
        const ComponentLayoutDef *layout = &g_Layouts[i];
        LOG_ENTITY_DEBUG("  %s (%s): TypeIndex=%u, Size=0x%x, Properties=%d",
                       layout->componentName,
                       layout->shortName ? layout->shortName : "?",
                       layout->componentTypeIndex,
                       layout->componentSize,
                       layout->propertyCount);
    }
}
