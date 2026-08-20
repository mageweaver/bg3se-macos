#ifndef COMPONENT_OPS_GUARD_H
#define COMPONENT_OPS_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Call the game's AddImmediateDefaultComponent slot behind a C++ exception
 * handler.
 *
 * Component types that cannot be default-constructed make the game throw. With
 * a C caller there is no handler in the frame chain, so the runtime aborts via
 * std::terminate and the process dies before Lua can react. This wrapper
 * provides the handler.
 *
 * @return true if the call completed, false if it threw.
 */
bool bg3se_add_immediate_default_component_guarded(void *fn, void *ops,
                                                   uint64_t entity,
                                                   int retry_count);

#ifdef __cplusplus
}
#endif

#endif /* COMPONENT_OPS_GUARD_H */
