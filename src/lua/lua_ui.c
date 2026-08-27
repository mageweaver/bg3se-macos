/**
 * lua_ui.c - Lua bindings for Ext.UI (Noesis)
 *
 * These were stubs, on the stated premise that "macOS BG3 does not use Noesis".
 * That premise was wrong: the macOS binary carries the whole Noesis framework,
 * with the visual-tree helpers exported. See src/ui/noesis.c for how the view
 * roots are obtained.
 *
 * What a caller gets back is a handle to a live Noesis element. Nothing is
 * copied and nothing is owned: the game creates and destroys these, so every
 * call re-validates the pointer before touching it and answers nil rather than
 * following a stale one.
 */

#include "lua_ui.h"
#include "../core/logging.h"
#include "../ui/noesis.h"

#include "../../lib/lua/src/lauxlib.h"

#include <string.h>

#define NOESIS_ELEMENT_MT "BG3SE.NoesisElement"

typedef struct {
    void *element;
} NoesisElementUD;

static int lua_ui_stub_method(lua_State *L);
static void push_element(lua_State *L, void *element);

static NoesisElementUD *check_element(lua_State *L, int index) {
    return (NoesisElementUD *)luaL_checkudata(L, index, NOESIS_ELEMENT_MT);
}

/** element:Find(name) -> element or nil */
static int lua_noesis_find(lua_State *L) {
    NoesisElementUD *ud = check_element(L, 1);
    const char *name = luaL_checkstring(L, 2);
    push_element(L, noesis_find_name(ud->element, name));
    return 1;
}

/** element:VisualChild(index) -> element or nil. One-based, as mods index it. */
static int lua_noesis_visual_child(lua_State *L) {
    NoesisElementUD *ud = check_element(L, 1);
    lua_Integer i = luaL_checkinteger(L, 2);
    if (i < 1) {
        lua_pushnil(L);
        return 1;
    }
    push_element(L, noesis_get_child(ud->element, (unsigned int)(i - 1)));
    return 1;
}

/** element:GetRoot() -> the root above this element */
static int lua_noesis_element_root(lua_State *L) {
    NoesisElementUD *ud = check_element(L, 1);
    push_element(L, noesis_root_of(ud->element));
    return 1;
}

static int lua_noesis_index(lua_State *L) {
    NoesisElementUD *ud = check_element(L, 1);
    const char *key = luaL_checkstring(L, 2);

    if (strcmp(key, "VisualChildrenCount") == 0) {
        lua_pushinteger(L, noesis_child_count(ud->element));
        return 1;
    }
    if (strcmp(key, "Find") == 0) {
        lua_pushcfunction(L, lua_noesis_find);
        return 1;
    }
    if (strcmp(key, "VisualChild") == 0) {
        lua_pushcfunction(L, lua_noesis_visual_child);
        return 1;
    }
    if (strcmp(key, "GetRoot") == 0) {
        lua_pushcfunction(L, lua_noesis_element_root);
        return 1;
    }
    if (strcmp(key, "_ptr") == 0) {
        lua_pushlightuserdata(L, ud->element);
        return 1;
    }

    /*
     * Anything else answers with a function returning nil, which is what the
     * previous stub did for everything. Mods walk long Noesis chains and check
     * the result, not each step, so an unknown step has to stay callable.
     */
    lua_pushcfunction(L, lua_ui_stub_method);
    return 1;
}

static int lua_noesis_tostring(lua_State *L) {
    NoesisElementUD *ud = check_element(L, 1);
    lua_pushfstring(L, "NoesisElement(%p)", ud->element);
    return 1;
}

static void push_element(lua_State *L, void *element) {
    if (!element) {
        lua_pushnil(L);
        return;
    }

    NoesisElementUD *ud = (NoesisElementUD *)lua_newuserdatauv(L, sizeof(NoesisElementUD), 0);
    ud->element = element;

    if (luaL_newmetatable(L, NOESIS_ELEMENT_MT)) {
        lua_pushcfunction(L, lua_noesis_index);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_noesis_tostring);
        lua_setfield(L, -2, "__tostring");
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "__metatable");
    }
    lua_setmetatable(L, -2);
}

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
 * Ext.UI.GetRoot() -> the most recent Noesis view root
 *
 * Never nil, even before a view exists. MCM writes
 *
 *     Ext.UI.GetRoot():Find("ContentRoot")
 *
 * in one expression with no nil check, so a nil root raised out of its KeyInput
 * handler and took the whole keybinding path down. Before any view has been
 * created this still returns the inert stub, whose every method answers nil --
 * which MCM does check for.
 */
static int lua_ui_get_root(lua_State *L) {
    noesis_init();

    void *root = noesis_get_root();
    if (root) {
        push_element(L, root);
        return 1;
    }

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
    noesis_init();
    lua_pushboolean(L, noesis_ready() ? 1 : 0);
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

    noesis_init();
    LOG_LUA_INFO("Registered Ext.UI namespace (Noesis bridge %s)",
                 noesis_ready() ? "live" : "waiting for a view");
}
