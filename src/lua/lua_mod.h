/**
 * BG3SE-macOS - Ext.Mod Lua Bindings
 *
 * Provides mod information and query functions.
 *
 * API:
 *   Ext.Mod.IsModLoaded(modGuid) -> boolean
 *   Ext.Mod.GetLoadOrder() -> table
 *   Ext.Mod.GetMod(modGuid) -> table|nil
 *   Ext.Mod.GetBaseMod() -> table
 *   Ext.Mod.GetModManager() -> table
 *
 * Issue #6: NetChannel API dependency
 */

#ifndef LUA_MOD_H
#define LUA_MOD_H

#include <lua.h>

/**
 * Register the Ext.Mod namespace.
 *
 * @param L               Lua state
 * @param ext_table_index Stack index of Ext table
 */
void lua_mod_register(lua_State *L, int ext_table_index);

/**
 * Parse modsettings.lsx into the UUID cache now, at startup.
 *
 * The game rewrites modsettings.lsx to the loaded save's mod list during a
 * session, so a lazy first parse can miss mods that were present at launch.
 * Priming at init pins GetLoadOrder/GetMod to the launch-time load order,
 * matching Windows BG3SE's in-memory ModManager semantics.
 */
void lua_mod_prime_uuid_cache(void);

/**
 * Check if a mod is loaded by UUID.
 *
 * @param mod_uuid The mod's UUID
 * @return true if mod is loaded
 */
bool lua_mod_is_loaded(const char *mod_uuid);

#endif /* LUA_MOD_H */
