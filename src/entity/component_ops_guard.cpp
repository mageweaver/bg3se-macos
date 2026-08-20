/*
 * Exception guard for ComponentOps dispatch.
 *
 * entity:CreateComponent ends by calling the game's virtual
 * AddImmediateDefaultComponent slot. For component types that cannot be
 * default-constructed the game raises a C++ exception. The caller is a C
 * function, so the unwinder finds no handler in the frame chain and the runtime
 * aborts through std::terminate — killing the process rather than returning an
 * error to Lua. pcall cannot contain that, because the process is already gone
 * by the time Lua would regain control.
 *
 * Interposing this C++ frame gives the unwinder a handler to land on, turning a
 * hard abort into a false return.
 *
 * Caveat: an exception thrown partway through an ECS mutation may leave the
 * component store partially updated. Surviving with a reported failure is
 * strictly better than aborting, but callers should treat a false return as
 * "the entity may be in an indeterminate state for this component type".
 */

#include <cstdint>

extern "C" {

/* Must match entity_system.c exactly: (ComponentOps*, EntityHandle, int). */
typedef void (*AddImmediateDefaultComponentFn)(void *component_ops,
                                               uint64_t entity_handle,
                                               int retry_count);

bool bg3se_add_immediate_default_component_guarded(void *fn, void *ops,
                                                   uint64_t entity,
                                                   int retry_count) {
    if (!fn || !ops) return false;
    try {
        ((AddImmediateDefaultComponentFn)fn)(ops, entity, retry_count);
        return true;
    } catch (...) {
        return false;
    }
}

}  /* extern "C" */
