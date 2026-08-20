/**
 * lua_table.h - Ext.Table Lua bindings
 *
 * Windows reference: BG3Extender/Lua/Libs/Table.inl RegisterTableLib().
 */

#ifndef LUA_TABLE_H
#define LUA_TABLE_H

#include <lua.h>

// Register the Ext.Table namespace on the Ext table at ext_table_idx.
void lua_table_register(lua_State *L, int ext_table_idx);

#endif  // LUA_TABLE_H
