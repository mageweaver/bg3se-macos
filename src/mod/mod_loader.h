/**
 * BG3SE-macOS - Mod Loader Module
 *
 * Detects enabled mods from modsettings.lsx and identifies
 * which mods have Script Extender support.
 */

#ifndef BG3SE_MOD_LOADER_H
#define BG3SE_MOD_LOADER_H

#include <lua.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Constants
// ============================================================================

#define MAX_MODS 128
#define MAX_MOD_NAME_LEN 256
#define MAX_PATH_LEN 1024

// ============================================================================
// Mod Detection API
// ============================================================================

/**
 * Parse modsettings.lsx and detect enabled mods.
 * Also identifies which mods have ScriptExtender support.
 */
void mod_detect_enabled(void);

/**
 * Get the number of detected mods.
 */
int mod_get_detected_count(void);

/**
 * Get the name of a detected mod by index.
 * @return Mod name, or NULL if index out of range
 */
const char *mod_get_detected_name(int index);

/**
 * Get the UUID of a detected mod by load-order index.
 * @return Mod UUID (empty string if unavailable), or NULL if out of range
 */
const char *mod_get_detected_uuid(int index);

/**
 * Get the number of SE mods (mods with Script Extender support).
 */
int mod_get_se_count(void);

/**
 * Get the name of an SE mod by index.
 * @return Mod name, or NULL if index out of range
 */
const char *mod_get_se_name(int index);

/**
 * Get an SE mod's resolved internal directory name by index — the <dir> in
 * Mods/<dir>/ScriptExtender/... paths. Differs from mod_get_se_name() when
 * the modsettings.lsx display name doesn't match the PAK's directory.
 * @return Directory name, or NULL if index out of range
 */
const char *mod_get_se_dir(int index);

/**
 * Get the UUID of a detected SE mod by index (empty string if unknown).
 */
const char *mod_get_se_uuid(int index);

/**
 * Hook invoked after a mod chunk is loaded (function on top of the Lua stack)
 * but before it is executed, so the loader can install the per-mod _ENV
 * (Mods.<ModTable>) on the chunk. See main.c mod_env_apply().
 */
typedef void (*mod_chunk_env_hook_t)(lua_State *L);
void mod_loader_set_chunk_env_hook(mod_chunk_env_hook_t hook);

// ============================================================================
// PAK File Helpers
// ============================================================================

/**
 * Check if a PAK file contains ScriptExtender/Config.json with "Lua" feature.
 */
int mod_pak_has_script_extender(const char *pak_path, const char *mod_name);

/**
 * Like mod_pak_has_script_extender, but resolves the mod's internal directory
 * name: tries Mods/<mod_name>/ first, then Mods/<pak filename stem>/.
 * @return 1 if found (dir_out written when non-NULL), 0 otherwise
 */
int mod_pak_find_se_dir(const char *pak_path, const char *mod_name,
                        char *dir_out, size_t dir_size);

/**
 * Read Mods/<dir_name>/ScriptExtender/Config.json out of the mod's PAK.
 * @return malloc'd NUL-terminated content (caller frees), or NULL
 */
char *mod_pak_get_config_json(const char *dir_name);

/**
 * Find the PAK file containing a mod in the Mods folder.
 * @return 1 if found and pak_path_out set, 0 if not found
 */
int mod_find_pak(const char *mod_name, char *pak_path_out, size_t pak_path_size);

/**
 * Load and execute a Lua file from a PAK archive.
 * @return 1 on success, 0 on failure
 */
int mod_load_lua_from_pak(lua_State *L, const char *pak_path, const char *lua_path);

// ============================================================================
// Current Mod State (for Ext.Require)
// ============================================================================

/**
 * Set the current mod context for Ext.Require.
 */
void mod_set_current(const char *mod_name, const char *lua_base_path, const char *pak_path);

/**
 * Get the current mod name.
 */
const char *mod_get_current_name(void);

/**
 * Get the current mod's Lua base path.
 */
const char *mod_get_current_lua_base(void);

/**
 * Get the current mod's PAK path.
 */
const char *mod_get_current_pak_path(void);

#ifdef __cplusplus
}
#endif

#endif // BG3SE_MOD_LOADER_H
