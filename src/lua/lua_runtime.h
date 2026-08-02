/**
 * BG3SE-macOS - Lua Runtime Ownership (Wave 7 Phase 1, E2.0)
 *
 * Owner abstraction for the (eventually two) Lua VMs. Every raw lua_State*
 * cache in the codebase migrates onto a LuaRuntime so that context derives
 * from the owning VM instead of a mutable global, and registry references
 * can be validated against an owner + generation before use.
 *
 * Threading contract: register/unregister and the generation/alive fields
 * are gate-protected (lua_gate, or single-threaded init/shutdown). The L
 * field is _Atomic so non-gate threads (ServerWorker signal handlers,
 * CGEventTap, Dobby hook threads, logging callbacks) can pre-check liveness
 * before taking the gate — the same acquire/release pattern the migrated
 * per-module caches used. Pre-check reads are advisory only: any actual Lua
 * entry must re-resolve the state under the gate. Design + full state
 * inventory: docs/dual-vm/E2.0-state-ownership-audit.md.
 */

#ifndef LUA_RUNTIME_H
#define LUA_RUNTIME_H

#include <lua.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "lua_context.h"

typedef struct LuaRuntime {
    _Atomic(lua_State *) L; /* NULL when not alive */
    LuaContext context;     /* fixed identity of this runtime */
    uint32_t generation;    /* bumped on every unregister; refs carry a copy */
    bool alive;
} LuaRuntime;

/* Publish a state as the server or client runtime. ctx must be
 * LUA_CONTEXT_SERVER or LUA_CONTEXT_CLIENT; re-registering an alive runtime
 * is refused (unregister first). */
void lua_runtime_register(LuaContext ctx, lua_State *L);

/* Clear the runtime's state pointer and bump its generation, so any
 * reference tagged with the old generation is recognizably dead. */
void lua_runtime_unregister(LuaContext ctx);

LuaRuntime *lua_runtime_server(void);
LuaRuntime *lua_runtime_client(void);

/* Live state for a desired context: the exact-context runtime when alive,
 * else the sole alive runtime (single-VM fallback, reachable only until the
 * first client registration — e.g. when client VM allocation failed and the
 * process runs single-VM), else NULL. The fallback is compatibility-only:
 * the first client
 * registration latches dual-VM mode for the rest of the process, after which
 * a dead exact-context runtime resolves to NULL — never to the other VM,
 * whose registry would mis-resolve the caller's refs. Safe to call from any
 * thread as a liveness pre-check; Lua entry requires re-resolving under the
 * lua_gate. */
lua_State *lua_runtime_state_for(LuaContext ctx);

/* Resolve the runtime owning a calling lua_State. Coroutine states resolve
 * through their main thread. Returns NULL for unregistered states. */
LuaRuntime *lua_runtime_for_state(lua_State *L);

/* Context of the calling state's runtime; falls back to the legacy global
 * context (lua_context_get) while unmigrated states exist. */
LuaContext lua_runtime_context_of(lua_State *L);

#endif /* LUA_RUNTIME_H */
