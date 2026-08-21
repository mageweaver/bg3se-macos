/**
 * lua_staticdata.c - Lua bindings for Ext.StaticData API
 *
 * Provides Lua access to immutable game data like Feats, Races, Backgrounds, etc.
 *
 * API:
 *   Ext.StaticData.GetAll(type) - Get all entries of a type as array of tables
 *   Ext.StaticData.Get(type, guid) - Get single entry by GUID string
 *   Ext.StaticData.GetCount(type) - Get count of entries for a type
 */

#include "lua_staticdata.h"
#include "../staticdata/staticdata_manager.h"
#include "../core/logging.h"
#include <lua.h>
#include <lauxlib.h>
#include <string.h>
#include <stdio.h>
#include "../core/safe_memory.h"
#include <string.h>

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Push a static data entry as a Lua table.
 */
static void push_staticdata_entry(lua_State *L, StaticDataType type, StaticDataPtr entry) {
    if (!entry) {
        lua_pushnil(L);
        return;
    }

    lua_newtable(L);

    // Add ResourceUUID (GUID)
    char guid_str[40];
    if (staticdata_get_guid_string(type, entry, guid_str, sizeof(guid_str))) {
        lua_pushstring(L, guid_str);
        lua_setfield(L, -2, "ResourceUUID");
    }

    // Add Name if available
    const char* name = staticdata_get_name(type, entry);
    if (name) {
        lua_pushstring(L, name);
        lua_setfield(L, -2, "Name");
    }

    // Add DisplayName if available
    const char* display_name = staticdata_get_display_name(type, entry);
    if (display_name) {
        lua_pushstring(L, display_name);
        lua_setfield(L, -2, "DisplayName");
    }

    // Add Type
    lua_pushstring(L, staticdata_type_name(type));
    lua_setfield(L, -2, "Type");

    // Add raw pointer for debugging
    lua_pushlightuserdata(L, entry);
    lua_setfield(L, -2, "_ptr");
}

// ============================================================================
// Ext.StaticData.GetAll(type)
// ============================================================================

/**
 * Get all entries of a static data type.
 *
 * @param type Type name string (e.g., "Feat", "Race")
 * @return Array table of entry tables, or nil on error
 */
static int lua_staticdata_getall(lua_State *L) {
    const char* type_name = luaL_checkstring(L, 1);

    int type = staticdata_type_from_name(type_name);
    if (type < 0) {
        return luaL_error(L, "Unknown static data type: %s", type_name);
    }

    if (!staticdata_has_manager((StaticDataType)type)) {
        // Manager not yet captured - return empty table
        // The hook will capture it when game code accesses it
        lua_newtable(L);
        return 1;
    }

    int count = staticdata_get_count((StaticDataType)type);
    if (count < 0) {
        lua_newtable(L);
        return 1;
    }

    lua_createtable(L, count, 0);

    for (int i = 0; i < count; i++) {
        StaticDataPtr entry = staticdata_get_by_index((StaticDataType)type, i);
        if (entry) {
            push_staticdata_entry(L, (StaticDataType)type, entry);
            lua_rawseti(L, -2, i + 1);  // Lua arrays are 1-indexed
        }
    }

    return 1;
}

// ============================================================================
// Ext.StaticData.Get(type, guid)
// ============================================================================

/**
 * Get a single static data entry by GUID.
 *
 * @param type Type name string
 * @param guid GUID string (e.g., "e7ab823e-32b2-49f8-b7b3-7f9c2d4c1f5e")
 * @return Entry table, or nil if not found
 */
static int lua_staticdata_get(lua_State *L) {
    const char* type_name = luaL_checkstring(L, 1);
    const char* guid_str = luaL_checkstring(L, 2);

    int type = staticdata_type_from_name(type_name);
    if (type < 0) {
        return luaL_error(L, "Unknown static data type: %s", type_name);
    }

    if (!staticdata_has_manager((StaticDataType)type)) {
        lua_pushnil(L);
        return 1;
    }

    StaticDataPtr entry = staticdata_get_by_guid_string((StaticDataType)type, guid_str);
    if (!entry) {
        lua_pushnil(L);
        return 1;
    }

    push_staticdata_entry(L, (StaticDataType)type, entry);
    return 1;
}

// ============================================================================
// Ext.StaticData.GetCount(type)
// ============================================================================

/**
 * Get the count of entries for a static data type.
 *
 * @param type Type name string
 * @return Count integer, or -1 if type not available
 */
static int lua_staticdata_getcount(lua_State *L) {
    const char* type_name = luaL_checkstring(L, 1);

    int type = staticdata_type_from_name(type_name);
    if (type < 0) {
        return luaL_error(L, "Unknown static data type: %s", type_name);
    }

    int count = staticdata_get_count((StaticDataType)type);
    lua_pushinteger(L, count);
    return 1;
}

// ============================================================================
// Ext.StaticData.GetTypes()
// ============================================================================

/**
 * Get list of supported static data type names.
 *
 * @return Array table of type name strings
 */
static int lua_staticdata_gettypes(lua_State *L) {
    lua_createtable(L, STATICDATA_COUNT, 0);

    for (int i = 0; i < STATICDATA_COUNT; i++) {
        lua_pushstring(L, staticdata_type_name((StaticDataType)i));
        lua_rawseti(L, -2, i + 1);
    }

    return 1;
}

// ============================================================================
// Ext.StaticData.IsReady(type)
// ============================================================================

/**
 * Check if a static data type is ready (manager captured).
 *
 * @param type Type name string (optional - if omitted, checks any)
 * @return boolean
 */
static int lua_staticdata_isready(lua_State *L) {
    if (lua_gettop(L) == 0) {
        // No argument - check if any manager is ready
        lua_pushboolean(L, staticdata_manager_ready());
        return 1;
    }

    const char* type_name = luaL_checkstring(L, 1);
    int type = staticdata_type_from_name(type_name);
    if (type < 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    lua_pushboolean(L, staticdata_has_manager((StaticDataType)type));
    return 1;
}

// ============================================================================
// Ext.StaticData.DumpStatus()
// ============================================================================

/**
 * Dump static data manager status to log (debug function).
 */
static int lua_staticdata_dumpstatus(lua_State *L) {
    (void)L;
    staticdata_dump_status();
    return 0;
}

// ============================================================================
// Ext.StaticData.DumpEntries(type, max)
// ============================================================================

/**
 * Dump entries of a type to log (debug function).
 *
 * @param type Type name string
 * @param max Maximum entries to dump (optional, default all)
 */
static int lua_staticdata_dumpentries(lua_State *L) {
    const char* type_name = luaL_checkstring(L, 1);
    int max = (int)luaL_optinteger(L, 2, -1);

    int type = staticdata_type_from_name(type_name);
    if (type < 0) {
        return luaL_error(L, "Unknown static data type: %s", type_name);
    }

    staticdata_dump_entries((StaticDataType)type, max);
    return 0;
}

// ============================================================================
// Ext.StaticData.Probe(type, range)
// ============================================================================

/**
 * Probe a manager for structure discovery (debug function).
 *
 * @param type Type name string
 * @param range Byte range to probe (optional, default 256)
 */
static int lua_staticdata_probe(lua_State *L) {
    const char* type_name = luaL_checkstring(L, 1);
    int range = (int)luaL_optinteger(L, 2, 256);

    int type = staticdata_type_from_name(type_name);
    if (type < 0) {
        return luaL_error(L, "Unknown static data type: %s", type_name);
    }

    staticdata_probe_manager((StaticDataType)type, range);
    return 0;
}

/**
 * Try to capture managers via TypeContext traversal (debug function).
 */
static int lua_staticdata_trytypecontext(lua_State *L) {
    (void)L;
    staticdata_try_typecontext_capture();
    return 0;
}

/**
 * Dump feat array memory for debugging structure layout (debug function).
 */
static int lua_staticdata_dumpfeatmemory(lua_State *L) {
    (void)L;
    staticdata_dump_feat_memory();
    return 0;
}

// ============================================================================
// Ext.StaticData.LoadFridaCapture([type])
// ============================================================================

/**
 * Load captured managers from Frida capture file.
 *
 * Workflow:
 * 1. In terminal: frida -U -n "Baldur's Gate 3" -l tools/frida/capture_featmanager_live.js
 * 2. In game: Open respec or level-up and click on feats
 * 3. In console: Ext.StaticData.LoadFridaCapture()  -- or LoadFridaCapture("Feat")
 * 4. Now GetAll("Feat") will return actual feat data
 *
 * @param type Optional type name string (defaults to "Feat")
 * @return boolean true if capture loaded successfully
 */
static int lua_staticdata_loadfridacapture(lua_State *L) {
    bool success;

    if (lua_gettop(L) == 0 || lua_isnil(L, 1)) {
        // No argument - load Feat (backwards compatible)
        success = staticdata_load_frida_capture();
    } else {
        // Type argument provided
        const char* type_name = luaL_checkstring(L, 1);
        int type = staticdata_type_from_name(type_name);
        if (type < 0) {
            return luaL_error(L, "Unknown static data type: %s", type_name);
        }
        success = staticdata_load_frida_capture_type((StaticDataType)type);
    }

    lua_pushboolean(L, success);
    return 1;
}

/**
 * Check if Frida capture is available.
 *
 * @param type Optional type name string (defaults to "Feat")
 * @return boolean true if capture file exists
 */
static int lua_staticdata_fridacaptureavailable(lua_State *L) {
    bool available;

    if (lua_gettop(L) == 0 || lua_isnil(L, 1)) {
        // No argument - check Feat (backwards compatible)
        available = staticdata_frida_capture_available();
    } else {
        // Type argument provided
        const char* type_name = luaL_checkstring(L, 1);
        int type = staticdata_type_from_name(type_name);
        if (type < 0) {
            lua_pushboolean(L, 0);
            return 1;
        }
        available = staticdata_frida_capture_available_type((StaticDataType)type);
    }

    lua_pushboolean(L, available);
    return 1;
}

/**
 * Get raw manager info for debugging.
 *
 * @param type Type name string
 * @return Table with {ptr, array_ptr, count, count_offset, array_offset, is_session}
 */
static int lua_staticdata_proberaw(lua_State *L) {
    const char* type_name = luaL_checkstring(L, 1);

    int type = staticdata_type_from_name(type_name);
    if (type < 0) {
        return luaL_error(L, "Unknown static data type: %s", type_name);
    }

    StaticDataRawInfo info;
    if (!staticdata_get_raw_info((StaticDataType)type, &info)) {
        lua_pushnil(L);
        return 1;
    }

    lua_newtable(L);

    lua_pushinteger(L, (lua_Integer)info.manager_ptr);
    lua_setfield(L, -2, "ptr");

    lua_pushinteger(L, (lua_Integer)info.array_ptr);
    lua_setfield(L, -2, "array_ptr");

    lua_pushinteger(L, info.count);
    lua_setfield(L, -2, "count");

    lua_pushinteger(L, info.count_offset);
    lua_setfield(L, -2, "count_offset");

    lua_pushinteger(L, info.array_offset);
    lua_setfield(L, -2, "array_offset");

    lua_pushboolean(L, info.is_session);
    lua_setfield(L, -2, "is_session");

    return 1;
}

/**
 * Manually trigger manager capture attempt.
 * Useful for debugging or if auto-capture at SessionLoaded didn't find managers.
 *
 * Uses TypeContext traversal + real manager probing + Frida capture fallback.
 *
 * @return number of managers captured
 */
static int lua_staticdata_triggercapture(lua_State *L) {
    int captured = staticdata_post_init_capture();
    lua_pushinteger(L, captured);
    return 1;
}

/**
 * Force capture managers by directly calling Get<T> functions.
 * Uses captured ImmutableDataHeadmaster (requires at least one manager captured first).
 *
 * This is useful for capturing Origin/Progression without entering character creation.
 * Also attempts hash lookup for types without Get<T> hooks (Race, God, FeatDescription).
 *
 * @return number of managers newly captured
 */
static int lua_staticdata_forcecapture(lua_State *L) {
    int captured = staticdata_force_capture();
    // Also try hash lookup for types without Get<T> hooks
    captured += staticdata_hash_lookup_capture();
    lua_pushinteger(L, captured);
    return 1;
}

/**
 * Force capture managers via hash lookup only.
 * For types without Get<T> hooks: Race, God, FeatDescription, Feat.
 *
 * @return number of managers newly captured via hash lookup
 */
static int lua_staticdata_hashlookup(lua_State *L) {
    int captured = staticdata_hash_lookup_capture();
    lua_pushinteger(L, captured);
    return 1;
}

// ============================================================================
// Registration
// ============================================================================

/*
 * Ext.StaticData.ProbeStride(type) -> measuredStride, recordsFound, configured
 *
 * Diagnostic for validating the per-type entry_size values, which were mostly
 * estimates. A wrong stride does not fail loudly -- enumeration still returns
 * entries, but ones landing off a record boundary expose unrelated bytes as a
 * ResourceUUID. Compare the measured stride against the configured one.
 */
static int lua_staticdata_probestride(lua_State* L) {
    int type = (int)luaL_checkinteger(L, 1);
    if (type < 0 || type >= STATICDATA_COUNT) {
        return luaL_error(L, "ProbeStride: type out of range");
    }
    int hits = 0;
    int stride = staticdata_probe_stride((StaticDataType)type, &hits);
    lua_pushinteger(L, stride);
    lua_pushinteger(L, hits);
    lua_pushinteger(L, staticdata_get_configured_entry_size((StaticDataType)type));
    return 3;
}

/* Format a raw 16-byte Guid the same way staticdata_get_guid_string does. */
static void push_guid_string(lua_State *L, const uint8_t *g) {
    char buf[40];
    uint32_t d1; uint16_t d2, d3;
    memcpy(&d1, g + 0, 4);
    memcpy(&d2, g + 4, 2);
    memcpy(&d3, g + 6, 2);
    snprintf(buf, sizeof(buf),
             "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             d1, d2, d3, g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    lua_pushstring(L, buf);
}

/*
 * Ext.StaticData.GetSources(type) -> { [modGuid] = { resourceGuid, ... } }
 *
 * Windows returns bank_->ResourceGuidsByMod directly
 * (Lua/Libs/StaticData.inl:120).
 */
static int lua_staticdata_getsources(lua_State *L) {
    const char *type_name = luaL_checkstring(L, 1);
    int type = staticdata_type_from_name(type_name);
    if (type < 0) {
        return luaL_error(L, "Unknown static data type: %s", type_name);
    }

    void *keys = NULL, *values = NULL;
    uint32_t count = 0;
    if (!staticdata_get_sources_shape((StaticDataType)type, &keys, &values, &count)) {
        lua_pushnil(L);
        lua_pushstring(L, "ResourceGuidsByMod unavailable for this type");
        return 2;
    }

    lua_createtable(L, 0, (int)count);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t modguid[16];
        if (!safe_memory_read((mach_vm_address_t)((uintptr_t)keys + i * 16),
                             modguid, 16)) {
            continue;
        }
        /* value is an Array<Guid> header: buf, capacity, size */
        void *buf = NULL; uint32_t n = 0;
        uintptr_t vh = (uintptr_t)values + (uintptr_t)i * 16;
        if (!safe_memory_read_pointer((mach_vm_address_t)vh, &buf) ||
            !safe_memory_read_u32((mach_vm_address_t)(vh + 0x0C), &n)) {
            continue;
        }
        if (n > 200000) n = 0;

        push_guid_string(L, modguid);
        lua_createtable(L, (int)n, 0);
        for (uint32_t j = 0; j < n && buf; j++) {
            uint8_t rg[16];
            if (!safe_memory_read(
                    (mach_vm_address_t)((uintptr_t)buf + (uintptr_t)j * 16), rg, 16)) {
                break;
            }
            push_guid_string(L, rg);
            lua_rawseti(L, -2, (int)(j + 1));
        }
        lua_rawset(L, -3);
    }
    return 1;
}

/*
 * Ext.StaticData.GetByModId(type, modGuid) -> { resourceGuid, ... } | nil
 * Windows does try_get on the same map (StaticData.inl:125).
 */
static int lua_staticdata_getbymodid(lua_State *L) {
    const char *type_name = luaL_checkstring(L, 1);
    const char *want = luaL_checkstring(L, 2);
    int type = staticdata_type_from_name(type_name);
    if (type < 0) {
        return luaL_error(L, "Unknown static data type: %s", type_name);
    }

    void *keys = NULL, *values = NULL;
    uint32_t count = 0;
    if (!staticdata_get_sources_shape((StaticDataType)type, &keys, &values, &count)) {
        lua_pushnil(L);
        return 1;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint8_t modguid[16];
        if (!safe_memory_read((mach_vm_address_t)((uintptr_t)keys + i * 16),
                             modguid, 16)) {
            continue;
        }
        push_guid_string(L, modguid);
        const char *have = lua_tostring(L, -1);
        int match = (have && strcasecmp(have, want) == 0);
        lua_pop(L, 1);
        if (!match) continue;

        void *buf = NULL; uint32_t n = 0;
        uintptr_t vh = (uintptr_t)values + (uintptr_t)i * 16;
        if (!safe_memory_read_pointer((mach_vm_address_t)vh, &buf) ||
            !safe_memory_read_u32((mach_vm_address_t)(vh + 0x0C), &n)) {
            break;
        }
        if (n > 200000) n = 0;
        lua_createtable(L, (int)n, 0);
        for (uint32_t j = 0; j < n && buf; j++) {
            uint8_t rg[16];
            if (!safe_memory_read(
                    (mach_vm_address_t)((uintptr_t)buf + (uintptr_t)j * 16), rg, 16)) {
                break;
            }
            push_guid_string(L, rg);
            lua_rawseti(L, -2, (int)(j + 1));
        }
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

/* Ext.StaticData.ProbeSourcesOffset(type, modGuidString) -> offset | -1 */
static int lua_staticdata_probesources(lua_State *L) {
    const char *type_name = luaL_checkstring(L, 1);
    const char *guid = luaL_checkstring(L, 2);
    int type = staticdata_type_from_name(type_name);
    if (type < 0) return luaL_error(L, "Unknown static data type: %s", type_name);

    unsigned int d1, d2, d3, b[8];
    if (sscanf(guid, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x",
               &d1,&d2,&d3,&b[0],&b[1],&b[2],&b[3],&b[4],&b[5],&b[6],&b[7]) != 11) {
        return luaL_error(L, "bad guid");
    }
    uint8_t g[16];
    uint32_t v1 = (uint32_t)d1; uint16_t v2 = (uint16_t)d2, v3 = (uint16_t)d3;
    memcpy(g + 0, &v1, 4); memcpy(g + 4, &v2, 2); memcpy(g + 6, &v3, 2);
    for (int i = 0; i < 8; i++) g[8 + i] = (uint8_t)b[i];

    lua_pushinteger(L, staticdata_probe_sources_offset((StaticDataType)type, g));
    return 1;
}

static const luaL_Reg staticdata_funcs[] = {
    {"GetAll", lua_staticdata_getall},
    {"Get", lua_staticdata_get},
    {"GetCount", lua_staticdata_getcount},
    {"GetTypes", lua_staticdata_gettypes},
    {"IsReady", lua_staticdata_isready},
    {"DumpStatus", lua_staticdata_dumpstatus},
    {"DumpEntries", lua_staticdata_dumpentries},
    {"Probe", lua_staticdata_probe},
    {"ProbeRaw", lua_staticdata_proberaw},
    {"TryTypeContext", lua_staticdata_trytypecontext},
    {"LoadFridaCapture", lua_staticdata_loadfridacapture},
    {"FridaCaptureAvailable", lua_staticdata_fridacaptureavailable},
    {"DumpFeatMemory", lua_staticdata_dumpfeatmemory},
    {"TriggerCapture", lua_staticdata_triggercapture},
    {"ForceCapture", lua_staticdata_forcecapture},
    {"HashLookup", lua_staticdata_hashlookup},
    {"ProbeStride", lua_staticdata_probestride},
    {"GetSources", lua_staticdata_getsources},
    {"GetByModId", lua_staticdata_getbymodid},
    {"ProbeSourcesOffset", lua_staticdata_probesources},
    {NULL, NULL}
};

void lua_staticdata_register(lua_State *L, int ext_table_idx) {
    // Convert to absolute index before pushing new values
    if (ext_table_idx < 0) {
        ext_table_idx = lua_gettop(L) + ext_table_idx + 1;
    }

    // Create Ext.StaticData table
    lua_newtable(L);

    // Register functions
    for (const luaL_Reg* func = staticdata_funcs; func->name; func++) {
        lua_pushcfunction(L, func->func);
        lua_setfield(L, -2, func->name);
    }

    // Set Ext.StaticData
    lua_setfield(L, ext_table_idx, "StaticData");

    log_message("[Lua] Registered Ext.StaticData API");
}
