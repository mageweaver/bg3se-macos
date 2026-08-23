/**
 * lua_ui.c - Lua Bindings for Ext.UI (Noesis Stub API)
 *
 * MCM uses Ext.UI for ESC menu button injection via Noesis.
 * macOS BG3 does not use Noesis, so these are graceful stubs.
 * MCM checks for nil returns and degrades to IMGUI-only mode.
 */

#include "lua_ui.h"
#include "../core/logging.h"

#include "../../lib/lua/src/lauxlib.h"

static int s_warned = 0;

static void warn_once(const char *func) {
    if (!s_warned) {
        log_message("[INFO] [Ext.UI] %s called — Noesis UI not available on macOS (MCM will use IMGUI fallback)", func);
        s_warned = 1;
    }
}

/**
 * Any method call on a Noesis stub widget: returns nil.
 */
static int lua_ui_stub_method(lua_State *L) {
    lua_pushnil(L);
    return 1;
}

/**
 * __index for the stub widget: every field resolves to a function returning
 * nil, so arbitrary Noesis chains stay callable.
 */
static int lua_ui_stub_index(lua_State *L) {
    lua_pushcfunction(L, lua_ui_stub_method);
    return 1;
}

/**
 * Ext.UI.GetRoot() -> inert widget stub (never nil)
 *
 * Returning nil here broke MCM: Noesis.lua does
 *     Ext.UI.GetRoot():Find("ContentRoot")
 * in one call with no nil check, so a nil root raised "attempt to index a nil
 * value" out of its KeyInput handler and took the keybinding path down with
 * it. MCM does check the RESULT of Find, so an object whose every method
 * returns nil degrades exactly the way MCM expects.
 */
static int lua_ui_get_root(lua_State *L) {
    warn_once("GetRoot");

    lua_newtable(L);
    lua_newtable(L);
    lua_pushcfunction(L, lua_ui_stub_index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    return 1;
}

/**
 * Ext.UI.RegisterType(name) -> nil (no-op)
 */
static int lua_ui_register_type(lua_State *L) {
    (void)luaL_checkstring(L, 1);
    warn_once("RegisterType");
    lua_pushnil(L);
    return 1;
}

/**
 * Ext.UI.Instantiate(name) -> nil
 */
static int lua_ui_instantiate(lua_State *L) {
    (void)luaL_checkstring(L, 1);
    warn_once("Instantiate");
    lua_pushnil(L);
    return 1;
}

/**
 * Ext.UI.IsReady() -> false
 */
static int lua_ui_is_ready(lua_State *L) {
    lua_pushboolean(L, 0);
    return 1;
}

/**
 * Ext.UI.SetValue(path, value) -> nil (no-op)
 */
static int lua_ui_set_value(lua_State *L) {
    (void)L;
    return 0;
}

/**
 * Ext.UI.GetValue(path) -> nil
 */
static int lua_ui_get_value(lua_State *L) {
    (void)L;
    lua_pushnil(L);
    return 1;
}

void lua_ext_register_ui(lua_State *L, int ext_table_idx) {
    // Normalize index
    if (ext_table_idx < 0) ext_table_idx = lua_gettop(L) + ext_table_idx + 1;

    // Create Ext.UI table
    lua_newtable(L);

    lua_pushcfunction(L, lua_ui_get_root);
    lua_setfield(L, -2, "GetRoot");

    lua_pushcfunction(L, lua_ui_register_type);
    lua_setfield(L, -2, "RegisterType");

    lua_pushcfunction(L, lua_ui_instantiate);
    lua_setfield(L, -2, "Instantiate");

    lua_pushcfunction(L, lua_ui_is_ready);
    lua_setfield(L, -2, "IsReady");

    lua_pushcfunction(L, lua_ui_set_value);
    lua_setfield(L, -2, "SetValue");

    lua_pushcfunction(L, lua_ui_get_value);
    lua_setfield(L, -2, "GetValue");

    // Set Ext.UI = table
    lua_setfield(L, ext_table_idx, "UI");

    LOG_LUA_INFO("Registered Ext.UI namespace (Noesis stubs for MCM compatibility)");
}
