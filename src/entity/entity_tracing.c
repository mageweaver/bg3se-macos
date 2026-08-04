/**
 * BG3SE-macOS - Prototype Entity Event Tracing
 *
 * Expected console usage:
 *
 *   Ext.Entity.EnableTracing()
 *   -- Exercise gameplay or entity/component operations.
 *   local trace = Ext.Entity.GetTrace()
 *   Ext.Print(trace.Enabled, trace.Dropped, #trace.Events)
 *   Ext.Print(trace.Events[1].Entity, trace.Events[1].Op, trace.Events[1].Seq)
 *   Ext.Entity.DisableTracing()
 *   Ext.Entity.ClearTrace()
 *
 * The Windows implementation returns a component change tree. This prototype
 * returns the flat bounded log described in entity_tracing.h.
 */

#include "entity_tracing.h"

#include "entity_events.h"
#include "../core/logging.h"

#include <lauxlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define ENTITY_TRACING_CAPACITY 4096

enum {
    ENTITY_TRACING_OP_CREATE = 0,
    ENTITY_TRACING_OP_DESTROY = 1
};

typedef struct {
    uint64_t entity_handle;
    uint8_t op;
    uint64_t seq;
} EntityTracingRecord;

typedef struct {
    bool enabled;
    uint64_t dropped;
    size_t count;
    EntityTracingRecord records[ENTITY_TRACING_CAPACITY];
} EntityTracingSnapshot;

static pthread_mutex_t g_entity_tracing_mutex = PTHREAD_MUTEX_INITIALIZER;
static EntityTracingRecord g_entity_tracing_records[ENTITY_TRACING_CAPACITY];
static size_t g_entity_tracing_start = 0;
static size_t g_entity_tracing_count = 0;
static uint64_t g_entity_tracing_dropped = 0;
static uint64_t g_entity_tracing_next_seq = 0;
static bool g_entity_tracing_enabled = false;

static void entity_tracing_observe(uint64_t entity_handle, uint32_t event) {
    uint8_t op;
    if (event == ENTITY_EVENT_CREATE) {
        op = ENTITY_TRACING_OP_CREATE;
    } else if (event == ENTITY_EVENT_DESTROY) {
        op = ENTITY_TRACING_OP_DESTROY;
    } else {
        return;
    }

    pthread_mutex_lock(&g_entity_tracing_mutex);
    if (!g_entity_tracing_enabled) {
        pthread_mutex_unlock(&g_entity_tracing_mutex);
        return;
    }

    EntityTracingRecord record = {
        .entity_handle = entity_handle,
        .op = op,
        .seq = ++g_entity_tracing_next_seq
    };

    if (g_entity_tracing_count < ENTITY_TRACING_CAPACITY) {
        size_t index = (g_entity_tracing_start + g_entity_tracing_count)
            % ENTITY_TRACING_CAPACITY;
        g_entity_tracing_records[index] = record;
        g_entity_tracing_count++;
    } else {
        g_entity_tracing_records[g_entity_tracing_start] = record;
        g_entity_tracing_start = (g_entity_tracing_start + 1) % ENTITY_TRACING_CAPACITY;
        g_entity_tracing_dropped++;
    }

    pthread_mutex_unlock(&g_entity_tracing_mutex);
}

static int entity_tracing_lua_enable(lua_State *L) {
    pthread_mutex_lock(&g_entity_tracing_mutex);
    g_entity_tracing_enabled = true;
    pthread_mutex_unlock(&g_entity_tracing_mutex);

    log_message("[INFO] [EntityTracing] Capture enabled");
    lua_pushboolean(L, true);
    return 1;
}

static int entity_tracing_lua_disable(lua_State *L) {
    pthread_mutex_lock(&g_entity_tracing_mutex);
    g_entity_tracing_enabled = false;
    pthread_mutex_unlock(&g_entity_tracing_mutex);

    log_message("[INFO] [EntityTracing] Capture disabled");
    lua_pushboolean(L, true);
    return 1;
}

static int entity_tracing_lua_get_trace(lua_State *L) {
    EntityTracingSnapshot snapshot;

    pthread_mutex_lock(&g_entity_tracing_mutex);
    snapshot.enabled = g_entity_tracing_enabled;
    snapshot.dropped = g_entity_tracing_dropped;
    snapshot.count = g_entity_tracing_count;
    for (size_t i = 0; i < snapshot.count; i++) {
        size_t index = (g_entity_tracing_start + i) % ENTITY_TRACING_CAPACITY;
        snapshot.records[i] = g_entity_tracing_records[index];
    }
    pthread_mutex_unlock(&g_entity_tracing_mutex);

    lua_createtable(L, 0, 3);

    lua_pushboolean(L, snapshot.enabled);
    lua_setfield(L, -2, "Enabled");

    lua_pushinteger(L, (lua_Integer)snapshot.dropped);
    lua_setfield(L, -2, "Dropped");

    lua_createtable(L, (int)snapshot.count, 0);
    for (size_t i = 0; i < snapshot.count; i++) {
        const EntityTracingRecord *record = &snapshot.records[i];
        lua_createtable(L, 0, 3);

        lua_pushinteger(L, (lua_Integer)record->entity_handle);
        lua_setfield(L, -2, "Entity");

        lua_pushstring(L, record->op == ENTITY_TRACING_OP_CREATE ? "Create" : "Destroy");
        lua_setfield(L, -2, "Op");

        lua_pushinteger(L, (lua_Integer)record->seq);
        lua_setfield(L, -2, "Seq");

        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "Events");

    return 1;
}

static int entity_tracing_lua_clear(lua_State *L) {
    pthread_mutex_lock(&g_entity_tracing_mutex);
    memset(g_entity_tracing_records, 0, sizeof(g_entity_tracing_records));
    g_entity_tracing_start = 0;
    g_entity_tracing_count = 0;
    g_entity_tracing_dropped = 0;
    g_entity_tracing_next_seq = 0;
    pthread_mutex_unlock(&g_entity_tracing_mutex);

    log_message("[INFO] [EntityTracing] Trace cleared");
    lua_pushboolean(L, true);
    return 1;
}

void entity_tracing_register_lua(lua_State *L, int entity_table_index) {
    if (!L) return;

    int entity_index = lua_absindex(L, entity_table_index);
    if (!lua_istable(L, entity_index)) {
        log_message("[WARN] [EntityTracing] Registration target is not a table");
        return;
    }

    static const struct luaL_Reg functions[] = {
        { "EnableTracing", entity_tracing_lua_enable },
        { "DisableTracing", entity_tracing_lua_disable },
        { "GetTrace", entity_tracing_lua_get_trace },
        { "ClearTrace", entity_tracing_lua_clear },
        { NULL, NULL }
    };

    for (const struct luaL_Reg *function = functions; function->name; function++) {
        lua_pushcfunction(L, function->func);
        lua_setfield(L, entity_index, function->name);
    }

    entity_events_set_observer(entity_tracing_observe);
    log_message("[INFO] [EntityTracing] Registered 4 Lua functions");
}
