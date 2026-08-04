/**
 * BG3SE-macOS - Prototype Entity Event Tracing
 *
 * This is a partial surface compared with the Windows entity/component trace
 * tree. It exposes a flat bounded create/destroy log pending C3-4 replication.
 */

#ifndef ENTITY_TRACING_H
#define ENTITY_TRACING_H

#include <lua.h>

#ifdef __cplusplus
extern "C" {
#endif

void entity_tracing_register_lua(lua_State *L, int entity_table_index);

#ifdef __cplusplus
}
#endif

#endif // ENTITY_TRACING_H
