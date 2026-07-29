#ifndef LUA_GATE_H
#define LUA_GATE_H

/*
 * lua_gate - cross-thread mutual exclusion for the shared lua_State.
 *
 * The Lua VM is single-threaded, but four threads enter it:
 *   - the game/ServerWorker thread (fake_Event: ticks, Osiris dispatch)
 *   - the GCD console poll timer (socket/file console commands)
 *   - the Metal render thread (Ext.IMGUI widget callbacks)
 *   - the input hook thread (hotkey callbacks)
 *
 * Any two of these running Lua concurrently corrupts VM internals
 * (observed: ServerWorker jumping through a NULL CallInfo pointer while
 * a console command ran Ext.Stats.GetAll — 2026-07-28 crash, PID 97193).
 *
 * The mutex is recursive: a locked section may synchronously trigger
 * another entry point on the same thread (e.g. an Osiris callback firing
 * a nested event dispatch).
 */

void lua_gate_lock(void);

/* Returns 1 and holds the gate on success, 0 if another thread holds it.
 * For periodic pollers that can just retry on their next tick. */
int lua_gate_trylock(void);

void lua_gate_unlock(void);

#endif // LUA_GATE_H
