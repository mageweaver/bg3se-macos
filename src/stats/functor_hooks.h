/**
 * functor_hooks.h - Stats Functor Hook System
 *
 * Hooks for intercepting functor execution to fire Lua events.
 * Implements ExecuteFunctor, AfterExecuteFunctor, and damage events.
 */

#ifndef FUNCTOR_HOOKS_H
#define FUNCTOR_HOOKS_H

#include <stdbool.h>

#include "functor_types.h"  // FUNCTOR_ADDRS_VERIFIED_BUILD (install gate in main.c)

/**
 * Initialize the functor hook system.
 * Must be called after Dobby is ready and game module is loaded.
 * Dispatch resolves the server VM through the Lua runtime registry.
 *
 * @return true on success
 */
bool functor_hooks_init(void);

/**
 * Shutdown the functor hook system.
 * Removes all installed hooks.
 */
void functor_hooks_shutdown(void);

/**
 * Check if functor hooks are active.
 */
bool functor_hooks_is_active(void);

/**
 * Number of functor hooks actually installed (0-10). A value below 10 means
 * some functor contexts will never fire events even though is_active() is
 * true — surfaced via Ext.Debug.GetHookStatus().functor_hooks_installed.
 */
int functor_hooks_get_installed_count(void);

/**
 * Get count of functor events fired.
 */
uint64_t functor_hooks_get_event_count(void);

/**
 * Get the original (pre-hook) function pointer for a context type.
 * Used by ExecuteFunctors Lua API to call the game's functor execution code.
 *
 * @param ctx_type Context type (0-8, see FunctorContextType enum)
 * @return Original function pointer, or NULL if hooks not installed or invalid type
 */
void* functor_hooks_get_original_proc(int ctx_type);

#endif // FUNCTOR_HOOKS_H
