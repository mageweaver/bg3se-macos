/**
 * lua_input.c - Ext.Input Lua Bindings
 *
 * Provides Lua access to input injection and state queries.
 */

#include "input.h"
#include "../core/logging.h"
#include "../lua/lua_gate.h"
#include "../lua/lua_runtime.h"

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <string.h>

// ============================================================================
// Lua Hotkey Registry
// ============================================================================

#define MAX_LUA_HOTKEYS 32

typedef struct {
    int callback_ref;       // Lua registry reference
    int handle;             // C hotkey handle
    bool active;
} LuaHotkey;

static LuaHotkey s_lua_hotkeys[MAX_LUA_HOTKEYS];
static int s_lua_hotkey_count = 0;

/**
 * C callback wrapper that invokes the Lua callback.
 * Hotkeys are client-owned (E2.0 audit 1.3), but their refs are created by
 * mod code running in the bootstrap VM — resolve SERVER until E2.3 splits
 * bootstraps and Category-2 owner tags land.
 */
static void lua_hotkey_callback(void *userData) {
    int idx = (int)(intptr_t)userData;
    if (idx < 0 || idx >= MAX_LUA_HOTKEYS) return;
    if (!lua_runtime_state_for(LUA_CONTEXT_SERVER)) return;

    // May fire from the input hook thread - serialize Lua access (lua_gate.h).
    // Resolve state and slot only under the gate: shutdown unregisters the
    // runtime while holding it, so a waiter blocked here must re-resolve
    // instead of using a captured (possibly freed) state.
    lua_gate_lock();
    lua_State *L = lua_runtime_state_for(LUA_CONTEXT_SERVER);
    if (!L || !s_lua_hotkeys[idx].active) {
        lua_gate_unlock();
        return;
    }
    int ref = s_lua_hotkeys[idx].callback_ref;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (lua_isfunction(L, -1)) {
        if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            LOG_INPUT_ERROR("Hotkey callback error: %s", err ? err : "unknown");
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_gate_unlock();
}

// ============================================================================
// Helper: Parse key from string or number
// ============================================================================

static uint16_t parse_key_code(lua_State *L, int idx) {
    if (lua_type(L, idx) == LUA_TNUMBER) {
        return (uint16_t)lua_tointeger(L, idx);
    }

    if (lua_type(L, idx) == LUA_TSTRING) {
        const char *name = lua_tostring(L, idx);

        // Common key name mappings
        if (strcasecmp(name, "Escape") == 0 || strcasecmp(name, "Esc") == 0)
            return kVK_Escape;
        if (strcasecmp(name, "Return") == 0 || strcasecmp(name, "Enter") == 0)
            return kVK_Return;
        if (strcasecmp(name, "Tab") == 0)
            return kVK_Tab;
        if (strcasecmp(name, "Space") == 0)
            return kVK_Space;
        if (strcasecmp(name, "Backspace") == 0 || strcasecmp(name, "Delete") == 0)
            return kVK_Delete;
        if (strcasecmp(name, "Grave") == 0 || strcasecmp(name, "Backtick") == 0 ||
            strcmp(name, "`") == 0 || strcmp(name, "~") == 0)
            return kVK_ANSI_Grave;

        // Function keys
        if (strcasecmp(name, "F1") == 0) return kVK_F1;
        if (strcasecmp(name, "F2") == 0) return kVK_F2;
        if (strcasecmp(name, "F3") == 0) return kVK_F3;
        if (strcasecmp(name, "F4") == 0) return kVK_F4;
        if (strcasecmp(name, "F5") == 0) return kVK_F5;
        if (strcasecmp(name, "F6") == 0) return kVK_F6;
        if (strcasecmp(name, "F7") == 0) return kVK_F7;
        if (strcasecmp(name, "F8") == 0) return kVK_F8;
        if (strcasecmp(name, "F9") == 0) return kVK_F9;
        if (strcasecmp(name, "F10") == 0) return kVK_F10;
        if (strcasecmp(name, "F11") == 0) return kVK_F11;
        if (strcasecmp(name, "F12") == 0) return kVK_F12;

        // Arrow keys
        if (strcasecmp(name, "Left") == 0 || strcasecmp(name, "LeftArrow") == 0)
            return kVK_LeftArrow;
        if (strcasecmp(name, "Right") == 0 || strcasecmp(name, "RightArrow") == 0)
            return kVK_RightArrow;
        if (strcasecmp(name, "Up") == 0 || strcasecmp(name, "UpArrow") == 0)
            return kVK_UpArrow;
        if (strcasecmp(name, "Down") == 0 || strcasecmp(name, "DownArrow") == 0)
            return kVK_DownArrow;

        // Single character - try to map to key code
        if (strlen(name) == 1) {
            char c = name[0];
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';  // Uppercase

            switch (c) {
                case 'A': return kVK_ANSI_A;
                case 'B': return kVK_ANSI_B;
                case 'C': return kVK_ANSI_C;
                case 'D': return kVK_ANSI_D;
                case 'E': return kVK_ANSI_E;
                case 'F': return kVK_ANSI_F;
                case 'G': return kVK_ANSI_G;
                case 'H': return kVK_ANSI_H;
                case 'I': return kVK_ANSI_I;
                case 'J': return kVK_ANSI_J;
                case 'K': return kVK_ANSI_K;
                case 'L': return kVK_ANSI_L;
                case 'M': return kVK_ANSI_M;
                case 'N': return kVK_ANSI_N;
                case 'O': return kVK_ANSI_O;
                case 'P': return kVK_ANSI_P;
                case 'Q': return kVK_ANSI_Q;
                case 'R': return kVK_ANSI_R;
                case 'S': return kVK_ANSI_S;
                case 'T': return kVK_ANSI_T;
                case 'U': return kVK_ANSI_U;
                case 'V': return kVK_ANSI_V;
                case 'W': return kVK_ANSI_W;
                case 'X': return kVK_ANSI_X;
                case 'Y': return kVK_ANSI_Y;
                case 'Z': return kVK_ANSI_Z;
                case '0': return kVK_ANSI_0;
                case '1': return kVK_ANSI_1;
                case '2': return kVK_ANSI_2;
                case '3': return kVK_ANSI_3;
                case '4': return kVK_ANSI_4;
                case '5': return kVK_ANSI_5;
                case '6': return kVK_ANSI_6;
                case '7': return kVK_ANSI_7;
                case '8': return kVK_ANSI_8;
                case '9': return kVK_ANSI_9;
            }
        }

        LOG_INPUT_WARN("Unknown key name: %s", name);
    }

    return 0;
}

// ============================================================================
// Ext.Input.InjectKeyPress(key, [modifiers])
// ============================================================================

static int lua_input_inject_key_press(lua_State *L) {
    uint16_t keyCode = parse_key_code(L, 1);
    uint32_t modifiers = INPUT_MOD_NONE;

    if (lua_gettop(L) >= 2 && lua_istable(L, 2)) {
        // Parse modifiers table
        lua_getfield(L, 2, "Ctrl");
        if (lua_toboolean(L, -1)) modifiers |= INPUT_MOD_CTRL;
        lua_pop(L, 1);

        lua_getfield(L, 2, "Shift");
        if (lua_toboolean(L, -1)) modifiers |= INPUT_MOD_SHIFT;
        lua_pop(L, 1);

        lua_getfield(L, 2, "Alt");
        if (lua_toboolean(L, -1)) modifiers |= INPUT_MOD_ALT;
        lua_pop(L, 1);

        lua_getfield(L, 2, "Cmd");
        if (lua_toboolean(L, -1)) modifiers |= INPUT_MOD_CMD;
        lua_pop(L, 1);
    } else if (lua_gettop(L) >= 2 && lua_isnumber(L, 2)) {
        modifiers = (uint32_t)lua_tointeger(L, 2);
    }

    input_inject_key_press(keyCode, modifiers);
    return 0;
}

// ============================================================================
// Ext.Input.InjectKeyDown(key, [modifiers])
// ============================================================================

static int lua_input_inject_key_down(lua_State *L) {
    uint16_t keyCode = parse_key_code(L, 1);
    uint32_t modifiers = INPUT_MOD_NONE;

    if (lua_gettop(L) >= 2 && lua_isnumber(L, 2)) {
        modifiers = (uint32_t)lua_tointeger(L, 2);
    }

    input_inject_key_down(keyCode, modifiers);
    return 0;
}

// ============================================================================
// Ext.Input.InjectKeyUp(key, [modifiers])
// ============================================================================

static int lua_input_inject_key_up(lua_State *L) {
    uint16_t keyCode = parse_key_code(L, 1);
    uint32_t modifiers = INPUT_MOD_NONE;

    if (lua_gettop(L) >= 2 && lua_isnumber(L, 2)) {
        modifiers = (uint32_t)lua_tointeger(L, 2);
    }

    input_inject_key_up(keyCode, modifiers);
    return 0;
}

// ============================================================================
// Ext.Input.IsKeyPressed(key)
// ============================================================================

static int lua_input_is_key_pressed(lua_State *L) {
    uint16_t keyCode = parse_key_code(L, 1);
    lua_pushboolean(L, input_is_key_pressed(keyCode));
    return 1;
}

// ============================================================================
// Ext.Input.GetModifiers()
// ============================================================================

static int lua_input_get_modifiers(lua_State *L) {
    uint32_t mods = input_get_modifiers();

    // Return as table
    lua_newtable(L);

    lua_pushboolean(L, (mods & INPUT_MOD_SHIFT) != 0);
    lua_setfield(L, -2, "Shift");

    lua_pushboolean(L, (mods & INPUT_MOD_CTRL) != 0);
    lua_setfield(L, -2, "Ctrl");

    lua_pushboolean(L, (mods & INPUT_MOD_ALT) != 0);
    lua_setfield(L, -2, "Alt");

    lua_pushboolean(L, (mods & INPUT_MOD_CMD) != 0);
    lua_setfield(L, -2, "Cmd");

    lua_pushboolean(L, (mods & INPUT_MOD_CAPS) != 0);
    lua_setfield(L, -2, "CapsLock");

    lua_pushinteger(L, mods);
    lua_setfield(L, -2, "Raw");

    return 1;
}

// ============================================================================
// Ext.Input.IsInitialized()
// ============================================================================

static int lua_input_is_initialized(lua_State *L) {
    lua_pushboolean(L, input_is_initialized());
    return 1;
}

// ============================================================================
// Ext.Input.RegisterHotkey(key, modifiers, callback, [name])
// ============================================================================

static int lua_input_register_hotkey(lua_State *L) {
    uint16_t keyCode = parse_key_code(L, 1);
    uint32_t modifiers = (uint32_t)luaL_optinteger(L, 2, 0);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    const char *name = luaL_optstring(L, 4, NULL);

    if (s_lua_hotkey_count >= MAX_LUA_HOTKEYS) {
        return luaL_error(L, "Maximum hotkey limit reached (%d)", MAX_LUA_HOTKEYS);
    }

    // Find a free slot
    int idx = -1;
    for (int i = 0; i < MAX_LUA_HOTKEYS; i++) {
        if (!s_lua_hotkeys[i].active) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        return luaL_error(L, "No free hotkey slots");
    }

    // Store callback reference
    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // Register with C hotkey system
    int handle = input_register_hotkey(keyCode, modifiers,
                                       lua_hotkey_callback,
                                       (void *)(intptr_t)idx,
                                       name);

    if (handle == 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return luaL_error(L, "Failed to register hotkey");
    }

    // Store in our registry
    s_lua_hotkeys[idx].callback_ref = ref;
    s_lua_hotkeys[idx].handle = handle;
    s_lua_hotkeys[idx].active = true;
    s_lua_hotkey_count++;

    LOG_INPUT_INFO("Registered Lua hotkey: %s (key=%d, mods=0x%x) -> handle %d",
                   name ? name : "(unnamed)", keyCode, modifiers, handle);

    lua_pushinteger(L, handle);
    return 1;
}

// ============================================================================
// Ext.Input.UnregisterHotkey(handle)
// ============================================================================

static int lua_input_unregister_hotkey(lua_State *L) {
    int handle = (int)luaL_checkinteger(L, 1);

    // Find and remove
    for (int i = 0; i < MAX_LUA_HOTKEYS; i++) {
        if (s_lua_hotkeys[i].active && s_lua_hotkeys[i].handle == handle) {
            luaL_unref(L, LUA_REGISTRYINDEX, s_lua_hotkeys[i].callback_ref);
            s_lua_hotkeys[i].active = false;
            s_lua_hotkey_count--;

            input_unregister_hotkey(handle);

            lua_pushboolean(L, 1);
            return 1;
        }
    }

    lua_pushboolean(L, 0);
    return 1;
}

// ============================================================================
// Registration
// ============================================================================

void lua_input_register(lua_State *L, int ext_table_index) {
    // Create Ext.Input table
    lua_newtable(L);

    // Register functions
    lua_pushcfunction(L, lua_input_inject_key_press);
    lua_setfield(L, -2, "InjectKeyPress");

    lua_pushcfunction(L, lua_input_inject_key_down);
    lua_setfield(L, -2, "InjectKeyDown");

    lua_pushcfunction(L, lua_input_inject_key_up);
    lua_setfield(L, -2, "InjectKeyUp");

    lua_pushcfunction(L, lua_input_is_key_pressed);
    lua_setfield(L, -2, "IsKeyPressed");

    lua_pushcfunction(L, lua_input_get_modifiers);
    lua_setfield(L, -2, "GetModifiers");

    lua_pushcfunction(L, lua_input_is_initialized);
    lua_setfield(L, -2, "IsInitialized");

    lua_pushcfunction(L, lua_input_register_hotkey);
    lua_setfield(L, -2, "RegisterHotkey");

    lua_pushcfunction(L, lua_input_unregister_hotkey);
    lua_setfield(L, -2, "UnregisterHotkey");

    // Set Ext.Input
    lua_setfield(L, ext_table_index, "Input");

    // Also expose modifier constants
    lua_pushinteger(L, INPUT_MOD_NONE);
    lua_setfield(L, ext_table_index, "MOD_NONE");

    lua_pushinteger(L, INPUT_MOD_SHIFT);
    lua_setfield(L, ext_table_index, "MOD_SHIFT");

    lua_pushinteger(L, INPUT_MOD_CTRL);
    lua_setfield(L, ext_table_index, "MOD_CTRL");

    lua_pushinteger(L, INPUT_MOD_ALT);
    lua_setfield(L, ext_table_index, "MOD_ALT");

    lua_pushinteger(L, INPUT_MOD_CMD);
    lua_setfield(L, ext_table_index, "MOD_CMD");

    LOG_INPUT_INFO("Registered Ext.Input namespace");
}
