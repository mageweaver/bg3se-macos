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
#include <mach-o/dyld.h>
#include "../resource/resource_manager.h"
#include "../core/safe_memory.h"

#define NOESIS_ELEMENT_MT "BG3SE.NoesisElement"

typedef struct {
    void *element;
} NoesisElementUD;

static int lua_ui_stub_method(lua_State *L);
static void push_element(lua_State *L, void *element);
static int lua_noesis_get_property(lua_State *L);

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

/**
 * element:GetProperty(name) -> value|nil
 *
 * Only Name is served. Noesis property reflection lives in TypeClass internals
 * that are not exported, and MCM -- the only consumer here -- asks for Name and
 * nothing else. Answering nil for the rest is what the previous stub did and
 * what callers already handle.
 */
static int lua_noesis_get_property(lua_State *L) {
    NoesisElementUD *ud = check_element(L, 1);
    const char *prop = luaL_checkstring(L, 2);

    if (strcmp(prop, "Name") == 0) {
        const char *nm = noesis_element_name(ud->element);
        if (nm) lua_pushstring(L, nm); else lua_pushnil(L);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

/** element:Child(index) -> logical child. Mods walk the logical tree, not the
 *  visual one: MCM iterates root.ChildrenCount / root:Child(i) to find a named
 *  widget, and the two trees are not the same shape. */
static int lua_noesis_child(lua_State *L) {
    NoesisElementUD *ud = check_element(L, 1);
    lua_Integer i = luaL_checkinteger(L, 2);
    if (i < 1) {
        lua_pushnil(L);
        return 1;
    }
    push_element(L, noesis_logical_child(ud->element, (unsigned int)(i - 1)));
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

    if (strcmp(key, "Name") == 0) {
        const char *nm = noesis_element_name(ud->element);
        if (nm) lua_pushstring(L, nm); else lua_pushnil(L);
        return 1;
    }
    if (strcmp(key, "GetProperty") == 0) {
        lua_pushcfunction(L, lua_noesis_get_property);
        return 1;
    }
    if (strcmp(key, "ChildrenCount") == 0) {
        lua_pushinteger(L, noesis_logical_child_count(ud->element));
        return 1;
    }
    if (strcmp(key, "Child") == 0) {
        lua_pushcfunction(L, lua_noesis_child);
        return 1;
    }
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


/*
 * There is deliberately no probe here that calls into Noesis.
 *
 * Finding a view root by scanning UIManager was tried and repeatedly took the
 * game down, including once with a validated, image-resident vtable. The
 * pointer check was not the problem: Noesis is a live UI framework being driven
 * by the game's own threads, and every one of these attempts called into it
 * from the console/timer thread. Validating an address makes the dereference
 * safe; it does nothing about touching a tree another thread is mutating.
 *
 * Whatever supplies a root eventually has to run on the UI thread -- a callback
 * the game already invokes there, not a hook and not the console.
 * noesis_register_root exists for that.
 */

/**
 * Ext.UI._FindNameOffset(element, expectedName) -> offset|nil
 *
 * One-time discovery: scan a FrameworkElement for a Symbol id that resolves to
 * a name we already know, and remember where it sat. SymbolManager::GetString
 * is a table lookup on an interned id, so a wrong candidate yields nothing
 * rather than following a pointer.
 */
static int lua_ui_find_name_offset(lua_State *L) {
    NoesisElementUD *ud = check_element(L, 1);
    const char *expected = luaL_checkstring(L, 2);

    for (int off = 0; off <= 0x400; off += 4) {
        uint32_t symbol = 0;
        if (!safe_memory_read_u32((mach_vm_address_t)((uint8_t *)ud->element + off), &symbol)) {
            continue;
        }
        if (symbol == 0 || symbol > 0x100000) continue;

        const char *text = noesis_symbol_string(symbol);
        if (text && strcmp(text, expected) == 0) {
            noesis_set_name_offset(off);
            lua_pushinteger(L, off);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

/**
 * Ext.UI._Symbol(id) -> string|nil -- resolve an interned Noesis Symbol.
 *
 * Lets the Name field be located without knowing a name up front: scan an
 * element's words, resolve each as a symbol, and the offset that yields
 * readable text across several elements is the one.
 */
static int lua_ui_symbol(lua_State *L) {
    lua_Integer id = luaL_checkinteger(L, 1);
    if (id <= 0 || id > 0x100000) {
        lua_pushnil(L);
        return 1;
    }
    const char *text = noesis_symbol_string((unsigned int)id);
    if (text) lua_pushstring(L, text); else lua_pushnil(L);
    return 1;
}

/** Ext.UI._ReadU32(address) -> integer|nil -- read-only word access. */
static int lua_ui_read_u32(lua_State *L) {
    lua_Integer addr = luaL_checkinteger(L, 1);
    uint32_t v = 0;
    if (!safe_memory_read_u32((mach_vm_address_t)(uintptr_t)addr, &v)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, v);
    return 1;
}

/** Ext.UI._ImageBase() -> integer. Read-only; useful for locating globals. */
static int lua_ui_image_base(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)(uintptr_t)_dyld_get_image_header(0));
    return 1;
}


/*
 * Ext.UI._ScanRM(depth) -- read-only survey of the ResourceManager.
 *
 * Norbyte reaches the UI root by reading, not hooking:
 *
 *     (*ls__gGlobalResourceManager)->UIManager->field_88.Canvas
 *
 * We already resolve that same singleton as ls::ResourceManager::m_ptr. The
 * offsets of UIManager within it, and of Canvas within UIManager, are what has
 * to be found -- and the field_XX names in upstream's headers describe a
 * different build, so they are a starting point rather than an answer.
 *
 * This only reads. Nothing here calls into Noesis: doing that from this thread
 * is what destabilised the game four times over, and a candidate is identified
 * by its vtable landing in the main image, which is a property of the bytes
 * rather than something that has to be asked of the framework.
 */
static int lua_ui_scan_rm(lua_State *L) {
    lua_Integer max_off = luaL_optinteger(L, 1, 0x600);

    void *rm = resource_manager_get();
    lua_newtable(L);
    if (!rm) return 1;

    /* Entry 0 carries the base so callers can search it directly. */
    lua_pushinteger(L, (lua_Integer)(uintptr_t)rm);
    lua_setfield(L, -2, "Base");

    uintptr_t lo = (uintptr_t)_dyld_get_image_header(0);
    int written = 0;

    for (lua_Integer off = 0; off <= max_off; off += 8) {
        void *member = NULL;
        if (!safe_memory_read_pointer((mach_vm_address_t)((uint8_t *)rm + off), &member)) continue;
        if (!member || (uintptr_t)member < 0x100000000ull) continue;

        void *vtable = NULL;
        if (!safe_memory_read_pointer((mach_vm_address_t)member, &vtable)) continue;
        if (!vtable || (uintptr_t)vtable < lo) continue;

        void *slot0 = NULL;
        if (!safe_memory_read_pointer((mach_vm_address_t)vtable, &slot0) || !slot0) continue;

        lua_newtable(L);
        lua_pushinteger(L, off);                              lua_setfield(L, -2, "Offset");
        lua_pushinteger(L, (lua_Integer)(uintptr_t)member);   lua_setfield(L, -2, "Ptr");
        lua_pushinteger(L, (lua_Integer)((uintptr_t)vtable - lo)); lua_setfield(L, -2, "VtableRva");
        lua_rawseti(L, -2, ++written);
    }
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

    lua_pushcfunction(L, lua_ui_image_base);
    lua_setfield(L, -2, "_ImageBase");
    lua_pushcfunction(L, lua_ui_find_name_offset);
    lua_setfield(L, -2, "_FindNameOffset");
    lua_pushcfunction(L, lua_ui_symbol);
    lua_setfield(L, -2, "_Symbol");
    lua_pushcfunction(L, lua_ui_read_u32);
    lua_setfield(L, -2, "_ReadU32");
    lua_pushcfunction(L, lua_ui_scan_rm);
    lua_setfield(L, -2, "_ScanRM");

    // Set Ext.UI = table
    lua_setfield(L, ext_table_idx, "UI");

    noesis_init();
    LOG_LUA_INFO("Registered Ext.UI namespace (Noesis bridge %s)",
                 noesis_ready() ? "live" : "waiting for a view");
}
