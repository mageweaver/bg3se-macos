/**
 * BG3SE-macOS - User Variables System Implementation
 *
 * Entity-attached custom variables with persistence.
 * Storage: ~/Library/Application Support/BG3SE/uservars.json
 */

#include "user_variables.h"
#include "campaign_key.h"
#include "../lua/lua_json.h"
#include "../core/logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#include <lauxlib.h>
#include <lualib.h>

// ============================================================================
// Static State
// ============================================================================

// User variables (entity-attached)
static UserVariablePrototype g_Prototypes[UVAR_MAX_PROTOTYPES];
static int g_PrototypeCount = 0;

static EntityVariables g_Entities[UVAR_MAX_ENTITIES];
static int g_EntityCount = 0;

// Mod variables (global per-mod)
static ModVariables g_Mods[UVAR_MAX_MODS];
static int g_ModCount = 0;
static bool g_ModsDirty = false;

static bool g_Initialized = false;
static bool g_Dirty = false;

// File paths for persistence
static char g_PersistPath[PATH_MAX] = {0};
static char g_ModPersistPath[PATH_MAX] = {0};

// ============================================================================
// Helper: Get persistence file path
// ============================================================================

/* Persisted variables are per-playthrough (campaign_key.h). While no campaign
 * is loaded the legacy machine-wide file is still used, so anything written at
 * the main menu is not lost; once a campaign is known the per-campaign file
 * takes over. The cached paths are rebuilt whenever the key changes. */
static char g_PathsBuiltForKey[CAMPAIGN_KEY_MAX] = {0};

static void persist_paths_invalidate_if_campaign_changed(void) {
    const char *key = campaign_key_get();
    const char *current = key ? key : "";
    if (strncmp(g_PathsBuiltForKey, current, sizeof(g_PathsBuiltForKey) - 1) == 0) {
        return;
    }
    strncpy(g_PathsBuiltForKey, current, sizeof(g_PathsBuiltForKey) - 1);
    g_PathsBuiltForKey[sizeof(g_PathsBuiltForKey) - 1] = '\0';
    g_PersistPath[0] = '\0';
    g_ModPersistPath[0] = '\0';
}

/* Build "<BG3SE>/<dir>/<campaign>.json", or the legacy "<BG3SE>/<legacy>" when
 * no campaign is loaded. Creates the per-campaign directory on demand. */
static void build_persist_path(char *out, size_t out_size,
                               const char *dir, const char *legacy_name) {
    const char *home = getenv("HOME");
    if (!home) return;

    const char *key = campaign_key_get();
    if (!key) {
        snprintf(out, out_size, "%s/Library/Application Support/BG3SE/%s",
                 home, legacy_name);
        return;
    }

    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof(dir_path),
             "%s/Library/Application Support/BG3SE/%s", home, dir);
    mkdir(dir_path, 0755);  // EEXIST is fine

    snprintf(out, out_size, "%s/%s.json", dir_path, key);
}

/* A campaign's store starts empty, as a new save does upstream (mod and user
 * variables live in the savegame there). The machine-wide file that predates
 * per-campaign storage (2026-09-11) used to be copied into every campaign seen
 * for the first time; that also seeded brand-new games with another
 * playthrough's state (ISF's "already shipped" marks, so a new game never got
 * its items or camp-chest mailbox). The legacy file is now only the store for
 * the main menu, before any campaign is known. */
static const char* get_persist_path(void) {
    persist_paths_invalidate_if_campaign_changed();
    if (g_PersistPath[0] == '\0') {
        build_persist_path(g_PersistPath, sizeof(g_PersistPath),
                           "uservars", "uservars.json");
    }
    return g_PersistPath;
}

static const char* get_mod_persist_path(void) {
    persist_paths_invalidate_if_campaign_changed();
    if (g_ModPersistPath[0] == '\0') {
        build_persist_path(g_ModPersistPath, sizeof(g_ModPersistPath),
                           "modvars", "modvars.json");
    }
    return g_ModPersistPath;
}

// ============================================================================
// Helper: Free a UserVariable's allocated memory
// ============================================================================

static void free_variable(UserVariable *var) {
    if (var && (var->type == UVAR_TYPE_STRING || var->type == UVAR_TYPE_TABLE)) {
        if (var->value.string) {
            free(var->value.string);
            var->value.string = NULL;
        }
    }
    if (var) {
        var->type = UVAR_TYPE_NULL;
        var->dirty = false;
    }
}

// ============================================================================
// Helper: Find entity by GUID
// ============================================================================

static int find_entity_index(const char *guid) {
    for (int i = 0; i < g_EntityCount; i++) {
        if (strcmp(g_Entities[i].guid, guid) == 0) {
            return i;
        }
    }
    return -1;
}

// ============================================================================
// Helper: Find mod by UUID
// ============================================================================

static int find_mod_index(const char *uuid) {
    for (int i = 0; i < g_ModCount; i++) {
        if (strcmp(g_Mods[i].uuid, uuid) == 0) {
            return i;
        }
    }
    return -1;
}

// ============================================================================
// Helper: Find mod prototype by key
// ============================================================================

static int find_mod_prototype_index(ModVariables *mod, const char *key) {
    if (!mod || !mod->prototypes) return -1;
    for (int i = 0; i < mod->prototype_count; i++) {
        if (strcmp(mod->prototypes[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

// ============================================================================
// Initialization
// ============================================================================

void uvar_init(void) {
    if (g_Initialized) return;

    memset(g_Prototypes, 0, sizeof(g_Prototypes));
    memset(g_Entities, 0, sizeof(g_Entities));
    memset(g_Mods, 0, sizeof(g_Mods));
    g_PrototypeCount = 0;
    g_EntityCount = 0;
    g_ModCount = 0;
    g_Dirty = false;
    g_ModsDirty = false;
    g_Initialized = true;

    LOG_LUA_INFO("User variables system initialized");
}

void uvar_shutdown(void) {
    if (!g_Initialized) return;

    // Free all entity variable storage
    for (int i = 0; i < g_EntityCount; i++) {
        if (g_Entities[i].vars) {
            for (int j = 0; j < g_Entities[i].var_count; j++) {
                free_variable(&g_Entities[i].vars[j]);
            }
            free(g_Entities[i].vars);
            g_Entities[i].vars = NULL;
        }
    }

    // Free all mod variable storage
    for (int i = 0; i < g_ModCount; i++) {
        if (g_Mods[i].vars) {
            for (int j = 0; j < g_Mods[i].var_count; j++) {
                free_variable(&g_Mods[i].vars[j]);
            }
            free(g_Mods[i].vars);
            g_Mods[i].vars = NULL;
        }
        if (g_Mods[i].prototypes) {
            free(g_Mods[i].prototypes);
            g_Mods[i].prototypes = NULL;
        }
    }

    g_PrototypeCount = 0;
    g_EntityCount = 0;
    g_ModCount = 0;
    g_Initialized = false;

    LOG_LUA_INFO("User variables system shutdown");
}

// ============================================================================
// Prototype Registration
// ============================================================================

int uvar_register_prototype(const char *key, uint32_t flags) {
    if (!g_Initialized) uvar_init();

    // Check if already registered
    for (int i = 0; i < g_PrototypeCount; i++) {
        if (strcmp(g_Prototypes[i].key, key) == 0) {
            // Update flags
            g_Prototypes[i].flags = flags;
            LOG_LUA_INFO("Updated user variable prototype: %s (flags=0x%x)", key, flags);
            return i;
        }
    }

    // Register new prototype
    if (g_PrototypeCount >= UVAR_MAX_PROTOTYPES) {
        LOG_LUA_ERROR("Max prototypes reached (%d)", UVAR_MAX_PROTOTYPES);
        return -1;
    }

    int idx = g_PrototypeCount++;
    strncpy(g_Prototypes[idx].key, key, UVAR_MAX_KEY_LENGTH - 1);
    g_Prototypes[idx].key[UVAR_MAX_KEY_LENGTH - 1] = '\0';
    g_Prototypes[idx].flags = flags;
    g_Prototypes[idx].registered = true;

    LOG_LUA_INFO("Registered user variable: %s (flags=0x%x)", key, flags);
    return idx;
}

const UserVariablePrototype* uvar_get_prototype(const char *key) {
    for (int i = 0; i < g_PrototypeCount; i++) {
        if (strcmp(g_Prototypes[i].key, key) == 0) {
            return &g_Prototypes[i];
        }
    }
    return NULL;
}

int uvar_get_prototype_index(const char *key) {
    for (int i = 0; i < g_PrototypeCount; i++) {
        if (strcmp(g_Prototypes[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

// ============================================================================
// Entity Variable Storage
// ============================================================================

EntityVariables* uvar_get_or_create_entity(const char *guid, uint64_t handle) {
    if (!g_Initialized) uvar_init();

    // Find existing
    int idx = find_entity_index(guid);
    if (idx >= 0) {
        // Update handle if changed
        g_Entities[idx].entity_handle = handle;
        return &g_Entities[idx];
    }

    // Create new
    if (g_EntityCount >= UVAR_MAX_ENTITIES) {
        LOG_LUA_ERROR("Max entities reached (%d)", UVAR_MAX_ENTITIES);
        return NULL;
    }

    idx = g_EntityCount++;
    strncpy(g_Entities[idx].guid, guid, UVAR_GUID_LENGTH - 1);
    g_Entities[idx].guid[UVAR_GUID_LENGTH - 1] = '\0';
    g_Entities[idx].entity_handle = handle;
    g_Entities[idx].vars = NULL;
    g_Entities[idx].var_count = 0;

    return &g_Entities[idx];
}

EntityVariables* uvar_get_entity(const char *guid) {
    int idx = find_entity_index(guid);
    return (idx >= 0) ? &g_Entities[idx] : NULL;
}

// ============================================================================
// Variable Get/Set
// ============================================================================

void uvar_set(lua_State *L, const char *guid, uint64_t handle,
              const char *key, int value_index) {
    // Get prototype
    int proto_idx = uvar_get_prototype_index(key);
    if (proto_idx < 0) {
        // Auto-register with default flags if not registered
        proto_idx = uvar_register_prototype(key,
            UVAR_FLAG_IS_ON_SERVER | UVAR_FLAG_WRITEABLE_SERVER |
            UVAR_FLAG_PERSISTENT | UVAR_FLAG_SYNC_ON_TICK);
        if (proto_idx < 0) {
            luaL_error(L, "Failed to register variable '%s'", key);
            return;
        }
    }

    // Get or create entity storage
    EntityVariables *ent = uvar_get_or_create_entity(guid, handle);
    if (!ent) {
        luaL_error(L, "Failed to create entity storage for %s", guid);
        return;
    }

    // Ensure vars array is large enough
    if (ent->vars == NULL || ent->var_count <= proto_idx) {
        int new_count = proto_idx + 1;
        UserVariable *new_vars = realloc(ent->vars, new_count * sizeof(UserVariable));
        if (!new_vars) {
            luaL_error(L, "Out of memory");
            return;
        }
        // Initialize new slots
        for (int i = ent->var_count; i < new_count; i++) {
            new_vars[i].type = UVAR_TYPE_NULL;
            new_vars[i].dirty = false;
            new_vars[i].value.string = NULL;
        }
        ent->vars = new_vars;
        ent->var_count = new_count;
    }

    // Free old value
    free_variable(&ent->vars[proto_idx]);

    // Convert Lua value to UserVariable
    UserVariable *var = &ent->vars[proto_idx];
    int vtype = lua_type(L, value_index);

    switch (vtype) {
        case LUA_TNIL:
            var->type = UVAR_TYPE_NULL;
            break;

        case LUA_TBOOLEAN:
            var->type = UVAR_TYPE_BOOLEAN;
            var->value.boolean = lua_toboolean(L, value_index);
            break;

        case LUA_TNUMBER:
            if (lua_isinteger(L, value_index)) {
                var->type = UVAR_TYPE_INTEGER;
                var->value.integer = lua_tointeger(L, value_index);
            } else {
                var->type = UVAR_TYPE_NUMBER;
                var->value.number = lua_tonumber(L, value_index);
            }
            break;

        case LUA_TSTRING: {
            var->type = UVAR_TYPE_STRING;
            size_t len;
            const char *str = lua_tolstring(L, value_index, &len);
            var->value.string = malloc(len + 1);
            if (var->value.string) {
                memcpy(var->value.string, str, len);
                var->value.string[len] = '\0';
            }
            break;
        }

        case LUA_TTABLE: {
            /* Value first, THEN buffinit -- the buffer box must stay on top of
             * the stack for the whole append or a grow corrupts it and stores
             * garbage. See the matching comment in mvar_set. */
            var->type = UVAR_TYPE_TABLE;

            lua_pushvalue(L, value_index);
            int value_abs = lua_gettop(L);

            luaL_Buffer b;
            luaL_buffinit(L, &b);
            json_stringify_value(L, value_abs, &b);
            luaL_pushresult(&b);

            size_t json_len = 0;
            const char *json = lua_tolstring(L, -1, &json_len);
            char *stored = malloc(json_len + 1);
            if (stored && json) {
                memcpy(stored, json, json_len);
                stored[json_len] = '\0';
                free(var->value.string);
                var->value.string = stored;
            } else {
                free(stored);
                LOG_LUA_ERROR("Failed to store JSON for entity variable %s (%zu bytes)",
                              key ? key : "?", json_len);
            }

            lua_pop(L, 1);            /* the result string */
            lua_remove(L, value_abs); /* the copy of the value */
            break;
        }

        default:
            LOG_LUA_WARN("Unsupported type for user variable: %s", lua_typename(L, vtype));
            var->type = UVAR_TYPE_NULL;
            break;
    }

    var->dirty = true;
    g_Dirty = true;

    LOG_LUA_DEBUG("Set %s.Vars.%s (type=%d)", guid, key, var->type);
}

int uvar_get(lua_State *L, const char *guid, const char *key) {
    // Get prototype
    int proto_idx = uvar_get_prototype_index(key);
    if (proto_idx < 0) {
        lua_pushnil(L);
        return 1;
    }

    // Get entity
    EntityVariables *ent = uvar_get_entity(guid);
    if (!ent || !ent->vars || ent->var_count <= proto_idx) {
        lua_pushnil(L);
        return 1;
    }

    // Get variable
    UserVariable *var = &ent->vars[proto_idx];

    switch (var->type) {
        case UVAR_TYPE_NULL:
            lua_pushnil(L);
            break;

        case UVAR_TYPE_BOOLEAN:
            lua_pushboolean(L, var->value.boolean);
            break;

        case UVAR_TYPE_INTEGER:
            lua_pushinteger(L, var->value.integer);
            break;

        case UVAR_TYPE_NUMBER:
            lua_pushnumber(L, var->value.number);
            break;

        case UVAR_TYPE_STRING:
            if (var->value.string) {
                lua_pushstring(L, var->value.string);
            } else {
                lua_pushnil(L);
            }
            break;

        case UVAR_TYPE_TABLE:
            if (var->value.string) {
                // Parse JSON back to table
                const char *end = json_parse_value(L, var->value.string);
                if (!end) {
                    LOG_LUA_ERROR("Failed to parse stored JSON for %s.%s", guid, key);
                    lua_pushnil(L);
                }
            } else {
                lua_pushnil(L);
            }
            break;

        default:
            lua_pushnil(L);
            break;
    }

    return 1;
}

void uvar_mark_dirty(const char *guid, const char *key) {
    int proto_idx = uvar_get_prototype_index(key);
    if (proto_idx < 0) return;

    EntityVariables *ent = uvar_get_entity(guid);
    if (!ent || !ent->vars || ent->var_count <= proto_idx) return;

    ent->vars[proto_idx].dirty = true;
    g_Dirty = true;
}

int uvar_get_entities_with_variable(lua_State *L, const char *key) {
    int proto_idx = uvar_get_prototype_index(key);
    if (proto_idx < 0) {
        lua_newtable(L);
        return 1;
    }

    lua_newtable(L);
    int table_idx = 1;

    for (int i = 0; i < g_EntityCount; i++) {
        EntityVariables *ent = &g_Entities[i];
        if (ent->vars && ent->var_count > proto_idx &&
            ent->vars[proto_idx].type != UVAR_TYPE_NULL) {
            lua_pushstring(L, ent->guid);
            lua_rawseti(L, -2, table_idx++);
        }
    }

    return 1;
}

// ============================================================================
// Persistence
// ============================================================================

void uvar_save_all(lua_State *L) {
    if (!g_Initialized || !g_Dirty) return;

    const char *path = get_persist_path();
    if (!path || path[0] == '\0') {
        LOG_LUA_ERROR("No persist path for user variables");
        return;
    }

    // Build JSON structure
    lua_newtable(L);  // Root table
    int root_idx = lua_gettop(L);

    // Save prototypes
    lua_newtable(L);
    for (int i = 0; i < g_PrototypeCount; i++) {
        if ((g_Prototypes[i].flags & UVAR_FLAG_PERSISTENT) == 0) continue;
        lua_pushinteger(L, g_Prototypes[i].flags);
        lua_setfield(L, -2, g_Prototypes[i].key);
    }
    lua_setfield(L, root_idx, "_prototypes");

    // Save entities
    lua_newtable(L);
    int entities_idx = lua_gettop(L);

    for (int i = 0; i < g_EntityCount; i++) {
        EntityVariables *ent = &g_Entities[i];
        if (!ent->vars) continue;

        lua_newtable(L);  // Entity vars table
        int ent_idx = lua_gettop(L);
        bool has_persistent = false;

        for (int j = 0; j < ent->var_count && j < g_PrototypeCount; j++) {
            if ((g_Prototypes[j].flags & UVAR_FLAG_PERSISTENT) == 0) continue;
            if (ent->vars[j].type == UVAR_TYPE_NULL) continue;

            has_persistent = true;
            uvar_get(L, ent->guid, g_Prototypes[j].key);
            lua_setfield(L, ent_idx, g_Prototypes[j].key);
        }

        if (has_persistent) {
            lua_setfield(L, entities_idx, ent->guid);
        } else {
            lua_pop(L, 1);  // Pop empty entity table
        }
    }
    lua_setfield(L, root_idx, "entities");

    // Stringify and write
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    json_stringify_value(L, root_idx, &b);
    luaL_pushresult(&b);

    size_t json_len;
    const char *json = lua_tolstring(L, -1, &json_len);

    // Atomic write
    char temp_path[PATH_MAX];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

    FILE *f = fopen(temp_path, "w");
    if (f) {
        fwrite(json, 1, json_len, f);
        fclose(f);
        if (rename(temp_path, path) == 0) {
            LOG_LUA_INFO("Saved user variables: %zu bytes", json_len);
            g_Dirty = false;
        } else {
            LOG_LUA_ERROR("Failed to rename temp file: %s", strerror(errno));
            unlink(temp_path);
        }
    } else {
        LOG_LUA_ERROR("Failed to create temp file: %s", strerror(errno));
    }

    lua_pop(L, 2);  // Pop JSON string and root table
}

void uvar_load_all(lua_State *L) {
    if (!g_Initialized) uvar_init();

    const char *path = get_persist_path();
    if (!path || path[0] == '\0') return;

    FILE *f = fopen(path, "r");
    if (!f) {
        LOG_LUA_INFO("No user variables file to load");
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 10 * 1024 * 1024) {
        fclose(f);
        return;
    }

    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        return;
    }

    size_t read_size = fread(json, 1, size, f);
    fclose(f);
    json[read_size] = '\0';

    // Parse JSON
    const char *end = json_parse_value(L, json);
    if (!end || !lua_istable(L, -1)) {
        LOG_LUA_ERROR("Failed to parse user variables JSON");
        free(json);
        if (lua_gettop(L) > 0) lua_pop(L, 1);
        return;
    }
    int root_idx = lua_gettop(L);

    // Load prototypes
    lua_getfield(L, root_idx, "_prototypes");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_isstring(L, -2) && lua_isinteger(L, -1)) {
                const char *key = lua_tostring(L, -2);
                uint32_t flags = (uint32_t)lua_tointeger(L, -1);
                uvar_register_prototype(key, flags);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);  // Pop _prototypes

    // Load entities
    lua_getfield(L, root_idx, "entities");
    if (lua_istable(L, -1)) {
        int entities_idx = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, entities_idx) != 0) {
            if (lua_isstring(L, -2) && lua_istable(L, -1)) {
                const char *guid = lua_tostring(L, -2);
                int ent_vars_idx = lua_gettop(L);

                // Iterate entity variables
                lua_pushnil(L);
                while (lua_next(L, ent_vars_idx) != 0) {
                    if (lua_isstring(L, -2)) {
                        const char *key = lua_tostring(L, -2);
                        uvar_set(L, guid, 0, key, lua_gettop(L));
                    }
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);  // Pop entities

    lua_pop(L, 1);  // Pop root table
    free(json);

    g_Dirty = false;
    LOG_LUA_INFO("Loaded user variables");
}

void uvar_flush(lua_State *L) {
    if (g_Dirty) {
        uvar_save_all(L);
    }
}

// ============================================================================
// Entity Vars Proxy Metatable
// ============================================================================

// Userdata for entity vars proxy
typedef struct {
    char guid[UVAR_GUID_LENGTH];
    uint64_t handle;
} EntityVarsProxy;

static int entity_vars_proxy_index(lua_State *L) {
    EntityVarsProxy *proxy = (EntityVarsProxy*)luaL_checkudata(L, 1, "BG3EntityVars");
    const char *key = luaL_checkstring(L, 2);

    return uvar_get(L, proxy->guid, key);
}

static int entity_vars_proxy_newindex(lua_State *L) {
    EntityVarsProxy *proxy = (EntityVarsProxy*)luaL_checkudata(L, 1, "BG3EntityVars");
    const char *key = luaL_checkstring(L, 2);
    // value is at index 3

    uvar_set(L, proxy->guid, proxy->handle, key, 3);
    return 0;
}

static int entity_vars_proxy_pairs(lua_State *L) {
    EntityVarsProxy *proxy = (EntityVarsProxy*)luaL_checkudata(L, 1, "BG3EntityVars");

    // Return all variables for this entity as an iterator
    lua_newtable(L);

    EntityVariables *ent = uvar_get_entity(proxy->guid);
    if (ent && ent->vars) {
        for (int i = 0; i < ent->var_count && i < g_PrototypeCount; i++) {
            if (ent->vars[i].type != UVAR_TYPE_NULL) {
                uvar_get(L, proxy->guid, g_Prototypes[i].key);
                lua_setfield(L, -2, g_Prototypes[i].key);
            }
        }
    }

    // Use pairs on the table we just built
    lua_getglobal(L, "pairs");
    lua_pushvalue(L, -2);
    lua_call(L, 1, 3);

    return 3;
}

static int entity_vars_proxy_tostring(lua_State *L) {
    EntityVarsProxy *proxy = (EntityVarsProxy*)luaL_checkudata(L, 1, "BG3EntityVars");
    lua_pushfstring(L, "EntityVars(%s)", proxy->guid);
    return 1;
}

void uvar_push_entity_proxy(lua_State *L, const char *guid, uint64_t handle) {
    // Create userdata
    EntityVarsProxy *proxy = (EntityVarsProxy*)lua_newuserdata(L, sizeof(EntityVarsProxy));
    strncpy(proxy->guid, guid, UVAR_GUID_LENGTH - 1);
    proxy->guid[UVAR_GUID_LENGTH - 1] = '\0';
    proxy->handle = handle;

    // Set metatable
    luaL_getmetatable(L, "BG3EntityVars");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        // Create metatable if not exists
        luaL_newmetatable(L, "BG3EntityVars");

        lua_pushcfunction(L, entity_vars_proxy_index);
        lua_setfield(L, -2, "__index");

        lua_pushcfunction(L, entity_vars_proxy_newindex);
        lua_setfield(L, -2, "__newindex");

        lua_pushcfunction(L, entity_vars_proxy_pairs);
        lua_setfield(L, -2, "__pairs");

        lua_pushcfunction(L, entity_vars_proxy_tostring);
        lua_setfield(L, -2, "__tostring");
    }
    lua_setmetatable(L, -2);
}

// ============================================================================
// Lua API: Ext.Vars.RegisterUserVariable(name, opts)
// ============================================================================

static int lua_register_user_variable(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    uint32_t flags = 0;

    // Parse options
    lua_getfield(L, 2, "Server");
    if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_IS_ON_SERVER;
        flags |= UVAR_FLAG_WRITEABLE_SERVER;  // Default writeable if on server
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "Client");
    if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_IS_ON_CLIENT;
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "WriteableOnServer");
    if (lua_isboolean(L, -1)) {
        if (lua_toboolean(L, -1)) {
            flags |= UVAR_FLAG_WRITEABLE_SERVER;
        } else {
            flags &= ~UVAR_FLAG_WRITEABLE_SERVER;
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "WriteableOnClient");
    if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_WRITEABLE_CLIENT;
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "Persistent");
    if (!lua_isboolean(L, -1) || lua_toboolean(L, -1)) {
        // Default true
        flags |= UVAR_FLAG_PERSISTENT;
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "SyncToClient");
    if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_SYNC_TO_CLIENT;
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "SyncToServer");
    if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_SYNC_TO_SERVER;
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "SyncOnWrite");
    if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_SYNC_ON_WRITE;
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "SyncOnTick");
    if (!lua_isboolean(L, -1) || lua_toboolean(L, -1)) {
        // Default true
        flags |= UVAR_FLAG_SYNC_ON_TICK;
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "DontCache");
    if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_DONT_CACHE;
    }
    lua_pop(L, 1);

    int idx = uvar_register_prototype(key, flags);
    lua_pushboolean(L, idx >= 0);
    return 1;
}

// ============================================================================
// Lua API: Ext.Vars.GetEntitiesWithVariable(varName)
// ============================================================================

static int lua_get_entities_with_variable(lua_State *L) {
    const char *key = luaL_checkstring(L, 1);
    return uvar_get_entities_with_variable(L, key);
}

// ============================================================================
// Lua API: Ext.Vars.SyncUserVariables()
// ============================================================================

static int lua_sync_user_variables(lua_State *L) {
    uvar_save_all(L);
    return 0;
}

// ============================================================================
// Lua API: Ext.Vars.DirtyUserVariables([entityGuid], [varName])
// ============================================================================

static int lua_dirty_user_variables(lua_State *L) {
    const char *guid = lua_isstring(L, 1) ? lua_tostring(L, 1) : NULL;
    const char *key = lua_isstring(L, 2) ? lua_tostring(L, 2) : NULL;

    if (!guid) {
        // Mark all dirty
        for (int i = 0; i < g_EntityCount; i++) {
            EntityVariables *ent = &g_Entities[i];
            if (ent->vars) {
                for (int j = 0; j < ent->var_count; j++) {
                    ent->vars[j].dirty = true;
                }
            }
        }
        g_Dirty = true;
    } else if (!key) {
        // Mark all vars for entity dirty
        EntityVariables *ent = uvar_get_entity(guid);
        if (ent && ent->vars) {
            for (int j = 0; j < ent->var_count; j++) {
                ent->vars[j].dirty = true;
            }
            g_Dirty = true;
        }
    } else {
        // Mark specific var dirty
        uvar_mark_dirty(guid, key);
    }

    return 0;
}

// ============================================================================
// Mod Variables Implementation
// ============================================================================

ModVariables* mvar_get_or_create_mod(const char *mod_uuid) {
    if (!g_Initialized) uvar_init();

    // Find existing
    int idx = find_mod_index(mod_uuid);
    if (idx >= 0) {
        return &g_Mods[idx];
    }

    // Create new
    if (g_ModCount >= UVAR_MAX_MODS) {
        // Fires once per attempt, which in a large profile means hundreds of
        // identical lines that say nothing about the consequence.
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_LUA_ERROR("mod variable storage full at %d mods; '%s' and any "
                          "later mod cannot register Ext.Vars and may fail to "
                          "initialise.", UVAR_MAX_MODS, mod_uuid);
        }
        return NULL;
    }

    idx = g_ModCount++;
    strncpy(g_Mods[idx].uuid, mod_uuid, UVAR_GUID_LENGTH - 1);
    g_Mods[idx].uuid[UVAR_GUID_LENGTH - 1] = '\0';
    g_Mods[idx].prototypes = NULL;
    g_Mods[idx].prototype_count = 0;
    g_Mods[idx].vars = NULL;
    g_Mods[idx].var_count = 0;
    g_Mods[idx].dirty = false;

    return &g_Mods[idx];
}

ModVariables* mvar_get_mod(const char *mod_uuid) {
    int idx = find_mod_index(mod_uuid);
    return (idx >= 0) ? &g_Mods[idx] : NULL;
}

int mvar_register_prototype(const char *mod_uuid, const char *key, uint32_t flags) {
    ModVariables *mod = mvar_get_or_create_mod(mod_uuid);
    if (!mod) return -1;

    // Check if already registered
    int existing = find_mod_prototype_index(mod, key);
    if (existing >= 0) {
        mod->prototypes[existing].flags = flags;
        LOG_LUA_INFO("Updated mod variable prototype: %s.%s (flags=0x%x)", mod_uuid, key, flags);
        return existing;
    }

    // Register new prototype
    int new_count = mod->prototype_count + 1;
    UserVariablePrototype *new_protos = realloc(mod->prototypes,
                                                 new_count * sizeof(UserVariablePrototype));
    if (!new_protos) {
        LOG_LUA_ERROR("Out of memory for mod prototype");
        return -1;
    }

    mod->prototypes = new_protos;
    int idx = mod->prototype_count++;
    strncpy(mod->prototypes[idx].key, key, UVAR_MAX_KEY_LENGTH - 1);
    mod->prototypes[idx].key[UVAR_MAX_KEY_LENGTH - 1] = '\0';
    mod->prototypes[idx].flags = flags;
    mod->prototypes[idx].registered = true;

    LOG_LUA_INFO("Registered mod variable: %s.%s (flags=0x%x)", mod_uuid, key, flags);
    return idx;
}

void mvar_set(lua_State *L, const char *mod_uuid, const char *key, int value_index) {
    ModVariables *mod = mvar_get_or_create_mod(mod_uuid);
    if (!mod) {
        luaL_error(L, "Failed to get/create mod storage for %s", mod_uuid);
        return;
    }

    /* The stored JSON is about to be replaced, so any cached parse of the old
     * value is stale. The next read re-parses and re-caches. */
    mvar_cache_invalidate(L, mod_uuid, key);

    // Get or create prototype
    int proto_idx = find_mod_prototype_index(mod, key);
    if (proto_idx < 0) {
        // Auto-register with default flags
        proto_idx = mvar_register_prototype(mod_uuid, key,
            UVAR_FLAG_IS_ON_SERVER | UVAR_FLAG_WRITEABLE_SERVER |
            UVAR_FLAG_PERSISTENT | UVAR_FLAG_SYNC_ON_TICK);
        if (proto_idx < 0) {
            luaL_error(L, "Failed to register mod variable '%s'", key);
            return;
        }
    }

    // Ensure vars array is large enough
    if (mod->vars == NULL || mod->var_count <= proto_idx) {
        int new_count = proto_idx + 1;
        UserVariable *new_vars = realloc(mod->vars, new_count * sizeof(UserVariable));
        if (!new_vars) {
            luaL_error(L, "Out of memory");
            return;
        }
        // Initialize new slots
        for (int i = mod->var_count; i < new_count; i++) {
            new_vars[i].type = UVAR_TYPE_NULL;
            new_vars[i].dirty = false;
            new_vars[i].value.string = NULL;
        }
        mod->vars = new_vars;
        mod->var_count = new_count;
    }

    // Free old value
    free_variable(&mod->vars[proto_idx]);

    // Convert Lua value to UserVariable
    UserVariable *var = &mod->vars[proto_idx];
    int vtype = lua_type(L, value_index);

    switch (vtype) {
        case LUA_TNIL:
            var->type = UVAR_TYPE_NULL;
            break;

        case LUA_TBOOLEAN:
            var->type = UVAR_TYPE_BOOLEAN;
            var->value.boolean = lua_toboolean(L, value_index);
            break;

        case LUA_TNUMBER:
            if (lua_isinteger(L, value_index)) {
                var->type = UVAR_TYPE_INTEGER;
                var->value.integer = lua_tointeger(L, value_index);
            } else {
                var->type = UVAR_TYPE_NUMBER;
                var->value.number = lua_tonumber(L, value_index);
            }
            break;

        case LUA_TSTRING: {
            var->type = UVAR_TYPE_STRING;
            size_t len;
            const char *str = lua_tolstring(L, value_index, &len);
            var->value.string = malloc(len + 1);
            if (var->value.string) {
                memcpy(var->value.string, str, len);
                var->value.string[len] = '\0';
            }
            break;
        }

        case LUA_TTABLE: {
            /* Serialize to JSON.
             *
             * ORDER MATTERS: luaL_buffinit pushes the buffer box, and in Lua
             * 5.4 that box must be on top of the stack whenever the buffer
             * grows. Pushing the value AFTER buffinit leaves the table sitting
             * above the box, so the first append large enough to force a
             * resize corrupts the stack and stores garbage -- an 11-byte
             * "P\x1a..." instead of the object. Small tables fit the inline
             * buffer and appeared to work, which is why this only bit mods
             * storing something sizeable (AppearanceEditEnhanced persists a
             * 164-key character template). Same trap as the PersistentVars
             * 4-byte-"null" bug. Push the value first, then init the buffer. */
            var->type = UVAR_TYPE_TABLE;

            lua_pushvalue(L, value_index);
            int value_abs = lua_gettop(L);

            luaL_Buffer b;
            luaL_buffinit(L, &b);
            json_stringify_value(L, value_abs, &b);
            luaL_pushresult(&b);

            size_t json_len = 0;
            const char *json = lua_tolstring(L, -1, &json_len);
            char *stored = malloc(json_len + 1);
            if (stored && json) {
                memcpy(stored, json, json_len);
                stored[json_len] = '\0';
                free(var->value.string);   /* replacing the previous payload */
                var->value.string = stored;
            } else {
                free(stored);
                LOG_LUA_ERROR("Failed to store JSON for mod %s.%s (%zu bytes)",
                              mod_uuid, key, json_len);
            }

            lua_pop(L, 1);          /* the result string */
            lua_remove(L, value_abs); /* the copy of the value */
            break;
        }

        default:
            LOG_LUA_WARN("Unsupported type for mod variable: %s", lua_typename(L, vtype));
            var->type = UVAR_TYPE_NULL;
            break;
    }

    var->dirty = true;
    mod->dirty = true;
    g_ModsDirty = true;

    LOG_LUA_DEBUG("Set mod %s.%s (type=%d)", mod_uuid, key, var->type);
}

/* Table-valued mod variables are stored as JSON. Re-parsing on every read
 * returns a FRESH table each time, which silently breaks the read-modify-write
 * pattern every mod uses:
 *
 *     ModVars.OriginalTemplates[char] = {}          -- mutates a throwaway
 *     ModVars.OriginalTemplates = ModVars.OriginalTemplates   -- re-reads an
 *                                                   -- empty copy, writes that
 *     ModVars.OriginalTemplates[char][k] = v        -- indexes nil -> ERROR
 *
 * That is AppearanceEditEnhanced Utils.lua:779-780, and the raised error aborts
 * its whole resculpt: the new appearance is never applied to the character AND
 * the throwaway character creation made is never removed, leaving the player
 * with two of the same companion. Upstream hands back a cached table per
 * variable, so the nested write survives until the mod writes it back.
 *
 * Cache: registry["BG3SE_ModVarCache"]["<uuid>|<key>"] -> the parsed table.
 * Invalidated whenever the variable is assigned, so the next read reflects the
 * newly stored JSON. */
#define MODVAR_CACHE_REGISTRY_KEY "BG3SE_ModVarCache"

static void mvar_cache_key(char *out, size_t outSize, const char *mod_uuid, const char *key) {
    snprintf(out, outSize, "%s|%s", mod_uuid ? mod_uuid : "?", key ? key : "?");
}

/* Push registry["BG3SE_ModVarCache"], creating it if needed. Leaves it on the stack. */
static void mvar_cache_push_table(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, MODVAR_CACHE_REGISTRY_KEY);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, MODVAR_CACHE_REGISTRY_KEY);
    }
}

/* Pushes the cached table and returns true, or pushes nothing and returns false. */
static bool mvar_cache_get(lua_State *L, const char *mod_uuid, const char *key) {
    char ck[UVAR_GUID_LENGTH + 96];
    mvar_cache_key(ck, sizeof(ck), mod_uuid, key);
    mvar_cache_push_table(L);
    lua_getfield(L, -1, ck);
    if (lua_istable(L, -1)) {
        lua_remove(L, -2);      /* drop the cache table, leave the value */
        return true;
    }
    lua_pop(L, 2);
    return false;
}

/* Caches the value at the top of the stack (left in place). */
static void mvar_cache_put(lua_State *L, const char *mod_uuid, const char *key) {
    if (!lua_istable(L, -1)) return;
    char ck[UVAR_GUID_LENGTH + 96];
    mvar_cache_key(ck, sizeof(ck), mod_uuid, key);
    mvar_cache_push_table(L);
    lua_pushvalue(L, -2);       /* the value */
    lua_setfield(L, -2, ck);
    lua_pop(L, 1);              /* drop the cache table */
}

void mvar_cache_invalidate(lua_State *L, const char *mod_uuid, const char *key) {
    if (!L) return;
    char ck[UVAR_GUID_LENGTH + 96];
    mvar_cache_key(ck, sizeof(ck), mod_uuid, key);
    mvar_cache_push_table(L);
    lua_pushnil(L);
    lua_setfield(L, -2, ck);
    lua_pop(L, 1);
}

void mvar_cache_clear(lua_State *L) {
    if (!L) return;
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, MODVAR_CACHE_REGISTRY_KEY);
}

int mvar_get(lua_State *L, const char *mod_uuid, const char *key) {
    ModVariables *mod = mvar_get_mod(mod_uuid);
    if (!mod) {
        lua_pushnil(L);
        return 1;
    }

    int proto_idx = find_mod_prototype_index(mod, key);
    if (proto_idx < 0 || !mod->vars || mod->var_count <= proto_idx) {
        lua_pushnil(L);
        return 1;
    }

    UserVariable *var = &mod->vars[proto_idx];

    switch (var->type) {
        case UVAR_TYPE_NULL:
            lua_pushnil(L);
            break;

        case UVAR_TYPE_BOOLEAN:
            lua_pushboolean(L, var->value.boolean);
            break;

        case UVAR_TYPE_INTEGER:
            lua_pushinteger(L, var->value.integer);
            break;

        case UVAR_TYPE_NUMBER:
            lua_pushnumber(L, var->value.number);
            break;

        case UVAR_TYPE_STRING:
            if (var->value.string) {
                lua_pushstring(L, var->value.string);
            } else {
                lua_pushnil(L);
            }
            break;

        case UVAR_TYPE_TABLE:
            if (var->value.string) {
                /* Same table for every read until the variable is assigned, so
                 * `vars.X.Y = v` survives long enough for the mod to write it
                 * back (see the cache comment above). */
                if (mvar_cache_get(L, mod_uuid, key)) {
                    break;
                }
                const char *end = json_parse_value(L, var->value.string);
                if (!end) {
                    /* Show the payload: a variable that will not round-trip
                     * silently reads back as nil, which looks to the mod like
                     * its state vanished. Without the text there is nothing to
                     * debug from. */
                    size_t jlen = strlen(var->value.string);
                    LOG_LUA_ERROR("Failed to parse stored JSON for mod %s.%s "
                                  "(%zu bytes): %.400s%s",
                                  mod_uuid, key, jlen, var->value.string,
                                  jlen > 400 ? " [...truncated]" : "");
                    lua_pushnil(L);
                } else {
                    mvar_cache_put(L, mod_uuid, key);
                }
            } else {
                lua_pushnil(L);
            }
            break;

        default:
            lua_pushnil(L);
            break;
    }

    return 1;
}

void mvar_mark_dirty(const char *mod_uuid, const char *key) {
    ModVariables *mod = mvar_get_mod(mod_uuid);
    if (!mod) return;

    if (key) {
        int proto_idx = find_mod_prototype_index(mod, key);
        if (proto_idx >= 0 && mod->vars && mod->var_count > proto_idx) {
            mod->vars[proto_idx].dirty = true;
        }
    }
    mod->dirty = true;
    g_ModsDirty = true;
}

void mvar_save_all(lua_State *L) {
    if (!g_Initialized || !g_ModsDirty) return;

    const char *path = get_mod_persist_path();
    if (!path || path[0] == '\0') {
        LOG_LUA_ERROR("No persist path for mod variables");
        return;
    }

    // Build JSON structure
    lua_newtable(L);
    int root_idx = lua_gettop(L);

    for (int i = 0; i < g_ModCount; i++) {
        ModVariables *mod = &g_Mods[i];
        if (!mod->vars || !mod->prototypes) continue;

        lua_newtable(L);
        int mod_idx = lua_gettop(L);
        bool has_persistent = false;

        for (int j = 0; j < mod->var_count && j < mod->prototype_count; j++) {
            if ((mod->prototypes[j].flags & UVAR_FLAG_PERSISTENT) == 0) continue;
            if (mod->vars[j].type == UVAR_TYPE_NULL) continue;

            has_persistent = true;
            mvar_get(L, mod->uuid, mod->prototypes[j].key);
            lua_setfield(L, mod_idx, mod->prototypes[j].key);
        }

        if (has_persistent) {
            lua_setfield(L, root_idx, mod->uuid);
        } else {
            lua_pop(L, 1);
        }
    }

    // Stringify and write
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    json_stringify_value(L, root_idx, &b);
    luaL_pushresult(&b);

    size_t json_len;
    const char *json = lua_tolstring(L, -1, &json_len);

    // Atomic write
    char temp_path[PATH_MAX];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);

    FILE *f = fopen(temp_path, "w");
    if (f) {
        fwrite(json, 1, json_len, f);
        fclose(f);
        if (rename(temp_path, path) == 0) {
            LOG_LUA_INFO("Saved mod variables: %zu bytes", json_len);
            g_ModsDirty = false;
            for (int i = 0; i < g_ModCount; i++) {
                g_Mods[i].dirty = false;
            }
        } else {
            LOG_LUA_ERROR("Failed to rename temp file: %s", strerror(errno));
            unlink(temp_path);
        }
    } else {
        LOG_LUA_ERROR("Failed to create temp file: %s", strerror(errno));
    }

    lua_pop(L, 2);  // Pop JSON string and root table
}

/* Switch persisted variables to a different playthrough.
 *
 * Values are dropped and reloaded from the new campaign's store; prototypes are
 * kept, because mods register those once during bootstrap and will not do it
 * again when a campaign changes. The caller must flush the previous campaign's
 * data BEFORE changing the key, or the pending write lands in the new
 * campaign's file. */
void vars_on_campaign_changed(lua_State *L) {
    if (!g_Initialized) uvar_init();

    for (int i = 0; i < g_ModCount; i++) {
        if (g_Mods[i].vars) {
            for (int j = 0; j < g_Mods[i].var_count; j++) {
                free_variable(&g_Mods[i].vars[j]);
            }
            free(g_Mods[i].vars);
            g_Mods[i].vars = NULL;
        }
        g_Mods[i].var_count = 0;
        g_Mods[i].dirty = false;
    }
    g_ModsDirty = false;

    for (int i = 0; i < g_EntityCount; i++) {
        if (g_Entities[i].vars) {
            for (int j = 0; j < g_Entities[i].var_count; j++) {
                free_variable(&g_Entities[i].vars[j]);
            }
            free(g_Entities[i].vars);
            g_Entities[i].vars = NULL;
        }
        g_Entities[i].var_count = 0;
    }
    g_EntityCount = 0;
    g_Dirty = false;

    mvar_cache_clear(L);

    uvar_load_all(L);
    mvar_load_all(L);
}

void mvar_load_all(lua_State *L) {
    if (!g_Initialized) uvar_init();

    const char *path = get_mod_persist_path();
    if (!path || path[0] == '\0') return;

    FILE *f = fopen(path, "r");
    if (!f) {
        LOG_LUA_INFO("No mod variables file to load");
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 10 * 1024 * 1024) {
        fclose(f);
        return;
    }

    char *json = malloc(size + 1);
    if (!json) {
        fclose(f);
        return;
    }

    size_t read_size = fread(json, 1, size, f);
    fclose(f);
    json[read_size] = '\0';

    // Parse JSON
    const char *end = json_parse_value(L, json);
    if (!end || !lua_istable(L, -1)) {
        LOG_LUA_ERROR("Failed to parse mod variables JSON");
        free(json);
        if (lua_gettop(L) > 0) lua_pop(L, 1);
        return;
    }
    int root_idx = lua_gettop(L);

    // Iterate mods
    lua_pushnil(L);
    while (lua_next(L, root_idx) != 0) {
        if (lua_isstring(L, -2) && lua_istable(L, -1)) {
            const char *mod_uuid = lua_tostring(L, -2);
            int mod_vars_idx = lua_gettop(L);

            // Iterate mod variables
            lua_pushnil(L);
            while (lua_next(L, mod_vars_idx) != 0) {
                if (lua_isstring(L, -2)) {
                    const char *key = lua_tostring(L, -2);
                    mvar_set(L, mod_uuid, key, lua_gettop(L));
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }

    lua_pop(L, 1);  // Pop root table
    free(json);

    g_ModsDirty = false;
    LOG_LUA_INFO("Loaded mod variables");
}

// ============================================================================
// Mod Variables Proxy Metatable
// ============================================================================

typedef struct {
    char uuid[UVAR_GUID_LENGTH];
} ModVarsProxy;

static int mod_vars_proxy_index(lua_State *L) {
    ModVarsProxy *proxy = (ModVarsProxy*)luaL_checkudata(L, 1, "BG3ModVars");
    const char *key = luaL_checkstring(L, 2);

    return mvar_get(L, proxy->uuid, key);
}

static int mod_vars_proxy_newindex(lua_State *L) {
    ModVarsProxy *proxy = (ModVarsProxy*)luaL_checkudata(L, 1, "BG3ModVars");
    const char *key = luaL_checkstring(L, 2);

    mvar_set(L, proxy->uuid, key, 3);
    return 0;
}

static int mod_vars_proxy_pairs(lua_State *L) {
    ModVarsProxy *proxy = (ModVarsProxy*)luaL_checkudata(L, 1, "BG3ModVars");

    lua_newtable(L);

    ModVariables *mod = mvar_get_mod(proxy->uuid);
    if (mod && mod->vars && mod->prototypes) {
        for (int i = 0; i < mod->var_count && i < mod->prototype_count; i++) {
            if (mod->vars[i].type != UVAR_TYPE_NULL) {
                mvar_get(L, proxy->uuid, mod->prototypes[i].key);
                lua_setfield(L, -2, mod->prototypes[i].key);
            }
        }
    }

    lua_getglobal(L, "pairs");
    lua_pushvalue(L, -2);
    lua_call(L, 1, 3);

    return 3;
}

static int mod_vars_proxy_tostring(lua_State *L) {
    ModVarsProxy *proxy = (ModVarsProxy*)luaL_checkudata(L, 1, "BG3ModVars");
    lua_pushfstring(L, "ModVars(%s)", proxy->uuid);
    return 1;
}

void mvar_push_mod_proxy(lua_State *L, const char *mod_uuid) {
    ModVarsProxy *proxy = (ModVarsProxy*)lua_newuserdata(L, sizeof(ModVarsProxy));
    strncpy(proxy->uuid, mod_uuid, UVAR_GUID_LENGTH - 1);
    proxy->uuid[UVAR_GUID_LENGTH - 1] = '\0';

    luaL_getmetatable(L, "BG3ModVars");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        luaL_newmetatable(L, "BG3ModVars");

        lua_pushcfunction(L, mod_vars_proxy_index);
        lua_setfield(L, -2, "__index");

        lua_pushcfunction(L, mod_vars_proxy_newindex);
        lua_setfield(L, -2, "__newindex");

        lua_pushcfunction(L, mod_vars_proxy_pairs);
        lua_setfield(L, -2, "__pairs");

        lua_pushcfunction(L, mod_vars_proxy_tostring);
        lua_setfield(L, -2, "__tostring");
    }
    lua_setmetatable(L, -2);
}

// ============================================================================
// Lua API: Ext.Vars.RegisterModVariable(modUuid, name, opts)
// ============================================================================

static int lua_register_mod_variable(lua_State *L) {
    const char *mod_uuid = luaL_checkstring(L, 1);
    const char *key = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);

    uint32_t flags = 0;

    // Parse options (same as user variables)
    lua_getfield(L, 3, "Server");
    if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_IS_ON_SERVER;
        flags |= UVAR_FLAG_WRITEABLE_SERVER;
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "Client");
    if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_IS_ON_CLIENT;
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "Persistent");
    if (!lua_isboolean(L, -1) || lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_PERSISTENT;
    }
    lua_pop(L, 1);

    lua_getfield(L, 3, "SyncOnTick");
    if (!lua_isboolean(L, -1) || lua_toboolean(L, -1)) {
        flags |= UVAR_FLAG_SYNC_ON_TICK;
    }
    lua_pop(L, 1);

    int idx = mvar_register_prototype(mod_uuid, key, flags);
    lua_pushboolean(L, idx >= 0);
    return 1;
}

// ============================================================================
// Lua API: Ext.Vars.GetModVariables(modUuid)
// ============================================================================

static int lua_get_mod_variables(lua_State *L) {
    const char *mod_uuid = luaL_checkstring(L, 1);

    // Ensure mod exists
    mvar_get_or_create_mod(mod_uuid);

    // Return proxy
    mvar_push_mod_proxy(L, mod_uuid);
    return 1;
}

// ============================================================================
// Lua API: Ext.Vars.SyncModVariables()
// ============================================================================

static int lua_sync_mod_variables(lua_State *L) {
    mvar_save_all(L);
    return 0;
}

// ============================================================================
// Lua API: Ext.Vars.DirtyModVariables([modUuid], [varName])
// ============================================================================

static int lua_dirty_mod_variables(lua_State *L) {
    const char *mod_uuid = lua_isstring(L, 1) ? lua_tostring(L, 1) : NULL;
    const char *key = lua_isstring(L, 2) ? lua_tostring(L, 2) : NULL;

    if (!mod_uuid) {
        // Mark all mods dirty
        for (int i = 0; i < g_ModCount; i++) {
            g_Mods[i].dirty = true;
            if (g_Mods[i].vars) {
                for (int j = 0; j < g_Mods[i].var_count; j++) {
                    g_Mods[i].vars[j].dirty = true;
                }
            }
        }
        g_ModsDirty = true;
    } else {
        mvar_mark_dirty(mod_uuid, key);
    }

    return 0;
}

// ============================================================================
// Registration
// ============================================================================

void uvar_register_lua(lua_State *L, int ext_vars_index) {
    // Convert negative index to absolute
    if (ext_vars_index < 0) {
        ext_vars_index = lua_gettop(L) + ext_vars_index + 1;
    }

    // Add functions to Ext.Vars table
    lua_pushcfunction(L, lua_register_user_variable);
    lua_setfield(L, ext_vars_index, "RegisterUserVariable");

    lua_pushcfunction(L, lua_get_entities_with_variable);
    lua_setfield(L, ext_vars_index, "GetEntitiesWithVariable");

    lua_pushcfunction(L, lua_sync_user_variables);
    lua_setfield(L, ext_vars_index, "SyncUserVariables");

    lua_pushcfunction(L, lua_dirty_user_variables);
    lua_setfield(L, ext_vars_index, "DirtyUserVariables");

    // Add mod variable functions
    lua_pushcfunction(L, lua_register_mod_variable);
    lua_setfield(L, ext_vars_index, "RegisterModVariable");

    lua_pushcfunction(L, lua_get_mod_variables);
    lua_setfield(L, ext_vars_index, "GetModVariables");

    lua_pushcfunction(L, lua_sync_mod_variables);
    lua_setfield(L, ext_vars_index, "SyncModVariables");

    lua_pushcfunction(L, lua_dirty_mod_variables);
    lua_setfield(L, ext_vars_index, "DirtyModVariables");

    // Create BG3EntityVars metatable
    luaL_newmetatable(L, "BG3EntityVars");

    lua_pushcfunction(L, entity_vars_proxy_index);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, entity_vars_proxy_newindex);
    lua_setfield(L, -2, "__newindex");

    lua_pushcfunction(L, entity_vars_proxy_pairs);
    lua_setfield(L, -2, "__pairs");

    lua_pushcfunction(L, entity_vars_proxy_tostring);
    lua_setfield(L, -2, "__tostring");

    lua_pop(L, 1);  // Pop BG3EntityVars metatable

    // Create BG3ModVars metatable
    luaL_newmetatable(L, "BG3ModVars");

    lua_pushcfunction(L, mod_vars_proxy_index);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, mod_vars_proxy_newindex);
    lua_setfield(L, -2, "__newindex");

    lua_pushcfunction(L, mod_vars_proxy_pairs);
    lua_setfield(L, -2, "__pairs");

    lua_pushcfunction(L, mod_vars_proxy_tostring);
    lua_setfield(L, -2, "__tostring");

    lua_pop(L, 1);  // Pop BG3ModVars metatable

    // Initialize system
    uvar_init();

    LOG_LUA_INFO("User and mod variables API registered");
}
