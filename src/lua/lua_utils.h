/**
 * lua_utils.h - Ext.Utils Lua bindings (Windows parity surface)
 *
 * Registers the Windows Ext.Utils functions that do not require engine
 * reverse engineering. The caller builds the Ext.Utils table and leaves it on
 * top of the Lua stack; lua_utils_register() adds to it in place.
 *
 * Windows reference: BG3Extender/Lua/Libs/Utils.inl RegisterUtilsLib().
 */

#ifndef LUA_UTILS_H
#define LUA_UTILS_H

#include <lua.h>

/**
 * Add the ported Ext.Utils functions to the table at the top of the stack.
 * The stack is left unchanged (the Utils table remains on top).
 */
void lua_utils_register(lua_State *L);

#endif  // LUA_UTILS_H
