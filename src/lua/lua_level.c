/**
 * lua_level.c - Lua bindings for Ext.Level API
 *
 * Provides Lua access to level physics and tile queries.
 *
 * API:
 *   Ext.Level.GetCurrentLevel() - Get current level pointer (or nil)
 *   Ext.Level.GetPhysicsScene() - Get physics scene pointer (or nil)
 *   Ext.Level.GetAiGrid() - Get AI grid pointer (or nil)
 *   Ext.Level.IsReady() - Check if LevelManager is available
 *   Raycasts are explicit warn-once ABI deferrals.
 *   Ext.Level.TestBox(pos, extents, physType, includeGroup, excludeGroup) -> hit array or nil
 *   Ext.Level.TestSphere(pos, radius, physType, includeGroup, excludeGroup) -> hit array or nil
 *   Ext.Level.SweepCylinderClosest(src, dst, extents, ...) -> hit table or nil
 *   Ext.Level.SweepCylinderAll(src, dst, extents, ...) -> array of hit tables
 *   Ext.Level.GetHeightsAt(x, z) -> array of heights
 *   Ext.Level.GetEntitiesOnTile(pos) -> copied EntityHandle array
 *   Ext.Level.GetTileRawDebugInfo(x, z) -> copied raw tile diagnostics or nil
 *   Ext.Level.GetPathById(pathId) -> copied path state or nil
 *   Ext.Level.GetActivePathfindingRequests() -> copied path-state array
 *   Ext.Level.FindPath(pathId, immediate?) -> copied path result or false
 *   Ext.Level.ReleasePath(pathId) -> boolean
 */

#include "lua_level.h"
#include "../level/level_manager.h"
#include "../core/logging.h"
#include <lua.h>
#include <lauxlib.h>
#include <string.h>

/* Forward declarations for shared hit-result helpers */
static int push_hit_or_nil(lua_State *L, bool found, const LevelPhysicsHit *hit);
static int push_hit_all(lua_State *L, const LevelPhysicsHitAll *hits);

static void warn_deferred_once(bool *warned, const char *name,
                               const char *reason) {
    if (!*warned) {
        log_message("[Level] WARN: Ext.Level.%s is deferred on macOS: %s",
                    name, reason);
        *warned = true;
    }
}

// ============================================================================
// Helper: Read vec3 from Lua table at stack index
// ============================================================================

static bool read_vec3(lua_State *L, int idx, float out[3]) {
    if (!lua_istable(L, idx)) return false;

    for (int i = 0; i < 3; i++) {
        lua_rawgeti(L, idx, i + 1);
        if (!lua_isnumber(L, -1)) {
            lua_pop(L, 1);
            return false;
        }
        out[i] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    return true;
}

static bool read_path_id(lua_State *L, int idx, int32_t *out_path_id) {
    if (!out_path_id || !lua_isinteger(L, idx)) return false;

    lua_Integer value = lua_tointeger(L, idx);
    if (value < INT32_MIN || value > INT32_MAX) return false;

    *out_path_id = (int32_t)value;
    return true;
}

static void push_path_state(lua_State *L, const LevelAiPathState *state) {
    lua_newtable(L);

    lua_pushinteger(L, state->path_id);
    lua_setfield(L, -2, "PathId");
    lua_pushboolean(L, state->search_started);
    lua_setfield(L, -2, "SearchStarted");
    lua_pushboolean(L, state->search_complete);
    lua_setfield(L, -2, "SearchComplete");
    lua_pushboolean(L, state->goal_found);
    lua_setfield(L, -2, "GoalFound");
    lua_pushboolean(L, state->destination_reached);
    lua_setfield(L, -2, "DestinationReached");
    lua_pushnumber(L, state->moving_bound);
    lua_setfield(L, -2, "MovingBound");
    lua_pushnumber(L, state->standing_bound);
    lua_setfield(L, -2, "StandingBound");
    lua_pushnumber(L, state->moving_bound2);
    lua_setfield(L, -2, "MovingBound2");
    lua_pushinteger(L, state->node_count);
    lua_setfield(L, -2, "NodeCount");
}

static void push_path_node(lua_State *L, const LevelAiPathNode *node) {
    lua_newtable(L);

    lua_newtable(L);
    for (int i = 0; i < 3; i++) {
        lua_pushnumber(L, node->position[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "Position");

    /* EntityHandle convention in this codebase: raw Lua integer. */
    lua_pushinteger(L, (lua_Integer)node->portal);
    lua_setfield(L, -2, "Portal");
    lua_pushnumber(L, node->distance);
    lua_setfield(L, -2, "Distance");
    lua_pushnumber(L, node->distance_modifier);
    lua_setfield(L, -2, "DistanceModifier");
    lua_pushinteger(L, node->flags);
    lua_setfield(L, -2, "Flags");
}

static void push_path_result(lua_State *L,
                             const LevelAiPathResult *result) {
    push_path_state(L, &result->state);

    if (result->state.search_complete && result->state.goal_found) {
        lua_newtable(L);
        for (size_t i = 0; i < result->nodes_count; i++) {
            push_path_node(L, &result->nodes[i]);
            lua_rawseti(L, -2, (lua_Integer)i + 1);
        }
        lua_setfield(L, -2, "Nodes");
    }
}

// ============================================================================
// Singleton Accessors
// ============================================================================

/**
 * Ext.Level.IsReady() -> boolean
 */
static int lua_level_is_ready(lua_State *L) {
    lua_pushboolean(L, level_manager_ready());
    return 1;
}

/**
 * Ext.Level.GetCurrentLevel() -> lightuserdata or nil
 */
static int lua_level_get_current(lua_State *L) {
    void *level = level_get_current();
    if (level) {
        lua_pushlightuserdata(L, level);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/**
 * Ext.Level.GetPhysicsScene() -> lightuserdata or nil
 */
static int lua_level_get_physics_scene(lua_State *L) {
    void *physics = level_get_physics_scene();
    if (physics) {
        lua_pushlightuserdata(L, physics);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/**
 * Ext.Level.GetAiGrid() -> lightuserdata or nil
 */
static int lua_level_get_aigrid(lua_State *L) {
    void *aigrid = level_get_aigrid();
    if (aigrid) {
        lua_pushlightuserdata(L, aigrid);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// ============================================================================
// Physics Raycasting
// ============================================================================

/**
 * Ext.Level.RaycastClosest(src, dst, physType, includeGroup, excludeGroup, context)
 *   src: {x, y, z} table
 *   dst: {x, y, z} table
 *   physType: integer (physics type flags)
 *   includeGroup: integer (group mask)
 *   excludeGroup: integer (group mask)
 *   context: integer (raycast context)
 *   Returns: hit table {Normal={x,y,z}, Position={x,y,z}, Distance=n, PhysicsGroup=n} or nil
 */
/**
 * Ext.Level.RaycastClosest(src, dst, physType, includeGroup, excludeGroup, context)
 *   Returns: hit table or nil
 *
 * Undeferred 2026-08-20. The trailing by-value ls::Function<bool(PhysicsShape
 * const*)> is passed as 0x40 zeroed bytes (NULL MethodTable), which the engine
 * treats as "no shape filter" and services with its own s_IgnoreFilterClosest.
 * See ghidra/offsets/PHYSICS_VMT_AUDIT.md.
 */
static int lua_level_raycast_closest(lua_State *L) {
    float src[3], dst[3];
    if (!read_vec3(L, 1, src)) {
        return luaL_error(L, "Ext.Level.RaycastClosest: src must be a {x,y,z} table");
    }
    if (!read_vec3(L, 2, dst)) {
        return luaL_error(L, "Ext.Level.RaycastClosest: dst must be a {x,y,z} table");
    }

    uint32_t physics_type  = (uint32_t)luaL_optinteger(L, 3, 1);
    uint32_t include_group = (uint32_t)luaL_optinteger(L, 4, 0x7FFFFFFF);
    uint32_t exclude_group = (uint32_t)luaL_optinteger(L, 5, 0);
    int context            = (int)luaL_optinteger(L, 6, 0);

    LevelPhysicsHit hit;
    bool found = level_raycast_closest(src, dst, &hit, physics_type,
                                       include_group, exclude_group, context);
    return push_hit_or_nil(L, found, &hit);
}

/**
 * Ext.Level.RaycastAll(src, dst, physType, includeGroup, excludeGroup, context)
 *   src: {x, y, z} table
 *   dst: {x, y, z} table
 *   physType: integer (physics type flags)
 *   includeGroup: integer (group mask)
 *   excludeGroup: integer (group mask)
 *   context: integer (raycast context)
 *   Returns: array of hit tables {Normal={x,y,z}, Position={x,y,z}, Distance=n, PhysicsGroup=n}
 *            or empty table if no hits
 */
/**
 * Ext.Level.RaycastAll(src, dst, physType, includeGroup, excludeGroup, context)
 *   Returns: array of hit tables, or nil when there are no hits
 *
 * Undeferred 2026-08-20. The old warn-and-nil stub claimed the trailing
 * by-value ls::Optional<PhysicsSceneScopedReadLock&> "cannot be constructed
 * safely from C". That is no longer true: it is the same disengaged 16-byte
 * optional RaycastAny already ships against this audited image, and the
 * ls::PhysicsHitAll& out-parameter is the one the working SweepAll/TestBox
 * bindings already pass. See ghidra/offsets/PHYSICS_VMT_AUDIT.md.
 */
static int lua_level_raycast_all(lua_State *L) {
    float src[3], dst[3];
    if (!read_vec3(L, 1, src)) {
        return luaL_error(L, "Ext.Level.RaycastAll: src must be a {x,y,z} table");
    }
    if (!read_vec3(L, 2, dst)) {
        return luaL_error(L, "Ext.Level.RaycastAll: dst must be a {x,y,z} table");
    }

    uint32_t physics_type  = (uint32_t)luaL_optinteger(L, 3, 1);
    uint32_t include_group = (uint32_t)luaL_optinteger(L, 4, 0x7FFFFFFF);
    uint32_t exclude_group = (uint32_t)luaL_optinteger(L, 5, 0);
    int context            = (int)luaL_optinteger(L, 6, 0);

    LevelPhysicsHitAll hits;
    memset(&hits, 0, sizeof(hits));
    bool found = level_raycast_all(src, dst, &hits, physics_type,
                                   include_group, exclude_group, context);
    /* Windows returns an empty PhysicsHitAll rather than null on a miss
     * (Level.inl:277), so callers can index the arrays unconditionally. */
    if (!found) {
        memset(&hits, 0, sizeof(hits));
    }
    return push_hit_all(L, &hits);
}

/**
 * Ext.Level.RaycastAny(src, dst, physType, includeGroup, excludeGroup, context)
 *   Returns: boolean (true if any hit)
 */
static int lua_level_raycast_any(lua_State *L) {
    float src[3], dst[3];
    if (!read_vec3(L, 1, src)) {
        return luaL_error(
            L, "Ext.Level.RaycastAny: src must be a {x,y,z} table");
    }
    if (!read_vec3(L, 2, dst)) {
        return luaL_error(
            L, "Ext.Level.RaycastAny: dst must be a {x,y,z} table");
    }

    uint32_t physics_type = (uint32_t)luaL_optinteger(L, 3, 1);
    uint32_t include_group = (uint32_t)luaL_optinteger(L, 4, 0x7FFFFFFF);
    uint32_t exclude_group = (uint32_t)luaL_optinteger(L, 5, 0);
    int context = (int)luaL_optinteger(L, 6, 0);

    lua_pushboolean(L, level_raycast_any(
        src, dst, physics_type, include_group, exclude_group, context));
    return 1;
}

/**
 * Ext.Level.TestBox(pos, extents, physType, includeGroup, excludeGroup)
 *   pos: {x, y, z} table (center position)
 *   extents: {x, y, z} table (half-extents)
 *   Returns: array of overlap hit tables, or nil when there are no overlaps
 */
static int lua_level_test_box(lua_State *L) {
    float pos[3], extents[3];

    if (!read_vec3(L, 1, pos)) {
        return luaL_error(L, "Ext.Level.TestBox: pos must be a {x,y,z} table");
    }
    if (!read_vec3(L, 2, extents)) {
        return luaL_error(L, "Ext.Level.TestBox: extents must be a {x,y,z} table");
    }

    uint32_t phys_type = (uint32_t)luaL_optinteger(L, 3, 1);
    uint32_t include_group = (uint32_t)luaL_optinteger(L, 4, 0x7FFFFFFF);
    uint32_t exclude_group = (uint32_t)luaL_optinteger(L, 5, 0);

    LevelPhysicsHitAll hits;
    bool overlap = level_test_box(pos, extents, &hits,
                                  phys_type, include_group, exclude_group);
    if (!overlap) {
        lua_pushnil(L);
        return 1;
    }
    return push_hit_all(L, &hits);
}

/**
 * Ext.Level.TestSphere(pos, radius, physType, includeGroup, excludeGroup)
 *   pos: {x, y, z} table (center position)
 *   radius: number
 *   Returns: array of overlap hit tables, or nil when there are no overlaps
 */
static int lua_level_test_sphere(lua_State *L) {
    float pos[3];

    if (!read_vec3(L, 1, pos)) {
        return luaL_error(L, "Ext.Level.TestSphere: pos must be a {x,y,z} table");
    }

    float radius = (float)luaL_checknumber(L, 2);
    uint32_t phys_type = (uint32_t)luaL_optinteger(L, 3, 1);
    uint32_t include_group = (uint32_t)luaL_optinteger(L, 4, 0x7FFFFFFF);
    uint32_t exclude_group = (uint32_t)luaL_optinteger(L, 5, 0);

    LevelPhysicsHitAll hits;
    bool overlap = level_test_sphere(pos, radius, &hits,
                                     phys_type, include_group, exclude_group);
    if (!overlap) {
        lua_pushnil(L);
        return 1;
    }
    return push_hit_all(L, &hits);
}

// ============================================================================
// Sweep Functions
// ============================================================================

/**
 * Push a single LevelPhysicsHit as a Lua table (or nil if not found).
 */
static int push_hit_or_nil(lua_State *L, bool found, const LevelPhysicsHit *hit) {
    if (!found) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);

    lua_newtable(L);
    for (int i = 0; i < 3; i++) { lua_pushnumber(L, hit->normal[i]); lua_rawseti(L, -2, i+1); }
    lua_setfield(L, -2, "Normal");

    lua_newtable(L);
    for (int i = 0; i < 3; i++) { lua_pushnumber(L, hit->position[i]); lua_rawseti(L, -2, i+1); }
    lua_setfield(L, -2, "Position");

    lua_pushnumber(L, hit->distance);
    lua_setfield(L, -2, "Distance");

    lua_pushinteger(L, hit->physics_group);
    lua_setfield(L, -2, "PhysicsGroup");

    return 1;
}

/**
 * Push a LevelPhysicsHitAll as a Lua array of hit tables.
 * VERIFIED: Normals/Positions are Array<glm::vec3> where glm::vec3 = float[3] (12 bytes, no padding).
 * Stride is 3 floats per element — confirmed from Windows BG3SE Physics.h:PhysicsHitAll.
 */
/*
 * Windows returns phx::PhysicsHitAll, which is a struct of parallel arrays
 * (ExtIdeHelpers.lua @class PhxPhysicsHitAll: Distances[], Normals[],
 * Positions[], PhysicsGroup[], ...). The port returned an array of per-hit
 * tables instead, so Windows mod code reading result.Positions[i] got nil.
 *
 * Emit both shapes on the same table: the integer-keyed per-hit entries the
 * port already produced, plus the named parallel arrays Windows exposes. That
 * gains parity without breaking anything already reading the array form.
 */
static int push_hit_all(lua_State *L, const LevelPhysicsHitAll *hits) {
    lua_newtable(L);

    uint32_t count = 0;
    bool usable = hits && hits->normals_ptr && hits->positions_ptr &&
                  hits->distances_ptr;

    /* Windows' RaycastAll never returns null -- on a miss it yields an object
     * whose arrays are empty (Level.inl:277, `return &gHits;`). Always emit the
     * named arrays so callers can index them unconditionally. */
    lua_newtable(L); lua_setfield(L, -2, "Positions");
    lua_newtable(L); lua_setfield(L, -2, "Normals");
    lua_newtable(L); lua_setfield(L, -2, "Distances");
    lua_newtable(L); lua_setfield(L, -2, "PhysicsGroup");

    if (!usable) {
        return 1;  // empty result, matching Windows' empty PhysicsHitAll
    }

    count = hits->normals_size;
    if (hits->positions_size < count) count = hits->positions_size;
    if (hits->distances_size < count) count = hits->distances_size;

    for (uint32_t i = 0; i < count; i++) {
        lua_newtable(L);

        lua_newtable(L);
        float *n = hits->normals_ptr + i * 3;
        for (int j = 0; j < 3; j++) { lua_pushnumber(L, n[j]); lua_rawseti(L, -2, j+1); }
        lua_setfield(L, -2, "Normal");

        lua_newtable(L);
        float *p = hits->positions_ptr + i * 3;
        for (int j = 0; j < 3; j++) { lua_pushnumber(L, p[j]); lua_rawseti(L, -2, j+1); }
        lua_setfield(L, -2, "Position");

        lua_pushnumber(L, hits->distances_ptr[i]);
        lua_setfield(L, -2, "Distance");

        if (hits->physics_group_ptr && i < hits->physics_group_size) {
            lua_pushinteger(L, hits->physics_group_ptr[i]);
        } else {
            lua_pushinteger(L, 0);
        }
        lua_setfield(L, -2, "PhysicsGroup");

        lua_rawseti(L, -2, (int)(i + 1));

        /* mirror into the Windows-shaped parallel arrays */
        lua_getfield(L, -1, "Positions");
        lua_newtable(L);
        for (int j = 0; j < 3; j++) {
            lua_pushnumber(L, hits->positions_ptr[i * 3 + j]);
            lua_rawseti(L, -2, j + 1);
        }
        lua_rawseti(L, -2, (int)(i + 1));
        lua_pop(L, 1);

        lua_getfield(L, -1, "Normals");
        lua_newtable(L);
        for (int j = 0; j < 3; j++) {
            lua_pushnumber(L, hits->normals_ptr[i * 3 + j]);
            lua_rawseti(L, -2, j + 1);
        }
        lua_rawseti(L, -2, (int)(i + 1));
        lua_pop(L, 1);

        lua_getfield(L, -1, "Distances");
        lua_pushnumber(L, hits->distances_ptr[i]);
        lua_rawseti(L, -2, (int)(i + 1));
        lua_pop(L, 1);

        lua_getfield(L, -1, "PhysicsGroup");
        lua_pushinteger(L, (hits->physics_group_ptr && i < hits->physics_group_size)
                             ? hits->physics_group_ptr[i] : 0);
        lua_rawseti(L, -2, (int)(i + 1));
        lua_pop(L, 1);
    }
    return 1;
}

/**
 * Ext.Level.SweepSphereClosest(src, dst, radius, physType, includeGroup, excludeGroup, context)
 *   Returns: hit table or nil
 */
static int lua_level_sweep_sphere_closest(lua_State *L) {
    float src[3], dst[3];
    if (!read_vec3(L, 1, src)) return luaL_error(L, "SweepSphereClosest: src must be {x,y,z}");
    if (!read_vec3(L, 2, dst)) return luaL_error(L, "SweepSphereClosest: dst must be {x,y,z}");
    float radius           = (float)luaL_checknumber(L, 3);
    uint32_t phys_type     = (uint32_t)luaL_optinteger(L, 4, 1);
    uint32_t include_group = (uint32_t)luaL_optinteger(L, 5, 0x7FFFFFFF);
    uint32_t exclude_group = (uint32_t)luaL_optinteger(L, 6, 0);
    int context            = (int)luaL_optinteger(L, 7, 0);
    LevelPhysicsHit hit;
    bool found = level_sweep_sphere_closest(
        src, dst, radius, &hit,
        phys_type, include_group, exclude_group, context);
    return push_hit_or_nil(L, found, &hit);
}

/**
 * Ext.Level.SweepSphereAll(src, dst, radius, physType, includeGroup, excludeGroup, context)
 *   Returns: array of hit tables
 */
static int lua_level_sweep_sphere_all(lua_State *L) {
    float src[3], dst[3];
    if (!read_vec3(L, 1, src)) return luaL_error(L, "SweepSphereAll: src must be {x,y,z}");
    if (!read_vec3(L, 2, dst)) return luaL_error(L, "SweepSphereAll: dst must be {x,y,z}");
    float radius           = (float)luaL_checknumber(L, 3);
    uint32_t phys_type     = (uint32_t)luaL_optinteger(L, 4, 1);
    uint32_t include_group = (uint32_t)luaL_optinteger(L, 5, 0x7FFFFFFF);
    uint32_t exclude_group = (uint32_t)luaL_optinteger(L, 6, 0);
    int context            = (int)luaL_optinteger(L, 7, 0);
    LevelPhysicsHitAll hits;
    bool found = level_sweep_sphere_all(
        src, dst, radius, &hits,
        phys_type, include_group, exclude_group, context);
    if (!found) {
        lua_pushnil(L);
        return 1;
    }
    return push_hit_all(L, &hits);
}

/**
 * Ext.Level.SweepCapsuleClosest(src, dst, radius, halfHeight, physType, includeGroup, excludeGroup, context)
 *   Returns: hit table or nil
 */
static int lua_level_sweep_capsule_closest(lua_State *L) {
    float src[3], dst[3];
    if (!read_vec3(L, 1, src)) return luaL_error(L, "SweepCapsuleClosest: src must be {x,y,z}");
    if (!read_vec3(L, 2, dst)) return luaL_error(L, "SweepCapsuleClosest: dst must be {x,y,z}");
    float radius           = (float)luaL_checknumber(L, 3);
    float half_height      = (float)luaL_checknumber(L, 4);
    uint32_t phys_type     = (uint32_t)luaL_optinteger(L, 5, 1);
    uint32_t include_group = (uint32_t)luaL_optinteger(L, 6, 0x7FFFFFFF);
    uint32_t exclude_group = (uint32_t)luaL_optinteger(L, 7, 0);
    int context            = (int)luaL_optinteger(L, 8, 0);
    LevelPhysicsHit hit;
    bool found = level_sweep_capsule_closest(
        src, dst, radius, half_height, &hit,
        phys_type, include_group, exclude_group, context);
    return push_hit_or_nil(L, found, &hit);
}

/**
 * Ext.Level.SweepCapsuleAll(src, dst, radius, halfHeight, physType, includeGroup, excludeGroup, context)
 *   Returns: array of hit tables
 */
static int lua_level_sweep_capsule_all(lua_State *L) {
    float src[3], dst[3];
    if (!read_vec3(L, 1, src)) return luaL_error(L, "SweepCapsuleAll: src must be {x,y,z}");
    if (!read_vec3(L, 2, dst)) return luaL_error(L, "SweepCapsuleAll: dst must be {x,y,z}");
    float radius           = (float)luaL_checknumber(L, 3);
    float half_height      = (float)luaL_checknumber(L, 4);
    uint32_t phys_type     = (uint32_t)luaL_optinteger(L, 5, 1);
    uint32_t include_group = (uint32_t)luaL_optinteger(L, 6, 0x7FFFFFFF);
    uint32_t exclude_group = (uint32_t)luaL_optinteger(L, 7, 0);
    int context            = (int)luaL_optinteger(L, 8, 0);
    LevelPhysicsHitAll hits;
    bool found = level_sweep_capsule_all(
        src, dst, radius, half_height, &hits,
        phys_type, include_group, exclude_group, context);
    if (!found) {
        lua_pushnil(L);
        return 1;
    }
    return push_hit_all(L, &hits);
}

/**
 * Ext.Level.SweepBoxClosest(src, dst, extents, physType, includeGroup, excludeGroup, context)
 *   extents: {x, y, z} half-extents
 *   Returns: hit table or nil
 */
static int lua_level_sweep_box_closest(lua_State *L) {
    float src[3], dst[3], extents[3];
    if (!read_vec3(L, 1, src))     return luaL_error(L, "SweepBoxClosest: src must be {x,y,z}");
    if (!read_vec3(L, 2, dst))     return luaL_error(L, "SweepBoxClosest: dst must be {x,y,z}");
    if (!read_vec3(L, 3, extents)) return luaL_error(L, "SweepBoxClosest: extents must be {x,y,z}");
    uint32_t phys_type     = (uint32_t)luaL_optinteger(L, 4, 1);
    uint32_t include_group = (uint32_t)luaL_optinteger(L, 5, 0x7FFFFFFF);
    uint32_t exclude_group = (uint32_t)luaL_optinteger(L, 6, 0);
    int context            = (int)luaL_optinteger(L, 7, 0);
    LevelPhysicsHit hit;
    bool found = level_sweep_box_closest(
        src, dst, extents, &hit,
        phys_type, include_group, exclude_group, context);
    return push_hit_or_nil(L, found, &hit);
}

/**
 * Ext.Level.SweepBoxAll(src, dst, extents, physType, includeGroup, excludeGroup, context)
 *   extents: {x, y, z} half-extents
 *   Returns: array of hit tables
 */
static int lua_level_sweep_box_all(lua_State *L) {
    float src[3], dst[3], extents[3];
    if (!read_vec3(L, 1, src))     return luaL_error(L, "SweepBoxAll: src must be {x,y,z}");
    if (!read_vec3(L, 2, dst))     return luaL_error(L, "SweepBoxAll: dst must be {x,y,z}");
    if (!read_vec3(L, 3, extents)) return luaL_error(L, "SweepBoxAll: extents must be {x,y,z}");
    uint32_t phys_type     = (uint32_t)luaL_optinteger(L, 4, 1);
    uint32_t include_group = (uint32_t)luaL_optinteger(L, 5, 0x7FFFFFFF);
    uint32_t exclude_group = (uint32_t)luaL_optinteger(L, 6, 0);
    int context            = (int)luaL_optinteger(L, 7, 0);
    LevelPhysicsHitAll hits;
    bool found = level_sweep_box_all(
        src, dst, extents, &hits,
        phys_type, include_group, exclude_group, context);
    if (!found) {
        lua_pushnil(L);
        return 1;
    }
    return push_hit_all(L, &hits);
}

/**
 * Ext.Level.SweepCylinderClosest(src, dst, extents, physType,
 *                                includeGroup, excludeGroup, context)
 *   extents: {x, y, z} cylinder extents
 *   Returns: hit table or nil
 */
static int lua_level_sweep_cylinder_closest(lua_State *L) {
    float src[3], dst[3], extents[3];
    if (!read_vec3(L, 1, src))     return luaL_error(L, "SweepCylinderClosest: src must be {x,y,z}");
    if (!read_vec3(L, 2, dst))     return luaL_error(L, "SweepCylinderClosest: dst must be {x,y,z}");
    if (!read_vec3(L, 3, extents)) return luaL_error(L, "SweepCylinderClosest: extents must be {x,y,z}");
    uint32_t phys_type     = (uint32_t)luaL_optinteger(L, 4, 1);
    uint32_t include_group = (uint32_t)luaL_optinteger(L, 5, 0x7FFFFFFF);
    uint32_t exclude_group = (uint32_t)luaL_optinteger(L, 6, 0);
    int context            = (int)luaL_optinteger(L, 7, 0);
    LevelPhysicsHit hit;
    bool found = level_sweep_cylinder_closest(src, dst, extents, &hit,
                                               phys_type, include_group,
                                               exclude_group, context);
    return push_hit_or_nil(L, found, &hit);
}

/**
 * Ext.Level.SweepCylinderAll(src, dst, extents, physType,
 *                            includeGroup, excludeGroup, context)
 *   extents: {x, y, z} cylinder extents
 *   Returns: array of hit tables
 */
static int lua_level_sweep_cylinder_all(lua_State *L) {
    float src[3], dst[3], extents[3];
    if (!read_vec3(L, 1, src))     return luaL_error(L, "SweepCylinderAll: src must be {x,y,z}");
    if (!read_vec3(L, 2, dst))     return luaL_error(L, "SweepCylinderAll: dst must be {x,y,z}");
    if (!read_vec3(L, 3, extents)) return luaL_error(L, "SweepCylinderAll: extents must be {x,y,z}");
    uint32_t phys_type     = (uint32_t)luaL_optinteger(L, 4, 1);
    uint32_t include_group = (uint32_t)luaL_optinteger(L, 5, 0x7FFFFFFF);
    uint32_t exclude_group = (uint32_t)luaL_optinteger(L, 6, 0);
    int context            = (int)luaL_optinteger(L, 7, 0);
    LevelPhysicsHitAll hits;
    bool found = level_sweep_cylinder_all(src, dst, extents, &hits,
                                           phys_type, include_group,
                                           exclude_group, context);
    if (!found) {
        lua_pushnil(L);
        return 1;
    }
    return push_hit_all(L, &hits);
}

// ============================================================================
// Tile Queries
// ============================================================================

/**
 * Ext.Level.GetHeightsAt(x, z) -> array of heights (or empty table)
 */
static int lua_level_get_heights_at(lua_State *L) {
    float x = (float)luaL_checknumber(L, 1);
    float z = (float)luaL_checknumber(L, 2);

    float *heights = NULL;
    size_t count = 0;
    (void)level_get_heights_at(x, z, &heights, &count);

    lua_createtable(L, (int)count, 0);
    for (size_t i = 0; i < count; i++) {
        lua_pushnumber(L, heights[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    level_free_heights(heights);

    return 1;
}

// ============================================================================
// AiGrid Tile and Pathfinding APIs
// ============================================================================

static int lua_level_get_entities_on_tile(lua_State *L) {
    float world_pos[3];
    if (!read_vec3(L, 1, world_pos)) {
        return luaL_error(
            L, "Ext.Level.GetEntitiesOnTile: pos must be a {x,y,z} table");
    }

    uint64_t *handles = NULL;
    size_t handle_count = 0;
    if (!level_aigrid_get_entities_on_tile(
            world_pos, &handles, &handle_count)) {
        /* Grid unavailable is distinct from an empty tile: return nil. */
        lua_pushnil(L);
        return 1;
    }

    lua_newtable(L);
    for (size_t i = 0; i < handle_count; i++) {
        /* Match Ext.Entity/GetHandle and event EntityHandle conventions. */
        lua_pushinteger(L, (lua_Integer)handles[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    level_aigrid_free_entity_handles(handles);
    return 1;
}

/**
 * Ext.Level.GetTileDebugInfo(pos) -> AiGridLuaTile-shaped table, or nil
 *
 * Undeferred 2026-08-20. MinHeight at +0x0a is CONFIRMED (two independent
 * accessor sites sharing the /50 scaling), and the flag decoders are plain
 * shifts over the engine's own 64-bit Flags word (Ai.h:41-80) rather than C
 * bitfields, so they are a data format rather than a compiler layout and port
 * directly. Two of the five shifts were already verified here and ship as
 * GetTileRawDebugInfo.
 */
static int lua_level_get_tile_debug_info(lua_State *L) {
    float pos[3];
    if (!read_vec3(L, 1, pos)) {
        return luaL_error(L, "Ext.Level.GetTileDebugInfo: pos must be a {x,y,z} table");
    }

    LevelAiTileDebugInfo info;
    if (!level_aigrid_get_tile_debug_info(pos[0], pos[2], &info)) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 13);
    lua_pushinteger(L, (lua_Integer)info.flags);            lua_setfield(L, -2, "Flags");
    lua_pushinteger(L, (lua_Integer)info.ground_surface);   lua_setfield(L, -2, "GroundSurface");
    lua_pushinteger(L, (lua_Integer)info.cloud_surface);    lua_setfield(L, -2, "CloudSurface");
    lua_pushinteger(L, (lua_Integer)info.material);         lua_setfield(L, -2, "Material");
    lua_pushinteger(L, (lua_Integer)info.unmapped_flags);   lua_setfield(L, -2, "UnmappedFlags");
    lua_pushinteger(L, (lua_Integer)info.extra_flags);      lua_setfield(L, -2, "ExtraFlags");
    lua_pushinteger(L, (lua_Integer)info.subgrid_id);       lua_setfield(L, -2, "SubgridId");
    lua_pushinteger(L, (lua_Integer)info.tile_x);           lua_setfield(L, -2, "TileX");
    lua_pushinteger(L, (lua_Integer)info.tile_y);           lua_setfield(L, -2, "TileY");
    lua_pushnumber(L, (lua_Number)info.min_height);         lua_setfield(L, -2, "MinHeight");
    lua_pushnumber(L, (lua_Number)info.max_height);         lua_setfield(L, -2, "MaxHeight");
    lua_pushinteger(L, (lua_Integer)info.metadata_index);   lua_setfield(L, -2, "MetaDataIndex");
    lua_pushinteger(L, (lua_Integer)info.surface_metadata_index);
    lua_setfield(L, -2, "SurfaceMetaDataIndex");
    return 1;
}

/**
 * Ext.Level.GetTileRawDebugInfo(x, z) -> raw diagnostic table or nil
 *
 * RawFlags is the complete native AiFlags word. GroundMask and CloudMask are
 * unconverted 8-bit fields; this surface intentionally claims no Windows
 * GetTileDebugInfo parity.
 */
static int lua_level_get_tile_raw_debug_info(lua_State *L) {
    float x = (float)luaL_checknumber(L, 1);
    float z = (float)luaL_checknumber(L, 2);

    LevelAiTileRawDebugInfo info;
    if (!level_aigrid_get_tile_raw_debug_info(x, z, &info)) {
        lua_pushnil(L);
        return 1;
    }

    lua_createtable(L, 0, 7);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "Raw");
    lua_pushinteger(L, (lua_Integer)info.raw_flags);
    lua_setfield(L, -2, "RawFlags");
    lua_pushinteger(L, info.ground_mask);
    lua_setfield(L, -2, "GroundMask");
    lua_pushinteger(L, info.cloud_mask);
    lua_setfield(L, -2, "CloudMask");
    lua_pushnumber(L, info.min_height);
    lua_setfield(L, -2, "MinHeight");
    lua_pushnumber(L, info.max_height);
    lua_setfield(L, -2, "MaxHeight");
    return 1;
}

static int lua_level_begin_pathfinding(lua_State *L) {
    static bool warned = false;
    warn_deferred_once(
        &warned, "BeginPathfinding",
        "AIGRID_PATHFINDING.md leaves complete character-path setup OPEN: "
        "the engine writes fields beyond AiGrid::CreatePath; returning nil");
    lua_pushnil(L);
    return 1;
}

static int lua_level_find_path(lua_State *L) {
    int32_t path_id = 0;
    if (!read_path_id(L, 1, &path_id)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    /*
     * Preserve Windows FindPath's synchronous behavior by default. Passing
     * false queues only; true/omitted also runs FindPathImmediate on the Lua
     * gate's game thread.
     */
    bool immediate = lua_gettop(L) < 2 || lua_toboolean(L, 2);

    LevelAiPathResult result;
    if (!level_aigrid_find_path(path_id, immediate, &result)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    push_path_result(L, &result);
    level_aigrid_free_path_result(&result);
    return 1;
}

static int lua_level_release_path(lua_State *L) {
    int32_t path_id = 0;
    if (!read_path_id(L, 1, &path_id)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    lua_pushboolean(L, level_aigrid_release_path(path_id));
    return 1;
}

static int lua_level_get_path_by_id(lua_State *L) {
    int32_t path_id = 0;
    LevelAiPathState state;
    if (!read_path_id(L, 1, &path_id)
        || !level_aigrid_get_path_by_id(path_id, &state)) {
        lua_pushnil(L);
        return 1;
    }

    push_path_state(L, &state);
    return 1;
}

static int lua_level_get_active_pathfinding_requests(lua_State *L) {
    LevelAiPathState *states = NULL;
    size_t state_count = 0;
    if (!level_aigrid_get_active_pathfinding_requests(
            &states, &state_count)) {
        /* Grid unavailable is distinct from no active requests: return nil. */
        lua_pushnil(L);
        return 1;
    }

    lua_newtable(L);
    for (size_t i = 0; i < state_count; i++) {
        push_path_state(L, &states[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    level_aigrid_free_path_states(states);
    return 1;
}

// ============================================================================
// Registration
// ============================================================================

static const struct luaL_Reg level_functions[] = {
    {"IsReady",            lua_level_is_ready},
    {"GetCurrentLevel",    lua_level_get_current},
    {"GetPhysicsScene",    lua_level_get_physics_scene},
    {"GetAiGrid",          lua_level_get_aigrid},
    {"RaycastClosest",       lua_level_raycast_closest},
    {"RaycastAll",           lua_level_raycast_all},
    {"RaycastAny",           lua_level_raycast_any},
    {"SweepSphereClosest",   lua_level_sweep_sphere_closest},
    {"SweepSphereAll",       lua_level_sweep_sphere_all},
    {"SweepCapsuleClosest",  lua_level_sweep_capsule_closest},
    {"SweepCapsuleAll",      lua_level_sweep_capsule_all},
    {"SweepBoxClosest",      lua_level_sweep_box_closest},
    {"SweepBoxAll",          lua_level_sweep_box_all},
    {"SweepCylinderClosest", lua_level_sweep_cylinder_closest},
    {"SweepCylinderAll",     lua_level_sweep_cylinder_all},
    {"TestBox",              lua_level_test_box},
    {"TestSphere",           lua_level_test_sphere},
    {"GetHeightsAt",         lua_level_get_heights_at},
    {"GetEntitiesOnTile",     lua_level_get_entities_on_tile},
    {"GetTileRawDebugInfo",   lua_level_get_tile_raw_debug_info},
    {"GetTileDebugInfo",      lua_level_get_tile_debug_info},
    {"BeginPathfinding",      lua_level_begin_pathfinding},
    {"FindPath",              lua_level_find_path},
    {"ReleasePath",           lua_level_release_path},
    {"GetPathById",           lua_level_get_path_by_id},
    {"GetActivePathfindingRequests",
                               lua_level_get_active_pathfinding_requests},
    {NULL, NULL}
};

void lua_level_register(lua_State *L, int ext_table_idx) {
    lua_newtable(L);

    for (const struct luaL_Reg *fn = level_functions; fn->name != NULL; fn++) {
        lua_pushcfunction(L, fn->func);
        lua_setfield(L, -2, fn->name);
    }

    lua_setfield(L, ext_table_idx - 1, "Level");
}
