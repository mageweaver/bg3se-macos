/**
 * BG3SE-macOS - Named ECS system update subscriptions.
 *
 * Implements Ext.Entity.OnSystemUpdate / OnSystemPostUpdate for the verified
 * 4.1.1.7398727 system registry layout.
 */

#ifndef ECS_SYSTEM_UPDATE_H
#define ECS_SYSTEM_UPDATE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lua_State;

void ecs_system_update_register_lua(struct lua_State *L, int entity_table_index);

/* EntityWorld lifecycle observer hooks, called by entity_events.c. */
void ecs_system_update_on_world_bound(void *entity_world, bool is_server);
void ecs_system_update_set_transition(bool in_transition);
void ecs_system_update_teardown(void);

#ifdef __cplusplus
}
#endif

#endif /* ECS_SYSTEM_UPDATE_H */
