/**
 * lua_table.c - Ext.Table Lua bindings
 *
 * Windows reference: BG3Extender/Lua/Libs/Table.inl (Find -> FindValue) and
 * BG3Extender/Lua/Shared/LuaTable.inl (FindValueTable / FindValueArray).
 *
 * Ext.Table.Find(container, value) searches `container` for `value` using raw
 * equality and returns the KEY it was found under, or nil. Windows scans a
 * plain table's array part first (yielding 1-based integer indices) and then
 * its hash part (yielding the node key); array-like proxies are scanned by
 * index. This implementation preserves that ordering and both key shapes, and
 * additionally accepts the macOS component array proxies through their __len /
 * __index metamethods, which occupy the same role as the Windows array proxy.
 */

#include "lua_table.h"

#include "../core/logging.h"

#include <lauxlib.h>
#include <stdbool.h>

// Scan a proxy object (non-table with __len and __index) by index, mirroring
// FindValueArray: 1..len, raw-compare each element, return the 1-based index.
static int table_find_in_proxy(lua_State *L) {
    if (luaL_getmetafield(L, 1, "__len") == LUA_TNIL) {
        lua_pushnil(L);
        return 1;
    }
    lua_pop(L, 1);

    lua_Integer len = (lua_Integer)luaL_len(L, 1);
    for (lua_Integer i = 1; i <= len; i++) {
        lua_geti(L, 1, i);            // element
        bool eq = lua_rawequal(L, -1, 2);
        lua_pop(L, 1);
        if (eq) {
            lua_pushinteger(L, i);
            return 1;
        }
    }

    lua_pushnil(L);
    return 1;
}

// Ext.Table.Find(container, value) -> key or nil
static int lua_table_find(lua_State *L) {
    luaL_checkany(L, 1);
    luaL_checkany(L, 2);

    if (!lua_istable(L, 1)) {
        return table_find_in_proxy(L);
    }

    // Array part first, so a value present in both parts reports its integer
    // index -- this is the Windows ordering (FindValueTable checks sizearray
    // before the node array).
    lua_Integer arrayLen = (lua_Integer)lua_rawlen(L, 1);
    for (lua_Integer i = 1; i <= arrayLen; i++) {
        lua_rawgeti(L, 1, i);
        bool eq = lua_rawequal(L, -1, 2);
        lua_pop(L, 1);
        if (eq) {
            lua_pushinteger(L, i);
            return 1;
        }
    }

    // Hash part. Skip the integer keys 1..arrayLen already covered above so a
    // match is not reported twice with different key types.
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        // stack: key(-2) value(-1)
        if (lua_isinteger(L, -2)) {
            lua_Integer k = lua_tointeger(L, -2);
            if (k >= 1 && k <= arrayLen) {
                lua_pop(L, 1);
                continue;
            }
        }

        if (lua_rawequal(L, -1, 2)) {
            lua_pop(L, 1);   // drop value, leave key
            return 1;
        }
        lua_pop(L, 1);       // drop value, keep key for lua_next
    }

    lua_pushnil(L);
    return 1;
}

void lua_table_register(lua_State *L, int ext_table_idx) {
    int ext = lua_absindex(L, ext_table_idx);

    lua_newtable(L);
    lua_pushcfunction(L, lua_table_find);
    lua_setfield(L, -2, "Find");
    lua_setfield(L, ext, "Table");

    LOG_LUA_INFO("Ext.Table: registered 1 function");
}
