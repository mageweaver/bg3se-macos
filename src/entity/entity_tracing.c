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
#include "entity_system.h"
#include "component_property.h"
#include "component_registry.h"
#include "../lifetime/lifetime.h"
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

/* Mirrors the observable subset of Windows EntityChangeFlags /
 * ComponentChangeFlags (EntitySystemHelpers.h). */
#define ENTITY_TRACE_FLAG_CREATE   0x1u
#define ENTITY_TRACE_FLAG_DESTROY  0x2u

typedef struct {
    uint64_t entity_handle;
    uint16_t type_index;   /* ECS ComponentTypeIndex that changed */
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

static void entity_tracing_observe(uint64_t entity_handle, uint32_t event,
                                  uint16_t type_index) {
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
        .type_index = type_index,
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

/*
 * Ext.Entity.EnableTracing([enable])
 *
 * Windows takes a bool (Entity.inl:263) and warns once that tracing is a
 * development tool. We match both. Windows additionally refuses unless
 * gExtender->GetConfig().DeveloperMode is set; macOS has no DeveloperMode
 * config (Ext.Debug.IsDeveloperMode is hardcoded false), so gating on it would
 * make tracing permanently unusable. The gate is therefore absent by necessity
 * and recorded as a divergence rather than silently emulated.
 */
static int entity_tracing_lua_enable(lua_State *L) {
    bool enable = lua_isnoneornil(L, 1) ? true : (lua_toboolean(L, 1) != 0);

    if (enable) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            log_message("[WARN] [EntityTracing] Entity tracing is a development "
                        "tool for tracking entity changes; it should not be used "
                        "in production!");
        }
    }

    pthread_mutex_lock(&g_entity_tracing_mutex);
    g_entity_tracing_enabled = enable;
    pthread_mutex_unlock(&g_entity_tracing_mutex);

    /* Without global capture the observer only ever sees component types that
     * some mod already subscribed to, which made tracing appear to work while
     * silently recording nothing. */
    int types = entity_events_enable_global_capture(enable);
    if (types < 0) {
        log_message("[WARN] [EntityTracing] Global capture unavailable "
                    "(entity world not bound) — trace will only observe "
                    "component types with active subscriptions");
    }

    log_message("[INFO] [EntityTracing] Capture %s (%d types)",
                enable ? "enabled" : "disabled", types);
    lua_pushboolean(L, true);
    return 1;
}

static int entity_tracing_lua_disable(lua_State *L) {
    pthread_mutex_lock(&g_entity_tracing_mutex);
    g_entity_tracing_enabled = false;
    pthread_mutex_unlock(&g_entity_tracing_mutex);

    entity_events_enable_global_capture(false);
    log_message("[INFO] [EntityTracing] Capture disabled");
    lua_pushboolean(L, true);
    return 1;
}

/*
 * Ext.Entity.GetTrace() -> ECSChangeLog-shaped table
 *
 * Windows returns ecs::ECSChangeLog (EntitySystemHelpers.h:86):
 *
 *   ECSChangeLog { Entities : map<EntityHandle, ECSEntityLog> }
 *   ECSEntityLog { Entity, Flags, Components : map<uint16, ECSComponentLog> }
 *   ECSComponentLog { ComponentType, Flags }
 *
 * We build the same nested shape from the create/destroy ring buffer. Flags use
 * the Windows bit meanings we can actually observe: entity Create/Destroy, and
 * per-component Create/Destroy.
 *
 * Coverage note: Windows' tracer additionally follows the entity command
 * buffer, the immediate world cache, replication, and in-place modifications
 * (ECSChangeTracerOptions). macOS only observes component add/remove signals,
 * so Components entries reflect those; there is no modification tracking. The
 * Enabled/Dropped fields are macOS extras retained for diagnostics.
 */
/*
 * Windows exposes ECSEntityLog::Entity as an EntityHandle, which surfaces in
 * Lua as an entity proxy — not a bare integer. Mirror that so mod code can do
 * `log.Entities[h].Entity:GetComponent(...)` exactly as it does on Windows.
 */
static void trace_push_entity(lua_State *L, uint64_t handle) {
    EntityUserdata *ud = (EntityUserdata*)lua_newuserdata(L, sizeof(EntityUserdata));
    ud->handle = (EntityHandle)handle;
    ud->lifetime = lifetime_lua_get_current(L);
    luaL_getmetatable(L, "BG3Entity");
    lua_setmetatable(L, -2);
}

/* Windows ECSComponentLog exposes Name (StringView) and Type (ExtComponentType?).
 *
 * The name comes from the ECS component registry, which covers every CCR type,
 * not from the property-layout table — that only describes the subset of
 * components whose fields we have mapped, so it leaves Name nil for most of the
 * types global tracing now observes. */
static void trace_set_component_name(lua_State *L, uint16_t type_index) {
    const ComponentInfo *info = component_registry_lookup_by_index(type_index);
    if (info && info->name && info->name[0]) {
        lua_pushstring(L, info->name);
        lua_setfield(L, -2, "Name");
        lua_pushstring(L, info->name);
        lua_setfield(L, -2, "Type");
        /* Windows sources OneFrame from the component type itself. */
        lua_pushboolean(L, info->is_one_frame);
        lua_setfield(L, -2, "OneFrame");
        return;
    }
    const ComponentLayoutDef *def = component_property_get_layout_by_index(type_index);
    if (def && def->componentName) {
        lua_pushstring(L, def->componentName);
        lua_setfield(L, -2, "Name");
        if (def->shortName) {
            lua_pushstring(L, def->shortName);
            lua_setfield(L, -2, "Type");
        }
    }
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

    /* Entities : map<EntityHandle, ECSEntityLog> */
    lua_newtable(L);
    for (size_t i = 0; i < snapshot.count; i++) {
        const EntityTracingRecord *r = &snapshot.records[i];
        uint32_t entity_flag = (r->op == ENTITY_TRACING_OP_CREATE)
            ? ENTITY_TRACE_FLAG_CREATE : ENTITY_TRACE_FLAG_DESTROY;

        /* fetch-or-create Entities[handle] */
        lua_pushinteger(L, (lua_Integer)r->entity_handle);
        lua_rawget(L, -2);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_createtable(L, 0, 3);

            trace_push_entity(L, r->entity_handle);
            lua_setfield(L, -2, "Entity");
            lua_pushinteger(L, 0);
            lua_setfield(L, -2, "Flags");
            /* Windows ECSEntityLog bitmask, flattened to named booleans. */
            lua_pushboolean(L, 0); lua_setfield(L, -2, "Create");
            lua_pushboolean(L, 0); lua_setfield(L, -2, "Destroy");
            lua_pushboolean(L, 0); lua_setfield(L, -2, "Dead");
            lua_pushboolean(L, 0); lua_setfield(L, -2, "Ignore");
            lua_pushboolean(L, 0); lua_setfield(L, -2, "Immediate");
            lua_newtable(L);
            lua_setfield(L, -2, "Components");

            lua_pushinteger(L, (lua_Integer)r->entity_handle);
            lua_pushvalue(L, -2);
            lua_rawset(L, -4);
        }

        /* Flags |= entity_flag */
        lua_getfield(L, -1, "Flags");
        lua_Integer flags = lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_pushinteger(L, flags | (lua_Integer)entity_flag);
        lua_setfield(L, -2, "Flags");
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, (r->op == ENTITY_TRACING_OP_CREATE) ? "Create" : "Destroy");

        /* Components[typeIndex] : fetch-or-create, then OR the flag */
        lua_getfield(L, -1, "Components");
        lua_pushinteger(L, (lua_Integer)r->type_index);
        lua_rawget(L, -2);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_createtable(L, 0, 2);
            lua_pushinteger(L, (lua_Integer)r->type_index);
            lua_setfield(L, -2, "ComponentType");
            lua_pushinteger(L, 0);
            lua_setfield(L, -2, "Flags");
            /* Windows ECSComponentLog bitmask, flattened to named booleans. */
            lua_pushboolean(L, 0); lua_setfield(L, -2, "Create");
            lua_pushboolean(L, 0); lua_setfield(L, -2, "Destroy");
            lua_pushboolean(L, 0); lua_setfield(L, -2, "OneFrame");
            lua_pushboolean(L, 0); lua_setfield(L, -2, "Replicate");
            lua_pushboolean(L, 0); lua_setfield(L, -2, "ReplicatedComponent");
            trace_set_component_name(L, r->type_index);

            lua_pushinteger(L, (lua_Integer)r->type_index);
            lua_pushvalue(L, -2);
            lua_rawset(L, -4);
        }
        lua_getfield(L, -1, "Flags");
        lua_Integer cflags = lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_pushinteger(L, cflags | (lua_Integer)entity_flag);
        lua_setfield(L, -2, "Flags");
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, (r->op == ENTITY_TRACING_OP_CREATE) ? "Create" : "Destroy");

        lua_pop(L, 2);   /* component log, Components */
        lua_pop(L, 1);   /* entity log */
    }
    lua_setfield(L, -2, "Entities");

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
