/**
 * BG3SE-macOS - User Variables System
 *
 * Provides entity-attached custom variables with persistence.
 * Based on Windows BG3SE Ext.Vars API.
 *
 * Usage:
 *   Ext.Vars.RegisterUserVariable("MyMod_Health", { Server = true, Persistent = true })
 *   entity.Vars.MyMod_Health = { current = 100, max = 150 }
 *   local data = entity.Vars.MyMod_Health
 */

#ifndef USER_VARIABLES_H
#define USER_VARIABLES_H

#include <lua.h>
#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// Constants
// ============================================================================

#define UVAR_MAX_PROTOTYPES 256
#define UVAR_MAX_KEY_LENGTH 128
#define UVAR_MAX_ENTITIES 4096
// One slot per mod that registers mod variables, across BOTH the server and
// client Lua states. 45 SE mods blow through 64 during the client bootstrap
// wave, and uvar_get_or_create then returns NULL to whichever mods come last -
// their Ext.Vars registration fails and their init aborts. Sized to match
// MAX_MODS in mod_loader.h; ModVariables is ~96 bytes, so ~0.1 MB.
#define UVAR_MAX_MODS 1024
#define UVAR_GUID_LENGTH 64

// ============================================================================
// Variable Flags (matches Windows BG3SE)
// ============================================================================

typedef enum {
    UVAR_FLAG_IS_ON_SERVER       = 1 << 0,
    UVAR_FLAG_IS_ON_CLIENT       = 1 << 1,
    UVAR_FLAG_SYNC_TO_CLIENT     = 1 << 2,
    UVAR_FLAG_SYNC_TO_SERVER     = 1 << 3,
    UVAR_FLAG_SYNC_ON_WRITE      = 1 << 4,
    UVAR_FLAG_DONT_CACHE         = 1 << 5,
    UVAR_FLAG_WRITEABLE_SERVER   = 1 << 6,
    UVAR_FLAG_WRITEABLE_CLIENT   = 1 << 7,
    UVAR_FLAG_SYNC_ON_TICK       = 1 << 8,
    UVAR_FLAG_PERSISTENT         = 1 << 9
} UserVariableFlags;

// ============================================================================
// Variable Types
// ============================================================================

typedef enum {
    UVAR_TYPE_NULL = 0,
    UVAR_TYPE_BOOLEAN,
    UVAR_TYPE_INTEGER,
    UVAR_TYPE_NUMBER,
    UVAR_TYPE_STRING,
    UVAR_TYPE_TABLE     // Stored as JSON string
} UserVariableType;

// ============================================================================
// Variable Prototype (registration info)
// ============================================================================

typedef struct {
    char key[UVAR_MAX_KEY_LENGTH];
    uint32_t flags;
    bool registered;
} UserVariablePrototype;

// ============================================================================
// Variable Value
// ============================================================================

typedef struct {
    UserVariableType type;
    bool dirty;
    union {
        bool boolean;
        int64_t integer;
        double number;
        char *string;       // Heap-allocated for strings and JSON tables
    } value;
} UserVariable;

// ============================================================================
// Entity Variables (all variables for one entity)
// ============================================================================

typedef struct {
    char guid[UVAR_GUID_LENGTH];
    uint64_t entity_handle;
    UserVariable *vars;             // Array of variables (indexed by prototype index)
    int var_count;
} EntityVariables;

// ============================================================================
// Mod Variables (global per-mod storage)
// ============================================================================

typedef struct {
    char uuid[UVAR_GUID_LENGTH];    // Module UUID
    UserVariablePrototype *prototypes;  // Mod-specific prototypes
    int prototype_count;
    UserVariable *vars;             // Array of variables
    int var_count;
    bool dirty;
} ModVariables;

// ============================================================================
// Public API
// ============================================================================

/**
 * Initialize the user variables system.
 * Must be called once during startup.
 */
void uvar_init(void);

/**
 * Shutdown and free all resources.
 */
void uvar_shutdown(void);

/**
 * Register a new user variable prototype.
 * Returns prototype index on success, -1 on failure.
 */
int uvar_register_prototype(const char *key, uint32_t flags);

/**
 * Get prototype by key name.
 * Returns NULL if not found.
 */
const UserVariablePrototype* uvar_get_prototype(const char *key);

/**
 * Get prototype index by key name.
 * Returns -1 if not found.
 */
int uvar_get_prototype_index(const char *key);

/**
 * Get or create entity variables storage for a GUID.
 */
EntityVariables* uvar_get_or_create_entity(const char *guid, uint64_t handle);

/**
 * Get entity variables storage for a GUID (read-only).
 * Returns NULL if entity has no variables.
 */
EntityVariables* uvar_get_entity(const char *guid);

/**
 * Set a variable value for an entity.
 * L is needed for table serialization.
 */
void uvar_set(lua_State *L, const char *guid, uint64_t handle,
              const char *key, int value_index);

/**
 * Get a variable value for an entity, push to Lua stack.
 * Returns number of values pushed (1 or 0).
 */
int uvar_get(lua_State *L, const char *guid, const char *key);

/**
 * Mark a variable as dirty (needs sync/save).
 */
void uvar_mark_dirty(const char *guid, const char *key);

/**
 * Get all entities that have a specific variable set.
 * Pushes a table of GUIDs to the Lua stack.
 */
int uvar_get_entities_with_variable(lua_State *L, const char *key);

/**
 * Save all persistent variables to JSON file.
 */
void uvar_save_all(lua_State *L);

/**
 * Load all persistent variables from JSON file.
 */
void uvar_load_all(lua_State *L);

/**
 * Flush dirty variables (called on tick).
 */
void uvar_flush(lua_State *L);

// ============================================================================
// Mod Variables API
// ============================================================================

/**
 * Register a mod variable prototype.
 * Returns prototype index on success, -1 on failure.
 */
int mvar_register_prototype(const char *mod_uuid, const char *key, uint32_t flags);

/**
 * Get or create mod variables storage for a module UUID.
 */
ModVariables* mvar_get_or_create_mod(const char *mod_uuid);

/**
 * Get mod variables storage (read-only).
 * Returns NULL if mod has no variables.
 */
ModVariables* mvar_get_mod(const char *mod_uuid);

/**
 * Set a mod variable value.
 */
void mvar_set(lua_State *L, const char *mod_uuid, const char *key, int value_index);

/**
 * Get a mod variable value, push to Lua stack.
 */
int mvar_get(lua_State *L, const char *mod_uuid, const char *key);

/**
 * Mark mod variables as dirty.
 */
void mvar_mark_dirty(const char *mod_uuid, const char *key);

/**
 * Save all mod variables.
 */
void mvar_save_all(lua_State *L);

/**
 * Load all mod variables.
 */
void mvar_load_all(lua_State *L);

/**
 * Push mod variables proxy for GetModVariables().
 */
void mvar_push_mod_proxy(lua_State *L, const char *mod_uuid);

// ============================================================================
// Lua Registration
// ============================================================================

/**
 * Register Ext.Vars functions (extends existing table).
 * ext_vars_index should be the Ext.Vars table on the stack.
 */
void uvar_register_lua(lua_State *L, int ext_vars_index);

/**
 * Create and push the Vars proxy metatable for an entity.
 * Used when accessing entity.Vars.
 */
void uvar_push_entity_proxy(lua_State *L, const char *guid, uint64_t handle);

#endif // USER_VARIABLES_H
