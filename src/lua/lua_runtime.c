/**
 * BG3SE-macOS - Lua Runtime Ownership Implementation
 *
 * See lua_runtime.h and docs/dual-vm/E2.0-state-ownership-audit.md.
 * Deliberately dependency-free (no logging) so tier0 links it standalone.
 */

#include "lua_runtime.h"

#include <stdio.h>

static LuaRuntime g_server_rt = { NULL, LUA_CONTEXT_SERVER, 0, false };
static LuaRuntime g_client_rt = { NULL, LUA_CONTEXT_CLIENT, 0, false };

/* Latched by the first client registration, never cleared. Once two VMs have
 * existed, cross-context fallback would resolve refs against the wrong VM's
 * registry (e.g. a client callback blocked on the gate during shutdown waking
 * to find the server VM), so state_for() becomes exact-context-only. */
static _Atomic(bool) g_dual_mode_begun = false;

static LuaRuntime *runtime_for_context(LuaContext ctx) {
    switch (ctx) {
        case LUA_CONTEXT_SERVER: return &g_server_rt;
        case LUA_CONTEXT_CLIENT: return &g_client_rt;
        default: return NULL;
    }
}

void lua_runtime_register(LuaContext ctx, lua_State *L) {
    LuaRuntime *rt = runtime_for_context(ctx);
    if (!rt || !L || rt->alive) {
        if (rt && rt->alive) {
            /* A refused re-registration is a lifecycle bug upstream — surface
             * it (stderr: this file stays logging-free for tier0 linking). */
            fprintf(stderr,
                    "[LuaRuntime] register(ctx=%d) refused: already alive "
                    "(existing=%p, attempted=%p)\n",
                    (int)ctx, (void *)rt->L, (void *)L);
        }
        return;
    }
    /* NOTE: the latch fires on any client registration, including a client
     * registered before any server — after which SERVER resolves NULL rather
     * than falling back. init_lua always registers server first. */
    if (rt == &g_client_rt) {
        atomic_store_explicit(&g_dual_mode_begun, true, memory_order_release);
    }
    /* alive before L: cross-thread pre-checks key off L alone, so the
     * runtime must be fully described before the pointer is published. */
    rt->alive = true;
    atomic_store_explicit(&rt->L, L, memory_order_release);
}

void lua_runtime_unregister(LuaContext ctx) {
    LuaRuntime *rt = runtime_for_context(ctx);
    if (!rt || !rt->alive) {
        return;
    }
    /* Generation first (design note 4: blocked waiters re-resolve to a dead
     * runtime), then retract the pointer so pre-checks fail immediately. */
    rt->generation++;
    atomic_store_explicit(&rt->L, NULL, memory_order_release);
    rt->alive = false;
}

LuaRuntime *lua_runtime_server(void) { return &g_server_rt; }
LuaRuntime *lua_runtime_client(void) { return &g_client_rt; }

lua_State *lua_runtime_state_for(LuaContext ctx) {
    LuaRuntime *want = (ctx == LUA_CONTEXT_CLIENT) ? &g_client_rt : &g_server_rt;
    LuaRuntime *other = (want == &g_client_rt) ? &g_server_rt : &g_client_rt;
    lua_State *L = atomic_load_explicit(&want->L, memory_order_acquire);
    if (L) {
        return L;
    }
    /* Single-VM fallback is compatibility-only: once a client runtime has
     * ever been registered, a dead exact-context runtime resolves to NULL. */
    if (atomic_load_explicit(&g_dual_mode_begun, memory_order_acquire)) {
        return NULL;
    }
    return atomic_load_explicit(&other->L, memory_order_acquire);
}

static lua_State *main_state_of(lua_State *L) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    lua_State *main = lua_tothread(L, -1);
    lua_pop(L, 1);
    return main ? main : L;
}

LuaRuntime *lua_runtime_for_state(lua_State *L) {
    if (!L) {
        return NULL;
    }
    lua_State *main = main_state_of(L);
    if (g_server_rt.alive && g_server_rt.L == main) {
        return &g_server_rt;
    }
    if (g_client_rt.alive && g_client_rt.L == main) {
        return &g_client_rt;
    }
    return NULL;
}

LuaContext lua_runtime_context_of(lua_State *L) {
    LuaRuntime *rt = lua_runtime_for_state(L);
    if (rt) {
        return rt->context;
    }
    return lua_context_get();
}
