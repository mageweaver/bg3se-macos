/**
 * lua_resource.c - Lua bindings for Ext.Resource API
 *
 * Provides Lua access to game resources (Visual, Sound, Material, etc.)
 *
 * API:
 *   Ext.Resource.Get(resourceId, type) - Get a resource by ID and type
 *   Ext.Resource.GetAll(type) - Get all resources of a type
 *   Ext.Resource.GetTypes() - Get list of supported resource types
 *   Ext.Resource.IsReady() - Check if ResourceManager is available
 */

#include "lua_resource.h"
#include "../resource/resource_manager.h"
#include "../strings/fixed_string.h"
#include "../core/logging.h"
#include "../core/safe_memory.h"
#include <lua.h>
#include <lauxlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Push a resource as a Lua table.
 * Resources are opaque structures - we expose the ID and pointer.
 */
static void push_resource_entry(lua_State *L, void* resource, ResourceBankType type) {
    if (!resource) {
        lua_pushnil(L);
        return;
    }

    lua_newtable(L);

    // Add Type
    const char* type_name = resource_type_name(type);
    if (type_name) {
        lua_pushstring(L, type_name);
        lua_setfield(L, -2, "Type");
    }

    // Try to get resource ID from +0x08 (common pattern for FixedString ID)
    uint32_t id = 0;
    if (safe_memory_read_u32((mach_vm_address_t)resource + 0x08, &id) && id != 0) {
        const char* name = fixed_string_resolve(id);
        if (name) {
            lua_pushstring(L, name);
            lua_setfield(L, -2, "ResourceId");
        } else {
            // Push as hex if can't resolve
            char hex[20];
            snprintf(hex, sizeof(hex), "0x%08x", id);
            lua_pushstring(L, hex);
            lua_setfield(L, -2, "ResourceId");
        }

        lua_pushinteger(L, id);
        lua_setfield(L, -2, "_id");
    }

    // Add raw pointer for debugging
    lua_pushlightuserdata(L, resource);
    lua_setfield(L, -2, "_ptr");

    // Format pointer as hex string
    char ptr_str[32];
    snprintf(ptr_str, sizeof(ptr_str), "0x%llx", (unsigned long long)(uintptr_t)resource);
    lua_pushstring(L, ptr_str);
    lua_setfield(L, -2, "_ptrHex");
}

// ============================================================================
// Collect resources callback for GetAll
// ============================================================================

typedef struct {
    lua_State* L;
    int count;
    ResourceBankType type;
} CollectResourcesContext;

static bool collect_resources_callback(void* resource, ResourceBankType type, void* user_data) {
    CollectResourcesContext* ctx = (CollectResourcesContext*)user_data;

    push_resource_entry(ctx->L, resource, type);
    lua_rawseti(ctx->L, -2, ctx->count + 1);  // Lua arrays are 1-indexed
    ctx->count++;

    return true;  // Continue iteration
}

// ============================================================================
// Ext.Resource.Get(resourceId, type)
// ============================================================================

/**
 * Get a resource by ID and type.
 *
 * @param resourceId Resource identifier string
 * @param type Resource type name (e.g., "Visual", "Sound")
 * @return Resource table, or nil if not found
 */
static int lua_resource_get(lua_State *L) {
    const char* resource_id = luaL_checkstring(L, 1);
    const char* type_name = luaL_checkstring(L, 2);

    int type = resource_type_from_name(type_name);
    if (type < 0) {
        return luaL_error(L, "Unknown resource type: %s", type_name);
    }

    // Both misses below return nil, and a mod cannot tell them apart: BG3AF
    // reports "No AnimationSet found for: <uuid>" either way, which is what
    // BG3SX, WickedAnims and GrazztRing all fail on. Say which it was.
    if (!resource_manager_ready()) {
        LOG_LUA_DEBUG("Ext.Resource.Get('%s', '%s'): resource manager not ready",
                      resource_id, type_name);
        lua_pushnil(L);
        return 1;
    }

    void* resource = resource_get_by_name((ResourceBankType)type, resource_id);
    if (!resource) {
        LOG_LUA_DEBUG("Ext.Resource.Get('%s', '%s'): not present in bank %d",
                      resource_id, type_name, type);
        lua_pushnil(L);
        return 1;
    }

    push_resource_entry(L, resource, (ResourceBankType)type);
    return 1;
}

// ============================================================================
// Ext.Resource.GetAll(type)
// ============================================================================

/**
 * Get all resources of a type.
 *
 * @param type Resource type name
 * @return Array table of resource tables
 */
static int lua_resource_getall(lua_State *L) {
    const char* type_name = luaL_checkstring(L, 1);

    int type = resource_type_from_name(type_name);
    if (type < 0) {
        return luaL_error(L, "Unknown resource type: %s", type_name);
    }

    if (!resource_manager_ready()) {
        lua_newtable(L);
        return 1;
    }

    // Create result table
    lua_newtable(L);

    // Collect resources via iteration
    CollectResourcesContext ctx = {
        .L = L,
        .count = 0,
        .type = (ResourceBankType)type
    };

    resource_iterate_all((ResourceBankType)type, collect_resources_callback, &ctx);

    return 1;
}

// ============================================================================
// Ext.Resource.GetTypes()
// ============================================================================

/**
 * Get list of supported resource type names.
 *
 * @return Array table of type name strings
 */
static int lua_resource_gettypes(lua_State *L) {
    lua_createtable(L, RESOURCE_TYPE_COUNT, 0);

    for (int i = 0; i < RESOURCE_TYPE_COUNT; i++) {
        const char* name = resource_type_name((ResourceBankType)i);
        if (name) {
            lua_pushstring(L, name);
            lua_rawseti(L, -2, i + 1);
        }
    }

    return 1;
}

// ============================================================================
// Ext.Resource.GetCount(type)
// ============================================================================

/**
 * Get the count of resources for a type.
 *
 * @param type Resource type name
 * @return Count integer, or -1 if type not available
 */
static int lua_resource_getcount(lua_State *L) {
    const char* type_name = luaL_checkstring(L, 1);

    int type = resource_type_from_name(type_name);
    if (type < 0) {
        return luaL_error(L, "Unknown resource type: %s", type_name);
    }

    int count = resource_get_count((ResourceBankType)type);
    lua_pushinteger(L, count);
    return 1;
}

// ============================================================================
// Ext.Resource.IsReady()
// ============================================================================

/**
 * Check if the ResourceManager is ready.
 *
 * @return boolean
 */
static int lua_resource_isready(lua_State *L) {
    lua_pushboolean(L, resource_manager_ready());
    return 1;
}

// ============================================================================
// Registration
// ============================================================================

void lua_resource_register(lua_State *L, int ext_table_idx) {
    // Convert to absolute index before pushing new values
    if (ext_table_idx < 0) {
        ext_table_idx = lua_gettop(L) + ext_table_idx + 1;
    }

    // Create Ext.Resource table
    lua_newtable(L);

    // Register functions
    lua_pushcfunction(L, lua_resource_get);
    lua_setfield(L, -2, "Get");

    lua_pushcfunction(L, lua_resource_getall);
    lua_setfield(L, -2, "GetAll");

    lua_pushcfunction(L, lua_resource_gettypes);
    lua_setfield(L, -2, "GetTypes");

    lua_pushcfunction(L, lua_resource_getcount);
    lua_setfield(L, -2, "GetCount");

    lua_pushcfunction(L, lua_resource_isready);
    lua_setfield(L, -2, "IsReady");

    // Set Ext.Resource = table
    lua_setfield(L, ext_table_idx, "Resource");

    log_message("[Resource] Ext.Resource API registered");
}
