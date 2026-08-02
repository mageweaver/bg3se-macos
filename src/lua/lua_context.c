/**
 * BG3SE-macOS - Lua Context Management Implementation
 *
 * Manages the current Lua execution context (Server/Client).
 * Used for API partitioning and proper bootstrap file loading.
 */

#include "lua_context.h"
#include "lua_runtime.h"
#include "logging.h"

#include <lauxlib.h>

// ============================================================================
// Static State
// ============================================================================

static LuaContext g_current_context = LUA_CONTEXT_NONE;
static int g_initialized = 0;

// Context name strings
static const char* g_context_names[] = {
    "None",
    "Server",
    "Client"
};

// ============================================================================
// Context Management Implementation
// ============================================================================

void lua_context_init(void) {
    if (g_initialized) return;

    g_current_context = LUA_CONTEXT_NONE;
    g_initialized = 1;

    LOG_LUA_INFO("Lua context system initialized");
}

void lua_context_set(LuaContext ctx) {
    if (ctx < LUA_CONTEXT_NONE || ctx > LUA_CONTEXT_CLIENT) {
        LOG_LUA_ERROR("Invalid context value: %d", ctx);
        return;
    }

    LuaContext old_ctx = g_current_context;
    g_current_context = ctx;

    if (old_ctx != ctx) {
        LOG_LUA_INFO("Lua context changed: %s -> %s",
                    lua_context_get_name(old_ctx),
                    lua_context_get_name(ctx));
    }
}

LuaContext lua_context_get(void) {
    return g_current_context;
}

int lua_context_is_server(void) {
    return g_current_context == LUA_CONTEXT_SERVER;
}

int lua_context_is_client(void) {
    return g_current_context == LUA_CONTEXT_CLIENT;
}

const char* lua_context_get_name(LuaContext ctx) {
    if (ctx >= LUA_CONTEXT_NONE && ctx <= LUA_CONTEXT_CLIENT) {
        return g_context_names[ctx];
    }
    return "Unknown";
}

// ============================================================================
// Lua API Implementation
// ============================================================================

/**
 * Effective context for a calling state (E2.1/E2.2). A client-VM caller
 * reports CLIENT by fixed runtime identity. The server VM runs BOTH
 * bootstrap phases until E2.3 splits them, with g_current_context flipping
 * between phases — so for it (and for unregistered states) the legacy
 * phase global stays authoritative. "Both runtimes alive" is NOT the right
 * dual-VM signal here: E2.2's script-empty client VM registers while
 * bootstraps are still single-VM.
 */
static LuaContext effective_context(lua_State *L) {
    LuaRuntime *rt = lua_runtime_for_state(L);
    if (rt && rt->context == LUA_CONTEXT_CLIENT) {
        return LUA_CONTEXT_CLIENT;
    }
    return g_current_context;
}

int lua_ext_context_isserver(lua_State *L) {
    lua_pushboolean(L, effective_context(L) == LUA_CONTEXT_SERVER);
    return 1;
}

int lua_ext_context_isclient(lua_State *L) {
    lua_pushboolean(L, effective_context(L) == LUA_CONTEXT_CLIENT);
    return 1;
}

int lua_ext_context_getcontext(lua_State *L) {
    lua_pushstring(L, lua_context_get_name(effective_context(L)));
    return 1;
}
